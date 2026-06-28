# Power management

`pm` is spangap-core's power-management layer for the ESP32-S3. It configures
ESP-IDF's dynamic frequency scaling (DFS, 240/80 MHz) and automatic light sleep,
exposes a small named-lock API that lets any subsystem keep the CPU fast or awake
while it needs to, runs CPU frequency boosting so tasks idle slow and burst fast,
manages the USB-serial peer presence, and accumulates per-mode power stats across
deep-sleep cycles. It does **not** initiate deep sleep itself — that is
[cron](cron.md)'s job, triggered when the last lock releases.

It is part of spangap-core, so it starts automatically when the straddle is in the
build.

## What it does

At boot `pmInit()` configures DFS at **240 MHz max / 80 MHz min with light sleep
enabled**, so an idle CPU drops to 80 MHz and, when no lock forbids it, into
automatic light sleep between events. From there the system's power behaviour is
the sum of who is holding which lock.

The required ESP-IDF options for any of this to work —
`CONFIG_PM_ENABLE`, `CONFIG_FREERTOS_USE_TICKLESS_IDLE` (automatic light sleep),
`CONFIG_PM_PROFILING` (per-mode/per-lock stats), and
`CONFIG_FREERTOS_THREAD_LOCAL_STORAGE_POINTERS≥2` (the boost TLS slot) — are
supplied by spangap-core's own board-agnostic
[`sdkconfig.defaults.spangap`](../esp-idf/sdkconfig.defaults.spangap). A consuming
board that overrides sdkconfig must keep them set; they are the contract pm
assumes.

### The lock API

Any subsystem keeps the CPU fast or awake by creating a named lock and acquiring
it; releasing the last one lets the system fall back to sleep. Locks are recursive
and carry profiling stats (hold time, acquire count) surfaced in `pm`. Signatures
in [`pm.h`](../esp-idf/include/pm.h):

```cpp
pm_lock_handle_t h;
pmLockCreate(PM_NO_LIGHT_SLEEP, "myservice", &h);  // string must be static
pmLockAcquire(h);            // recursive
// … CPU stays awake / fast while held …
pmLockRelease(h);
```

The four lock types:

| Type | Effect |
|---|---|
| `PM_CPU_FREQ_MAX` | Pin the CPU at 240 MHz. |
| `PM_APB_FREQ_MAX` | Pin the APB bus at its max frequency. |
| `PM_NO_LIGHT_SLEEP` | Forbid automatic light sleep (CPU stays clocked). |
| `PM_NO_DEEP_SLEEP` | Forbid deep sleep — pm's own bookkeeping, not an esp_pm lock. |

The first three delegate to `esp_pm_lock_*`; `PM_NO_DEEP_SLEEP` is tracked in
pm's own list. `deepSleepAllowed()` returns true only when **no lock of any type**
is held — that is the condition that ultimately lets cron sleep the device.

### Lock holders

Locks held by the platform (any consumer subsystem can add its own — a streaming
server, a peripheral driver — and it shows up in `pm` alongside these):

| Name | Type | Held while | Owner |
|---|---|---|---|
| `usb` | `NO_LIGHT_SLEEP` | a USB-serial host is attached (SOF detected; 5 s boot grace) | pm |
| `cron` | `NO_DEEP_SLEEP` | cron is disabled **or** the crontab is empty | [cron](cron.md) |
| `waittime` | `NO_DEEP_SLEEP` | a `waitForTime()` clock-sync barrier is in progress | core (`spangap_init`) |
| `waitflag` | `NO_DEEP_SLEEP` | a `waitForFlag()` readiness barrier is in progress | core (`spangap_init`) |
| `net` | `NO_DEEP_SLEEP` | WiFi is up | [spangap-net](../../spangap-net) |
| `datewait` | `NO_DEEP_SLEEP` | the boot NTP date wait is in progress | [spangap-net](../../spangap-net) |
| `<taskname>` | `CPU_FREQ_MAX` | that task is mid-boost (one lazily-created lock per task) | pm (boost) |

The `net` and `datewait` locks are **owned by spangap-net**, not pm — pm only
provides the mechanism. `esp_pm_dump_locks()` additionally lists IDF-internal
locks (wifi, rtos0/1, drivers) in the same table.

### CPU boost

Tasks run at the DFS floor (80 MHz) by default and burst to 240 MHz only while
actually handling an event. The mechanism is automatic for any ITS task:
`itsPoll()` boosts when a wait wakes from a **notify** — a real event: an ITS
message, an ISR (e.g. LoRa DIO1), input — and stays at the floor when the wait
merely **times out** (a routine housekeeping tick). The boost is held from the
notify-wake until the task's next block, then dropped.

Each task owns a recursive `CPU_FREQ_MAX` lock named after itself, created lazily
on first boost — so `pm` attributes boost time per task and a stuck boost names
its own leaker.

| API | Use |
|---|---|
| `pmBoostAuto(bool)` | The automatic take/drop driven by `itsPoll()` and `delay()`. TLS-tracked, idempotent, balanced in any order. |
| `pmBoost()` / `pmBoostEnd()` | Manual sustained boost for loops that aren't notify-driven and want 240 MHz held across their own blocks (net's `select`, webrtc). Recursive; pair each call. |
| `pmBoostHeld()` | Whether the current task holds its auto boost count. |

`delay()` drops the auto boost while it sleeps and restores it after, so a delay
inside event handling doesn't burn 240 MHz idling; manual `pmBoost()` holds are
untouched by it.

### USB D+ pullup

On a USB-serial-JTAG console, the host emits SOF packets every 1 ms, which wake
the CPU from light sleep even after the `usb` lock is released. `usb down`
disables the D+ pullup so the host sees a disconnect and stops the SOF traffic;
`usb up` resets the USB-serial-JTAG peripheral, re-enables the internal PHY and
pads, and restores hardware pull control. The disabled state persists across deep
sleep (a cold power cycle restores normal USB). Details in
[power-management-internals.md](power-management-internals.md).

### GPIO wake sources

A peripheral that must wake the CPU from light sleep on an interrupt line (LoRa
DIO, accelerometer INT, a button) registers its pin:

```cpp
pmGpioWakeEnable(pin, GPIO_INTR_HIGH_LEVEL);   // or GPIO_INTR_LOW_LEVEL
pmGpioWakeDisable(pin);
```

Wake is **level-triggered, not edge** — during light sleep the GPIO peripheral
clock is gated and only the RTC IO matrix runs, which matches levels, not edges.
That imposes an ISR discipline on the caller (disable the interrupt inside the
ISR, re-enable after the line drops); the full end-to-end path and the trade-off
against just holding a `NO_LIGHT_SLEEP` lock are in
[power-management-internals.md](power-management-internals.md).

### Deep sleep

pm never calls deep sleep directly. `pmLockRelease()` sets the `sys.going_down`
storage var the moment `deepSleepAllowed()` becomes true (every lock released);
cron observes that and sleeps the device until the next scheduled minute. So
`usb down` alone only reaches **light** sleep — deep sleep additionally needs a
crontab entry that can wake the device (otherwise the `cron` lock stays held).
See [cron](cron.md).

## CLI

pm registers three commands (run on-device via `spangap cli "<command>"`):

| Command | Does |
|---|---|
| `pm` | Current CPU/APB frequency, then (under profiling) per-mode time deltas since the last `pm` and totals since boot — deep sleep / light sleep / 80 MHz / 240 MHz as a percentage of wall time, deep sleep with its count. |
| `pm -v` | Adds the full lock table: every esp_pm lock plus pm's own `NO_DEEP_SLEEP` rows, plus Mode and Sleep stats (the light-sleep reject count is the "why no light sleep" signal). |
| `pm wifi [none\|min\|max]` | Read or set the WiFi modem power-save mode (`esp_wifi_set_ps`). |
| `usb up` / `usb down` | Reconnect / disconnect the USB-serial peer (above); bare `usb` reports connection state. |
| `top` | Per-task CPU%, stack, per-task DRAM/PSRAM, per-core busy, heap, uptime. |

Chain `usb down; sleep 30; usb up` on one line — the CLI splits the commands up
front, so they all run even after the console disconnects. (`pm deep|light|slow
inhibit|allow` exists as a debug knob that holds a `cli`-named lock; it is not
part of normal operation.)

The **`bat`** command (battery voltage and percent) is **not** pm — it lives in
core's system commands ([`cli_cmd_sys.cpp`](../esp-idf/src/cli_cmd_sys.cpp)) and
just reports the `battery.*` ephemerals a board's battery monitor publishes.
`net up` / `net down` likewise belong to [spangap-net](../../spangap-net), not pm.

## Storage keys

pm owns **no** `s.pm.*` storage keys. It reacts to `sys.going_down` (which it
writes) and is configured entirely through sdkconfig and the lock API.

## See also

- [power-management-internals.md](power-management-internals.md) — pmInit, the
  lock list and profiling, RTC stat accumulation across deep sleep, the USB and
  GPIO-wake mechanisms, boost internals, and pitfalls.
- [cron](cron.md) — the deep-sleep driver that pm's lock model feeds.
