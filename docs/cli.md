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
  log. See [logging](logging.md) for the log/CLI mode switch.
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
| `set <key>=<value>` | set a config key (or `set <key> <value>` — space also separates) |
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
| `usb` | USB-serial peer presence |
| `bat` | battery voltage + percent |

### Auth — see [auth.md](auth.md)

| Command | |
|---|---|
| `auth [...]` | enforcement state, realms, force-set a realm password |
| `passwd` | set the admin password (prompts twice) |

### System & power

| Command | |
|---|---|
| `reboot` | flush pending settings, then restart the device |
| `reset factory` | format the flash `state` partition and reboot |
| `sleep <seconds>` | block the session for N seconds |
| `its` | ITS connection + stream-pool snapshot |

`reboot` calls `storageSave()` before `esp_restart()` so no setting is lost.
`reset factory` wipes all user state in device flash and reboots (flash
repopulates defaults on the next boot); it is **refused when the device booted
from an SD card** — it prints how to wipe SD state instead (`format sd; mkdir
/sdcard/state; reboot`), since it is only meant for flash state (see
[fs.md](fs.md)). `its` prints `itsStatus`.

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
`cliRegisterCmd`, `cliWantsHelp`, `cliPrintf`/`cliWrite`/`cliWantsColor`,
`cliReadLine`/`cliReadRaw`/`cliTermSize` (interactive input + terminal geometry,
used by the ssh client for password prompts and pty sizing),
`cliGetCwd`/`cliSetCwd`/`cliCdToStartDir`/`cliResolveFsPath`,
`cliProcess`/`cliRunFile`, the `cli_mode_t`/`cli_color_t`/`cli_echo_t` enums,
the `cli_connect_t` connect payload, and the `CLI_PORT_TCP` (8081) /
`CLI_PORT_DC` (1) port constants. The CLI starts itself at boot — consumers
never call `cliInit`.

## Owned storage keys

| Key | Default | Meaning |
|---|---|---|
| `s.cli.version` | `2` | Schema version; gates the one-time default seed. |
| `s.cli.start_dir` | `/` | Directory a new session / bare `cd` starts in. |
| `s.cli.aliases.<name>` | — | One stored alias per key; value is the command it expands to. |

See [cli-internals.md](cli-internals.md) for the registry and dispatch, the line
editor, the CLI/serial task model, and maintainer pitfalls.
