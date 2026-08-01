# USB console transport

The chip reaches a host over one USB cable, and two different controllers can
drive it. `usb-console` is the switch between them, plus the rule for who owns
each serial port once the cable is up.

- **USB-Serial-JTAG** (the default) — the S3's built-in console controller. One
  serial port, hardware-implemented esptool reset, and the transport every
  `spangap flash` / monitor session expects.
- **TinyUSB CDC-ACM** (`usb cdc`) — a composite device presenting **two**
  serial ports. Port 0 carries the console (log + CLI) exactly as before; port 1
  is a second, independent port a straddle can hand to a client.

Only one of the two is on the wire at a time: the chip has a single internal USB
PHY, shared between the USB-Serial-JTAG controller and the USB-OTG core that
TinyUSB drives. Switching moves the PHY, so the host sees a disconnect and then
a fresh enumeration — a monitor attached across the switch must reopen the port.

It is part of spangap-core, so the verbs exist on every device whose console is
USB (`CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG`). On a UART console they report `n/a`
and do nothing.

## CLI

| Command | Does |
|---|---|
| `usb` | Peer presence, the transport the console is on, the port count, and the reason the last switch failed (if one did). |
| `usb cdc` | Move the console onto the two-port TinyUSB composite device. |
| `usb jtag` | Move it back onto the USB-Serial-JTAG controller. |
| `usb up` / `usb down` | Reconnect / disconnect the USB-serial peer — [power-management](power-management.md), unrelated to the transport. |

`usb CDC` and `usb JTAG` are accepted as the same verbs spelled the way the
transports are usually written; they are silent in `help` so the listing carries
one line per action.

A switch announces itself on the transport that is about to leave (`USB JTAG
serial port going away` / `USB CDC ports going away`), then moves. A host
watching the port can key off that line. Between the announcement and the far
side of the move nothing can be written to a wire, so console output is
**dropped** for that window rather than queued — the log file, the log ring and
every other log consumer are unaffected.

If the move fails the console stays where it was and the reason is printed once
the link is back up (`usb: switch to cdc failed (…)`), then kept so a later bare
`usb` repeats it — the first report goes out during a re-enumeration, when a
host is least likely to be showing anything.

## What the host sees

The composite device's strings are built per install rather than compiled in:

- **Product** — `s.net.hostname`.
- **Serial** — the low three bytes of the chip's MAC (its index within its OUI
  block, so unique per chip) followed by the hostname: `a1b2c3_lab1_`. macOS
  builds `/dev/cu.usbmodem<serial><interface>` from it, so two boards on one
  desk get distinguishable device nodes — the Kconfig default (`123456`) gives
  every board the same two names.

The same six MAC digits are logged every boot (`device a1b2c3`), so a host that
can read the log can tell which physical unit it is holding without reading USB
descriptors.

Both CDC interfaces share one USB interface string, so a host cannot tell port 0
from port 1 by name — only by interface number (port 0 first).

## Reset and reboot

The USB-Serial-JTAG controller implements the esptool reset convention (a
falling RTS with DTR low → restart; DTR high → ROM download mode) in hardware. A
CDC port only receives the line states, so the firmware acts on them itself, on
the console port only. It insists on the full two-step sequence rather than any
falling RTS: a port merely closing drops both lines, and treating that as a
reset request would restart the device every time a monitor went away.

The PHY routing lives in RTC registers that a software reset does not clear, so
a device that rebooted while on CDC would come back with no console attached to
anything. A shutdown handler therefore hands the PHY back before every clean
restart — `reboot`, an OTA swap, anything that goes through `esp_restart()` — so
a reboot always returns on USB-Serial-JTAG. **A panic bypasses it** and needs a
power cycle (or the EN pin) to clear the routing.

The transport is a runtime switch, not a persisted setting: nothing re-applies
`usb cdc` after a restart. Put it in the `/state/boot` script if a device should
always come up on CDC.

## Serial ports and handlers

`sys.usb.serial_ports` publishes how many serial ports exist right now — `1` on
USB-Serial-JTAG, `2` on CDC. A straddle that wants a port for its own protocol
claims one:

```cpp
serialPortClaim(1, "myservice", MY_ITS_PORT);   // second CDC port
serialPortRelease(1);
```

Port 0 is the console port (USB-Serial-JTAG, or CDC 0 while on `usb cdc`); port
1 exists only in the CDC case, so a claimant subscribes to
`sys.usb.serial_ports` and re-applies its claim when the transport changes.
A claim is dormant until a client actually attaches; while a client is attached,
the port's bytes are shuttled to the claiming task over ITS and the console is
detached from it. Core never interprets what crosses — the mechanism, the attach
triggers and the ITS contract are in
[cli-internals §3](cli-internals.md).

Two operator-visible consequences:

- **The console disappears from a claimed port 0** for as long as a client is
  attached; log and CLI return when it detaches.
- **esptool auto-reset does not work on a claimed CDC port.** A client closing
  the port drops DTR before RTS, which is indistinguishable from the reset
  sequence, so reset arming is suppressed there — otherwise an ordinary client
  exit would reboot the device.

## Cost

While the console is on CDC the platform holds a `NO_LIGHT_SLEEP` lock named
`usbcdc`, so the device does not light-sleep. Light sleep gates the USB clock,
and TinyUSB has no arrangement to survive that (the USB-Serial-JTAG controller
does, via `CONFIG_USJ_NO_AUTO_LS_ON_CONNECTION`) — a nap would drop the CDC link
and the host would have to replug. `pm` shows the lock; see
[power-management](power-management.md).

The TinyUSB device stack is linked into every build and holds internal `.bss`
whether or not it is ever installed, which is paid for out of the internal-DRAM
reserve regardless of whether a device ever runs `usb cdc`.

## sdkconfig contract

Supplied by core's [`sdkconfig.defaults.spangap`](../esp-idf/sdkconfig.defaults.spangap);
a board that overrides sdkconfig must keep them.

| Option | Why |
|---|---|
| `CONFIG_TINYUSB_CDC_ENABLED=y`, `CONFIG_TINYUSB_CDC_COUNT=2` | Two CDC-ACM ports is the hardware ceiling: each claims an interrupt IN endpoint plus a bulk IN/OUT pair, and the OTG core has five IN endpoints past EP0. |
| `CONFIG_VFS_MAX_COUNT=12` | The CDC console registers `/dev/tusbcdc` as a filesystem, and IDF's slot accounting never gives a slot back (see the internals). |
| `CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=98304` | Covers the boot-peak internal-DRAM demand, which the linked-in TinyUSB stack raises. |
| `CONFIG_ESP_SYSTEM_PANIC_REBOOT_DELAY_SECONDS=5` | On a USB console the device leaves the bus the moment it resets; without the delay the backtrace is still in the TX FIFO when the host loses the port, and a crash reads as a silent reboot. |

## See also

- [usb-console-internals.md](usb-console-internals.md) — the PHY hand-over
  sequence, stream redirection, teardown ordering, descriptors, and pitfalls.
- [cli-internals §3](cli-internals.md) — the serial-port handler registry.
- [power-management.md](power-management.md) — `usb up`/`usb down`, the D+
  pullup, and the lock model.
