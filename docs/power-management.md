# Power Management

ESP-IDF power management on ESP32S3 with DFS (Dynamic Frequency Scaling) and automatic light sleep.

## Configuration

Required `sdkconfig` settings (set in the consuming app's `sdkconfig.defaults`):
- `CONFIG_PM_ENABLE=y` — enables PM framework
- `CONFIG_FREERTOS_USE_TICKLESS_IDLE=y` — required for automatic light sleep
- `CONFIG_PM_PROFILING=y` — enables per-lock time stats

PM is configured once at boot in `pmInit()`:
```
max_freq_mhz = 240, min_freq_mhz = 80, light_sleep_enable = true
```

## PM Lock API

Per-caller locks via `pmLockCreate/pmLockAcquire/pmLockRelease` (pm.h):

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

Diptych itself takes these:

| Name | Type | Holder |
|------|------|--------|
| `"usb"` | NO_LIGHT_SLEEP | USB host connected (SOF detection, 5s boot grace) |
| `"network"` | NO_DEEP_SLEEP | WiFi up (STA connected or AP) |
| `"storage"` | NO_DEEP_SLEEP | after `set`/`unset` until `save` |
| `"boot"` | NO_DEEP_SLEEP | during `waitForTime()` until NTP syncs |
| `"cron"` | NO_DEEP_SLEEP | held by default; released when `s.cron.enable=1` AND cron entries exist |

Consumer apps add their own — anything that needs to keep the CPU awake while running (a streaming server, a peripheral driver) acquires its own named lock and shows up in `pm` output alongside diptych's.

## Deep Sleep Triggering

Deep sleep is not initiated directly by any module during normal operation. Instead, `pmLockRelease()` sets the `sys.going_down` ephemeral config var whenever `deepSleepAllowed()` becomes true (all locks released). The cron task subscribes to this var and enters deep sleep until the next minute boundary (+1s).

The cron lock prevents accidental deep sleep: without `s.cron.enable=1` and at least one cron entry, the lock stays held and `sys.going_down` is never set. This means `usb down` alone only achieves light sleep — deep sleep requires an explicit cron configuration that can wake the device.

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

On actual shutdown: sets `net.up` ephemeral var to 0 (subscribers react accordingly), then `ntpStop()` + `mdns_free()` before `esp_wifi_stop()` (so mDNS goodbye multicast can be sent while RF is up), then `esp_wifi_deinit()` to fully tear down WiFi driver tasks (saves ~5mA vs `esp_wifi_stop()` alone). On `net up`: `esp_wifi_init()` + `esp_wifi_start()` to reinitialize. WiFi state survives deep sleep via `RTC_DATA_ATTR`.

`WIFI_PS_MAX_MODEM` is set after network up for aggressive modem sleep.

## GPIO Wake Sources

Peripherals that need to wake the CPU from light sleep on an interrupt line (LoRa modem DIO, accelerometer INT, button, etc.) register the GPIO via:

```cpp
pmGpioWakeEnable(int pin, int wakeLevel);   // GPIO_INTR_HIGH_LEVEL or LOW_LEVEL
pmGpioWakeDisable(int pin);
```

The first call lazily flips `esp_sleep_enable_gpio_wakeup()`. Each call wires both `gpio_wakeup_enable(pin, level)` and `gpio_set_intr_type(pin, level)`. Multiple pins can be registered; `pmGpioWakeDisable` only undoes the per-pin part — the global wakeup-source enable stays on for any other registered pins.

**Why level, not edge.** During light sleep the GPIO peripheral clock is gated, so edge detection doesn't run. Only the RTC IO matrix is awake, and it can match levels but not edges. ESP-IDF couples the wake trigger with the GPIO peripheral interrupt type — `gpio_wakeup_enable` requires `GPIO_INTR_HIGH_LEVEL` or `GPIO_INTR_LOW_LEVEL`. Edges that occur during sleep are lost; only the asserted level survives.

**End-to-end wake path:**

1. Sleep with the line in the inactive level (e.g. DIO1 low).
2. Peripheral asserts the line. RTC IO matrix matches the configured level → CPU wakes.
3. GPIO peripheral re-clocks, sees the level still asserted, fires the level-triggered ISR immediately. No separate "post-wake" callback is needed; the persistent level *is* the bridge.
4. ISR services the peripheral (which drops the line back to inactive), then re-enables the GPIO interrupt for the next event.

**Level-mode ISR discipline.** A naive level-triggered ISR re-fires continuously while the line stays asserted. Callers must either:

- disable the interrupt inside the ISR (`gpio_intr_disable`) and re-enable after the peripheral is serviced and the line has dropped; or
- service the peripheral fast enough inside the ISR to drop the line before returning (rarely possible).

The reticulous LoRa transport uses pattern 1: an IDF HAL trampoline calls `gpio_intr_disable(pin)` before invoking the registered callback, and the consumer task calls `gpio_intr_enable(pin)` after draining the peripheral's IRQ register (which drops DIO1 low). See `reticulous/main/esp_idf_hal.cpp` for the trampoline and `reticulous/main/lora.cpp` for the consumer.

**Trade-off vs. a PM lock.** Holding a `PM_NO_LIGHT_SLEEP` lock while a peripheral is armed is simpler (no ISR-discipline contract) but burns ~3 mA continuous even when nothing is happening. Use the lock when the peripheral itself draws far more than that (LoRa RX is ~5 mA, so it's a wash); use GPIO wake when the peripheral has a near-zero quiescent draw and we want the CPU to actually sleep.

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

- [`pm.cpp`](../diptych-core/src/pm.cpp) — `pmInit()`, `pmLockCreate/Acquire/Release`, `pmGpioWakeEnable/Disable`, `pmPollUsb()`, `cliUsbDown()`/`cliUsbUp()`, `pm` command, deep sleep lock table
- [`pm.h`](../diptych-core/include/pm.h) — `pm_lock_type_t` enum, `pmLockCreate/Acquire/Release` API, `pmGpioWakeEnable/Disable`, `deepSleepAllowed()`
- [`net.cpp`](../diptych-core/src/net.cpp) — `esp_wifi_deinit()`/`esp_wifi_init()`, `esp_sleep_enable_wifi_wakeup()`, `"net"` deep sleep lock
- consumer's `sdkconfig.defaults` — PM, tickless idle, profiling config
