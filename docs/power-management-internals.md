# Power management — internals

Maintainer reference for `pm.cpp` / `pm.h`. The [operator guide](power-management.md)
is the user-facing surface. Source:
[`esp-idf/src/pm.cpp`](../esp-idf/src/pm.cpp),
[`esp-idf/include/pm.h`](../esp-idf/include/pm.h),
[`esp-idf/include/compat.h`](../esp-idf/include/compat.h).

## 1. What pm adds

- **`pmInit()`** — called from `spangapInit()` (after `logInit`/`cliInit`, before
  `authInit` and `cronWakeupHandler`). Runs `esp_pm_configure(240 max / 80 min /
  light_sleep_enable=true)`, creates the `usb` `NO_LIGHT_SLEEP` lock, and handles
  the USB-across-deep-sleep restore (§4).
- **Lock list** — `pmLockCreate` / `pmLockAcquire` / `pmLockRelease`, the four
  `pm_lock_type_t`, and `deepSleepAllowed()`. ESP types wrap `esp_pm_lock_*`;
  `PM_NO_DEEP_SLEEP` is pm-private bookkeeping. The list head is guarded by
  `pmListMux`; `esp_pm_lock_create` runs outside that spinlock.
- **`sys.going_down` trigger** — `pmLockRelease()` writes `sys.going_down=1` when
  `deepSleepAllowed()` flips true. This is the only place that signals sleep; pm
  itself never calls `esp_deep_sleep_start()`. **Currently commented out** — deep
  sleep is not supported at the moment, along with cron's `going_down`
  subscription / `cronDeepSleep()` and the `pm` command's deep-sleep line and
  NO_DEEP_SLEEP lock rows.
- **CPU boost** — `pmBoostAuto` / `pmBoost` / `pmBoostEnd` / `pmBoostHeld`, per-task
  `CPU_FREQ_MAX` locks, the `boost_task_t` registry, and the `TLS_PM_BOOST` slot
  (§7).
- **RTC stat accumulation** — `rtcAccumModeUs` / `rtcPmModeUs` / `rtcPmEverCalled`
  / `rtcDeepSleepCount` / `rtcDeepSleepUs` (all `RTC_DATA_ATTR`) and
  `pmRecordDeepSleep()` (§3).
- **Central RTC validity** — `rtcRamValid()` / `rtcRamSetValid()` and
  `RTC_APP_MAGIC` (`0x5ECC0001`) live here (declared in `compat.h`); every module's
  RTC state keys off this one magic word rather than its own.
- **USB management** — `pmPollUsb()` (called ~1 Hz from the log task), `cliUsbUp`
  / `cliUsbDown`, and `rtcUsbDisabled` (§4).
- **GPIO wake** — `pmGpioWakeEnable` / `pmGpioWakeDisable` and `s_gpioWakeArmed`
  (§5).
- **CLI** — `pm`, `top`, `usb` registered by `pmRegisterCmds()` (called from
  cli init); plus `heapDump()` for malloc-failure diagnostics.

## 2. The NO_DEEP_SLEEP list and profiling

`pm_lock` nodes form a singly linked list (`pmLockList`). Each node carries
`type`, `esp_handle` (null for `PM_NO_DEEP_SLEEP`), a recursive `count`,
`times_taken`, and `time_held`/`last_taken` for profiling. `pmLockAcquire`
stamps `last_taken` on the 0→1 transition and acquires the esp lock if present;
`pmLockRelease` accumulates `time_held` on the 1→0 transition, releases the esp
lock, and then evaluates `deepSleepAllowed()` → `sys.going_down`.

`deepSleepAllowed()` walks the whole list and returns false if any node's
`count > 0`. `pmDumpLocks()` (the `pm -v` body) captures `esp_pm_dump_locks()`
into a heap buffer via `funopen`, then splices pm's own `NO_DEEP_SLEEP` rows in
**before** the trailing "Mode stats:" / "Sleep stats:" sections (esp_pm can't see
the handle-less locks). Under `CONFIG_PM_PROFILING` those rows include live hold
time and a percent-of-uptime column.

## 3. RTC PM stats across deep sleep

`esp_pm_impl_dump_stats` resets on every deep-sleep wake, so per-mode totals must
be carried in RTC RAM. The awake side is four buckets parsed by `pmParseModeStats`
from the IDF dump's `SLEEP` / `APB_MIN` / `APB_MAX` / `CPU_MAX` rows (light sleep,
80 MHz, APB-max, 240 MHz) into `modeUs[]`. `pmRecordDeepSleep(durationUs)` — called
by cron just before sleeping — bumps `rtcDeepSleepCount` / `rtcDeepSleepUs` and,
under profiling, folds the current `modeUs[]` into `rtcAccumModeUs[]` so the
pre-sleep awake time isn't lost when the IDF counters reset.

`pm` then computes grand totals as `rtcAccumModeUs[i] + currentModeUs[i]`, shows a
"since last pm" delta against the `rtcPmModeUs[]` snapshot (skipped on the first
call, gated by `rtcPmEverCalled`), and re-snapshots. All four awake modes plus
deep sleep are printed as a percentage of wall time and sum to 100%; the deep
sleep line carries the cycle count.

## 4. USB SOF / D+ pullup

Only compiled when `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG` (a UART console has no
peer presence and leaves the `usb` lock released). `usb_serial_jtag_is_connected()`
is SOF-based and noisy below ~1 Hz, so `pmPollUsb()` rate-limits its evaluation to
once a second and debounces:

- **Up** acts immediately (keep the console alive ASAP) — acquire `usb` if not
  held, on connection or within the 5 s boot grace.
- **Down** needs **3 consecutive 1 Hz down samples**, and before conceding it
  calls `cliUsbUp()` once to force a clean re-enumeration (a slow monitor respawn
  or a controller wedged after a light-sleep nap looks like a disconnect). Only if
  that recovery brings no host back does it release `usb`.

`cliUsbDown()` disables the D+ pullup via
`usb_serial_jtag_ll_phy_enable_pull_override()` (all pulls off → host sees
disconnect, SOF stops), sets `rtcUsbDisabled`, and releases the lock. `cliUsbUp()`
re-acquires the lock first, then **resets the peripheral**
(`usb_serial_jtag_ll_reset_register()`) and re-enables PHY + pads + hardware pull
— the reset is required because light sleep gates the USB clock and leaves the
state machine unresponsive to bare pullup restore.

**RTC persistence:** `rtcUsbDisabled` (`RTC_DATA_ATTR`) survives deep sleep. On
wake, `pmInit()` checks it (with `rtcRamValid()`): if set, it disables the pullup
immediately and **skips acquiring `usb`**, so a device put to `usb down` before
sleeping doesn't have USB re-block the next sleep cycle. A cold power cycle clears
RTC RAM and USB returns to normal.

## 5. GPIO wake path

`pmGpioWakeEnable(pin, level)` validates `level` is `GPIO_INTR_HIGH_LEVEL` or
`GPIO_INTR_LOW_LEVEL`, sets the pin's interrupt type and `gpio_wakeup_enable`, and
on the first call flips the global `esp_sleep_enable_gpio_wakeup()`
(`s_gpioWakeArmed`). `pmGpioWakeDisable(pin)` undoes only the per-pin
`gpio_wakeup_disable` — the global enable stays on for any other registered pins.

**Why level, not edge:** in light sleep the GPIO peripheral clock is gated, so
only the RTC IO matrix runs and it matches levels. Edges during sleep are lost;
the persistent asserted level *is* the wake. End to end: sleep with the line in
its inactive level → peripheral asserts it → RTC matches the level, CPU wakes →
the GPIO peripheral re-clocks, sees the level still asserted, fires the
level-triggered ISR immediately (no separate post-wake callback needed).

**Level-mode ISR discipline:** a level ISR re-fires continuously while the line
stays asserted, so the caller must disable the interrupt inside the ISR
(`gpio_intr_disable`) and re-enable it only after servicing the peripheral drops
the line — or service fast enough to drop it before returning (rarely possible).
The trade-off vs. holding a `PM_NO_LIGHT_SLEEP` lock: the lock is simpler but
burns ~3 mA continuously while armed; GPIO wake lets the CPU actually sleep and
suits near-zero-quiescent peripherals.

## 6. Boost internals

`pmBoostAuto(on)` keys off the `TLS_PM_BOOST` slot (slot 1; slot 0 is reserved for
IDF, and `CONFIG_FREERTOS_THREAD_LOCAL_STORAGE_POINTERS` must exceed
`TLS_PM_BOOST` — a `#error` enforces it). The TLS slot holds the task's boost lock
while it holds its one auto count, else null, so take/drop are idempotent and
balanced regardless of call order.

`boostLockEnsure()` finds-or-creates the current task's `CPU_FREQ_MAX` lock from
the `s_boostTasks[BOOST_MAX_TASKS]` (24) registry: a free slot is claimed under
`s_boostMux`, then the entry is private to its single-threaded task thereafter
(lookups/acquires need no further locking). Each entry keeps a **private copy** of
the task name because `pmLockCreate` stores the name pointer by reference and locks
are never freed (the TCB name can outlive the task). A full registry → `nullptr`
→ that task's boost silently no-ops.

`boostReleaseLean()` is the auto-boost hot-path release (runs on every `itsPoll`
block): same count/stats bookkeeping as `pmLockRelease` but **skips** the
`deepSleepAllowed()` walk and `sys.going_down` write — boost locks are
`CPU_FREQ_MAX` and never gate deep sleep. Manual `pmBoost()`/`pmBoostEnd()` share
the per-task lock but are not TLS-tracked, so they survive across blocks
independently of the auto count.

## 7. WiFi and net are spangap-net's

pm's `pm wifi` command sets `esp_wifi_set_ps()` directly, but everything else
about WiFi and sleep — the `net` deep-sleep lock, `esp_sleep_enable_wifi_wakeup()`
(DTIM-interval wake for incoming packets), `WIFI_PS_MAX_MODEM`, and the graceful
`net down` shutdown sequence (`esp_wifi_deinit()` to fully tear down driver tasks,
saving ~5 mA over `esp_wifi_stop()` alone) — lives in
[spangap-net](../../spangap-net) (`net.cpp`, `ntp.cpp`). Don't re-document or
duplicate it here; pm only provides the lock mechanism net plugs into.

## 8. Pitfalls

- **Deep sleep is never initiated in pm.** It is always the downstream effect of
  the last lock releasing: `pmLockRelease` → `sys.going_down` → cron sleeps. Don't
  add a direct sleep call here.
- **`usb down` alone reaches only light sleep.** With no crontab wake entry the
  `cron` lock stays held, `deepSleepAllowed()` never returns true, and the device
  light-sleeps but never deep-sleeps. Deep sleep requires a [cron](cron.md) wake
  config.
- **Lock names must be static.** `pmLockCreate` stores the `name` pointer by
  reference; a stack/temporary string dangles. The boost registry copies task
  names for exactly this reason.
- **GPIO wake is level, with an ISR contract.** A caller that registers a wake pin
  and writes a naive level ISR will re-fire forever; the interrupt must be
  disabled in the ISR and re-enabled after the line drops.
- **Task watchdog:** `CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU1=n` — core 1 runs
  web/RTSP tasks that can starve IDLE1 during active sessions; the IDLE0 watchdog
  on core 0 stays active.
- **Irreducible floor:** the lwIP timer thread (~1.3 mA), started by
  `esp_netif_init()`, can't be stopped without `esp_netif_deinit()` (not cleanly
  supported in ESP-IDF 5.x) — the minimum power floor once networking is
  initialised.
