# ITS — Inter-Task Streaming

Socket-style, point-to-point connections between FreeRTOS tasks. Header:
[`its.h`](../esp-idf/include/its.h).

ITS is spangap's only inter-task communication primitive. Every cross-task
signal — HTTP requests, log lines, CLI commands, config-change notifications,
file I/O, mesh packets — flows through ITS, and consumer apps use it the same way
for their own tasks. Tasks open numbered "ports" the way a UNIX server opens TCP
ports; clients connect to a server task by name (or task handle) and port.
Per-task **aux** messages handle the one-shot signals that don't fit a connection
model. ITS is part of the platform: any task that calls an ITS function is
registered on first use, so there is nothing to initialize.

Maintainer reference — the layer's internal tables, framing, and pitfalls — is in
[its-internals.md](its-internals.md).

## Concepts

### Task

Any FreeRTOS task that calls `itsServerInit`, `itsClientInit`, or `itsOnAux` is
registered as an ITS task. Each registered task gets:

- An **inbox** (FreeRTOS Queue) carrying connect / disconnect / aux / forward
  signaling.
- A per-task table of **server ports** (max `ITS_MAX_PORTS = 8`) and **aux ports**
  (also `ITS_MAX_PORTS`).
- A task notification used as the wake mechanism for `itsPoll`.

A task can be a server, a client, or both, simultaneously.

### Port

A 16-bit number identifying an endpoint within a task. Servers explicitly open
ports they accept on; aux callbacks are registered against ports they listen on.
Server ports and aux ports live in **separate namespaces** within a task: the
same number can mean "server port 5" and "aux port 5" without colliding (they're
stored in different tables).

A port is declared once as one of two kinds, and the choice lives in the **port**,
never in a message:

- **Stream** — a continuous byte ring. `itsSend`/`itsRecv` behave like
  `xStreamBufferSend`/`Receive`: partial sends and partial reads, returning the
  bytes actually transferred.
- **Packet** — discrete whole messages. `itsSend` writes one message atomically;
  `itsRecv` returns exactly one message body per call. Used by the browser
  DataChannel ports where one wire message must map to one logical frame.

By convention, port numbers are arbitrary but stable: each module's header
declares its port constants so other tasks use them by name.

### Connection

A point-to-point pipe between a client task and a server task on a specific server
port. Each connection has:

- An **ITS handle** — a global slot index, the same number on both sides.
- Two **stream buffers** (client→server `to`, server→client `from`). Either side
  may be sized 0 (one-way / aux-only servers).
- Two **refs** — `clientRef` and `serverRef`, each chosen by its side at connect
  time (typically an index into that task's own per-connection state). The
  opposite side's ref is what gets passed to the disconnect callback, so neither
  side needs a handle-to-slot mapping.

### Aux message

A one-shot, fire-and-forget message sent to a port on a target task without
setting up a connection. The receiver registers `itsOnAux(port, cb)`; the sender
calls `itsSendAux("taskname", port, …)`. Aux is for things that don't fit a
connection model: subscribe/unsubscribe commands, change notifications, periodic
nudges, register-this-port handshakes. ITS rejects an aux send to a port with no
registered callback, so misrouted messages fail loudly at startup instead of
corrupting silently later.

### Pickup wait

By default `itsSendAux` returns once the message is in the receiver's inbox.
Pass `ITS_WAIT_PICKUP` to instead block until the receiver's `itsPoll` has
actually invoked the callback. Useful when the sender needs the receiver to read
or modify shared memory the message points at before the sender continues.

## How other straddles plug in

ITS is the wiring between every platform task. The core tasks each anchor their
own ports; consumer straddles connect to them by name, and add their own port
constants for their own server tasks.

| Module | Header | Ports |
|--------|--------|-------|
| log | [`log.h`](../esp-idf/include/log.h) | `LOG_PORT_TCP=8080` (stream, `nc`), `LOG_PORT_DC=1` (packet, browser `log:1`) |
| cli | [`cli.h`](../esp-idf/include/cli.h) | `CLI_PORT_TCP=8081` (stream, `nc` + serial), `CLI_PORT_DC=1` (packet, browser `cli:1`) |
| storage | [`storage.h`](../esp-idf/include/storage.h) | `STORAGE_CONFIG_PORT=1` (packet, browser `storage:1`), `STORAGE_CHANGE_PORT=42` (change dispatch on subscribers), `STORAGE_SAVE_PORT=43` (reserved), `STORAGE_OP_PORT=44` (write-op lists → the storage actor) |
| fs | [`fs.cpp`](../esp-idf/src/fs.cpp) | `FS_OP_PORT=1` (POSIX ops aux, on `fs`), `FS_STREAM_PORT=2` (streaming write, on `fs_strm`), `FS_READ_PORT=3` (streaming read, on `fs_strm`), `FS_STREAM_SYNC_PORT=4` (stream-sync aux, on `fs_strm`) |

Networking and web ports are not defined in spangap-core. They live in their
owning straddles and are named here for the full map:

- **spangap-net** (`net.h`): `NET_PORT_REG_PORT=0` (TCP endpoint registration),
  `NET_CMD_PORT=1` (CLI control).
- **spangap-web** (`web.h`): `WEB_HTTP_PORT=80`, `WEB_HTTPS_PORT=443`,
  `WEB_PATH_REG_PORT=0` (URL registration); (`webrtc_task.h`): `WEBRTC_PORT=4433`.

Consumer apps add their own port constants in their own headers — for example a
camera straddle might reserve `CAMERA_CMD_PORT`, `RECORD_CMD_PORT`,
`RTSP_PORT=554`, one per server task.

## A minimal example

A client task connecting to the log task's TCP stream port and reading lines:

```cpp
static void onRecv(int handle, size_t avail) {
    char buf[256];
    size_t n = itsRecv(handle, buf, sizeof(buf), 0);
    /* ... consume n bytes ... */
}

static void taskFn(void*) {
    itsClientInit(/*maxConns=*/2);
    log_connect_t req = { .ansi = LOG_NO_ANSI };
    int h = itsConnect("log", LOG_PORT_TCP, &req, sizeof(req),
                       pdMS_TO_TICKS(500), /*ref*/-1, onRecv, /*onDisc*/nullptr);
    if (h < 0) return;
    for (;;) itsPoll();          /* onRecv fires when log lines arrive */
}
```

Every ITS task's main loop is some variant of: `while (itsPoll(0)) {}` to drain
everything pending, do other work, then `itsPoll(blockTime)` to sleep until ITS
wakes the task (an inbox message, or a connection's buffer crossing its trigger
level). A task with nothing else to do calls `itsPoll()` with no argument and
blocks forever.

## Public surface

All functions are declared in [`its.h`](../esp-idf/include/its.h). The header is
the authoritative reference; this is the map.

| Area | Functions |
|------|-----------|
| Server lifecycle | `itsServerInit`, `itsServerPortOpen`, `itsServerPortClose` |
| Server callbacks | `itsServerOnConnect`, `itsServerOnBusy`, `itsServerOnDisconnect`, `itsServerOnRecv` |
| Server plumbing | `itsInject`, `itsServerForward`, `itsServerForwardByTaskHandle`, `itsServerPort`, `itsServerActive` |
| Client | `itsClientInit`, `itsConnect`, `itsConnectByTaskHandle`, `itsRef` |
| Disconnect | `itsDisconnect` (either side; `-1` closes all of this task's connections) |
| Aux | `itsOnAux`, `itsSendAux`, `itsSendAuxByTaskHandle`, `itsSendAuxOwnedByTaskHandle` |
| Poll | `itsPoll` |
| Data | `itsSend`, `itsRecv`, `itsSendOwned`, `itsRecvRef` |
| Flow control | `itsSetTriggerLevel` (high-water), `itsSetFreeNotify` / `itsWaitForSpace` (low-water) |
| Introspection | `itsConnected`, `itsBytesAvailable`, `itsSpacesAvailable`, `itsIsEmpty`, `itsIsFull`, `itsSendIsEmpty`, `itsSendDrain`, `itsReset`, `itsRemoteTask`, `itsRecvBufSize`, `itsSendBufSize`, `itsActiveTotal`, `itsTaskMem`, `itsStatus` |

Errors are logged directly through ESP-IDF (`ESP_LOGE("its", "[%s] …",
pcTaskGetName(NULL), …)`) rather than the `info()`/`warn()` macros, so the layer
stands alone with no dependency on the log task. The calling task's name is
prepended to every error; failures that involve a *second* task (connect /
forward / aux send) also embed the remote task name in `[brackets]`.

## Configuration

ITS owns no `s.*` storage keys. Its sizing is compile-time, exposed as Kconfig
knobs in [`Kconfig`](../esp-idf/Kconfig):

| Symbol | Default | Meaning |
|--------|---------|---------|
| `CONFIG_SPANGAP_ITS_INBOX_DEPTH` | 32 | Default depth (messages) of each task's inbox queue. Per-task override: the `inboxDepth` argument to `itsServerInit`/`itsClientInit`. |
| `CONFIG_SPANGAP_ITS_INBOX_MSG_MAX` | 4096 | Default upper bound on a single inbox message payload (connect handshake / aux / forward). Floored at `ITS_MAX_MSG_DATA` (320). Per-task override: `itsServerInit`'s `inboxMaxMsgLen`. |
| `CONFIG_SPANGAP_ITS_PKT_DEPTH` | 16 | Default in-flight whole-message slots per direction on a packet link. Per-port override: the `depth` argument to `itsServerPortOpen`. |
| `CONFIG_SPANGAP_ITS_MSG_MAX` | 65536 | Default largest single message a packet link accepts (the size guard, not the backpressure window). Per-port override: `maxMsg` argument to `itsServerPortOpen`. |

## CLI

```
its     ITS connection + stream-pool status snapshot
```

`its` prints a complete status snapshot at the CLI: total active connections
(`N/128`), one line per active connection in the form `[clientTask] ->
[serverTask:port]`, and stream-pool usage grouped by buffer size. (The same
snapshot is available programmatically through `itsStatus()`.) Run it on-device
with `spangap cli "its"`.
</content>
</invoke>
