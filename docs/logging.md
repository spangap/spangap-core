# Logging

The log task is spangap's logging hub: an ITS server that fans every log line out
to the serial console, the browser, plain-TCP `nc` clients, and an optional file
on SD. Source [`src/log.cpp`](../esp-idf/src/log.cpp), header
[`log.h`](../esp-idf/include/log.h). It comes up as part of the platform
(`logInit`) — consumers never start it.

Logging is built on ESP-IDF's `ESP_LOGx` and its `vprintf` hook: spangap installs
its own `vprintf` so it can capture, reformat, color, and route what the standard
macros emit. Code uses the short macros below; the task does the rest.

Maintainer reference — the task's wiring, the ring buffer, the serial console
state machine, and pitfalls — is in [logging-internals.md](logging-internals.md).

## Writing log lines

| Macro | Level | Maps to |
|-------|-------|---------|
| `err(fmt, …)` | error | `ESP_LOGE` |
| `warn(fmt, …)` | warn | `ESP_LOGW` |
| `info(fmt, …)` | info | `ESP_LOGI` |
| `dbg(fmt, …)` | debug | `ESP_LOGD` |
| `verb(fmt, …)` | verbose | `ESP_LOGV` |

Each macro uses the **calling task's name** as the ESP-IDF tag, and the log task
renders that as a `[taskname]` prefix on the line. **Code must not prefix the task
name itself** — it is added automatically. A line emitted on a task whose name is
not the desired tag (e.g. NTP code running on the `network` task) shows that
task's name; truly unregistered contexts that want a tag prefix it manually.

## Levels

`none`, `error`, `warn`, `info`, `debug`, `verbose` — set by full word or first
letter (`n`/`e`/`w`/`i`/`d`/`v`). The level applies globally via
`esp_log_level_set("*", …)`, so at `debug` or `verbose` ESP-IDF's own internal
component logs surface too. Per-tag overrides are independent (see storage keys).

## Timestamps

`log timestamp` / `log notimestamp` (or `set s.log.timestamp=1`) toggle a
wall-clock stamp at the start of each line: `Mon DD HH:MM:SS.mmm`, millisecond
precision from `gettimeofday`, rendered dark-grey on ANSI terminals.

## Ports

The log task is an ITS server on two ports:

- `LOG_PORT_TCP = 8080` — stream mode, plain bytes for raw `nc` access (over
  spangap-net's TCP).
- `LOG_PORT_DC = 1` — packet mode, one log line per DataChannel message,
  addressed from the browser as `log:1`.

Both ports are **bidirectional**: a connected consumer may also send lines *to*
the log, which are fanned out to the file and to every *other* consumer (never
echoed back). See [logging-internals.md](logging-internals.md).

## Storage keys

Logging owns the `s.log.*` tree. Defaults are installed by the module at boot:

| Key | Default | Meaning |
|-----|---------|---------|
| `s.log.level` | `info` | Global log level. |
| `s.log.timestamp` | `1` | Show the wall-clock timestamp on each line. |
| `s.log.tag.<tag>` | (unset) | Per-tag level override; value `-` means inherit the global level. |
| `s.log.dir` | `/sdcard/log` | Directory for dated log files and where relative `logfile` paths resolve. |
| `s.log.file.name` | `""` | Active log-file path (empty = file logging off). Relative names resolve against `s.log.dir`. |
| `s.log.file.level` | `verbose` | Minimum level written to the file (`verbose` = no filtering). |
| `s.log.file.interval` | `5` | Flush/fsync coalescing window, seconds. |
| `s.log.file.paste` | `48` | Scrollback (KiB) replayed from the log file to a newly connected `log:1` consumer (browser / on-device viewer). A DCEP `{"backlog":N}` connect overrides it per-client; `0` disables replay. |
| `s.log.colors.error` | `0;31` | ANSI color (red) for error lines. |
| `s.log.colors.warn` | `0;33` | ANSI color (yellow) for warn lines. |
| `s.log.colors.info` | `0;32` | ANSI color (green) for info lines. |
| `s.log.colors.debug` | `0;37` | ANSI color (light grey) for debug lines. |
| `s.log.colors.verbose` | `0;90` | ANSI color (dark grey) for verbose lines. |
| `s.log.colors.timestamp` | `0;90` | ANSI color (dark grey) for the timestamp. |

The log task subscribes to `s.log.file.*` and `s.log.colors.*` changes and
applies them live (it closes the old file and opens the new one on a path change).

## CLI

```
log [tag] [level]          show or set the log level — globally, or for one tag
log timestamp              turn the wall-clock timestamp on
log notimestamp            turn it off
logfile [level] [path|off] start/stop the SD log file
logrotate [days]           rotate to today's dated file; prune old ones
```

- `log` with no argument prints the current global level. `log <level>` sets the
  global level. `log <tag>` prints that tag's level. `log <tag> <level>` sets a
  per-tag override (`-` to inherit).
- `logfile` with no argument starts today's dated file in `s.log.dir`. A `level`
  argument filters what is written (e.g. `logfile info` = only E/W/I); a `path`
  argument sets an explicit file (relative paths resolve against `s.log.dir`, and
  the directory is created if needed). `logfile off` stops file logging.
- `logrotate` switches to today's dated file. With a `days` argument it also
  deletes `YYYYMMDD.log` files older than N days from `s.log.dir` (a default cron
  entry runs `logrotate 7` daily). Only operates when the current file is in the
  dated format.

Run any of these on-device with `spangap cli "<command>"`.

## API

Beyond the macros, [`log.h`](../esp-idf/include/log.h) exposes:

| Function | Purpose |
|----------|---------|
| `logIsDebug()` | True iff the global level is debug-or-finer — guard expensive `dbg()`-only work. |
| `logIsDebug(tag)` | Same, resolving the per-tag override first. |
| `logApplyLevels()` | Re-read global + per-tag levels from storage and apply (boot + on change). |
| `logSetGlobal(level)` | Set the global level (writes storage + applies). |
| `logSetTag(tag, level)` | Set a per-tag override (`-` = inherit). |
| `cfd(fd)` | Returns `"{fd} "` when at debug level, `""` otherwise — prefix per-connection messages with a handle. |
</content>
