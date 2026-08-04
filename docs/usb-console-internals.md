# usb-console internals

Maintainer reference for [`src/usb_ports.cpp`](../esp-idf/src/usb_ports.cpp) —
the console transport switch. The serial-port handler registry that rides on it
lives in `cli.cpp` and is documented in [cli-internals §3](cli-internals.md).
Operator view: [usb-console.md](usb-console.md).

Everything here is compiled only under `SPANGAP_CDC_BUILT` (`cli.h`) —
`CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG && CONFIG_SPANGAP_USB_CDC`. The `#else` half
supplies stubs, and the three transport flags (`consoleOnCdc`,
`consoleSwitchPending`, `consoleWriteDead`) are defined either way and stay
false, so `cli.cpp`, `log.cpp` and `pm.cpp` need no Kconfig guard of their own.

## 0. Build wiring

`espressif/esp_tinyusb: "^2.2"` in `idf_component.yml` fetches the stack
unconditionally — a component-manager dependency cannot be conditioned on a
`CONFIG_` symbol. What is conditional is the link: `CMakeLists.txt` appends
`espressif__esp_tinyusb` to `REQUIRES` only under `CONFIG_SPANGAP_USB_CDC`
(managed components carry the `<namespace>__<name>` spelling), and an unrequired
archive contributes no code and no `.bss`. `CONFIG_SPANGAP_USB_CDC` also
`select`s `CONFIG_TINYUSB_CDC_ENABLED`, which is what drops `cdc.c` /
`vfs_tinyusb.c` from the component's own source list.

That `.bss` is internal DRAM, spent out of the pool the boot-peak DMA
allocations draw on (`CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL`). The reserve is
sized for the image's own peak and does not move with this symbol — but the
headroom it leaves is thin, and a shortfall there shows up as a mid-boot stall
rather than a clean failure. See [memory.md](memory.md).

`SERIAL_PORT_COUNT` (`cli.h`) follows the same symbol: 2 when the transport is
built, 1 when it is not, so a default image carries no port-1 registry.

## 1. One PHY, two controllers

`tinyusb_driver_install()` routes the internal USB PHY to the USB-OTG core,
which takes the USB-Serial-JTAG console off the wire mid-command. The reverse
move is **not** symmetric: `usb_del_phy()` drops the pull override but leaves
the FSLS PHY routed to the OTG wrap, and the USB-Serial-JTAG peripheral's RX
interrupt does not survive the round trip. `pmUsbSerialJtagReattach()`
(`pm.h`, implemented in `pm.cpp`) is what actually completes the hand-back: it
resets the peripheral, re-enables PHY and pads via the same path `usb up` uses,
drains stale RX, and flushes what was queued.

It is called **unconditionally** on every failure path and at the end of
`cdcClose()`. Making it contingent on a successful uninstall leaves the device
with no console and no way to ask for one short of a reset.

`pmPollUsb()` early-returns while `consoleOnCdc`. The USB-Serial-JTAG
controller reads permanently disconnected once it has lost the PHY, so the
recovery path would otherwise reset that peripheral and re-enable its PHY every
60 s, fighting the OTG core for the pads.

## 2. The three flags

All three are `extern "C" volatile bool`, defined here unconditionally and read
from `cli.cpp`, `log.cpp` and `pm.cpp`.

| Flag | Set while | Read by |
|---|---|---|
| `consoleOnCdc` | the console runs on CDC 0 | every console I/O site: `cliFlush`, `serialEmit`, the serial task's read paths, `pmPollUsb`, the `usb` status line |
| `consoleSwitchPending` | a switch is in progress | the serial task's idle branch — it must **poll** rather than park on the USB-Serial-JTAG RX ring, or it blocks on a controller that is being taken away and the new console is deaf |
| `consoleWriteDead` | no transport owns the pads | `serialEmit` and both of `log.cpp`'s `stdout` mirrors — writes are dropped, not queued |

`consoleSwitchPending` starts **earlier** than `consoleWriteDead`: the warning
that announces the move has to go out on the transport that is about to leave,
and a host watches for it.

## 3. Switch sequence (`switchConsole`)

1. `consoleSwitchPending = true`, `cliSerialResumeLog()`, 150 ms — a CLI session
   cannot outlive the port it runs on, and a watching host needs the notice as a
   *log* line: CLI output is addressed to one session and stops at the wire,
   while the log fans out to every consumer.
2. `warn(…going away)`, 250 ms for the fan-out to leave the device.
3. `consoleWriteDead = true`.
4. `cdcClose()` if on CDC; `cdcOpen(TINYUSB_CDC_ACM_MAX)` if moving to CDC.
5. Clear both flags, `publishSerialPorts()`.
6. `reportSwitchFailure()` — **last**, and after the pending flag is cleared,
   because it waits up to 3 s for a host on the reattached link and the serial
   task must be back on its normal path by then.

## 4. Streams

`tinyusb_console_init(TINYUSB_CDC_ACM_0)` carries the whole console across in
one call: `logVprintf`'s direct `stdout` echo, the serial task's prompts and
banners, and the standard streams.

The way back is `restoreJtagStreams()`, **not** `tinyusb_console_deinit()`: that
restores by reopening `/dev/uart/CONFIG_ESP_CONSOLE_UART_NUM`, and a board whose
console is USB has no UART, so the number is `-1`, every `freopen` fails, and
the device is left with NULL standard streams and no console at all.

Two details that are easy to lose:

- **`freopen("/dev/console", …)`, not the USB-Serial-JTAG device node.** The
  console VFS is the layer that knows which transport the console is configured
  for; the device node is a different path with its own write gating, which
  drops whole chunks whenever its SOF-derived connected flag reads false — as it
  does right after a reattach, before any tick has updated it.
- **`setvbuf(stdout, _IOLBF)` after every `freopen`.** Reopening drops the line
  buffering `spangapInit` set, leaving `stdout` fully buffered — log lines then
  sit in the FILE buffer until something fills it, which on an idle device is
  never.

`freopen()` need not preserve a descriptor number, so **fd 0 is not a reliable
console after a switch**: the serial task reads the driver (`consoleCdcRead` /
`usb_serial_jtag_read_bytes`) rather than `STDIN_FILENO`, and both the console
byte path and the handler shuttle do the same.

`restoreJtagStreams()` is separate from the PHY hand-back because the two belong
at opposite ends of a teardown — the streams must leave the CDC device before it
is dismantled, the PHY only after — and a bring-up that fails partway needs both
in that order.

## 5. Teardown ordering (`cdcClose`)

`consoleOnCdc = false` first (every other task's console writes must be back on
the USB-Serial-JTAG path before the CDC VFS goes away), 20 ms, then the streams,
then `tud_disconnect()` and a **250 ms** pause. Both parts of that pause matter:
the USB-Serial-JTAG controller asserts its own pull-up within a few hundred
microseconds of the reattach, so without a gap the host coalesces detach and
attach into no event at all and never re-enumerates; and `tusb_deinit()` refuses
while the stack still believes it is attached.

That refusal is **unrecoverable**: `tinyusb_driver_uninstall()` stops its task
first and returns on failure, so `usb_del_phy()` never runs, the internal PHY
stays marked in use, and every later install fails in `usb_new_phy()` with
"selected PHY is in use". The console still works — the reattach is
unconditional — but `usb cdc` is shut until a reboot. That is what
`cdcLastError` records and what a later bare `usb` repeats.

## 6. Two error strings, on purpose

- `cdcFailReason` — the per-step reason a bring-up failed, written where it
  happens, which is always a moment with no console.
- `cdcLastError` — the switch-level summary, kept until a person has seen it and
  afterwards so `usb` can repeat it.

Both exist because a log call made mid-switch is discarded whole (the log's
route out is gated on a connection flag that has not caught up), and because the
report itself lands during a re-enumeration, where a host that has just opened
the port has every reason to treat what it finds as stale.

`reportSwitchFailure()` waits **bounded** (30 × 100 ms) for
`usb_serial_jtag_is_connected()` — a device with nothing plugged in must not
hold the CLI task waiting for a reader that is not coming — and writes both
through `consoleWriteRaw()` (the answer to a command someone typed, which must
land on the console they typed it on) and through `err()` (which reaches the log
file and every other consumer either way).

`consoleWriteRaw()` / `consoleFlush()` are the two `cli.h` primitives this
needs: a write that bypasses both the CLI session and the log, and a flush that
pushes the stream buffer *and* the USB-Serial-JTAG TX FIFO, which otherwise
holds a short write until a newline. Without the flush a backlog waits for
whatever writes next — press a key an hour later and it arrives then.

## 7. Descriptors

Built per install (`buildDescStrings`) rather than compiled in, because the
product and serial strings are derived from `s.net.hostname`. TinyUSB reads the
table whenever the host asks, so it must outlive the install — hence file-scope
buffers.

- **31 characters is the ceiling.** `esp_tinyusb` converts through a 32-entry
  buffer and truncates anything longer silently. The serial is
  `%02x%02x%02x_%.23s_` — MAC digits first so the hostname, the part a person
  reads, ends the name and a host appending an interface number lands after the
  trailing underscore rather than against the hex.
- Index 0 is a **language ID pair** (two raw bytes), not text. Indices 1–4 are
  manufacturer, product, serial, CDC interface. Both CDC interfaces share index
  4; giving each its own would take a custom configuration descriptor.

## 8. esptool reset convention

`cdcLineStateCb` is installed on both ports and keeps `prevRts`/`prevDtr` **per
port** — one shared copy turns the host settling port 1's lines into a phantom
edge on port 0. Arming is `prevRts && !prevDtr` (reset asserted); the action is
the falling RTS out of that state, with DTR at that moment choosing ROM download
(`RTC_CNTL_FORCE_DOWNLOAD_BOOT`) over an ordinary restart. Insisting on the
prior state is what separates a reset request from a port closing: a host with
the port open holds DTR asserted, so a close never passes through it.

The restart runs on a spawned task, not the TinyUSB task, which has a device to
keep answering until the restart lands; it waits 400 ms so the log's fan-out
(the browser's WebRTC channel among them) can leave the device first.

Arming is skipped entirely on a claimed port and on port 1 — see
[cli-internals §3](cli-internals.md).

Every `cdcOpen` resets `prevRts`/`prevDtr`/`resetPending`. They persist across a
close, and stale values make a host's first edge read as the middle of a reset
sequence — or a genuine one read as a repeat and be dropped.

## 9. Draining

Two drains, same reason: bytes that arrived while nobody was reading are not
input for the session about to start.

- `cdcDrain(itf)` after each `tinyusb_cdcacm_init` — bounded to 16 passes,
  because a host writing as fast as this reads would otherwise hold the loop
  indefinitely, and this runs where a stall is expensive.
- The same loop in `pmUsbSerialJtagReattach()`, plus `tud_cdc_n_write_clear` on
  a DTR rise so a new session does not receive what the last one left in the TX
  FIFO.

Delivered late, those bytes land as a burst of keystrokes nobody typed — which
opens a CLI session nobody asked for.

## 10. VFS slot accounting

`CONFIG_VFS_MAX_COUNT=12` (default 8) is **not** headroom for taste. IDF's
accounting is one-way: `esp_vfs_unregister()` clears the table entry but leaves
`s_vfs_count` at its high-water mark, and `esp_vfs_register_fs()` refuses on
that count before it looks for a free entry. A filesystem that comes and goes —
`/dev/tusbcdc`, once per switch — therefore registers once and is then refused
forever with `ESP_ERR_NO_MEM`, its slot sitting empty. The count settles at the
peak rather than climbing per cycle, so one spare slot would do; 12 leaves four.

A running device fills the default 8 exactly: console, uart, usbserjtag, null,
lwip sockets, littlefs, FAT, and `/dev/tusbcdc`. `cdcOpen` names this in its
error string, because `ESP_ERR_NO_MEM` here is not about the heap.

## 11. Pitfalls

- **Never make the PHY hand-back conditional.** Any early return that skips
  `pmUsbSerialJtagReattach()` leaves a device with no console.
- **A panic bypasses the shutdown handler.** The routing lives in RTC registers
  that `esp_restart()` does not clear (it resets the CPUs and a fixed list of
  peripherals; neither USB controller is on it), so a panic while on CDC comes
  back with the PHY still routed to OTG and nothing driving it. Power cycle.
- **Do not gate console output on DTR.** Web Serial does not define whether
  opening a port asserts it and Chrome need not; a console that only writes once
  DTR is seen goes permanently mute against such a host.
- **`consoleCdcRead` is one byte at a time and console-only.** A handler's
  stream goes through `consoleCdcReadPort`/`consoleCdcWritePort`.
- **`consoleCdcPortCount()` reports 0, not 1, while the console is on
  USB-Serial-JTAG.** `sys.usb.serial_ports` is published from
  `publishSerialPorts()`, which does the mapping; do not derive it at the call
  site.
