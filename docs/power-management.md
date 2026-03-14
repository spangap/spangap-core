# Power Management

ESP-IDF power management on ESP32S3 with DFS (Dynamic Frequency Scaling) and automatic light sleep.

## Configuration

`sdkconfig.defaults`:
- `CONFIG_PM_ENABLE=y` — enables PM framework
- `CONFIG_FREERTOS_USE_TICKLESS_IDLE=y` — required for automatic light sleep

PM is configured once at boot in `pmInit()`:
```
max_freq_mhz = 240, min_freq_mhz = 80, light_sleep_enable = true
```

## PM Locks

Three locks manage power states:

| Lock | Type | Purpose |
|------|------|---------|
| `cpu_boost` | `CPU_FREQ_MAX` | Held during active audio/camera/RTSP/web streaming. Keeps CPU at 240 MHz. |
| `no_sleep` | `NO_LIGHT_SLEEP` | Held to prevent light sleep (ref-counted via `allowLightSleep()`). |
| `usb` | `NO_LIGHT_SLEEP` | Held while USB Serial JTAG host is connected. Prevents light sleep so console works. |

The `no_sleep` lock is managed by `allowLightSleep(bool)`, called by audio, camera, RTSP, and web tasks when they start/stop streaming.

The `usb` lock is polled every 200ms from the log task using `usb_serial_jtag_is_connected()`, which detects USB host presence via SOF packets. Power banks (no SOF) are correctly identified as "not connected" — lock releases automatically. A 5-second grace period after boot keeps the lock held regardless of SOF detection, giving the host time to enumerate before light sleep gates the USB clock.

## USB D+ Pullup

USB SOF packets arrive every 1ms from the host, waking the CPU from light sleep even after the USB PM lock is released. The `usb down` CLI command disables the D+ pullup via `usb_serial_jtag_ll_phy_enable_pull_override()`, signaling device disconnect to the host and stopping SOF packets.

`usb up` does a full USB Serial JTAG peripheral reset (`usb_serial_jtag_ll_reset_register()`), re-enables the internal PHY and pads, and restores hardware pull control. The peripheral reset is necessary because light sleep gates the USB clock, leaving the internal state machine in a confused state that won't respond to host enumeration with just a pullup restore.

## Power States

| State | CPU | Light Sleep | Current | Condition |
|-------|-----|-------------|---------|-----------|
| Streaming | 240 MHz fixed | No | ~120 mA | `allowLightSleep(false)` held |
| Idle + USB | 240/80 MHz DFS | No | ~25 mA | USB host connected |
| Idle + no USB | 240/80 MHz DFS | Yes | ~5 mA | USB disconnected or `usb down` |

## WiFi and Sleep

`esp_sleep_enable_wifi_wakeup()` is called at WiFi init and on wifi up. This registers WiFi as a light sleep wakeup source — CPU wakes for incoming packets between DTIM beacon intervals.

On `wifi down`: `ntpStop()` + `mdns_free()` before `esp_wifi_stop()` (so mDNS goodbye multicast can be sent while RF is up), then `esp_wifi_deinit()` to fully tear down WiFi driver tasks (saves ~5mA vs `esp_wifi_stop()` alone). On `wifi up`: `esp_wifi_init()` + `esp_wifi_start()` to reinitialize.

`WIFI_PS_MAX_MODEM` is set after network up for aggressive modem sleep.

## CLI Commands

- `pm` — Show current CPU frequency, USB connection state, and all PM lock status
- `usb down` — Disable D+ pullup, release USB PM lock, allow light sleep
- `usb up` — Reset USB Serial JTAG peripheral, re-enable PHY/pads, acquire PM lock
- `usb down; sleep 30; usb up` — Semicolons split commands upfront so all execute even after USB disconnects

## Task Watchdog

`CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU1=n` — Core 1 runs RTSP + web streaming tasks which can starve IDLE1 during active sessions. The IDLE0 watchdog on core 0 remains active.

## Irreducible Floor

The lwIP timer thread (~1.3mA), started by `esp_netif_init()`, cannot be stopped without `esp_netif_deinit()` (not cleanly supported in ESP-IDF 5.x). This is the minimum power floor with networking initialized.

## Key Files

- `main/ipc.cpp` — `pmInit()`, `allowLightSleep()`, `allowSlow()`, `allowDeepSleep()`, `pmPollUsb()`, `cliUsbDown()`/`cliUsbUp()`, PM lock management
- `main/ipc.h` — `pmInit()`, `allowLightSleep()`, `allowSlow()`, `allowDeepSleep()` API
- `main/wifi_task.cpp` — `esp_wifi_deinit()`/`esp_wifi_init()`, `esp_sleep_enable_wifi_wakeup()`
- `sdkconfig.defaults` — PM and tickless idle config
