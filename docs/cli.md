# cli — the on-device command line

`cli` is spangap-core's command line: a single command registry and line editor
that every straddle plugs verbs into, reachable over several channels at once.
It is how you inspect and drive a running device — read and set storage, walk the
filesystem, tail logs, reboot, run scripts. Commands register themselves at boot;
core ships a base set and each straddle in the build adds its own.

## Reaching the CLI

The same command line is served on four channels simultaneously:

- **`spangap cli "<command>"`** — runs one command on the live device from the
  build host (the everyday path).
- **Serial console** — typing any character at the log view switches the USB
  serial console into CLI mode; an empty Enter (or `exit`) returns to the live
  log. Enter at the log view does *not* open a session — it names the transport
  the console is on instead. See [logging](logging.md) for the log/CLI mode
  switch and [usb-console](usb-console.md) for the transports.
- **Raw TCP** — `nc <device> 8081` (`CLI_PORT_TCP`), exposed by
  [spangap-net](../../spangap-net); a net-less image simply has no TCP listener.
- **Browser DataChannel** — the xterm.js terminal window over WebRTC, addressed
  as `cli:1` (`CLI_PORT_DC`).

A minimal session:

```
$ spangap cli "set s.net.hostname=lab1"
$ spangap cli "show s.net.hostname"
s.net.hostname=lab1
```

## The command framework

- **Registration.** A module calls `cliRegisterCmd(name, fn)` at init. The table
  is kept alphabetically sorted, and dispatch is **longest-prefix match** — so
  `mount sd` and `mount` are two separate commands and the more specific one
  wins.
- **Help convention** (uniform across every command):
  - `<cmd> help` → one short line (this is what bare `help` lists).
  - `<cmd> -h` / `<cmd> --help` → fuller per-command help.
  - `<cmd>` with no args → status (there is no separate `status` verb).
  - `cliWantsHelp(args)` covers all three help spellings in one guard.
- **Verb abbreviation** is `cliVerbIs(tok, full, minLen)`: a verb matches on any
  prefix of itself at least `minLen` characters long, so `lora a` and `lxmf a`
  reach `announce`, and `minLen` is what stops a one-letter form from reaching a
  verb whose neighbours share its opening letters (`lora supe` needs all four
  against `sf` and `sync`). A longer word that merely starts the same way —
  `announces` against `announce` — is not a prefix, so the two remain distinct
  commands and only their dispatch order decides which a full spelling reaches.
- **Output** is flush-left and **silent on success** — a command that worked
  prints nothing (or just its requested data). Use `cliPrintf`/`cliWrite`; color
  is emitted only when `cliWantsColor()` is true.
- **Multiple commands** on one line are separated by `;`. A line whose first
  non-space character is `#` is a comment.
- **Modes**: `cli_mode_t` is `CLI_ANSI` (interactive: device-side echo, line
  editing, history, ANSI) or `CLI_LINE` (the client sends finished lines, no
  echo). `cli_color_t` gates color independently.

### Session working directory

Each session has a current directory. Bare `cd` (and a fresh session) starts at
`s.cli.start_dir`. `cliResolveFsPath` resolves relative paths against the cwd for
filesystem commands. Cron and other non-interactive callers default to `/`.

### Boot script

At the very end of boot — after every platform *and* consumer command is
registered — `cli` runs `<stateDir>/boot` as a CLI script if it exists (one
command per line, `#` comments, blank lines skipped). This is the device's
startup customisation hook; running it last guarantees any verb a straddle
contributes is already available. `run <file>` executes any script the same way.

## Command manual

Many verbs are owned by a sibling core doc; those are listed here with a
one-liner and a pointer. The CLI-framework's own commands are documented in full.

### Filesystem — see [fs.md](fs.md)

| Command | |
|---|---|
| `ls [path]` | list a directory |
| `cd [path]` | change cwd (bare `cd` → `s.cli.start_dir`) |
| `pwd` | print cwd |
| `cat <file>` | print a file |
| `cp <src> <dst>` | copy |
| `mv <src> <dst>` | move/rename |
| `rm <path>` | remove |
| `mkdir <path>` | make a directory |
| `df` | filesystem usage |
| `mount` / `mount sd` | mount status / mount the SD card |
| `format flash` / `format sd [KB]` | reformat a filesystem |

### Storage / config — see [storage.md](storage.md)

| Command | |
|---|---|
| `set <key>[=<value>]` | set a config key (or `set <key> <value>` — space also separates; bare `set <key>` sets `1`) |
| `reset <key>` | set a config key to `0` |
| `unset <key>` | delete a key |
| `show [prefix]` | print keys |
| `save` | flush pending settings to the state store |

### Logging — see [logging.md](logging.md)

| Command | |
|---|---|
| `log` | log level / view control |
| `logfile` | log-to-file control |
| `logrotate` | rotate log files |

### Power / system status — see [power-management.md](power-management.md)

| Command | |
|---|---|
| `pm` | power-management state and locks |
| `top` | task CPU / stack snapshot |
| `usb` | USB-serial peer presence, console transport, last switch error |
| `bat` | battery voltage + percent |

### USB console transport — see [usb-console.md](usb-console.md)

| Command | |
|---|---|
| `usb cdc` | move the console onto a two-port TinyUSB CDC device (`CONFIG_SPANGAP_USB_CDC`; `n/a` without it) |
| `usb jtag` | move it back onto the USB-Serial-JTAG controller |

### Auth — see [auth.md](auth.md)

| Command | |
|---|---|
| `auth [...]` | enforcement state, realms, force-set a realm password |
| `auth -O` | [onboarding output](onboarding-output.md): `<realm>=set\|unset\|locked` |
| `passwd` | set the admin password (prompts twice) |

### System & power

| Command | |
|---|---|
| `reboot` | flush pending settings, then restart the device |
| `backup` | reboot into safe mode and stream the state store out |
| `restore` | reboot into safe mode to take a backup archive back in |
| `reset factory [flash\|sd\|both]` | wipe user state and reboot; default target flash |
| `sleep <seconds>` | block the session for N seconds |
| `its` | ITS connection + stream-pool snapshot |

`reboot` calls `storageSave()` before `esp_restart()` so no setting is lost.

`backup`, `restore` and `reset factory` all do the same thing: persist the
matching `s.sys.*` flag and reboot into a [safe-mode](safe-mode.md) boot, which
performs the operation with nothing else touching the state store and reboots
again. `reset factory` wipes rather than transfers — it overwrites the flash
region above the firmware with random bytes, about a minute per 12 MB, and the
device comes back on its own access point. Its target is explicit, so booting
from SD no longer refuses the command.

These three set the flag and restart **inline**, on the CLI's own task. Writing
the key by hand (`set s.sys.restore=1`) reaches the same place by a different
road: a watcher on the cron task notices and reboots for you. That watcher is
there for writers that cannot restart the device themselves — the browser, an
rnsh session, a `/state/boot` line — and the commands above deliberately do not
depend on it. `its` prints `itsStatus`.

### Scripting & aliases

| Command | |
|---|---|
| `help [<cmd>]` | list all commands, or show one command's fuller help |
| `alias [<name> <cmd>]` | define / list aliases |
| `unalias <name>` | remove an alias |
| `run <file>` | run a CLI script file (one command per line) |
| `exit` | end this CLI session |

`help` with no argument prints one line per registered command (the per-command
`help` one-liners, in alphabetical order); `help <cmd>` prints that command's
`-h` help. `alias` with no value lists or shows aliases; `alias <name> <cmd>`
defines one. An alias name may not contain `.`. Aliases persist as storage keys
(`s.cli.aliases.<name>`) and expand once at the start of a line — the alias value
replaces the first word, with the rest of the line appended as arguments; an
alias is **not** re-expanded recursively.

## Public surface

Declared in [`include/cli.h`](../esp-idf/include/cli.h):
`cliRegisterCmd`, `cliWantsHelp`, `cliVerbIs`, `cliPrintf`/`cliWrite`/`cliWantsColor`,
`cliReadLine`/`cliReadRaw`/`cliTermSize` (interactive input + terminal geometry,
used by the ssh client for password prompts and pty sizing),
`cliGetCwd`/`cliSetCwd`/`cliCdToStartDir`/`cliResolveFsPath`,
`cliProcess`/`cliRunFile`, the `cli_mode_t`/`cli_color_t`/`cli_echo_t` enums,
the `cli_connect_t` connect payload, and the `CLI_PORT_TCP` (8081) /
`CLI_PORT_DC` (1) port constants. The CLI starts itself at boot — consumers
never call `cliInit`.

The same header carries the serial-port surface:
`serialPortClaim`/`serialPortRelease` and the `serial_handler_connect_t` connect
payload ([usb-console.md](usb-console.md)), plus `consoleWriteRaw`,
`consoleFlush` and `cliSerialResumeLog` — the console primitives a transport
switch needs (write past both the CLI session and the log, push the hardware TX
FIFO, and hand the serial console back to the live log the way a trailing `;`
does).

## Owned storage keys

| Key | Default | Meaning |
|---|---|---|
| `s.cli.version` | `2` | Schema version; gates the one-time default seed. |
| `s.cli.start_dir` | `/` | Directory a new session / bare `cd` starts in. |
| `s.cli.aliases.<name>` | — | One stored alias per key; value is the command it expands to. |

See [cli-internals.md](cli-internals.md) for the registry and dispatch, the line
editor, the CLI/serial task model, and maintainer pitfalls.
