# Logging — internals

Maintainer reference for the log task. The [operator guide](logging.md) covers the
macros, levels, and CLI; this document is for changing the implementation. Source
[`src/log.cpp`](../esp-idf/src/log.cpp), header [`log.h`](../esp-idf/include/log.h).

## 1. What the module adds

- An ESP-IDF `vprintf` hook (`logVprintf`, installed with `esp_log_set_vprintf`)
  that captures everything the `ESP_LOGx` macros emit.
- A static DRAM ring buffer (`logRing`, `LOG_RING_SIZE = 8192`) holding captured
  bytes until the log task drains and fans them out.
- A **log task**: an ITS server on `LOG_PORT_TCP` (8080, stream) and `LOG_PORT_DC`
  (1, packet), both bidirectional, each port `toSize = fromSize = 2048`. Inbox
  guard `itsServerInit(1600)`.
- Reformatting + ANSI colorization (`logReformat`, `colorizePreformatted`).
- An optional SD log file (an fs-worker stream handle) with level filtering and
  time-gated flush coalescing.
- Scrollback paste-back (`logPasteBack`): the tail of the log file replayed to a
  newly connected `log:1` consumer, deferred out of the connect handshake.
- Level management (`logApplyLevels` / `logSetGlobal` / `logSetTag`) over the
  `s.log.*` storage tree, with live subscriptions for `s.log.file.*` and
  `s.log.colors.*`.
- The `log`, `logfile`, and `logrotate` CLI commands (`logRegisterCmds`).
- Default install (`logInstallDefaults`, versioned by `s.log.version`) seeding the
  `s.log` tree and a daily `logrotate 7` cron entry.

The CLI output-routing helpers (`itsCliWrite`, `cronCliWrite`, the `cliOut`
function pointer) and the serial console live in the CLI/serial code that pairs
with this module; their behavior is described in §4–§5 because the routing is one
system.

## 2. Capture and fan-out

`logVprintf` runs on whatever task called the `ESP_LOGx` macro. It formats into a
PSRAM-backed `std::string` (so it never eats DRAM stack), writes the bytes into
`logRing` under a spinlock that serializes concurrent writers, and — while the
serial console is in log mode — mirrors the line straight to `stdout` with a
direct `fwrite`. The static DRAM ring means logging never depends on the heap
being intact, so a line about heap corruption still reaches the wire.

The log task drains the ring each loop pass, runs `logReformat` (timestamp +
level char + `[task]` prefix, color per `s.log.colors.*` for ANSI consumers), and
fans each line out to:

- **serial** — via `logVprintf`'s direct `fwrite(stdout)` (there is no ITS
  connection from serial to the log task);
- **plain-TCP** consumers on `LOG_PORT_TCP` (stream mode, `nc`-compatible, over
  spangap-net);
- **browser** consumers on `LOG_PORT_DC` (packet mode, one line per DataChannel
  message; `webrtc_task` forwards the `log:1` DataChannel here directly — the log
  task does no WebSocket upgrade);
- **the log file** if open (ANSI-stripped plain text).

Fan-out sends use `timeout = 0`; the per-port 2048-byte buffers absorb bursts and
a slow consumer drops bytes rather than stalling the log task. A consumer opts
into level coloring by sending a `log_connect_t{LOG_ANSI}` connect payload;
otherwise it gets an ANSI-stripped copy.

## 3. Bidirectional inbound

Both server ports accept inbound lines. A consumer on `log:1` or `LOG_PORT_TCP`
may write lines *to* the log task; `logSlotDrainInbound()` reads each slot
non-blocking every loop pass, splits on `\n`, and `logInboundLineOut()` fans each
complete line out to the file + every **other** consumer + serial — **never echoed
back to the source**.

Inbound lines bypass `logReformat` entirely (no `<ts>` / `[task]` / level char is
prepended), so the source stamps its own. An ANSI consumer gets the line as-is if
it already carries escapes, or `colorizePreformatted()` re-applies grey-on-timestamp
+ level color around a recognized level char; plain consumers and the file get an
ANSI-stripped copy. `lineLevel()` / `findLevelCharPos()` recognize a level with a
relaxed heuristic — a single `E`/`W`/`I`/`D`/`V` bounded by spaces or at line
start — so both the device's native `<ts> L [task] msg` and a free-form
`<ts> L Browser: msg` are colored correctly.

## 4. CLI output routing

CLI command output does not go through the log fan-out; it is written back to the
issuing client over its ITS connection. The `cliOut` function pointer selects the
sink: `itsCliWrite` (a single `itsSend` loop — partial-send in stream mode for
TCP/serial, one packet per call in DataChannel mode) for an interactive client,
or `cronCliWrite` (routes the command's stdout through `info("cron: …")`) for a
cron-injected command. Cron **injects** each command into the CLI task with an
`info("cli: …")` echo before `cliProcess`, the same prefix `cliRunFile` uses.
LINE-mode clients get output only, no echo.

## 5. Serial console — log/CLI mode switch

The serial task is a stateful shuttle between the on-wire console (log output) and
the on-device CLI. While `serialInCli == false` (the default), `logVprintf`
mirrors every line to `stdout`; while `true` it suppresses that mirror so command
output and prompts are not tangled with live log lines. The four user-visible
transitions:

| Trigger | Visible effect | Mechanism |
|---------|----------------|-----------|
| Any key (not bare `\n`/`\r`) in log mode | blank line, `"CLI mode, hit return on prompt to return to log"`, blank line, `<host> $ ` | serial task `itsConnect`s `cli:1`, sets `serialInCli=true`, prints the banner, forwards the keystroke. |
| Empty return at the prompt | blank line, `"Resuming log"`, blank line, log resumes | line editor's empty-enter branch writes the banner over the CLI connection, then `itsDisconnect`s; the serial task sees the disconnect and stays silent. |
| Line ending in `;` | `\rResuming log\r\r`, command runs while logs already flow | line editor writes the overwrite banner, flips `serialInCli=false` *before* `cliProcess` so live log reaches the wire during the command, then sets `cliUsbSerialAutoResumeLog=true` to finalize the disconnect after draining. |
| Ctrl-C (`0x03`) on serial | blank line, `"Press Ctrl-] to exit monitor"`, blank line, log resumes | serial task intercepts `0x03` before forwarding, aborts the CLI line by `itsDisconnect`ing the handle, prints the hint via direct `printf`. No-op if already in log mode. |

TCP/WS CLI clients see none of this — they stay in CLI mode and just re-prompt on
empty enter; Ctrl-C is forwarded to the line editor as an ordinary byte (ignored).
The prompt is `<s.net.hostname> $ `, re-read every prompt so a `set
s.net.hostname=…` shows immediately; a one-shot TCP client keying off the prompt
should match a trailing `"$ "`, not the whole string.

## 6. Log file

The log file is an **ITS stream handle** (`logFile`), opened with
`fs_open_stream(path, "a", 16384, 4096)` — a 16 KB stream buffer that the fs
worker drains once 4 KB accumulate, so the log task never touches flash on its own
stack (its stack is PSRAM). Writes go through `itsSend(logFile, …)`
(`logFileWrite`), ANSI-stripped plain text. `logFileMsg` writes `"log file
opened"` / `"log file closed"` markers into the file itself. `s.log.file.level`
filters what is written per line (`logFileLevelMax`, parsed from the level word);
`s.log.file.interval` (seconds → `logFileIntervalMs`) drives `logFileFlush`, a
time-gated tail flush via `fs_stream_sync` that pushes the last sub-4 KB batch to
the card so a crash/power-loss window is bounded (`interval=0` leaves pure
size-batching). The task subscribes to `s.log.file.name`, `s.log.file.level`, and
`s.log.file.interval` and applies each live; a name change closes the old handle
and opens the new path (creating `s.log.dir` first).

`logFileOpen` refuses an `/sdcard/`-prefixed path when `sdAvailable()` is false —
one `warn`, then file logging stays off — rather than blocking on a doomed mount.

## 7. Scrollback paste-back

When a `log:1` consumer connects, `logDcConnect` defers paste-back **out of the
connect handshake** (a multi-KB read from the slow SD-backed file pushed through
the 2 KB connection buffer would stall the handshake past the client's connect
timeout and leak the server slot). It sets `pastePending`; the task loop runs
`logPasteBack` once the client is connected and draining, before the live fan-out
so history precedes live lines. The tail size is the DCEP `{"backlog":N}` bytes if
given, else `s.log.file.paste` KiB (default 48; `0` disables). The connect JSON
may also carry `{"ansi":0}` (on-device viewers, plain pipes) to receive the tail
and live lines with no colour escapes. The tail is read in one PSRAM buffer,
re-colored per level, and chunked on newline boundaries into ≤1500-byte ITS
sends.

## 8. Other work on the log task loop

The log task's loop also calls `pmPollUsb()` once per pass (the ~1 Hz USB
presence poll, see [power-management-internals.md](power-management-internals.md))
and, when the debug Kconfig is enabled, hosts the heap-corruption hunt:
`CONFIG_SPANGAP_HEAP_INTEGRITY_POLL` runs `heap_caps_check_integrity` every
`CONFIG_SPANGAP_HEAP_INTEGRITY_POLL_MS`, and `CONFIG_SPANGAP_WATCH_ADDR` arms a
hardware store-watchpoint on both cores at startup. Both are off by default and
documented in [memory-internals.md](memory-internals.md).

## 9. Pitfalls

- **PSRAM-stack tasks must not `printf`.** Use the `err()`/`warn()`/`info()` /
  `dbg()`/`verb()` macros; `logVprintf` formats into a PSRAM string, but a raw
  `printf` from a PSRAM-stack task can fault.
- **Code must not prefix the task name.** `logReformat` adds `[task]`; a
  hand-written prefix double-stamps.
- **The ring is static DRAM on purpose** — it must survive heap corruption so the
  crash that mentions it still gets logged. Don't move it to a heap allocation.
- **Inbound lines are not reformatted** — don't add a `[task]`/level prefix to
  them; the source owns its own formatting, and `colorizePreformatted` only
  re-colors what is already there.
- **The level applies globally** via `esp_log_level_set("*", …)`, so raising the
  global level to `debug`/`verbose` also unmutes ESP-IDF's internal component
  logs. Per-tag overrides set with `-` re-inherit the global level on apply.
</content>
