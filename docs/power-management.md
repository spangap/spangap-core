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

Two locks manage power states:

| Lock | Type | Purpose |
|------|------|---------|
| `streaming` | `CPU_FREQ_MAX` | Held during active audio/camera/RTSP/web streaming. Keeps CPU at 240 MHz, prevents DFS and light sleep. |
| `usb` | `NO_LIGHT_SLEEP` | Held while USB Serial JTAG host is connected. Allows DFS (240→80 MHz) but prevents light sleep so console output works. |

The `streaming` lock is managed by `allowLightSleep(bool)`, called by audio, camera, RTSP, and web tasks when they start/stop streaming.

The `usb` lock is polled from the log task using `usb_serial_jtag_is_connected()`, which detects USB host presence via SOF packets. Power banks (no SOF) are correctly identified as "not connected".

## Power States

| State | CPU | Light Sleep | Current | Condition |
|-------|-----|-------------|---------|-----------|
| Streaming | 240 MHz fixed | No | ~120 mA | `allowLightSleep(false)` held |
| Idle + USB | 240/80 MHz DFS | No | ~25 mA | USB host connected |
| Idle + no USB | 240/80 MHz DFS | Yes | 7–10 mA | USB disconnected or `sleep` CLI command |

## Idle Poll Interval

When idle (no locks held), `ipcReceive()` extends task timeouts to a 200ms grid (`IDLE_POLL_MS`). This gives PM longer idle windows to enter light sleep or hold the CPU at minimum frequency.

## CLI Commands

- `pm` — Show current CPU frequency, USB connection state, and all PM lock status
- `sleep` — Override USB detection and allow light sleep (will lose serial console; reboot to restore)

## USB Serial JTAG and Light Sleep

Light sleep powers down the USB Serial JTAG peripheral, killing the serial console. The `usb` PM lock prevents this while a host is connected. The `sleep` CLI command overrides this for testing power consumption with USB still physically attached.

When deployed without USB (or on a power bank), light sleep engages automatically — no configuration needed.

## Task Watchdog

`CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU1=n` — Core 1 runs RTSP + web streaming tasks which can starve IDLE1 during active sessions. The IDLE0 watchdog on core 0 remains active.

## Key Files

- `main/ipc.cpp` — `pmInit()`, `allowLightSleep()`, `allowSlow()`, `allowDeepSleep()`, `pmPollUsb()`, PM lock management
- `main/ipc.h` — `pmInit()`, `allowLightSleep()`, `allowSlow()`, `allowDeepSleep()` API
- `sdkconfig.defaults` — PM and tickless idle config
