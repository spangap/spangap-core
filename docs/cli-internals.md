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
- **`serial`** (4096-byte stack) is a byte shuttle between the USB serial port
  and the cli/log views. It is **an ITS client to `cli:1`/TCP** — the first
  non-newline keystroke flips `serialInCli = true`, connects to `CLI_PORT_TCP`
  with a `cli_connect_t{CLI_ANSI, from_usb_serial=1, …}`, and shuttles bytes
  both ways; an empty Enter, a trailing `;`, `^D`, or `^C` returns to the live
  log. The log/CLI serial mode switch and `serialInCli` suppression are owned by
  [logging](logging.md) — don't duplicate that here.

**Deferred close.** A slot that wants to hang up (trailing `;`, serial
empty-Enter, `exit`) sets `pendingClose`; the main loop tears the ITS handle down
only once `itsSendIsEmpty(h)` — i.e. the peer has read the last output byte.
Disconnecting inline races the recv task and truncates verbose output. Because a
local close doesn't fire cli's own ITS disconnect callback, the slot is reset
inline in this sweep or it would leak.

## 3. Session cwd

`s.cli.start_dir` (seeded `/`) is resolved by `cliResolvedStartDir`: read (default
`/`), force-absolute, collapse `.`/`..` via `cliCollapseAbsolute`, and fall back
to `/` if the path isn't an existing directory. `cliGetCwd` returns the active
slot's cwd or the resolved start dir when there's no interactive slot (cron).
USB-serial sessions persist cwd across reconnects in `cliUsbPersistCwd`.

## 4. Boot-script lifecycle

`cliRunFile(fsStatePath("/boot"))` runs from `spangapPostAppInit`, deliberately
**after every platform and consumer command is registered**. It reads the file
line by line (`#` comments and blank lines skipped), logging and `cliProcess`-ing
each, with a 50 ms inter-line delay so the log task drains under the boot burst.
Missing file is silently fine. `run <file>` shares the same `cliRunFile`.

## 5. Pitfalls

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
