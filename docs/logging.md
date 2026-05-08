# Logging

`log.cpp/h` — log task (ITS server, send-only + bidirectional inbound), ESP-IDF `vprintf` hook, log-level routing, optional log file on SD.

## Usage

- Macros: `err()`, `warn()`, `info()`, `dbg()`, `verb()` — route through ESP-IDF logging with task name as TAG. Mapped to `ESP_LOGE/W/I/D/V`.
- Log messages should NOT include task name prefix — the log task adds `[taskname]` automatically.
- Exception: `info()` from NTP shows `[network]` tag since it runs on the network task. Code on truly unregistered tasks should prefix manually.
- CLI responses route through `cliOut` function pointer — set to `itsCliWrite` (sends to active ITS handle) or `cronCliWrite` (routes through `info()`).

## Levels

`error`, `warn`, `info`, `debug`, `verbose`, `espdebug` (also enables ESP-IDF internal logging).

`espdebug` calls `esp_log_level_set("*", ESP_LOG_DEBUG)`; all other levels suppress ESP logging.

## Timestamps

`log timestamp` / `log notimestamp` CLI commands (or `set s.log.timestamp=1`). Format: `Mar 27 16:23:15.342` (wall clock via `fmtWallClock()`, ms precision from `gettimeofday`). Displayed at start of line before log level. Grey (`\033[0;90m`) on ANSI terminals.

## Transport — Log/CLI/Serial via ITS

Log task is ITS server (**bidirectional** — accepts inbound lines too, see below), CLI task is ITS server (bidirectional), serial task is ITS client for CLI only.

- Serial console gets log output via `logVprintf`'s direct `fwrite(stdout)` (no ITS connection from serial to log).
- Plain TCP clients go through network (stream-mode ports, `nc`-compatible).
- Browser clients go through `webrtc_task` on the packet-mode `LOG_PORT_DC` / `CLI_PORT_DC` ports. CLI/log tasks do no WS upgrade — the browser side is DC-only.
- Per-port buffers: `toSize=fromSize=2048` (PSRAM cost ~4 KB per connection — downstream layers absorb bursts).

## Bidirectional log inbound

Consumers connected to `log:1` (browser DC) or `LOG_PORT_TCP` (`nc` / external scripts) may also send lines TO the log task. `logSlotDrainInbound()` reads each slot non-blocking each main-loop pass, splits on `\n`, and `logInboundLineOut()` fans each complete line out to file + every OTHER consumer + serial stdout — never echoed back to the source.

Inbound lines bypass `logReformat` entirely (no `<ts>` / `[task]` / level char prepended) so the source can stamp its own. ANSI consumers either get the line as-is when it already has escapes, or `colorizePreformatted()` re-applies grey-on-timestamp + level-color around a recognized level char. Plain consumers + log file get an ANSI-stripped copy.

`lineLevel()` / `findLevelCharPos()` use a relaxed heuristic: a single E/W/I/D/V surrounded by spaces (or at start of line), so both the device's native `<ts> L [task] msg` and free-form `<ts> L Browser: msg` are recognized.

## CLI output routing

`itsCliWrite` is a single `itsSend` loop for both modes — in stream (TCP/serial) it partial-sends bytes; in packet (DC) it sends one packet per call. LINE mode clients get output only (no echo). Cron **injects** commands into the CLI task with `info("cli: …")` before each `cliProcess` (same prefix as `cliRunFile`). Cron **stdout** from commands still uses `cronCliWrite` → `info("cron: …")`.

Line editor (`cli_edit` struct): per-handle in CLI task. Backspace, left/right arrows, insert at cursor, history, tab completion. ANSI coloring for `CLI_ANSI` mode clients.

## Browser path

`webrtc_task` forwards DCs with label `log:1` / `cli:1` / `storage:1` directly to the respective tasks via `itsConnect`. `web.cpp` no longer receives `web_path_msg_t` aux for these paths.

## Log file

Log task writes ANSI-stripped plain text to SD card file. Config: `s.log.file.path` (empty=off), `s.log.file.level` (default "verbose" = no filtering), `s.log.file.interval` (flush/fsync coalescing, default 5s). Log task subscribes to path/level changes, closes old file and opens new one live. Direct `FILE*` on DRAM stack (SD card paths don't need the fs worker). Writes "log file opened"/"log file closed" markers directly to the file.

## CLI

- `log [tag] [level]` — set log level globally or per tag.
- `log timestamp` / `log notimestamp` — toggle timestamps.
- `logfile [level] [path|off]` — starts/stops log file. No args = today's dated file in `s.log.dir`. Level filters what's written (e.g. `logfile info` = only E/W/I). `logfile off` stops. Relative paths resolved against `s.log.dir`. Creates log directory if needed.
- `logrotate [days]` — rotates to today's dated log file. With days arg, deletes `YYYYMMDD.log` files older than N days from `s.log.dir`. Only works when current log file is in date format.

## Helpers

`safeStrncpy(dst, src, n)` (`compat.h`): replaces `strncpy` project-wide. Always NUL-terminates, logs `ESP_LOGE` on truncation. Only exceptions: WiFi config byte arrays in `net.cpp` (ESP-IDF `uint8_t[]`, not C strings).
