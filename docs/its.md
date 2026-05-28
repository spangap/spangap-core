# ITS — Inter-Task Streaming

Socket-style point-to-point connections between FreeRTOS tasks. Header: `its.h`.

ITS is spangap's only inter-task communication primitive. Every cross-task signal — HTTP requests, log lines, CLI commands, config-change notifications, file I/O — flows through ITS, and consumer apps use it the same way for their own tasks. Tasks open numbered "ports" the way a UNIX server opens TCP ports; clients connect to a server's task by name and port. Per-task aux messages handle the things that don't fit a connection model.

## Concepts

### Task

Any FreeRTOS task that calls `itsServerInit`, `itsClientInit`, or `itsOnAux` is registered as an ITS task. Each registered task gets:

- An **inbox** (FreeRTOS Queue) for connect / disconnect / aux / forward signaling.
- A per-task table of **server ports** (max `ITS_MAX_PORTS = 8`) and **aux ports** (also `ITS_MAX_PORTS`).
- A task notification used as the wake mechanism for `itsPoll`.

A task can be a server, a client, or both, simultaneously.

### Port

A 16-bit number identifying an endpoint within a task. Servers explicitly open ports they accept on; aux callbacks are registered against ports they listen on. Server ports and aux ports live in **separate namespaces** within a task: the same number can mean "server port 5" and "aux port 5" without colliding (they're stored in different tables).

By convention, port numbers are arbitrary but stable: each module's header file declares its port constants so other tasks can use them by name.

### Spangap-side ports

| Module | Constants |
|--------|-----------|
| [net.h](../spangap-core/include/net.h) | `NET_PORT_REG_PORT=0` (TCP endpoint registration), `NET_CMD_PORT=1` (CLI control) |
| [web.h](../spangap-core/include/web.h) | `WEB_HTTP_PORT=80`, `WEB_HTTPS_PORT=443`, `WEB_PATH_REG_PORT=0` (URL registration) |
| [webrtc_task.h](../spangap-core/include/webrtc_task.h) | `WEBRTC_PORT=4433` |
| [log.h](../spangap-core/include/log.h) | `LOG_PORT_TCP=8080` (stream, `nc`), `LOG_PORT_DC=1` (packet, WebRTC `log:1`) |
| [cli.h](../spangap-core/include/cli.h) | `CLI_PORT_TCP=8081` (stream, `nc` + serial), `CLI_PORT_DC=1` (packet, WebRTC `cli:1`) |
| [storage.h](../spangap-core/include/storage.h) | `STORAGE_CONFIG_PORT=1` (packet, WebRTC `storage:1`), `STORAGE_CHANGE_PORT=42` (change dispatch), `STORAGE_SAVE_PORT=43` (save-now signal) |
| [fs.h](../spangap-core/include/fs.h) | `FS_OP_PORT=1` (POSIX ops aux, on `fs`), `FS_STREAM_PORT=2` (streaming write, on `fs_strm`), `FS_READ_PORT=3` (streaming read, on `fs_strm`), `FS_STREAM_SYNC_PORT=4` (stream-sync aux, on `fs_strm`) |

Consumer apps add their own port constants in their own headers. One might, for example, reserve a `CAMERA_CMD_PORT`, `RECORD_CMD_PORT`, `RTSP_PORT=554`, etc., one per server task.

### Connection

A point-to-point pipe between a client task and a server task on a specific server port. Each connection has:

- An **ITS handle** — global slot index in a 64-entry table, same number on both sides.
- Two **stream buffers** (drawn from a shared PSRAM pool, see [Stream buffer pool](#stream-buffer-pool)):
  - `to` (client → server)
  - `from` (server → client)
  - Either side may be sized 0 (one-way / aux-only servers).
- Two **refs**: `clientRef` and `serverRef`, set by each side when the connection is established. Refs are local indices the task chooses (typically into its own per-connection state array). The opposite side's ref is what gets passed to the disconnect callback — so neither side needs a handle-to-slot mapping.

### Aux message

A one-shot, fire-and-forget message sent to a port on a target task without setting up a connection. The receiver registers `itsOnAux(port, cb)`; the sender calls `itsSendAux("taskname", port, ...)`. Payload size is bounded by the receiver's inbox item size.

Aux is for things that don't fit a connection model: subscribe/unsubscribe commands, change notifications, periodic nudges, register-this-port-with-net handshakes. Connections handle byte-stream protocols.

### Pickup wait

By default `itsSendAux` returns once the message is in the receiver's inbox queue. Pass `ITS_WAIT_PICKUP` to instead block until the receiver's `itsPoll` has actually invoked the callback. Pickup uses a small pool of binary semaphores (16 slots, oldest reclaimed if exhausted). Useful when the sender needs the receiver to read or modify shared memory the message points at before the sender continues.

## API

All functions are in [`its.h`](../spangap-core/include/its.h). Errors are logged via `ESP_LOGE("its", "[%s] ...", pcTaskGetName(NULL), ...)` — ITS uses ESP-IDF's logging directly (rather than `info()`/`warn()` macros) so the layer can stand alone. The calling task's name is prepended to every error log so consumers without a custom reformatter can still see who triggered the failure (ITS by definition runs on whatever task is using it, so the caller-task identity is the most important breadcrumb). Errors that involve a *second* task — connect/forward/aux send targeting another task — also embed the remote task name in the message body in `[brackets]`.

### Stream buffer pool

Connections draw their stream buffers from a system-wide PSRAM pool that grows on demand: when a port opens a connection that needs a buffer of size `S`, the pool returns a free entry of exactly that size, or allocates a new one if none is free. Entries are never freed — once allocated they stay in the pool forever and become reusable for the next connection asking for the same size. This avoids PSRAM fragmentation and lets each port pick whatever size it needs without having to declare the working set up front.

The pool table itself has a hard limit of 48 entries total (`ITS_MAX_POOL`); each unique size in use consumes at least one entry plus one per concurrent connection of that size.

### Server API

```cpp
bool itsServerInit(size_t inboxMaxMsgLen = 0, size_t inboxDepth = 0);
```

Mark this task as a server and create its inbox. Defaults: 96-byte payloads, 8-deep queue. Call once per task that intends to accept connections.

```cpp
bool itsServerPortOpen(uint16_t port, int maxHandles, size_t toSize, size_t fromSize);
void itsServerPortClose(uint16_t port);
```

Open / close one server port. Up to `ITS_MAX_PORTS = 8` per task (shared with aux ports). `maxHandles` is this port's connection cap; `toSize` and `fromSize` request stream buffer sizes (0 = no buffer for that direction). Closing a port forcibly disconnects any active connections on it.

```cpp
void itsServerOnConnect   (uint16_t port, its_connect_cb_t    cb);
void itsServerOnBusy      (uint16_t port, its_busy_cb_t       cb);
void itsServerOnDisconnect(uint16_t port, its_disconnect_cb_t cb);
void itsServerOnRecv      (uint16_t port, its_recv_cb_t       cb);
```

Per-port callbacks. The port must already be open. The callback knows its own port from registration, so the port number is **not** an argument.

```cpp
typedef int  (*its_connect_cb_t)(int handle, const void* data, size_t len);
typedef bool (*its_busy_cb_t)(const void* data, size_t len);
typedef void (*its_disconnect_cb_t)(int ref);
typedef void (*its_recv_cb_t)(int handle, size_t bytesAvail);
```

- **`onConnect`**: incoming connection. Returns `>= 0` to accept (the value becomes the local serverRef stored in the connection table); `< 0` to reject.
- **`onBusy`**: all of this port's `maxHandles` slots are full and a new connection is trying to come in. Return `false` to retry — typically after disconnecting an old client with `itsDisconnect(victim)`. Return `true` to give up and reject.
- **`onDisconnect`**: the remote side closed (or this side called `itsDisconnect` from a different context). `ref` is the local serverRef stored at connect time. The connection is already gone by the time the callback fires.
- **`onRecv`**: bytes are pending on `handle` after a poll wake. Optional. If unset, the task is expected to drain via its own logic.

```cpp
size_t itsServerInject(int handle, const void* data, size_t len);
int    itsServerForward(int handle, const char* targetServer, uint16_t port,
                        const void* data, size_t dataLen);
int    itsServerForwardByTaskHandle(int handle, TaskHandle_t targetTask, uint16_t port,
                                    const void* data, size_t dataLen);
```

`itsServerInject` writes bytes into a connection's incoming (client→server) buffer from the server's side. Used together with `itsServerForward` to put already-consumed protocol bytes back so the receiving task can re-parse them. The web task reads HTTP request headers, decides which target task should handle them based on the URL, then injects the headers back and forwards the connection — the target sees a fresh-looking request.

`itsServerForward` transfers a connection's ownership to another server task on a specific port. Stream buffers stay; the new owner sees an `onConnect` for that port. The original server is no longer the owner. Returns the (unchanged) handle on success.

```cpp
int  itsServerActive(int port = -1);
int  itsActiveTotal(void);
int  itsServerPort(int handle);
```

Diagnostics. `itsServerActive(-1)` counts all active connections this task holds across all its ports; pass a specific port to count just that one. `itsServerPort` returns the port a given handle is on, for the server side.

### Client API

```cpp
void itsClientInit(int maxConns, size_t inboxMaxMsgLen = 0, size_t inboxDepth = 0);
```

Mark this task as a client. `maxConns` caps how many simultaneous outbound connections this task can hold. Defaults match `itsServerInit`.

```cpp
int itsConnect(const char* serverName, uint16_t port,
               const void* data, size_t dataLen, TickType_t timeout,
               int ref = -1,
               its_recv_cb_t       onRecv       = nullptr,
               its_disconnect_cb_t onDisconnect = nullptr);
int itsConnectByTaskHandle(TaskHandle_t serverTask, uint16_t port,
                           const void* data, size_t dataLen, TickType_t timeout,
                           int ref = -1,
                           its_recv_cb_t       onRecv       = nullptr,
                           its_disconnect_cb_t onDisconnect = nullptr);
```

Connect to a server task by name (or task handle). Returns the ITS handle on success, `-1` on error / timeout / unopened port. The connect payload (`data`, `dataLen`) is delivered to the server's `onConnect` callback. `ref` is stored as this side's clientRef.

`onRecv` and `onDisconnect` are **per-connection** client-side callbacks. They're stored in the connection table at connect time so a single client task can handle multiple outbound connections to different services with different handlers, no runtime dispatch. They're written to the connection record before `itsConnect` returns, so by the time you have the handle the callbacks are already wired.

```cpp
int itsRef(int handle);
```

Returns this side's stored ref for `handle` (clientRef if you're the client side, serverRef if you're the server side, `-1` if neither).

### Disconnect

```cpp
void itsDisconnect(int handle);
```

Close a connection. Works from **either side** — if the calling task is the server end of `handle` it kicks the client; if it's the client end it closes from the client side. The remote side's matching disconnect callback fires.

Pass `handle == -1` to disconnect **all** connections (both server-owned and client-owned) currently held by the calling task. Useful for shutdown / graceful teardown.

There is no separate `itsServerKick` — `itsDisconnect` covers both. Choosing which side calls it is purely about which task is in a position to know it's time to close.

### Aux messages

```cpp
void itsOnAux(uint16_t port, its_aux_cb_t cb);

typedef void (*its_aux_cb_t)(TaskHandle_t sender, const void* data, size_t len);
```

Register a per-task aux callback for `port`. Up to `ITS_MAX_PORTS = 8` per task. Calling again for the same port replaces the existing callback (idempotent). Aux ports are independent of server ports. Port 0 is a perfectly normal aux port (no special "catch-all" semantics).

```cpp
bool itsSendAux(const char* taskName, uint16_t port,
                const void* data, size_t dataLen, TickType_t timeout,
                its_wait_t wait = ITS_WAIT_DELIVERY);
bool itsSendAuxByTaskHandle(TaskHandle_t task, uint16_t port,
                            const void* data, size_t dataLen, TickType_t timeout,
                            its_wait_t wait = ITS_WAIT_DELIVERY);
```

Send a one-shot aux message to a target task's registered port. `ITS_WAIT_DELIVERY` (default) returns once the message is in the receiver's inbox queue; `ITS_WAIT_PICKUP` blocks until the receiver's callback has finished running.

ITS validates that the receiver has registered an aux callback for the port before queueing — sending to an unregistered port logs an error and returns `false`. This catches misrouted aux messages immediately.

### itsPoll — universal blocking primitive

```cpp
bool itsPoll(TickType_t timeout = portMAX_DELAY);
```

Drain one inbox message (if any), then walk this task's connections and dispatch the registered `onRecv` callback for any connection that has unread bytes. Returns `true` if any work was done.

Every ITS task's main loop is some variant of:

```cpp
for (;;) {
  while (itsPoll(0)) {}      // drain everything pending
  // ... do other work ...
  itsPoll(blockTime);        // block until something arrives or timeout
}
```

`while (itsPoll(0)) {}` is the standard "drain to empty" idiom. `itsPoll(timeout > 0)` blocks on a task notification, waking when:

- A new aux / connect / disconnect / forward message hits the inbox.
- The remote side of any connection sends bytes that bring its outgoing buffer fill at or above the trigger level (see [Stream send-trigger](#stream-send-trigger)).

A task that needs to also poll non-ITS sources (e.g. select on a socket, periodic timer) calls `itsPoll(shortTimeout)` and interleaves its own work. Tasks that have nothing else to do call `itsPoll()` with no argument and block forever until ITS wakes them.

### Data API

```cpp
size_t itsSend(int handle, const void* data, size_t len, TickType_t timeout);
size_t itsRecv(int handle, void* buf, size_t maxLen, TickType_t timeout);
```

Stream-buffer semantics — partial send / partial recv, returns bytes actually transferred. `itsSend` may wake the remote side; see [Stream send-trigger](#stream-send-trigger) below.

```cpp
bool         itsConnected(int handle);
size_t       itsBytesAvailable(int handle);
size_t       itsSpacesAvailable(int handle);
bool         itsIsEmpty(int handle);
bool         itsIsFull(int handle);
bool         itsReset(int handle);
TaskHandle_t itsRemoteTask(int handle);
```

Standard introspection. `itsConnected` is the canonical "is this still alive" check. `itsBytesAvailable` reads from this side's incoming buffer (server reads from `to`, client reads from `from`); `itsSpacesAvailable` reads from this side's outgoing buffer.

### Stream send-trigger

```cpp
bool itsSetTriggerLevel(int handle, size_t triggerLevel);
```

Set the trigger level on **this side's incoming stream** (the buffer bytes are arriving in, from the caller's perspective). The default is 1 — every `itsSend` from the remote wakes the local task. Setting a higher trigger level means the remote's `itsSend` only notifies the local task once the buffer fill reaches `triggerLevel` bytes; smaller writes accumulate silently in the buffer until the threshold is crossed.

This replaces the older `itsSendSilent` + manual nudge pattern. The fs streaming-write server uses it to coalesce writes (see `fs_open_stream`):

```cpp
int handle = fs_open_stream(path, "ab", bufSize, triggerLevel);
// fs_strm sets trigger level on the connection; itsSend accumulates silently
// until the buffer fill crosses triggerLevel, at which point the worker drains
// with a single fwrite.
itsSend(handle, data, len, ...);
```

Setting the trigger level also calls `xStreamBufferSetTriggerLevel` on the underlying FreeRTOS stream buffer, so any blocking `xStreamBufferReceive` callers see consistent semantics.

### Status snapshot

```cpp
void itsStatus(void);
```

Logs a complete system status snapshot at INFO level — total active connections (`N/64`), one line per active connection in the form `[clientTask] -> [serverTask:port]`, and stream pool usage grouped by buffer size. Useful for diagnostics and `top`-style introspection.

## Implementation notes

### Connection table

Global, fixed-size: 64 entries (`ITS_MAX_CONNS`). Round-robin allocation. Both sides of a connection refer to it by the same handle (= slot index). Connection records carry the buffer pool indices, the server task / client task handles, the port number, and both sides' refs.

### Stream buffer pool

System-wide PSRAM pool, max 48 entries (`ITS_MAX_POOL`). Entries are allocated lazily: `poolGet(size)` reuses a free entry of exactly `size`, or allocates a new one if none is free. `poolFree` marks an entry unused (the buffer stays in the pool, ready for the next caller asking for the same size) and resets the trigger level back to 1. No entry is ever destroyed.

### Inbox

Per-task FreeRTOS Queue. Item size is `sizeof(its_header_t) + ITS_MAX_MSG_DATA` by default (96-byte max payload), depth 8. Customizable via `itsServerInit` / `itsClientInit` arguments. The header carries: sender, message type (CONNECT / DISCONNECT / AUX / FORWARD), port, length, handle, optional pickup-pool index.

Inbox queues are allocated via `xQueueCreateWithCaps(..., MALLOC_CAP_SPIRAM)` so they live in PSRAM rather than DRAM. See "ISR safety" below for the rule that makes this OK.

### ISR safety

**ITS is task-context only.** None of `itsSend` / `itsRecv` / `itsSendAux` / `itsConnect` / `itsPoll` may be called from an ISR.

Two reasons:
- The inbox queue is in PSRAM. `xQueueSendFromISR` on a PSRAM-backed queue crashes or hangs when the cache is disabled (which it is during any SPI flash operation — OTA, LittleFS commit, etc.). FreeRTOS doesn't enforce this; the symptom is a sporadic crash that only manifests during a flash write.
- ITS internals take spinlocks/mutexes and do non-trivial work per send (pool lookup, length checks, sender notify). Not bounded enough for an ISR anyway.

The right pattern for ISR → task communication on spangap is:

1. ISR writes a heap-resident flag (any plain global / volatile bool / counter).
2. ISR calls `vTaskNotifyGiveFromISR(taskHandle, &hp)` + `portYIELD_FROM_ISR(hp)`.
3. Target task's main loop blocks on `itsPoll(timeout)` (which uses `ulTaskNotifyTake` under the hood, so the task wakes from the ISR notification too) and reads the flag.

Example: `lora.cpp`'s DIO1 ISR calls `vTaskNotifyGiveFromISR(s_task, ...)`; the task loop drains the radio inside its `itsPoll` wake. No ITS calls from the ISR. lwIP's UDP recv callback does the same for `udp.cpp`.

### Disconnect message payload

When a server kicks a client, the client-side `cliDisconnectCb` (per-connection callback) is captured before `connFree` and embedded as raw bytes in the disconnect message payload. By the time the client wakes up to process the inbox message, the connection record is already freed — but the cb pointer travels with the message, so the client's dispatcher still calls the right callback. Function pointers in the same address space are safe to ship as raw bytes.

Client-initiated disconnects don't need this trick because the server's disconnect callback lives in the per-port table on the server task, which isn't going anywhere.

### Limits

| Constant | Default | Meaning |
|----------|---------|---------|
| `ITS_MAX_PORTS` | 8 | Max server ports + max aux ports per task (each in its own table) |
| `ITS_MAX_MSG_DATA` | 96 | Default max aux/connect/forward payload |
| `ITS_MAX_CONNS` | 64 | Global active connection table size |
| `ITS_MAX_POOL` | 48 | Global stream buffer pool size (lazy growth) |
| `ITS_MAX_TASKS` | 24 | Max ITS-registered tasks in the system |
| `ITS_MAX_PICKUP` | 16 | Pickup-wait semaphore pool size |

These are compile-time. Defaults are sized for moderately busy apps; bump them if you hit ceilings.

## Common patterns

### Server task accepting forwarded connections

```cpp
static int onConnect(int handle, const void* data, size_t len) {
    auto* cd = (const net_connect_t*)data;
    int slot = allocSlot(handle);
    if (slot < 0) return -1;
    handles[slot].tls = cd->tls;
    return slot;   // becomes serverRef
}

static void onDisconnect(int ref) {
    handles[ref] = {};
}

static void onRecv(int handle, size_t len) {
    char buf[512];
    size_t n = itsRecv(handle, buf, sizeof(buf), 0);
    // ... process ...
}

static void taskFn(void*) {
    itsServerInit();
    itsServerPortOpen(MY_PORT, MAX_CLIENTS, 4096, 16384);
    itsServerOnConnect(MY_PORT, onConnect);
    itsServerOnDisconnect(MY_PORT, onDisconnect);
    itsServerOnRecv(MY_PORT, onRecv);

    // Register with web so HTTP requests for "/myservice" forward here
    web_path_msg_t reg = { .itsPort = MY_PORT };
    safeStrncpy(reg.path, "myservice", sizeof(reg.path));
    while (!itsSendAux("web", WEB_PATH_REG_PORT, &reg, sizeof(reg), pdMS_TO_TICKS(500)))
        vTaskDelay(pdMS_TO_TICKS(100));

    for (;;) itsPoll();
}
```

### Aux-only command port

```cpp
static void onCmd(TaskHandle_t sender, const void* data, size_t len) {
    if (len < 1) return;
    uint8_t cmd = *(const uint8_t*)data;
    // ...
}

static void taskFn(void*) {
    itsOnAux(MY_CMD_PORT, onCmd);
    for (;;) {
        while (itsPoll(0)) {}
        // ... periodic work ...
        itsPoll(pdMS_TO_TICKS(50));
    }
}

// Caller anywhere:
uint8_t cmd = MY_CMD_DO_THING;
itsSendAux("mytask", MY_CMD_PORT, &cmd, 1, pdMS_TO_TICKS(500));
```

### Client connecting to a service

```cpp
static void onMyRecv(int handle, size_t len) {
    char buf[256];
    size_t n = itsRecv(handle, buf, sizeof(buf), 0);
    // ...
}

static void onMyDisconnect(int ref) {
    info("server gone\n");
}

static void taskFn(void*) {
    itsClientInit(2);
    int h = itsConnect("log", LOG_PORT_TCP, &req, sizeof(req), pdMS_TO_TICKS(500),
                       /*ref*/ -1, onMyRecv, onMyDisconnect);
    if (h < 0) return;

    for (;;) itsPoll();   // onMyRecv fires when bytes arrive
}
```

## What broke and why this design

ITS replaced an earlier IPC layer that used global broadcast queues and shared state. The current shape — point-to-point connections, per-port handlers, no global catch-alls — emerged from a series of bugs where messages would fan out unintentionally or dispatch on the wrong task. Anchoring everything to a port (and rejecting messages to unregistered ports loudly) makes wiring mistakes show up at startup instead of as silent corruption later.

`itsPoll` is the universal blocking primitive: every task's main loop is `while (itsPoll(0)) {}` followed by some non-ITS work, then `itsPoll(timeout)` to sleep. This means any module can wake any task by sending it an aux message or writing to a stream buffer it owns — no shared timers, no parallel notification systems, no per-module dispatch logic.

The "trigger level" send semantics replaced an explicit "silent send + manual nudge" API after that pattern caused 97% CPU spin in the fs worker (it kept being woken on every byte). Letting the receiver declare its own wake threshold pushes the policy to the side that knows what "enough data" means.
