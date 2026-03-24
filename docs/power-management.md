# Power Management

ESP-IDF power management on ESP32S3 with DFS (Dynamic Frequency Scaling) and automatic light sleep.

## Configuration

`sdkconfig.defaults`:
- `CONFIG_PM_ENABLE=y` — enables PM framework
- `CONFIG_FREERTOS_USE_TICKLESS_IDLE=y` — required for automatic light sleep
- `CONFIG_PM_PROFILING=y` — enables per-lock time stats

PM is configured once at boot in `pmInit()`:
```
max_freq_mhz = 240, min_freq_mhz = 80, light_sleep_enable = true
```

## PM Lock API

Per-caller locks via `pmLockCreate/pmLockAcquire/pmLockRelease` (ipc.h):

```cpp
enum pm_lock_type_t {
  PM_CPU_FREQ_MAX,    // → esp_pm_lock CPU_FREQ_MAX
  PM_APB_FREQ_MAX,    // → esp_pm_lock APB_FREQ_MAX
  PM_NO_LIGHT_SLEEP,  // → esp_pm_lock NO_LIGHT_SLEEP
  PM_NO_DEEP_SLEEP    // → our own bookkeeping
};

pmLockCreate(type, "name", &handle);
pmLockAcquire(handle);
pmLockRelease(handle);
```

ESP types delegate to `esp_pm_lock_*()`. `PM_NO_DEEP_SLEEP` is tracked in our own linked list with matching profiling stats (name, active count, total_count, time_held). `deepSleepAllowed()` walks the full list — returns false if any lock of any type has count > 0.

## Lock Holders

| Name | Type | Holder |
|------|------|--------|
| `"camera"` | NO_LIGHT_SLEEP | camera task while running |
| `"audio"` | NO_LIGHT_SLEEP | audio task while running |
| `"rtsp"` | NO_LIGHT_SLEEP + CPU_FREQ_MAX | RTSP during active streaming |
| `"usb"` | NO_LIGHT_SLEEP | USB host connected (SOF detection, 5s boot grace) |
| `"network"` | NO_DEEP_SLEEP | WiFi up (STA connected or AP) |
| `"storage"` | NO_DEEP_SLEEP | after `set`/`unset` until `save` |
| `"boot"` | NO_DEEP_SLEEP | during `waitForTime()` until NTP syncs |
| `"cron"` | NO_DEEP_SLEEP | held by default; released when `s.cron.enable=1` AND cron entries exist |

## Deep Sleep Triggering

Deep sleep is not initiated directly by any module during normal operation. Instead, `pmLockRelease()` broadcasts `MSG_SYS_SLEEP` whenever `deepSleepAllowed()` becomes true (all locks released). The cron task listens for this message and enters deep sleep until the next minute boundary (+1s).

The cron lock prevents accidental deep sleep: without `s.cron.enable=1` and at least one cron entry, the lock stays held and `MSG_SYS_SLEEP` is never broadcast. This means `usb down` alone only achieves light sleep — deep sleep requires an explicit cron configuration that can wake the device.

**Wakeup handler** (separate fast path): `cronWakeupHandler()` runs early in `app_main()` before full boot. If timer wakeup with no cron work this minute, goes right back to sleep without initializing any tasks. If work to do, returns true and full boot proceeds.

## RTC PM Stats Accumulation

ESP PM stats (`esp_pm_impl_dump_stats`) reset on each deep sleep wake. To maintain accurate "since boot" and "since last pm" totals across wake cycles:

- `rtcAccumModeUs[4]` (RTC RAM): accumulated awake-mode times from all previous wake sessions
- `rtcPmModeUs[4]` (RTC RAM): grand total snapshot at last `pm` command
- `rtcPmEverCalled` (RTC RAM): tracks whether `pm` has been run (for delta display)

`pmRecordDeepSleep()` (called just before `esp_deep_sleep_start()`) captures current ESP PM stats and adds them to `rtcAccumModeUs`. The `pm` command computes grand totals as `rtcAccumModeUs[i] + currentModeUs[i]`.

All four modes (deep sleep, light sleep, 80 MHz, 240 MHz) are shown as percentage of wall time, summing to 100%. Deep sleep line includes the count in parentheses. "Since last pm" delta section is skipped on first invocation.

## USB D+ Pullup

USB SOF packets arrive every 1ms from the host, waking the CPU from light sleep even after the USB PM lock is released. The `usb down` CLI command disables the D+ pullup via `usb_serial_jtag_ll_phy_enable_pull_override()`, signaling device disconnect to the host and stopping SOF packets.

`usb up` does a full USB Serial JTAG peripheral reset (`usb_serial_jtag_ll_reset_register()`), re-enables the internal PHY and pads, and restores hardware pull control. The peripheral reset is necessary because light sleep gates the USB clock, leaving the internal state machine in a confused state that won't respond to host enumeration with just a pullup restore.

**RTC persistence**: `usb down` state is stored in `rtcUsbDisabled` (RTC RAM), surviving deep sleep. On deep sleep wake, `pmInit()` checks this flag — if set, disables D+ pullup immediately and skips USB lock acquisition, preventing USB from blocking the next deep sleep cycle. Power cycle (cold boot) resets RTC RAM, restoring USB to normal.

## Power States

| State | CPU | Light Sleep | Current | Condition |
|-------|-----|-------------|---------|-----------|
| Streaming | 240 MHz fixed | No | ~120 mA | NO_LIGHT_SLEEP lock held |
| Idle + USB | 240/80 MHz DFS | No | ~25 mA | USB host connected |
| Idle + no USB | 240/80 MHz DFS | Yes | ~5 mA | USB disconnected or `usb down` |

## WiFi and Sleep

`esp_sleep_enable_wifi_wakeup()` is called at WiFi init and on network up. This registers WiFi as a light sleep wakeup source — CPU wakes for incoming packets between DTIM beacon intervals.

**Graceful shutdown** (`net down`): sets `wantDown` flag and waits for 30s of idle (no `netActivity()` calls from rtsp, web, log, or CLI network clients). Consumer tasks call `netActivity()` in their active loops. `net up` cancels a pending shutdown. `net down!` forces immediate teardown.

On actual shutdown: broadcasts `MSG_NETWORK_DOWN` to all tasks, then `ntpStop()` + `mdns_free()` before `esp_wifi_stop()` (so mDNS goodbye multicast can be sent while RF is up), then `esp_wifi_deinit()` to fully tear down WiFi driver tasks (saves ~5mA vs `esp_wifi_stop()` alone). On `net up`: `esp_wifi_init()` + `esp_wifi_start()` to reinitialize. WiFi state survives deep sleep via `RTC_DATA_ATTR`.

`WIFI_PS_MAX_MODEM` is set after network up for aggressive modem sleep.

## CLI Commands

- `pm` — Show CPU/APB frequency, per-mode time deltas since last `pm`, per-mode totals since boot, ESP lock table, deep sleep lock table
- `usb down` — Disable D+ pullup, release USB PM lock, allow light sleep
- `usb up` — Reset USB Serial JTAG peripheral, re-enable PHY/pads, acquire PM lock
- `usb down; sleep 30; usb up` — Semicolons split commands upfront so all execute even after USB disconnects
- `net down` — Graceful shutdown (waits for 30s idle)
- `net down!` — Immediate WiFi teardown
- `net up` — Bring WiFi up (cancels pending graceful shutdown)

## Task Watchdog

`CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU1=n` — Core 1 runs RTSP + web tasks which can starve IDLE1 during active sessions. The IDLE0 watchdog on core 0 remains active.

## Irreducible Floor

The lwIP timer thread (~1.3mA), started by `esp_netif_init()`, cannot be stopped without `esp_netif_deinit()` (not cleanly supported in ESP-IDF 5.x). This is the minimum power floor with networking initialized.

## Key Files

- `main/pm.cpp` — `pmInit()`, `pmLockCreate/Acquire/Release`, `pmPollUsb()`, `cliUsbDown()`/`cliUsbUp()`, `pm` command, deep sleep lock table
- `main/ipc.h` — `pm_lock_type_t` enum, `pmLockCreate/Acquire/Release` API, `deepSleepAllowed()`
- `main/net.cpp` — `esp_wifi_deinit()`/`esp_wifi_init()`, `esp_sleep_enable_wifi_wakeup()`, `"net"` deep sleep lock
- `sdkconfig.defaults` — PM, tickless idle, profiling config
