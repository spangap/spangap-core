# cron — internals

Maintainer reference for `cron.cpp` / `cron.h`. The [operator guide](cron.md) is
the user-facing surface; this document is for changing the code without breaking
the scheduling and deep-sleep contracts. Source:
[`esp-idf/src/cron.cpp`](../esp-idf/src/cron.cpp),
[`esp-idf/include/cron.h`](../esp-idf/include/cron.h).

## 1. The scheduling model

The steady-state flow, end to end:

```
entry change (s.cron.tab.* / s.cron.enable)
  → cronReschedule(): collect entries, refresh pm lock,
      cronNextMinute = min over ALL entries   of cronNextRun(entry, last+1)
      cronWakeMinute = min over non-A entries of cronNextRun(entry, last+1)
      (both RTC_DATA_ATTR — survive deep sleep and soft resets)
poll (cron task ~30s / end of boot / deep-sleep wake)
  → cronPoll(): now < cronNextMinute? return.        ← the entire common case
      else run every entry due in (cronLastMinute, now], flags applied,
      cronLastMinute = now, recompute both minutes from now+1
```

There is no per-minute crontab scan: the work happens only on entry changes and
on minutes where something actually runs. On hardware with a working RTC chip
the 30 s poll and the deep-sleep timer iteration (§4) both collapse into
"program the RTC alarm for `cronWakeMinute`, wake on the int pin, `cronPoll()`"
— that is the intended replacement seam (not built; no unit with a live RTC has
been seen yet — it will be a straddle of its own, like GPS).

- **`cronNextRun(parsed, fromMinute)`** — earliest epoch minute ≥ `fromMinute`
  whose local time matches all five fields, or `UINT32_MAX` if none within
  ~400 days (malformed/impossible entry — warned once per reschedule). It steps
  day-first (dom/mon/dow checked per day, then hour, then minute), each step
  renormalised through `mktime` with `tm_isdst=-1`, so a sparse schedule costs
  hundreds of field checks, not half a million.
- **Dueness is a window, not an equality.** `cronPoll` fires an entry when
  `cronNextRun(entry, cronLastMinute+1) <= now`: a poll that lands late — sleep
  timer skew, a busy CLI, a long ITS drain — still fires the slot once instead
  of silently skipping it.
- **Clock-jump guard.** If `cronLastMinute` is 0, ahead of now, or more than a
  day behind (cold boot zeroes RTC RAM; NTP's first sync jumps out of 1970; a
  manual clock set goes either way), the due window is meaningless: re-anchor
  `cronLastMinute = now`, recompute, run nothing. This also stops pre-NTP boots
  from firing wall-clock entries against a 1970 clock.

## 2. Task model

One FreeRTOS task, **core 0, prio 1, 4 KB stack**. Its loop is a single ITS wait
point:

```
for (;;) { while (itsPoll(30000ms)) {} cronPoll(); }
```

The 30 s ceiling bounds the worst-case latency between a minute boundary and the
job firing; it is not a precise tick, and thanks to the stored minute the poll
is a mutex-take plus one compare on quiet passes.

The task creates `cronStream` (a 256-byte `StreamBuffer`, element size 1) in its
own context so heap tracking attributes it to `cron`, runs one
`cronReschedule()`, then subscribes `storageSubscribeChanges("s.cron", …)` →
`cronReschedule()` — one subscription covers `s.cron.tab.*` edits and the
`s.cron.enable` switch. It also hosts `spangapWatchSafeModeFlags()` (a storage
subscription is delivered to its registering task, and this is core's long-lived
housekeeping actor).

**The end-of-boot resync is load-bearing.** Module onInits install their
`s.cron.tab.*` entries during the `serviceRunInit()` walk, possibly before the
cron task has registered its subscription (both run early, unordered against
each other). `spangapPostAppInit()` therefore calls `cronReschedule()` +
`cronPoll()` once after the walk — that pass sees everything boot wrote, and
the subscription owns all changes from then on.

Shared state (`s_entries` scratch, the RTC minute bookkeeping, the pm-lock
`released` flag) is guarded by `cronMutex`: `cronPoll`/`cronReschedule` run on
the cron task, and once on app_main at end of boot. The cron subscription stays
on the cron task rather than `onStorageTask=true` because `cronReschedule`
takes `cronMutex` and flips pm locks — too much to host inline at storage
dispatch.

`cronStream` is the only other cross-task surface: `cronPoll` (cron task) writes
commands in; `cronDrainCommands` (CLI task) reads them out and runs each through
`cliProcess()`, so cron jobs share the CLI's serialisation with typed input.

## 3. Deep-sleep handshake with pm

*(The entry paths are currently disabled — `cronDeepSleep` is `#if 0`, the
`sys.going_down` subscription is commented, and `cronWakeupHandler` never
re-sleeps — but the contract below is what the code implements when wired.)*

cron never calls `esp_deep_sleep_start()` on its own schedule. The path is:

1. `cronReschedule()` computes `allow = cronEnabled() && any s.cron.tab.* key`
   and releases the `cron` `PM_NO_DEEP_SLEEP` lock when allowed, else holds it
   (idempotent via a static `released` flag). The lock is acquired in
   `cronInit()` before the task runs, so the default state is *held* (deep
   sleep blocked) until proven otherwise.
2. With the `cron` lock released and every other power lock also released,
   `pmLockRelease()` (in [pm](power-management.md)) sees `deepSleepAllowed()`
   become true and sets `sys.going_down=1`.
3. cron's `sys.going_down` subscription fires `cronDeepSleep()`, which sleeps
   toward `cronWakeMinute` (§4) — never returns.
4. On the timer wake, `cronWakeupHandler()` (early in `spangapInit()`, before
   any task spawn) compares the clock against `cronWakeMinute`: minute not
   arrived (skew-margin wake) → hop again via `cronDeepSleep()` without
   finishing boot; arrived → boot proceeds and the cron task's poll services
   the minute.

`cronWakeMinute` deliberately skips `A`-flagged entries: `isDeepSleepWake`
(file static, set in the wake handler) suppresses `A`/`N` entries when the
minute is serviced by a deep-sleep wake, so waking for one would burn a boot
cycle to run nothing. The all-entries `cronNextMinute` still drives the awake
poll, where `A` entries do run. If only `A`/`N` entries exist there is no wake
target (`cronWakeMinute = UINT32_MAX`) — but the lock model already keeps such
a device from sleeping only when there are *no* entries at all, so a schedule
of only awake-work still permits sleep it can't wake from; that is the
operator's call, same as an empty schedule pinning the device awake is.

## 4. Skew-margin wake iteration

Sleep timers run off an RC oscillator; even light sleep has no crystal, and the
skew can reach single-digit percent. A single full-length sleep to the minute
could therefore overshoot it. `cronDeepSleep()` instead:

- targets `T = cronWakeMinute*60 + 1` (the +1 s lands inside the minute, not on
  its edge);
- each hop sleeps **85% of the remaining time** — with ≤~10% skew the wake can
  only land short of T, never past it;
- once ≤5 s remain it sleeps the full remainder (worst-case skew there is tens
  of milliseconds, still inside the minute);
- `cronWakeupHandler()` re-hops on every early wake. Remaining time shrinks
  ~7× per hop, so it converges in a handful of wakes.

Each hop calls `rtcRamSetValid()` and `pmRecordDeepSleep()` so pm's RTC mode
stats stay accurate across the nap.

## 5. Field matching and parsing

`cronParse()` tokenises the five time fields plus the flags field into a
`cron_parsed_t` and points `cmd` at the command tail. `cronMatchField()` handles
one field with comma lists (iterative, to bound recursion), `*`, `N`, `N-M`
ranges, and `/step` on any of those. `parseFlags()` maps `A`→`CRON_FLAG_AWAKE`,
`N`→`CRON_FLAG_NETWORK | CRON_FLAG_AWAKE` (N implies A), `-`→0. Both the due
check and `cronNextRun` use the same matcher, so they cannot disagree.

Entry collection is `storageForEach("s.cron.tab", …)` into the `s_entries`
scratch (file-static because `storageForEach`'s callback carries no user
pointer; `cronMutex` guards it).

## 6. Pitfalls

- **No entries (or disabled) pins the device awake.** The `cron` lock is held by
  default and released only with `s.cron.enable=1` *and* at least one
  `s.cron.tab.*` key. A device that should deep sleep but won't is almost always
  an empty schedule — there is intentionally no way to deep sleep without a
  schedule that can wake it back up.
- **Deep sleep is never initiated here directly.** It is always the downstream
  effect of the *last* power lock releasing (pm sets `sys.going_down`, cron
  reacts). Don't add a direct `esp_deep_sleep_start()` call on a cron timer —
  that bypasses every other holder's lock.
- **`cronWakeupHandler()` must stay ahead of task spawning in `spangapInit()`.**
  Its whole value is deciding *before* full boot whether to sleep again; moving
  it after the dispatcher would boot the entire system just to sleep a second
  later. Its decision is a compare against RTC RAM — it needs nothing else up.
- **Don't drop the end-of-boot `cronReschedule()`** (§2): without it, entries
  installed by onInits before the cron task subscribed are invisible until the
  next entry change, and the stored minutes go stale.
- **Never overshoot when arming a sleep timer.** The due-window check recovers
  a missed minute *once the device is up*, but a deep-sleeping device that
  overshoots wakes with the slot already past and nothing else to wake it —
  keep the 85%-of-remaining hop rule (§4).
- **Owners install with `storageDefault` and remove with `storageUnset`.**
  Default keeps a user's schedule tweak; `storageUnset` (not
  `storageDeleteTree`) fires the change subscription so cron actually
  reschedules.
- **`N` reads `wifi.sta.state` off the bus, not net's API.** core has no net
  dependency; a missing key means "never up", so `N` jobs silently don't run on
  a no-network build. That's intended, not a bug.
