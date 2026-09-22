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
`CONFIG_PM_PROFILING` (per-mode/per-lock stats),
`CONFIG_FREERTOS_THREAD_LOCAL_STORAGE_POINTERS≥2` (the boost TLS slot), and
`CONFIG_PM_SLP_IRAM_OPT` + `CONFIG_PM_RTOS_IDLE_OPT` (the sleep path itself in
IRAM, so a wake spends ~330 µs less awake running it) — are
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
| `usbcdc` | `NO_LIGHT_SLEEP` | the console runs on a TinyUSB CDC device (`usb cdc`) — a nap gates the USB clock and drops the link. Held host-or-no-host, which is why `usb down` on a CDC console first returns the console to USB-Serial-JTAG (releasing this) before killing the link | [usb-console](usb-console.md) |
| `cron` | `NO_DEEP_SLEEP` | cron is disabled **or** no `s.cron.tab.*` entry exists | [cron](cron.md) |
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

`delay()` (in [`compat.h`](../esp-idf/include/compat.h)) is the project's standard
wait: it drops the auto boost while it sleeps and restores it after, so a wait
inside event handling doesn't burn 240 MHz idling (manual `pmBoost()` holds are
untouched). **Prefer it over a bare `vTaskDelay`**, which holds the boost — and so
pins 240 MHz *and* blocks light sleep — for the entire wait. Waits shorter than the
tickless-idle floor (`CONFIG_FREERTOS_IDLE_TIME_BEFORE_SLEEP`, 3 ticks) skip the
drop, since they can't reach light sleep regardless. This is why `sleep` idles the
device: it calls `delay()`, so `usb down; sleep 30; usb up` genuinely light-sleeps
for the 30 s rather than spinning at 240 MHz.

### Idle discipline — park, don't poll

Automatic light sleep needs **both cores idle at once**, and the longest nap is
bounded by the *soonest* wake across every task. So a task that polls on a short
timer — even a cheap one — caps the whole chip's sleep window at its poll period
and pays a wake each time. The rule for an event-driven task: **block on the
actual wake source with a long or infinite timeout, not a short poll.**

- The **serial console** (`cli.cpp`) waits according to whether a host is on the
  wire, which `pmUsbAttached()` answers. **No host** — park on the task
  notification (`portMAX_DELAY`), because no byte can arrive at a controller
  nobody is driving: an unplugged console costs **zero** wakes. The two events
  that must still reach the task raise that notification themselves — pm calls
  `serialPortWake()` on the edge where it takes the `usb` lock, and a `usb cdc`
  switch calls it 400 ms before it takes the pads away. **Host attached** — read
  the driver's RX ring with a 2 s bound, so a keystroke wakes the task via the
  driver ISR and a network-issued `usb cdc` is still noticed; the bound is free
  because the `usb` lock forbids light sleep for as long as the host is there.
  50 ms while a framed-RPC frame is part-assembled, so an abandoned one times
  out promptly. The task also drops any open console CLI session when the USB
  link goes down (see [internals §4](power-management-internals.md)), so a
  session left open by the `usb down` command itself can't keep polling a dead
  console.
- The **log task** ticks at **1 Hz** while WiFi is up and **0.2 Hz** (5 s) when
  it is down — the tick exists only to service `pmPollUsb()`, and a WiFi-down
  node is the battery-first case. Log fan-out is `xTaskNotifyGive`-driven (every
  `logVprintf` notifies), so delivery stays instant without a fast poll.
- The **LoRa task** reads the radio (a SPI `getIrqFlags`) only when a DIO1 IRQ
  actually fired, not on every task wake — otherwise SPI traffic tracks *wakes*
  rather than *packets*. See [iface-lora internals](../../iface-lora/INTERNALS.md).

A corollary applies to storage: **don't publish telemetry nobody reads.** The
periodic stat publishers (`lora`/`rnsd` `publishStats`) gate on
`uiTelemetryWanted()` (`storage.h`) — true on an LCD build (the on-screen panes
read the keys) or when WiFi is up (a browser can pull them over the web
DataChannel), false otherwise. A headless, WiFi-down node therefore does no stat
churn; a UI re-populates the keys the moment it appears.

### Core placement — overlap for light sleep

Because light sleep needs both cores idle simultaneously, *where* a task runs
matters as much as how often it wakes. Two tasks whose busy windows coincide — a
producer and the consumer it feeds — should sit on **opposite** cores, so their
activity overlaps in time and the cores fall idle together, widening the
both-idle gaps sleep needs. Piling both on one core serialises them and misaligns
the idle windows.

`spawnTask`'s `core` argument takes named constants from
[`compat.h`](../esp-idf/include/compat.h) rather than bare `0`/`1`:

| Constant | Core | Use |
|---|---|---|
| `CORE_PRIMARY` | 0 | The app core — `rnsd` and the clients that queue into it. |
| `CORE_SECONDARY` | 1 | Housekeeping, and on an LCD build the LVGL render task. |
| `CORE_SECONDARY_NO_LCD` | 1 without LCD, else 0 | A hot task to overlap opposite the primary — but only when core 1 is free. On an LCD build (`CONFIG_SPANGAP_LCD`) core 1 is busy rendering, so it falls back to the primary. |

Example: on a no-LCD build the LoRa driver runs on `CORE_SECONDARY_NO_LCD`
(core 1), opposite the `rnsd` it feeds on core 0 — their RX/processing bursts
overlap instead of serialising, so both cores idle together after each burst.

### USB D+ pullup

On a USB-serial-JTAG console, the host emits SOF packets every 1 ms, which wake
the CPU from light sleep even after the `usb` lock is released. `usb down`
disables the D+ pullup so the host sees a disconnect and stops the SOF traffic;
`usb up` resets the USB-serial-JTAG peripheral, re-enables the internal PHY and
pads, and restores hardware pull control. The disabled state persists across deep
sleep (a cold power cycle restores normal USB). Details in
[power-management-internals.md](power-management-internals.md).

All of this is a USB-serial-JTAG notion. While the console runs on a TinyUSB CDC
device the USB-serial-JTAG controller does not own the USB PHY, so `pmPollUsb()`
early-returns rather than fight the OTG core for the pads, and the CDC link
holds its own `usbcdc` `NO_LIGHT_SLEEP` lock instead — light sleep gates the USB
clock, and TinyUSB has no arrangement to survive that. `usb down` on a CDC
console tears the composite device down first (console back on USB-serial-JTAG,
`usbcdc` released with it), then proceeds as above. `pmUsbSerialJtagReattach()`
is the hand-back the transport switch calls. See
[usb-console](usb-console.md).

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
cron entry that can wake the device (otherwise the `cron` lock stays held).
See [cron](cron.md).

## CLI

pm registers three commands (run on-device via `spangap cli "<command>"`):

| Command | Does |
|---|---|
| `pm` | Current CPU/APB frequency, then (under profiling) per-mode time deltas since the last `pm` and totals since boot — deep sleep / light sleep / 80 MHz / 240 MHz as a percentage of wall time, deep sleep with its count. |
| `pm -v` | Adds the full lock table: every esp_pm lock plus pm's own `NO_DEEP_SLEEP` rows, plus Mode and Sleep stats (the light-sleep reject count is the "why no light sleep" signal). |
| `pm wifi [none\|min\|max]` | Read or set the WiFi modem power-save mode (`esp_wifi_set_ps`). |
| `usb up` / `usb down` | Reconnect / disconnect the USB-serial peer (above); bare `usb` reports connection state, the console transport, and the reason the last transport switch failed. `usb cdc` / `usb jtag` move the console between controllers — [usb-console](usb-console.md), not pm. |
| `top` | Per-task CPU%, stack, per-task DRAM/PSRAM, per-core busy, heap, uptime. |
| `top -b <task> [hex]` | That task's internal-DRAM blocks grouped by exact size, largest first, with any block the kernel can name (a TCB is its own `TaskHandle_t`, a stack is `pxTaskGetStackStart`) labelled. `hex` adds the first 32 bytes of each size class, for identifying a struct by its contents. The DRAM column says how much a task holds; this says what — match a size against a `sizeof` in the suspected allocator. Blocks are billed to the task that ran malloc, so a server carries what it allocates for its clients. |

Chain `usb down; sleep 30; usb up` on one line — the CLI splits the commands up
front, so they all run even after the console disconnects.

`pm inhibit deep|light|slow` holds a `cli`-named lock until `pm allow` gives it
back — `slow` being the CPU, which it pins at its maximum. It is not part of
normal operation, and it is the first thing to reach for when a fault appears
only after a device has been running a while: a picture that comes apart, a
timing that was fine and then isn't, anything whose arrival coincides with
nothing that was changed. Holding the clock up either makes it go away — in
which case the fault belongs to whatever the CPU stopped keeping up with, and
`pm -v`'s frequency histogram says how much of the time that is — or it doesn't,
and a whole class of suspect is gone in one command.

The **`bat`** command (battery voltage and percent) is **not** pm — it lives in
core's system commands ([`cli_cmd_sys.cpp`](../esp-idf/src/cli_cmd_sys.cpp)) and
just reports the `battery.*` ephemerals a board's battery monitor publishes.
`net up` / `net down` likewise belong to [spangap-net](../../spangap-net), not pm.

## The activity sampler

While something is watching — an on-device Activity monitor or a browser one —
pm runs a 1 Hz sampler (`cpustat`, core 0) that fills a ring of per-second
samples: per-core busy percentages and PM-mode residency, one sample per second,
`s.sys.cpu_sample_buf` of them (320 by default, 5 bytes each; a piggy-backed
sampler such as -net's traffic ring uses the same length and beat). The rings
exist only while a watcher does, and `pmStatsHistory()` / `pmStatsAvg()` read
them. A monitor claims the sampler with **`pmStatsWatch(true)`**, which raises
`sys.stats.lcd_actmon` *and* acts on it — raising the flag alone leaves the
start waiting on a storage subscription delivered on the log task, which is a
monitor that opens onto an empty graph.

**That tick must never walk the task list.** `uxTaskGetSystemState()` holds the
FreeRTOS kernel lock across every task list — interrupts off on that core, and
the other core spinning for the same lock the moment it makes a scheduler call,
also with interrupts off. With forty tasks that is milliseconds, once a second,
during which nothing on the device can service an interrupt. It is invisible in
most things and fatal to anything with a hard deadline: an RGB panel's bounce
refill has 0.7 ms, so an open Activity monitor made the *display* lose sync once
a second, answering to none of the display's own settings.

So the tick takes per-core busy from the idle tasks' own run-time counters —
`ulTaskGetIdleRunTimeCounterForCore()`, two O(1) reads — and keeps no per-task
table at all. `top` is the one caller that wants per-task figures and samples
for itself when asked, which is where a walk belongs: once, on demand, by
someone who is watching for the answer.

## Storage keys

pm owns **no** `s.pm.*` storage keys. It reacts to `sys.going_down` (which it
writes) and is configured entirely through sdkconfig and the lock API.
`s.sys.cpu_sample_buf` (the sampler ring length, above) is a `sys` key rather
than a pm one; a monitor raises it to its own graph width, since the graph draws
one sample per pixel column.

## See also

- [power-management-internals.md](power-management-internals.md) — pmInit, the
  lock list and profiling, RTC stat accumulation across deep sleep, the USB and
  GPIO-wake mechanisms, boost internals, and pitfalls.
- [cron](cron.md) — the deep-sleep driver that pm's lock model feeds.
