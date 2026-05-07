# Cron

`cron.cpp/h` — minute-resolution scheduler with deep sleep support.

- **Entries**: `/state/crontab` file. Format: `min hour dom month dow flags command`. One entry per line, `#` comments.
- **Flags**: 6th field between time fields and command. `-` = none, `A` = awake only (skip on deep sleep wake), `N` = STA connected to upstream (AP-only does not count; implies A). Unflagged entries run on every wake including deep sleep.
- **Syntax**: `*`, plain numbers, `,` (lists), `-` (ranges), `/` (steps including `*/5`, `1-30/2`). No named months/days or `@` shortcuts.
- **Execution**: matching commands written to a stream buffer, drained by CLI task (serialized with user input).
- **Deep sleep lock**: `"cron"` NO_DEEP_SLEEP lock held by default. Released only when `s.cron.enable=1` AND crontab has entries. Re-acquired if cron is disabled or crontab is empty. Prevents accidental deep sleep from `usb down` when cron isn't configured to wake the device.
- **Deep sleep trigger**: `pmLockRelease()` sets `sys.going_down=1` when `deepSleepAllowed()` becomes true (all locks released). Cron task subscribes to `sys.going_down` and enters deep sleep until next minute boundary (+1s). Normal operation never calls deep sleep directly — it's always triggered by the last lock being released.
- **Wakeup handler**: `cronWakeupHandler()` runs early in `app_main()` after `fs_init()` — if timer wakeup with no cron work this minute, goes right back to sleep (never fully boots). If work to do, returns true and full boot proceeds.
- **RTC state**: `cronLastMinute` (epoch-minutes) in RTC RAM. Central `rtcRamValid()`/`rtcRamSetValid()` (compat.h/pm.cpp) replaces per-module magic words — set in main after all inits, checked on deep sleep wake. cam_driver keeps its own `rtc_cam_magic` (separate component).
- **Boot**: `cronWakeupHandler()` after `fs_init()`, `cronInit()` after all module inits, `cronPoll(true)` after ready.
- **Default-on**: `s.cron.enable` defaults to 1 so module-installed entries actually run on a fresh device.
- **Column layout**: `cronDefault` writes entries with all-4-wide columns to match the header in `data/factory_state/crontab` (`# min  hour dom  mon  dow  flag command`). Each schedule field is split and printf'd as `%-4s`.
- **Module-installed entries** (via `cronDefault(schedule, command)` in each module's `init()`, gated by the module's version key — appends only if no active-or-commented line has the same command):
  ```
  */15 *    *    *    *    N    duckdns update    # duckdns
  */15 *    *    *    *    N    upnp update       # upnp (renew port mappings + refresh ext IP)
  0    3    *    *    *    N    cert acme 30      # acme
  0    0    *    *    *    A    logrotate 7       # log
  ```
  Modules respect user edits across reboots (delete the line, or comment it out, and `cronDefault` won't re-add until the module's version is bumped).
- **Example**: intermittent WiFi for battery:
  ```
  */10 *    *    *    *    -    net up
  2/10 *    *    *    *    -    net down
  ```
  With `s.cron.enable=1` → WiFi active 2 min every 10 min, deep sleep the rest.
