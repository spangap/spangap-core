# cli — internals

Maintainer reference for the command line
([`src/cli.cpp`](../esp-idf/src/cli.cpp),
[`src/cli_cmd_sys.cpp`](../esp-idf/src/cli_cmd_sys.cpp),
[`include/cli.h`](../esp-idf/include/cli.h); filesystem verbs are in
`cli_cmd_fs.cpp`/`cli_cmd_mount.cpp`, others register from their owning module).
The [operator guide](cli.md) is the command manual; this is for changing the
code without breaking it.

## 1. What cli owns and adds

`cli` provides the command registry + dispatcher, the interactive line editor,
the per-client slot pool, two ITS server ports, and the serial console shuttle.
`cliInit` (auto-called from the platform init chain) seeds `s.cli` defaults,
registers the builtin commands on the main task, then spawns the `cli` and
`serial` tasks.

### Registry & dispatch

- Commands live in a `std::vector<cli_cmd_entry_t>` (`cliCmds()`,
  construct-on-first-use), kept **alphabetically sorted on insert** so `help`
  output is ordered. Each entry caches `cmdLen`. `cmd` is an un-owned
  `const char*` — every caller passes a string literal.
- **Longest-prefix match.** `cliProcess` (and `cliLongestCmdMatch` for tab
  completion) pick the registered command whose name is the longest prefix of
  the line that is followed by a space or end-of-string. This is why
  `mount sd`, `format flash`, `format sd`, and `reset factory` are registered as
  whole multi-word names alongside `mount`/`format`/`reset`.
- **Registration is init-time only.** All `cliRegisterCmd` calls run from module
  `*Init()` on the main task, before the cli task exists; the vector is
  unlocked. A later insert reallocates and would invalidate any held index, so
  never register a command after boot.
- `cliProcess` order: trim → drop `#`/empty → split on the first `;` and recurse
  → expand a leading alias once (`s.cli.aliases.<word>`, non-recursive) → longest
  match → call the callback with trimmed args.

### Builtin commands registered here

`cli_cmd_sys.cpp` (`cliCmdSysInit`): `reboot`, `reset factory`, `format flash`,
`format sd`, `sleep`, `run`, `its`, `bat`. `cli.cpp` (`cliBuiltinInit`):
`alias`, `unalias`, `exit`, `help`, plus it chains the other modules'
registration (`storageRegisterCmds`, `cliCmdFsInit`, `cliCmdMountInit`,
`pmRegisterCmds`, `logRegisterCmds`). `format flash`/`format sd`/`reset factory`
run on a **DRAM-stack worker** because the format disables the PSRAM cache while
the cli task is PSRAM-stacked; the command blocks on a semaphore until the worker
finishes so scripted one-liners don't race the format.

### Slots & ports

- `CLI_MAX_CLIENTS = 8` PSRAM-resident `cli_slot_t`s, each holding its ITS
  handle, a `cli_edit` line-editor state, mode/color/usbSerial/noPrompt flags, a
  LINE-mode accumulation buffer, reported terminal `cols`/`rows`, a `cwd[256]`,
  and a `pendingClose` flag.
- Two ITS server ports, opened on the cli task: **`CLI_PORT_TCP = 8081`**
  (stream-mode, 6 slots — raw `nc` and the on-device serial task) and
  **`CLI_PORT_DC = 1`** (packet-mode, 2 slots — the browser WebRTC terminal and
  the on-device LCD CLI). The two caps sum to the pool, so the DC pair stays
  guaranteed under a TCP flood. The TCP listener itself is exposed by
  spangap-net; core opens the port, net forwards external TCP/TLS into it.
- `cli_connect_t` (mode, `from_usb_serial`, color, `no_prompt`) is the connect
  payload. A zero-filled payload is `CLI_ANSI` + color-on; a single byte is read
  as just the mode; a larger/foreign descriptor (net's own `net_connect_t`) is
  treated as `CLI_LINE`. DC connects optionally carry a `"colsxrows"` string for
  pty geometry (default 80x24).

### Line editor (`cli_edit`, per slot)

Dynamic `std::string buf` with an insert-at-cursor cursor (no fixed cap),
plus a saved line for history browsing. `cliEditChar` is the state machine:
printable insert, backspace/DEL erase, `^A`/`^E` home/end, `^D` end-of-input,
arrow keys via the ESC `[` state machine (left/right move; up/down browse
history), and Tab completion. Cursor math is **wrap-aware** — `cliMoveCursor`
and `cliEditRefresh` reckon in terminal rows using the client's reported width,
so a line that wraps redraws and clears correctly. History is a shared
`std::deque<std::string>` (`HIST_SIZE = 20`, newest front, consecutive dups
skipped) common to all slots. Tab completion only fires for path-taking commands
(`ls cd mkdir rm cat df run logfile`) and reads the directory via `fs_listdir`
into a PSRAM buffer. Color escapes route through `cliColorWrite`, gated on
`cliWantsColor()`; cursor/edit sequences are always sent in `CLI_ANSI`.

### Interactive input helpers

`cliReadLine`/`cliReadRaw`/`cliTermSize` let a command read input mid-execution
(the ssh client uses them for password prompts and a live shell relay). They read
the active slot's ITS handle directly — the same byte stream the editor consumes
— and **keep pumping `itsPoll(0)` while parked** so new connections still get
accepted while a command blocks. `cliReadLine` strips escape sequences
(including bracketed-paste markers) so a typed/pasted secret isn't polluted, and
honors `cli_echo_t` (`CLI_ECHO`/`CLI_ECHO_STARS`/`CLI_ECHO_NONE`). Both carry a
~90 s safety deadline so a walked-away session can't wedge the cli task.

### Output routing

`cliOut` is the active write sink (set per dispatch to `itsCliWrite`, or
`cronCliWrite` for cron-driven commands). `cliPrintf` formats into a 256-byte
stack buffer; `cliWrite` chunks at 512 bytes. `itsCliWrite` applies
backpressure (`itsSendAll`: block-and-retry, give up after ~30 s of no drain or
peer loss) so a big `cat` throttles to the channel rate instead of wedging the
WebRTC/SCTP path, and translates lone `\n`→`\r\n` only on the ANSI+non-serial
(raw-PTY) path.

## 2. Tasks & threading

Two tasks, both prio 1, both spawned by `cliInit`:

- **`cli`** (6144-byte stack) owns the registry, the slot pool, and the two ITS
  ports. Its loop drains `itsPoll`, then per slot feeds received bytes to the
  line editor (ANSI) or buffers to newline (LINE), then runs a deferred-close
  sweep, then drains cron commands. `cliActiveSlot` is set around each slot's
  processing so `cliPrintf`/cwd/`cliReadLine` resolve to the right client.
- **`serial`** (4096-byte stack) is a byte shuttle between the USB serial ports
  and the cli/log views. It is **an ITS client** (`itsClientInit(2)` — a handler
  session must be able to coexist with a console CLI session) **to `cli:1`/TCP**
  — the first non-newline keystroke flips `serialInCli = true`, connects to
  `CLI_PORT_TCP` with a `cli_connect_t{CLI_ANSI, from_usb_serial=1, …}`, and
  shuttles bytes both ways; an empty Enter, a trailing `;`, `^D`, or `^C` returns
  to the live log. The log/CLI serial mode switch and `serialInCli` suppression
  are owned by [logging](logging.md) — don't duplicate that here. It also owns
  the serial-handler registry described in §3.

**Console I/O is transport-branched.** Every read, write and flush in this file
picks a path off `consoleOnCdc` ([usb-console](usb-console.md)): `cliFlush`
flushes the CDC queue or the USB-Serial-JTAG TX FIFO, `serialEmit` writes
`stdout` on CDC (the TinyUSB VFS does its own line-ending translation and has no
`is_connected` gate to route around) or takes the driver-level bypass otherwise,
and reads come from the driver rather than `STDIN_FILENO` — a switch reopens
`stdin` onto a fresh descriptor, so fd 0 can still refer to the device the
console has left.

**Deferred close.** A slot that wants to hang up (trailing `;`, serial
empty-Enter, `exit`) sets `pendingClose`; the main loop tears the ITS handle down
only once `itsSendIsEmpty(h)` — i.e. the peer has read the last output byte.
Disconnecting inline races the recv task and truncates verbose output. Because a
local close doesn't fire cli's own ITS disconnect callback, the slot is reset
inline in this sweep or it would leak.

## 3. Serial-port handlers

A task can take a serial port away from the console and become the endpoint for
whatever attaches to it — how a straddle exposes a framed protocol (a radio
modem's host interface, say) to a client on the other end of the USB cable. Core
stays generic: it knows "this port has a handler", never what the handler
speaks. Which controller drives those ports, and the switch between them, is
[usb-console](usb-console.md).

### Registry and claims

`serialPortClaim(port, task, itsPort)` / `serialPortRelease(port)` (`cli.h`).
Port 0 is the console port — the USB-Serial-JTAG controller, or CDC 0 while the
console is on `usb cdc`. Port 1 is the second CDC port, which exists only in the
CDC case, and only in a build with `CONFIG_SPANGAP_USB_CDC` (off by default —
`SERIAL_PORT_COUNT` is then 1 and the registry has no port-1 slot at all);
claiming it otherwise fails with a warn and a false return. One handler per
port; re-claiming with the same task and ITS port succeeds unchanged, so a
claimant can re-apply on every config pass.

`sys.usb.serial_ports` (1 or 2) is published from `usb_ports.cpp` on every
`usb cdc`/`usb jtag` switch and once at registration. It is written there rather
than derived from `consoleCdcPortCount()`, which reports **0**, not 1, while the
console is on USB-Serial-JTAG. A claimant subscribes to it to re-apply a port-1
claim when the transport changes.

The registry is plain statics plus `volatile bool` edges the serial task polls
and clears — the same cross-task mechanism `consoleSwitchPending` uses. Each flag
has a single writer; the registry itself only changes on claim and release.

### Attach and release

Detection differs by transport, and the reason is a hardware fact:

- **CDC (either port):** the host's DTR rise. Every pyserial-class client asserts
  DTR on open and drops it on close. `cdcLineStateCb` is installed on **both**
  ACM ports and keeps `prevRts`/`prevDtr` **per port** — one shared copy turns
  the host settling port 1's lines into a phantom edge on port 0.
- **USB-Serial-JTAG (port 0 only):** in band, on the first received `0xC0`. The
  peripheral **exposes no line state to software at all** — there is no DTR or
  RTS in the S3 driver, the LL, or the register struct; `usb_serial_jtag_is_-`
  `connected()` is SOF-derived and says nothing about a host opening a port.
  `0xC0` is the frame delimiter of KISS (the framing serial terminal-node
  controllers use), never a console keystroke, and the first byte such a client
  sends. It is forwarded, not swallowed. So a claimed port 0 on USB-Serial-JTAG
  is still an ordinary console until a client speaks.

**A claimed CDC port has its esptool reset arming disabled.** The arming pattern
is `prevRts && !prevDtr` followed by a falling RTS, and a normal pyserial close
drops DTR before RTS — indistinguishable from the reset sequence. Without the
suppression a clean client exit reboots the device. The trade-off is that esptool
auto-reset is unavailable on a claimed port.

On attach the serial task drops any CLI session on port 0, sets `serialInHandler`,
and `itsConnect`s the claimant with a `serial_handler_connect_t{serialPort}`. A
rejected connect (the handler already has a session) leaves the port exactly as
it was — on port 0, still a console. Release happens on DTR drop, on the
handler's own `itsDisconnect`, on `usb down` (`cliUsbSerialLinkDown`), or when
`usb jtag` takes port 1 away — in which case the claim stays and goes dormant
until the claimant re-applies it.

### `serialInHandler`

A **third** variable beside `serialInCli` and `cliHandle`, not a reuse of either.
The two existing ones deliberately diverge — a trailing-`;` command clears
`serialInCli` mid-session — and the CLI's own paths write it. `serialInHandler`
gates both of log.cpp's console mirrors (`logVprintf`'s direct `stdout` echo and
the inbound-line echo) and CLI entry: the port is carrying a client's protocol,
and console text pushed into that stream would corrupt it.

### Idle loop and the shuttle

The port-1 pump and the claim bookkeeping run at the top of the serial task's
loop, ahead of every console mode, so a claimed port 1 is serviced whether or not
the console has a CLI session open. The CDC idle branch waits on
`ulTaskNotifyTake` with a timeout rather than a plain `delay(50)`; TinyUSB's
`callback_rx` on a claimed port gives the serial task a notification, so a
client's bytes are not paced by the poll interval. The USB-Serial-JTAG idle
branch still parks indefinitely on the driver's ISR-fed RX ring — a keystroke or
a client's first `0xC0` wakes it, and an idle console costs no wakes.

Shuttle reads are **block** reads (`consoleCdcReadPort` → `tinyusb_cdcacm_read`,
or `usb_serial_jtag_read_bytes`), not the console's one-byte `consoleCdcRead`
path: a client that validates its configuration a quarter of a second after
sending it leaves no room for 50 ms per byte. Writes go out raw — no `\n`→`\r\n`
translation, which `serialEmit` applies to console output.
Stream-mode `itsSend` can accept part of a write, so the pump **carries the
unsent remainder forward** (`serialHdlPend`) and reads no more from the port
until it has drained: a framed protocol cannot survive losing the middle of a
frame.

Note that both CDC interfaces share USB string index 4, so a host cannot tell
ACM 0 from ACM 1 by name; giving each its own `iInterface` would take a custom
configuration descriptor.

## 4. Session cwd

`s.cli.start_dir` (seeded `/`) is resolved by `cliResolvedStartDir`: read (default
`/`), force-absolute, collapse `.`/`..` via `cliCollapseAbsolute`, and fall back
to `/` if the path isn't an existing directory. `cliGetCwd` returns the active
slot's cwd or the resolved start dir when there's no interactive slot (cron).
USB-serial sessions persist cwd across reconnects in `cliUsbPersistCwd`.

## 5. Boot-script lifecycle

`cliRunFile(fsStatePath("/boot"))` runs from `spangapPostAppInit`, deliberately
**after every platform and consumer command is registered**. It reads the file
line by line (`#` comments and blank lines skipped), logging and `cliProcess`-ing
each, with a 50 ms inter-line delay so the log task drains under the boot burst.
Missing file is silently fine. `run <file>` shares the same `cliRunFile`.

## 6. Pitfalls

- **Longest-prefix dispatch means multi-word verbs are first-class.** Anything
  like `mount sd` must be registered as its own whole-string command alongside
  `mount`; you cannot get a subcommand "for free" by registering only `mount`.
- **The help convention is mandatory.** Bare `help` lists commands by calling
  every callback with `"help"`; a command that doesn't print a one-liner for
  `args=="help"` is invisible in the listing. Follow the
  `help`/`-h`/`--help`/`""` contract (`cliWantsHelp`).
- **Register at init only.** The command vector is unlocked and reallocates on
  insert; registering off the main-task init chain races, and registering after
  boot can invalidate an index held mid-dispatch.
- **Never disconnect a slot inline from a command.** Set `pendingClose` and let
  the drain sweep close it, or verbose output is truncated on the wire.
- **`cliReadLine`/`cliReadRaw` must keep pumping `itsPoll`.** While a command
  blocks on input the main loop is parked in that command; without the inner
  `itsPoll` drain, every new connection (browser/LCD dialing `cli:1`) is rejected
  for the whole prompt.
- **Never gate a serial-handler write through `serialEmit`.** It translates line
  endings, which corrupts any framed protocol.
- **`cliSerialResumeLog()` only latches `cliUsbSerialAutoResumeLog`.** The latch
  belongs to a session; the log-mode idle branch clears it, or a call made with
  no session open (as `switchConsole` does) ends the *next* session the moment it
  opens.
