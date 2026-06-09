# CPU boost (notify-driven)

Status: **implemented**, and automatic — no per-task opt-in. `itsPoll` boosts the
CPU to max for the handling of any wake that came from a **notify** (an ITS
message, an ISR like lora's DIO1, input) — a real event — and stays at the DFS
floor for a wake that merely **timed out** (a routine housekeeping tick). The
boost is held from the notify-wake until the task's next block. Plus a manual
`pmBoost()`/`pmBoostEnd()` for sustained needs (heavy timeout-path work, or
continuous non-notify loops like net/webrtc).

**Update (per-task locks):** rather than one shared `"boost"` lock, each task now
owns a recursive `CPU_FREQ_MAX` lock named after the task, created lazily on its
first boost (registry in `pm.cpp`, `BOOST_MAX_TASKS`). Its count is that task's
auto count (0/1) plus its manual `pmBoost()` depth. This makes `pm` /
`esp_pm_dump_locks` attribute boost time and total_count per task, and a stuck
count names its own leaker — so the old `s_boostHolders` debug registry is gone.
The per-task "do I hold the auto count" marker still lives in TLS slot 1
(`CONFIG_FREERTOS_THREAD_LOCAL_STORAGE_POINTERS=2`), now holding the task's own
lock handle. The single-shared-lock design below is the original plan, kept for
context.

This supersedes the original per-task `spawnTask(boost=)` flag: the notify is a
better signal than a static per-task bool, because it boosts *exactly* when a
task has a real event to handle and leaves housekeeping ticks at the floor — no
per-task tuning, and it auto-handles cases like rnsd (boost on a packet, floor
on its 1 Hz `Transport::jobs()` tick) that would otherwise need internals study.

The goal is unchanged: run at 240 MHz *while handling work* and fall back to the
DFS floor *while idle/blocked* — without pinning the whole chip and without
hand-managing PM locks at every call site.

## Why this exists

With light sleep blocked (USB connected holds a `NO_LIGHT_SLEEP` lock) and
`min_freq_mhz = 80`, the system sits at 80 MHz whenever nothing holds a
`CPU_FREQ_MAX` lock. That's correct DFS behaviour. The problem is the two bad
ways to opt a task into 240 MHz:

- **Hold `CPU_FREQ_MAX` for the task's whole lifetime.** The lock's refcount
  stays > 0 even while the task is blocked, so the chip never drops to 80. This
  *pins* 240 MHz for a task that is asleep most of the time.
- **Acquire/release by hand around each piece of work.** Correct, but it
  smears PM lock calls across the whole codebase and is easy to get wrong
  (every early return, every error path, every block must release).

What we want: a task marked "boost" runs at 240 MHz while executing and
contributes nothing to the frequency floor while it is parked in a blocking
call. The refcount then naturally tracks "how many boost tasks are currently
running," and the chip is at 240 iff at least one of them is awake.

Non-goals: this is `CPU_FREQ_MAX` only. APB and light/deep sleep are unchanged.
(APB boost could ride the same mechanism later — see Open forks.)

## The mechanism

### One shared lock, counted

Do **not** create a lock per task. `pmLockCreate` prepends to `pmLockList` and
there is no delete; the list is for a fixed set of long-lived named locks that
the `pm` command walks. Per-task create/destroy would grow it unbounded and
pollute the stats.

`pmLockAcquire`/`pmLockRelease` are already recursive (counted). So use a single
process-wide lock:

```c
// pm.cpp, created once at PM init:
static pm_lock_handle_t s_boostLock;   // pmLockCreate(PM_CPU_FREQ_MAX, "boost", &s_boostLock)
```

Every boosted task holds exactly one count on `s_boostLock` while running, and
releases it across each block. The shared count aggregates for free: 240 MHz
while ≥1 boost task is awake, 80 MHz when all are blocked.

### Bracket the one block point

A boosted task's count must drop *only* while it is genuinely blocked. There is
exactly one place per blocking primitive to bracket:

- `itsPoll` → `ulTaskNotifyTake(pdTRUE, timeout)` (its.cpp:658, and :640 for the
  non-ITS path). This is the universal block point.
- the `delay()` / `vTaskDelay` wrapper → around `vTaskDelay`.
- the lwIP `select`/`recv` bracket in the TCP task → around the syscall
  (lwIP blocks internally; no wrapper can hide it).

The bracket is two calls exposed by PM:

```c
void pmBlockEnter(void);   // about to block: release this task's boost count (if any)
void pmBlockExit(void);    // back from blocking: re-acquire it (if any)
```

Both are no-ops for non-boosted tasks. Crucially the bracket goes around the
*actual wait only*, never the whole function — `itsPoll` drains the inbox and
dispatches callbacks first (its.cpp:647-654) and must do that at full speed; it
only calls `pmBlockEnter` when it is about to sleep (timeout > 0 and nothing was
drained). The `while (itsPoll(0)) {}` drain idiom must therefore cost nothing.

### Lifecycle

- `spawnTask(..., boost=true)` marks the new task boosted and acquires its one
  count, so it starts active.
- `killSelf()` (and any external delete) releases the held count, so a dying
  boosted task can't leave the floor stuck at 240.

## Where the per-task "am I boosted" bit lives

The bracket needs an O(1) "what's my boost handle" lookup on the block hot path.
Three options:

1. **FreeRTOS thread-local storage (recommended).** `spawnTask` stores
   `s_boostLock` (or `nullptr`) in a reserved TLS slot via
   `vTaskSetThreadLocalStoragePointerAndDelCallback`. `pmBlockEnter` does
   `pmLockRelease(tls_get(slot))` and `pmBlockExit` does `pmLockAcquire(...)`.
   `pmLock*(nullptr)` is already a no-op, so the non-boost path is branchless and
   free. O(1), works for any task (ITS-registered or not), and the del-callback
   gives cleanup-on-delete for free. Cost: needs
   `CONFIG_FREERTOS_THREAD_LOCAL_STORAGE_POINTERS` bumped and one reserved slot
   index (it is currently unset → IDF default).
2. **PM-owned `TaskHandle_t → handle` table.** No TLS config, but a linear scan
   per block, and PM has to manage add/remove on spawn/kill itself.
3. **Reuse ITS's `its_task_t`.** Rejected — see the ITS question. It only exists
   for ITS-registered tasks and it co-opts ITS's private state for a PM concern.

Recommendation: **TLS.** The hot-path lookup wins, and it decouples the feature
from whether the task uses ITS at all.

## The ITS question: should this be "part of ITS"?

`itsPoll` is the universal block point, and ITS already keeps a per-task
registry — so it is tempting to put the whole thing inside ITS.

**For folding it into ITS**
- The block point is already there. Two lines at its.cpp:658/640 cover almost
  all blocking with zero plumbing.
- ITS already has `its_task_t` keyed by `TaskHandle_t`; the boost handle could
  live there with no new storage.

**Against**
- Layering. ITS is IPC/messaging. CPU-frequency policy is orthogonal; baking it
  into ITS muddies ITS's responsibility and makes ITS un-buildable without PM
  (host builds, tests, a board with PM off).
- Coverage. Boost is about *all* blocking, not just ITS blocking — `vTaskDelay`,
  the lwIP `select` bracket, a future mutex wait. The `vTaskDelay` wrapper
  already lives outside ITS. If "boost" were an ITS feature, these would be
  second-class.
- `its_task_t` only exists for ITS-registered tasks. A boosted task that blocks
  via `itsPoll`'s non-ITS path (`myTask()==NULL`, its.cpp:639) has no
  `its_task_t` to hang the handle on. TLS has no such gap.

**Recommendation: split the seam.** PM **owns** the state and policy (the shared
lock, the TLS slot, `pmBlockEnter/Exit`, `pmTaskBoost`). ITS only **calls** the
two-line hook at its block point — `itsPoll` tells PM "I'm about to sleep / I'm
back," the same way `delay()` and the TCP bracket do. So the construct is *not*
part of ITS in the sense of owning anything; ITS is just one of several callers
of a PM primitive. The only new coupling is `its.cpp` → `pm.h` for two function
calls, which is honest: the block point genuinely is where the boost should drop.

## Edge cases / things to get right

- **Recursion is self-balancing.** With the shared counted lock, a boosted task
  holds 0 or 1 at any instant. If a drained ITS callback itself blocks
  (nested `itsPoll`/`delay`), its inner enter/exit pair release→re-acquire around
  its own wait; the outer level is untouched. Always paired at each block point.
- **`killSelf` cleanup is mandatory.** Release the held count before
  `vTaskDeleteWithCaps`. The TLS del-callback also covers external `vTaskDelete`.
- **Hot-path cost of `pmLockRelease`.** It calls `deepSleepAllowed()`, which
  walks `pmLockList` every release, and may `storageSet("sys.going_down", 1)`.
  On the boost block path (potentially every `itsPoll`) that list-walk is waste —
  the boost lock never gates deep sleep (that's `NO_DEEP_SLEEP`). Give
  `pmBlockEnter/Exit` a lean path that hits `esp_pm_lock_release/acquire` (plus
  lightweight stats) and skips the `going_down` bookkeeping.
- **Boost is for meaningful-interval blockers, not fast pollers.** A task that
  blocks every few hundred µs re-triggers a 240 up-switch on every wake and the
  deferred down-switch may never reach idle — so it pays overhead for no power
  win. Those tasks should stay `boost=false` and ride 80 MHz (e.g. kbpoll).
- **Blocking outside the wrapped primitives leaks the boost across the block.**
  A boosted task that calls a raw `vTaskDelay`, a mutex, or a driver that blocks
  internally holds 240 across that wait. `delay()` is wrapped; audit for raw
  blocks in boosted tasks.

## Proposed surface

```c
// pm.h
void pmBoostInit(void);                 // create s_boostLock (called from PM init)
void pmTaskBoost(TaskHandle_t, bool on);// mark/unmark a task; acquires on enable
void pmBlockEnter(void);                // release this task's boost (no-op if none)
void pmBlockExit(void);                 // re-acquire this task's boost (no-op if none)

// compat.h — spawnTask gains a trailing default arg (all existing call sites unaffected)
TaskHandle_t spawnTask(TaskFunction_t fn, const char* name, uint32_t stackBytes,
                       void* arg, UBaseType_t prio, BaseType_t core,
                       stack_caps_t stackMem = STACK_PSRAM, bool boost = false);
```

Call sites touched: `spawnTask` + `killSelf` (compat.h), the two block points in
`itsPoll` (its.cpp), the `delay()` wrapper (compat.h), and the TCP `select`
bracket. Everything else is unchanged.

## Open forks

1. ~~**TLS slot vs PM table**~~ — RESOLVED: TLS. Slot index 1 (`TLS_PM_BOOST`),
   slot 0 left for IDF; `pmTaskBoost` sets it, `pmBlockEnter/Exit` read it.
   `pm.cpp` `#error`s if `CONFIG_FREERTOS_THREAD_LOCAL_STORAGE_POINTERS` isn't
   bumped past it. Note: hw-tdeck's `sdkconfig` had manual menuconfig edits, so
   the `sdkconfig.defaults.spangap` bump was ignored by the staleness check and
   the count had to be set directly in the live `sdkconfig` — watch for this on
   any consumer whose sdkconfig has diverged from defaults.
2. **Which tasks to opt in.** Nothing is `boost=true` yet — that's a deliberate
   policy call. Candidates: the heavy/interactive ones (lcd render, net), *not*
   fast pollers (kbpoll stays false). Decide per-task with `pm`/`top` in hand.
3. **APB boost.** Some tasks need `APB_FREQ_MAX` (peripheral throughput), not
   CPU. Same mechanism with a second shared lock and a `boost` enum instead of a
   bool — defer until something actually needs it.
3. **Lean vs full `pmLock*` on the hot path.** Decide whether the boost bracket
   bypasses the `going_down` bookkeeping (recommended) and how/whether it keeps
   the `pm` stats line meaningful for the boost lock.
