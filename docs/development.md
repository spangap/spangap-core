# Development environment

How to build, flash, drive, and debug a spangap device for day-to-day
work — covering the common single-machine case and the optional pattern
where the build machine and the flash machine are different boxes.

For the platform conventions, recipes, and module map, see
[`../CLAUDE.md`](../CLAUDE.md). For local ESP-IDF workarounds, see
[idf-tweaks.md](idf-tweaks.md).

## `~/bin` symlinks — short command lines

Three tools are conventionally symlinked into `~/bin` (on `PATH`) so
every command in this doc — and in chat transcripts — starts with the
verb, not a long relative path:

| Command | Resolves to | Notes |
| --- | --- | --- |
| `idf.py` | `~/bin/idf.py` (venv-activating wrapper) | see Toolchain below |
| `flasher` | `spangap/scripts/flasher` | flash + monitor, daemon mode |
| `spangap-cli` | `spangap/scripts/spangap-cli` | TCP CLI client |

On a fresh machine, with `<workspace>` standing in for the directory
that holds your `spangap/` checkout:

```bash
ln -sf <workspace>/spangap/scripts/flasher     ~/bin/flasher
ln -sf <workspace>/spangap/scripts/spangap-cli ~/bin/spangap-cli
# idf.py wrapper is installed by EIM; see Toolchain
```

Everything below assumes these are in place. Don't write out the
`../spangap/scripts/...` paths in commands — use the bare names.

## Toolchain

ESP-IDF v5.5.4, installed via EIM at `~/.espressif/v5.5.4/esp-idf`
(Python venv at `~/.espressif/tools/python/v5.5.4/venv`).

The `~/bin/idf.py` wrapper auto-activates the venv on first invocation
so `idf.py` works from any shell or script. Its `is_sourced()` probe in
`~/.espressif/tools/activate_idf_v5.5.4.sh` checks `${0##*/}` against
the literal names `dash|-dash|bash|-bash|ksh|-ksh|sh|-sh`. When the
wrapper sources the script, `$0` is `idf.py`, so the probe decides
"not sourced", activate exits 1, and `idf.py` fails **silently (exit 1,
no output)**. If you ever see `idf.py` exit non-zero with an empty log,
this is why.

Workaround for scripts (and agents) that must work despite the bug —
activate explicitly and exec the real `idf.py`:

```sh
sh -c '. ~/.espressif/tools/activate_idf_v5.5.4.sh >/dev/null 2>&1;
       exec "$IDF_PYTHON_ENV_PATH/bin/python" "$IDF_PATH/tools/idf.py" "$@"' _ "$@"
```

Run this **from the consumer project directory** (the one with
`idf_ext.py`) — otherwise `--spangap` is rejected with
`No such option: --spangap`.

## Build

From a consumer's project directory (seccam, reticulous, or any other
spangap consumer):

```bash
idf.py build                                        # registry-resolved spangap-core
idf.py --spangap build                              # sibling-checkout dev mode
idf.py --spangap -p <serial> flash monitor          # <serial> = /dev/ttyACM0, /dev/tty.usbmodem*, COM3, ...
```

`--spangap` is implemented in each consumer's
[`idf_ext.py`](../../seccam/idf_ext.py); it injects a transient `path:`
for `spangap/spangap-core` into `main/idf_component.yml` for the
duration of the run and restores the manifest on exit. If a `--spangap`
run is killed before cleanup, the next invocation refuses to start until
you `mv main/.idf_component.yml.spangap-backup main/idf_component.yml`.
The committed manifest is never edited.

The build writes nothing special; capture it with shell redirection
(`idf.py build > /tmp/build.log 2>&1`) if you want to keep it.

## Flash + monitor

### Single machine, normal case

```bash
idf.py -p /dev/tty.usbmodemNNNN flash monitor       # exit monitor: Ctrl+]
```

Or, when you've already built and just want a fast reflash without
rerunning cmake:

```bash
flasher /dev/tty.usbmodemNNNN .
```

`flasher` reads `build/flasher_args.json`, runs esptool directly, then
drops you into `idf.py monitor`. On flash failure the esptool log is
renamed to `<project>/build/flasher.log` and `flasher` exits non-zero.

### Split VM + host

When the board is attached to a host machine but the developer is
editing/building in a VM that mounts the project tree over a shared
filesystem, the host runs the flasher in **daemon mode** and the VM
**signals** it:

```bash
# On the host, once per session:
flasher -d /dev/cu.usbmodemNNNN /path/to/project
#   flasher: daemon, watching .../build/flashme (poll 1s)
```

```bash
# In the VM, after each rebuild:
flasher host /path/to/project
#   flasher: signaled .../build/flashme
```

`flasher host` touches `<project>/build/flashme`; the daemon polls that
path at 1 Hz, deletes it, re-flashes, and drops into a fresh
`idf.py monitor`. If a new `flashme` appears while monitor is running,
a watcher kills the monitor so the next iteration cycles.

The monitor's output is tee'd to **both the host terminal and
`<project>/build/flasher.log`**, ANSI-stripped. Because `build/` is on
the shared volume, the VM (and any tooling running there) can
`tail -F build/flasher.log` to see what the device prints in real time.
This is the canonical way for tooling on the build side to read live
device output — there is no separate serial path from the VM.

> Device firmware timestamps are **UTC**; a CET/CEST build host is
> 1–2 h ahead of wall clock. A `flasher.log` whose newest line looks
> "1–2 h old" is current, not stale.

### Permissions across the VM/host boundary

The build dir is shared between two uids, so each side must be able to
delete and overwrite the other's files. Two settings keep this clean:

- Build with `umask 000` from whichever side has the tighter default —
  otherwise files come out 644 / dirs 755 and the other side can't
  replace them.
  ```bash
  umask 000 && idf.py --spangap build
  ```
- After a one-off mistake, `chmod -R a+rwX <project>/build` clears it up.

### macOS-specific flasher caveats

- macOS ships **bash 3.2**, which lacks `mapfile`. `flasher` uses a
  portable `while read -r` loop instead.
- macOS `nc` half-closes on stdin EOF in a way that lets the server
  tear down before the response is read — the `(echo X; sleep 1) | nc`
  idiom from Linux drops output here. Use `spangap-cli` (full duplex
  via Python) for scripted access. Interactive `nc` is fine.

### Logs

| Path | When | Notes |
| --- | --- | --- |
| `<project>/build/flasher.log` | flash success → monitor output; flash failure → esptool stdout/stderr | ANSI-stripped, on shared storage, tee'd to terminal; `tail -F` it from the VM |
| `/tmp/esptool.log` | during flash | tee'd from esptool; renamed to `flasher.log` only on failure |
| `<project>/build/<APP>.elf` | post-build | needed for `idf.py monitor` panic-frame symbolization (see Crash investigation) |

## Driving the device — remote CLI

Every spangap device exposes its CLI on serial, on the browser's
`cli:1` DataChannel, and — when `s.net.cli_port` is non-zero — on plain
TCP. Default is 0 (disabled). Enable per-device once, persist:

```text
$ set s.net.cli_port=2323
$ save
```

The setting takes effect immediately via the `s.net.*` storage
subscriber in [net.cpp:852](../spangap-core/src/net.cpp#L852), which
re-runs `epOpenAll()`. The log line `opening port 2323 (cli_port)`
confirms.

> The CLI has **no authentication**. Anything that reaches the TCP port
> can run anything `help` lists, including `reset factory`. Only enable
> on trusted networks. A `reset factory` formats the flash `state`
> partition and re-copies from `/fixed/factory_state/` on next boot
> (refused when booted from SD — see [storage.md](storage.md)); persist anything you need to survive
> that (e.g. `s.net.cli_port`) into the first-boot overlay under
> `/fixed/additional_state/` first, or you lock yourself out.

### `spangap-cli` — talk to the TCP CLI

[`scripts/spangap-cli`](../scripts/spangap-cli) is a single-file Python
tool. Two modes:

```bash
spangap-cli                                         # interactive (Ctrl-D to exit)
spangap-cli show s.net.cli_port                     # one-shot: args sent as one line
spangap-cli date                                    # any CLI command works
```

- `SPANGAP_HOST` — hostname or IP (**default `reticulous.local`**).
  Only set it for a different target; don't spell out the default.
- `SPANGAP_PORT` — TCP CLI port (default 2323).

One-shot mode reads output **until the device sends the CLI prompt
(ending in `"$ "` — the full prompt is `"<hostname> $ "`, e.g.
`reticulous $ `)**, not on a fixed timeout, so long-running commands
(`date wait`, `version check`, etc.) complete correctly. The prompt is
emitted by [cli.cpp](../spangap-core/src/cli.cpp) in both CLI_ANSI and
CLI_LINE modes on connect and after every processed line.

Protocol notes:

- **Line-oriented**: command + `\n`, read response. No echo, no escape
  sequences. Empty `\n` returns just a prompt.
- **`set` takes `key=value`** (not space-separated), and `set` /
  `unset` / `save` are **silent on success**.
- **Connection refused / no route**: either `s.net.cli_port` is 0, the
  device hasn't reached `NET_EV_UPSTREAM_UP` yet (the subscriber is
  gated on `netIsUp()`), or the device dropped to its fallback AP
  (`192.168.1.1`, SSID `reticulous`) because it couldn't join a known
  WiFi network — join that AP and reconfigure via the web UI.

## On-device debug surface

The CLI lives on the `cli:1` DataChannel (browser TerminalWindow), the
TCP port (`spangap-cli`), and serial. `help` lists every registered
command. Everything here is built into firmware that consumes
spangap-core; consumer apps inherit it for free. The high-value
diagnostic commands:

### `top` — tasks, CPU%, heap

[pm.cpp](../spangap-core/src/pm.cpp). Two snapshots 2 s apart of
`uxTaskGetSystemState`, merged with per-task heap totals from
`heap_caps_get_per_task_info()`. Columns:

```
Task   Core Pri Stack  CPU%  DRAM     PSRAM    Dblk Pblk
```

- `Stack` is `usStackHighWaterMark` — minimum free stack ever observed.
- `CPU%` is the delta share over the 2 s sample.
- `DRAM` / `PSRAM` only populated when `CONFIG_HEAP_TASK_TRACKING=y`.
  Pre-tracking and deleted-task allocation totals appear as separate rows.
- Idle tasks (`IDLE0` / `IDLE1`) are filtered from the live list but
  their delta drives the per-core busy % in the header.

### `pm` — power-mode time

[pm.cpp](../spangap-core/src/pm.cpp). Three sections plus two lock
tables. See [power-management.md](power-management.md) for the full
breakdown.

- "Now": current CPU/APB MHz (snapshot — always 240 MHz because the CLI
  runs on core 1 with `rtos1` lock).
- "Since last 'pm' command": per-mode time delta (deep sleep / light
  sleep / 80 MHz / 240 MHz, summing to 100%). RTC RAM persistence covers
  deep-sleep wakes.
- "Since boot": grand totals = `rtcAccumModeUs[i] + currentSession`.
- `esp_pm_dump_locks()` → ESP locks table, then
  `pmDumpDeepSleepLocks()` → our `NO_DEEP_SLEEP` table (cron, net,
  datewait, plus any caller-created locks).

### `usb up|down`, `net up|down|down!`, `pm wifi [none|min|max]`

`usb down` releases the USB lock and disables the D+ pullup so USB
Serial JTAG SOF traffic stops waking the CPU. `usb up` resets the
peripheral and re-acquires the lock. Persists across deep sleep via
`rtcUsbDisabled`.

`net up` / `net down` / `net down!` set `rtcWantUp` and trigger graceful
(30 s idle wait) or immediate teardown. Useful when reproducing
power-floor measurements or testing reconnection paths.

### `log` family

- `log [tag] [level]` — set log level globally or per tag. `level` ∈
  `error`, `warn`, `info`, `debug`, `verbose`, `espdebug` (the last also
  enables ESP-IDF internals).
- `log timestamp` / `log notimestamp` — toggle wall-clock timestamps.
- `logfile [level] [path|off]` — open/close an SD-card log file. No args
  = today's `YYYYMMDD.log` in `s.log.dir`. Level filters what's written
  (`logfile info` = E/W/I only). `off` stops. **Note:** on boards
  without a mounted SD card (e.g. T-Deck Plus, SD unused for v1)
  `logfile on` reports a path but writes silently fail — use the live
  `flasher.log` for device output instead.
- `logrotate [days]` — rotate to today's dated file, delete older than N.

### `show <prefix>`, `set`, `unset`, `save`

Storage probes: `show s.cam` lists every key starting with `s.cam`.
`set` / `unset` / `save` are silent on success. `save` forces an
immediate flush instead of waiting for the 60 s coalesce timer.

> Storage path segments are capped at 95 chars (buffers in
> [storage.cpp](../spangap-core/src/storage.cpp)); the CLI key buffer is
> 128. A write whose dotted key has an over-long segment now logs
> `storage: segment too long in key '…'` instead of silently no-op'ing
> — if you see that warn, a buffer needs bumping.

### `cert`, `wg`, `upnp`, `duckdns`, `web`

Module status reporters — each prints its current state, public
key/endpoint/IP/etc., or the active port-mapping table. `web` lists
active file mappings + URL prefixes registered by other tasks. Consumer
apps add their own status reporters in the same shape (rnsd adds
`rnstatus`, `rnpath`, `rnprobe`, `rnsd link`/`links`, etc.).

## Crash investigation

### Decoded panic backtrace from the device

The panic handler emits raw addresses (e.g.
`0x40381093:0x3fcc94f0 …`). `<APP>` is the consumer project name;
`build/<APP>.elf` is the post-build artifact. Two ways to symbolize:

1. **`idf.py monitor`** runs the live serial through
   `esp_idf_panic_decoder.PcAddressDecoder`, which calls `addr2line`
   against `build/<APP>.elf` and prints
   `--- 0xADDR: function at file:line` after each Backtrace line.
   Requires:
   - elf files present under `build/` and discoverable by monitor (it
     `rglob`s `*.elf` from `args.build_dir`); if monitor logs
     `ELF file '...' does not exist` it falls back to
     `SerialHandlerNoElf` and silently skips decoding. Reproduce with
     `python -c "from pathlib import Path; print(list(Path('build').rglob('*.elf')))"`
     from the same cwd.
   - `xtensa-esp32s3-elf-addr2line` on `PATH` (added by
     `. <IDF>/export.sh`).
   - `esp-idf-monitor` ≥ 1.5; current 5.5.4 ships 1.9.0.
   - `ESP_MONITOR_DECODE` env var unset (or anything other than `'0'`).

2. **Manual decode** when the panic is captured in a paste, screenshot,
   or log file:
   ```
   ~/.espressif/tools/xtensa-esp-elf/<ver>/xtensa-esp-elf/bin/xtensa-esp32s3-elf-addr2line \
       -pfiaC -e build/<APP>.elf 0x40381093 0x403807cb 0x4204867d 0x42018c7e
   ```
   Or programmatically (mirrors what monitor does internally):
   ```python
   from esp_idf_panic_decoder import PcAddressDecoder
   d = PcAddressDecoder('xtensa-esp32s3-elf-', ['build/<APP>.elf'])
   print(d.translate_addresses("Backtrace: 0x40381093:0x3fcc94f0 0x403807cb:0x3fcc9520"))
   ```

The `idf.py monitor` panic-frame decoder (`--decode-panic=backtrace`)
is a separate, more advanced feature — only auto-enabled for
**RISC-V** targets in `idf.py`
([serial_ext.py:157](file:///Users/rop/.espressif/v5.5.4/esp-idf/tools/idf_py_actions/serial_ext.py#L157)).
Xtensa targets rely on the inline addr2line decoder (above) plus the
device's own backtrace.

## Auto-triggered diagnostics

### `heapDump()` on demand

`heapDump(reason)` ([pm.cpp](../spangap-core/src/pm.cpp)) prints the
same per-task layout as `top` plus a header line, written to the log
stream. Useful from any consumer task that wants to forensically
capture allocation state at a failure point — e.g.
`heapDump("record write failed")` from an SD-write error handler.
Reveals which task's allocations have crowded out a working buffer.

### `info("cli: …")` from cron and `cliRunFile`

Cron and `/state/boot` / `/state/net_up` script execution log each
command via `info("cli: <line>")` *before* invoking it (on the caller's
task context — `[net_up]`, `[cron]`, etc.). Every script step is
therefore visible in the log even when no one is connected to the CLI.
Failures show up as the next `warn`/`err` line attributed to the same
task.

### `printf` from PSRAM-stack tasks

Forbidden — SPI flash disables the PSRAM cache during `printf`'s
internal `_lock` paths and the call stack lives in PSRAM. Use
`info()`/`warn()`/`err()` macros, which route through the log task's
DRAM ring buffer. The serial console echo is handled by `logVprintf`
directly, not by the offending task.

## Local IDF tweaks

spangap-core does not patch the ESP-IDF tree. Where IDF behavior needs
to change, the platform either:

- **Wraps it at link time** with `-Wl,--wrap=<symbol>` + a stub in
  `src/`, plus a CMake-time guard that fails the build if the wrapped
  symbol's call sites disappear. See [idf-tweaks.md](idf-tweaks.md) for
  the index of these and the heap-tracking case study.
- **Documents the required `sdkconfig` setting** for the consumer to
  set in their own `sdkconfig.defaults`.

[idf-tweaks.md](idf-tweaks.md) is the place to look — and to extend —
when an ESP-IDF upgrade reintroduces a regression.
