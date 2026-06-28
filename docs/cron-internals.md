# cron — internals

Maintainer reference for `cron.cpp` / `cron.h`. The [operator guide](cron.md) is
the user-facing surface; this document is for changing the code without breaking
the deep-sleep contract. Source: [`esp-idf/src/cron.cpp`](../esp-idf/src/cron.cpp),
[`esp-idf/include/cron.h`](../esp-idf/include/cron.h).

## 1. What cron adds

- **`cronInit()`** — wired as an `init:` hook in the straddle's `straddle.yaml`
  (`init: [storageInit, cronInit]`) and run by the generated
  `spangapInitStraddles()` dispatcher, **not** from `spangapInit()`. core heads
  the dispatcher's platform band, so it runs first of all straddle inits. It
  seeds `s.cron.enable=1` (version-gated on `s.cron.version` < `CRON_VERSION`,
  currently 1), creates the `cron` `PM_NO_DEEP_SLEEP` lock and **acquires it**,
  then spawns the `cron` task (stack 4096, prio 1, core 0).
- **`cronWakeupHandler()`** — called early in `spangapInit()` (right after
  `pmInit()` / `authInit()`, before the dispatcher and before any task spawn).
  The deep-sleep fast path: if this boot is a timer wake *and* RTC RAM is valid,
  it runs `cronPoll(false)` (dry run); if nothing matches this minute it goes
  **straight back to deep sleep without finishing boot** — no tasks, no straddle
  inits. If there is work, it returns and full boot proceeds. On any non-timer
  wake (cold boot, reset, GPIO) it returns immediately and boot proceeds
  normally.
- **`cronPoll(bool execute)`** — the matcher. Reads `<stateDir>/crontab` via
  `fsStatePath("/crontab")` (never a hard-coded `/state`), parses line by line,
  and for each match feeds the command into `cronStream`. `execute=false` is a
  pure dry run (used by the wake handler) — it neither advances `cronLastMinute`
  nor sends commands; `execute=true` advances the minute and sends. Returns
  whether any entry matched this minute.
- **`cronDrainCommands()`** — called by the CLI task in its main loop
  (`cli.cpp`). Pulls newline-framed commands out of `cronStream` and runs each
  through `cliProcess()`, so cron jobs share the CLI's serialisation with typed
  input.
- **`cronDefault(schedule, command)`** — append-if-absent crontab writer for
  sibling straddles (§4).
- **Storage keys** — `s.cron.enable` (default 1), `s.cron.version` (current 1).
- **RTC RAM** — `cronLastMinute` (`RTC_DATA_ATTR`), the epoch-minute already
  serviced, so a wake doesn't re-run a minute.
- **The `cron` deep-sleep lock** and the `sys.going_down` subscription that turns
  a last-lock-release into actual sleep (§3).

## 2. Task model

One FreeRTOS task, **core 0, prio 1, 4 KB stack**. Its loop is a single ITS wait
point:

```
for (;;) { while (itsPoll(30000ms)) {} cronPoll(true); }
```

`itsPoll` drains any pending ITS work; when it returns false (timeout, ~30 s
ceiling) cron polls the crontab. The 30 s ceiling bounds the worst-case latency
between a minute boundary and the job firing; it is not a precise tick.

The task creates `cronStream` (a 256-byte `StreamBuffer`, element size 1) in its
own context so heap tracking attributes it to `cron`, then calls
`cronUpdateLock()` once and subscribes:

- `storageSubscribeChanges("s.cron", …)` → `cronUpdateLock()` on any `s.cron.*`
  change.
- `storageSubscribeChanges("sys.going_down", …)` → `cronDeepSleep()` (never
  returns) when the value is non-zero.

`cronStream` is the only cross-task surface: `cronPoll` (cron task) writes
commands in; `cronDrainCommands` (CLI task) reads them out. Nothing else touches
cron's state across tasks.

## 3. Deep-sleep handshake with pm

cron never calls `esp_deep_sleep_start()` on its own schedule. The path is:

1. `cronUpdateLock()` computes `allow = cronEnabled() && crontabHasEntries()`.
   When allowed it **releases** the `cron` lock; otherwise it holds it. A static
   `released` flag makes the release/acquire idempotent. The lock is acquired in
   `cronInit()` before the task runs, so the default state is *held* (deep sleep
   blocked) until proven otherwise.
2. With the `cron` lock released and every other power lock also released,
   `pmLockRelease()` (in [pm](power-management.md)) sees `deepSleepAllowed()`
   become true and sets `sys.going_down=1`.
3. cron's `sys.going_down` subscription fires `cronDeepSleep()`, which computes
   the sleep duration as `61 - (now % 60)` seconds (the `+1` lands the wake
   inside the new minute, not on its edge), calls `rtcRamSetValid()`,
   `pmRecordDeepSleep(sleepUs)` (so pm's RTC mode stats stay accurate across the
   nap), arms `esp_sleep_enable_timer_wakeup()`, and enters deep sleep after a
   50 ms drain delay.
4. On the timer wake, `cronWakeupHandler()` runs the fast path (§1). If still no
   work, it loops straight back through `cronDeepSleep()` without a full boot.

`isDeepSleepWake` (file static, set in the wake handler) is what makes the `A`
and `N` flags skip on a brief timer wake: an `A`/`N`-flagged entry is suppressed
when `isDeepSleepWake` is true, so housekeeping that only matters while the device
is "really" up doesn't run during a fast wake-and-sleep cycle.

## 4. Field matching and column layout

`cronMatch()` tokenises the five time fields plus the flags field, then returns a
pointer to the command tail. `cronMatchField()` handles one field with comma
lists (iterative, to bound recursion), `*`, `N`, `N-M` ranges, and `/step` on
any of those. `parseFlags()` maps `A`→`CRON_FLAG_AWAKE`, `N`→`CRON_FLAG_NETWORK |
CRON_FLAG_AWAKE` (N implies A), `-`→0.

`cronDefault()` writes a new line formatted to match the column header in the
factory crontab: it splits `schedule` into up to six whitespace fields (five time
fields + flags) and prints each as `%-4s`, then the command verbatim — `"%-4s
%-4s %-4s %-4s %-4s %-4s %s\n"`. Before appending it scans every existing line
with `extractCronCommand()` (which skips a leading `#` and the six leading fields)
and bails if any line — active or commented — already carries that exact command
string. It appends a leading newline first if the file didn't end in one. The
write opens `"a"` if the file exists, `"w"` otherwise.

## 5. Pitfalls

- **An empty or disabled crontab pins the device awake.** The `cron` lock is held
  by default and released only with `s.cron.enable=1` *and* a non-comment entry
  present. A device that should deep sleep but won't is almost always a crontab
  with no active line — there is intentionally no way to deep sleep without a
  schedule that can wake it back up.
- **Deep sleep is never initiated here directly.** It is always the downstream
  effect of the *last* power lock releasing (pm sets `sys.going_down`, cron
  reacts). Don't add a direct `esp_deep_sleep_start()` call on a cron timer —
  that bypasses every other holder's lock.
- **`cronWakeupHandler()` must stay ahead of task spawning in `spangapInit()`.**
  Its whole value is deciding *before* full boot whether to sleep again; moving it
  after the dispatcher would boot the entire system just to sleep a second later.
- **Sibling defaults respect user edits until a version bump.** `cronDefault`
  matches on the command string across active *and* commented lines, so a user
  who deletes or comments a default keeps it gone — it only reappears when the
  owning straddle bumps its own `s.<mod>.version`. Don't make `cronDefault`
  re-add unconditionally.
- **`fsStatePath`, not `/state`.** The active state store can be on SD
  (`/sdcard/state`); always build the crontab path through `fsStatePath("/crontab")`.
- **`N` reads `wifi.sta.state` off the bus, not net's API.** core has no net
  dependency; a missing key means "never up", so `N` jobs silently don't run on a
  no-network build. That's intended, not a bug.
