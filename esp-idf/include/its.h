/**
 * ITS — Inter-Task Streaming
 *
 * Socket-style point-to-point connections between FreeRTOS tasks.
 *
 * Servers open numbered ports (each with its own connection limit and
 * stream buffer sizes); clients connect to a server task by name (or
 * task handle) and port. Connection attempts and aux messages targeted at
 * unopened/unregistered ports are rejected with an error log.
 *
 * Per-task inbox (FreeRTOS Queue) carries connect / disconnect / aux /
 * forward signaling. Per-connection FreeRTOS stream buffers (drawn from a
 * PSRAM pool that grows on demand and never shrinks) carry payload bytes.
 *
 * itsPoll() is the universal blocking primitive: it drains one inbox
 * message AND dispatches per-connection recv callbacks for connections
 * that have unread bytes. Tasks loop `while (itsPoll(0)) {}` to drain.
 *
 * Server-side callbacks (connect/busy/disconnect/recv) live in the
 * per-port table — they implicitly know their port from registration,
 * so the port number is not passed to the callback. Client-side
 * callbacks (recv/disconnect) live in the per-connection table and are
 * supplied as arguments to itsConnect(). This lets a single client task
 * connect to multiple services and have a different handler per
 * connection without runtime dispatching. Aux callbacks are per-task,
 * per-port.
 *
 * Flow-control primitives on the shared per-direction pool entry:
 *
 *   High-water (receiver-driven): itsSetTriggerLevel(handle, N) tells the
 *   remote sender "don't notify me until >= N bytes are queued for me."
 *   The sender's itsSend consults this threshold to decide whether to wake
 *   the receiver. Useful when the receiver prefers batched work.
 *
 *   Low-water (sender-driven): itsSetFreeNotify(handle, N) asks for a
 *   one-shot wake when the send buffer has >= N bytes of free space again.
 *   Fires a task notification on the sender (itsPoll returns) AND a
 *   dedicated per-buffer semaphore; the non-blocking arm is the natural
 *   fit for pump loops, while the blocking convenience itsWaitForSpace
 *   uses the semaphore and is safe to call inside a handler on a task
 *   whose main loop blocks in itsPoll. Packet-mode itsSend uses the
 *   blocking path internally to wait for a whole packet to fit.
 */

#ifndef ITS_H
#define ITS_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <cstdio>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define ITS_MAX_MSG_DATA  320
#define ITS_MAX_PORTS      8     /* per task: server ports + aux ports */

/** itsSendAux: wait for inbox delivery (default) or for receiver pickup. */
enum its_wait_t {
    ITS_WAIT_DELIVERY = 0,   /* return when message is in receiver's inbox */
    ITS_WAIT_PICKUP,         /* return when receiver has processed it */
};

/* ---- Callback signatures ---- */

/** Server: incoming connection on a port (handle is the new ITS handle).
 *  Return >= 0 to accept (return value is stored as the local serverRef),
 *  return < 0 to reject. */
typedef int  (*its_connect_cb_t)(int handle, const void* data, size_t len);

/** Server: all of this port's handles are in use. Return false to retry
 *  (after disconnecting a victim with itsDisconnect), return true to
 *  give up and reject the new connection. */
typedef bool (*its_busy_cb_t)(const void* data, size_t len);

/** Either side: connection closed by remote. ref is the local
 *  serverRef (server side) or clientRef (client side). */
typedef void (*its_disconnect_cb_t)(int ref);

/** Either side: pending bytes are available on `handle` after a poll wake.
 *  The callback should call itsRecv() to consume what it needs;
 *  itsPoll() will dispatch again on the next wake if more data arrives. */
typedef void (*its_recv_cb_t)(int handle, size_t bytesAvail);

/** Aux message arrived on a port the task registered an itsOnAux for. */
typedef void (*its_aux_cb_t)(TaskHandle_t sender, const void* data, size_t len);

/* ---- Server API ---- */

/** Initialize this task as a server. One server per task; sets up the
 *  inbox queue. Server ports are opened with itsServerPortOpen().
 *  inboxMaxMsgLen: max size of any single inbox message (0 = default).
 *  inboxDepth:     queue depth in messages (0 = default 8).
 *  no_pool:        when true, this server's connections bypass the shared
 *                  stream-buffer pool — each buffer is created fresh on connect
 *                  and deleted on disconnect (by the disconnect's receiving
 *                  end), so transient/large buffers return to the heap and the
 *                  server's buffers stay exactly attributed under per-task heap
 *                  tracking (no cross-task buffer inheritance). Default false —
 *                  no behaviour change for existing callers. */
bool itsServerInit(size_t inboxMaxMsgLen = 0, size_t inboxDepth = 0, bool no_pool = false);

/** Open a server port. Up to ITS_MAX_PORTS per task (shared with aux ports).
 *  packetBased: if true, itsSend/itsRecv on connections to this port carry
 *               discrete packets framed with a 4-byte header
 *               ([reserved=0][len_hi][len_mid][len_lo], 24-bit big-endian
 *               body length). itsSend writes one packet per call (atomic on
 *               the wire); itsRecv copies one body per call and returns its
 *               length. Both client and server must already know they are
 *               doing a packet protocol; the flag exists so ITS knows too.
 *  maxHandles: maximum concurrent connections this port accepts.
 *  toSize:     client→server buffer per connection (0 = none/aux-only).
 *  fromSize:   server→client buffer per connection (0 = none/aux-only). */
bool itsServerPortOpen(uint16_t port, bool packetBased, int maxHandles,
                       size_t toSize, size_t fromSize);

/** Close a server port (existing connections are disconnected). */
void itsServerPortClose(uint16_t port);

/** Per-port callbacks. The port must already have been opened. */
void itsServerOnConnect(uint16_t port, its_connect_cb_t cb);
void itsServerOnBusy(uint16_t port, its_busy_cb_t cb);
void itsServerOnDisconnect(uint16_t port, its_disconnect_cb_t cb);
void itsServerOnRecv(uint16_t port, its_recv_cb_t cb);

/** Number of active connections this server holds on `port`.
 *  port == -1 (default) counts across all of this task's open ports. */
int  itsServerActive(int port = -1);

/** Number of active ITS connections in the entire system. */
int  itsActiveTotal(void);

/** Emit a complete system status snapshot (connection table + stream pool
 *  usage). Defaults to plain `printf`; pass any `int (*)(const char*, ...)`
 *  to redirect output. */
void itsStatus(int (*print)(const char*, ...) = printf);

/** PSRAM held by a task's ITS objects: stream buffers for connections it
 *  serves (attributed to the server, which allocates them at connect) plus
 *  its single inbox queue's storage. For per-task memory breakdowns. */
typedef struct { size_t streamBytes; size_t inboxBytes; int streamBufs; } its_mem_t;
its_mem_t itsTaskMem(TaskHandle_t task);

/** Inject bytes into one side of a connection's stream-buffer pair without
 *  caller-identity checks. `asServer=true` writes to the server→client
 *  direction (as if the server called itsSend); `asServer=false` writes to
 *  the client→server direction (as if the client sent data).
 *  Callers include:
 *    - a server task pushing back already-consumed protocol bytes before
 *      calling itsServerForward (asServer=false),
 *    - a worker task spawned by the server to push response bytes on behalf
 *      of the connection owner (asServer=true). */
size_t itsInject(int handle, bool asServer, const void* data, size_t len,
                 TickType_t timeout = 0);

/** Hand off a connection's ownership to another server task.
 *  Stream buffers stay; the new owner sees an onConnect for `port`. */
int itsServerForward(int handle, const char* targetServer, uint16_t port,
                     const void* data, size_t dataLen);
int itsServerForwardByTaskHandle(int handle, TaskHandle_t targetTask, uint16_t port,
                                 const void* data, size_t dataLen);

/** Get the port a connection was accepted on. -1 if not active here. */
int itsServerPort(int handle);

/* ---- Client API ---- */

/** Initialize this task as a client. One client per task. Sets up inbox
 *  if not already set up by itsServerInit. */
void itsClientInit(int maxConns,
                   size_t inboxMaxMsgLen = 0, size_t inboxDepth = 0);

/** Connect to `serverName`:port. Returns ITS handle, or -1 on error/timeout.
 *  ref is stored as this client's clientRef for the connection.
 *  onRecv:       per-connection recv callback. Invoked from itsPoll() on
 *                this task whenever the connection has unread bytes.
 *  onDisconnect: per-connection disconnect callback. Invoked from
 *                itsPoll() when the server closes this connection. */
int itsConnect(const char* serverName, uint16_t port,
               const void* data, size_t dataLen, TickType_t timeout,
               int ref = -1,
               its_recv_cb_t onRecv = nullptr,
               its_disconnect_cb_t onDisconnect = nullptr);
int itsConnectByTaskHandle(TaskHandle_t serverTask, uint16_t port,
                           const void* data, size_t dataLen,
                           TickType_t timeout,
                           int ref = -1,
                           its_recv_cb_t onRecv = nullptr,
                           its_disconnect_cb_t onDisconnect = nullptr);

/** Get this side's stored ref for a connection. -1 if not active here. */
int itsRef(int handle);

/* ---- Disconnect (works from either side) ---- */

/** Close a connection. Works on either the server or client end of a
 *  connection owned by the calling task. The remote side's matching
 *  disconnect callback fires.
 *
 *  Pass `handle == -1` to disconnect ALL connections (both server-owned
 *  and client-owned) held by the calling task. Useful for shutdown. */
void itsDisconnect(int handle);

/* ---- Aux messages (per-task, per-port) ---- */

/** Register an aux callback for a port on this task. Up to ITS_MAX_PORTS
 *  per task (shared with server ports). Calling again for the same port
 *  replaces the existing callback. Port 0 is just an ordinary port. */
void itsOnAux(uint16_t port, its_aux_cb_t cb);

bool itsSendAux(const char* taskName, uint16_t port,
                const void* data, size_t dataLen, TickType_t timeout,
                its_wait_t wait = ITS_WAIT_DELIVERY);
bool itsSendAuxByTaskHandle(TaskHandle_t task, uint16_t port,
                            const void* data, size_t dataLen, TickType_t timeout,
                            its_wait_t wait = ITS_WAIT_DELIVERY);

/* ---- Universal blocking primitive ---- */

/** Drain one inbox message (if any) and dispatch per-connection recv
 *  callbacks for connections with bytes pending. Returns true if any work
 *  was done. With timeout > 0, blocks waiting for a task notification
 *  before retrying once. Use `while (itsPoll(0)) {}` to fully drain. */
bool itsPoll(TickType_t timeout = portMAX_DELAY);

/* ---- Data ---- */

/** Send bytes (stream mode) or one packet (packet mode).
 *
 *  Stream mode: same semantics as xStreamBufferSend. Returns bytes written.
 *
 *  Packet mode: writes [4-byte header][body] atomically (single writer per
 *  direction). Header is built internally; caller passes only the body.
 *    - timeout == 0: succeed and write the whole packet, or return 0 if the
 *      buffer doesn't currently have 4+len free.
 *    - timeout > 0: block until the whole packet fits, or return 0 on timeout.
 *      The wait is woken by the receiver consuming bytes (no busy poll).
 *  Returns body length on success, 0 on failure. Body length must fit in 24
 *  bits and 4+len must fit in the connection's stream buffer capacity. */
size_t itsSend(int handle, const void* data, size_t len, TickType_t timeout);

/** Receive bytes (stream mode) or one packet body (packet mode).
 *
 *  Stream mode: same semantics as xStreamBufferReceive. Returns bytes read.
 *
 *  Packet mode: reads exactly one packet's body into buf and returns its
 *  length. The 4-byte framing header is consumed and discarded.
 *    - timeout == 0: returns 0 if no whole packet is queued.
 *    - timeout > 0: blocks until a whole packet is available or timeout.
 *  If maxLen < body length, the packet is drained, an error is logged, and
 *  0 is returned (caller's buffer was undersized for this port's traffic). */
size_t itsRecv(int handle, void* buf, size_t maxLen, TickType_t timeout);

/** Set the trigger level on the caller's incoming stream (the buffer
 *  bytes are arriving in, from the caller's perspective). The remote
 *  task's itsSend path will only notify the caller once the buffer fill
 *  reaches `triggerLevel` bytes. Default is 1 (wake on every send).
 *  Setting to 0 is interpreted as 1. The trigger level is also applied
 *  to the underlying stream buffer so blocking xStreamBufferReceive
 *  callers (if any) see consistent semantics. */
bool itsSetTriggerLevel(int handle, size_t triggerLevel);

/** Arm a one-shot free-space notification on the caller's outgoing
 *  stream (the buffer the caller writes into, from the caller's
 *  perspective). When the remote consumes enough bytes that at least
 *  `freeBytes` of free space is available, ITS wakes the caller's task
 *  via xTaskNotifyGive (so a pending itsPoll() returns) and also gives
 *  a dedicated per-buffer semaphore (drained by itsWaitForSpace).
 *  One-shot: auto-cleared when fired; the caller re-arms for the next
 *  wake. Passing 0 for freeBytes clears any pending arm. If the space
 *  is already available at call time the wake fires immediately.
 *  Returns false on invalid handle. */
bool itsSetFreeNotify(int handle, size_t freeBytes);

/** Block the calling task until the caller's outgoing stream has at
 *  least `freeBytes` of free space, the connection is torn down, or the
 *  timeout expires. Uses the per-buffer semaphore; does not consume
 *  task notifications, so it is safe to call from a handler on a task
 *  whose main loop blocks in itsPoll. Returns true on success (space is
 *  available), false on timeout or disconnect. Passing 0 for freeBytes
 *  returns true immediately. */
bool itsWaitForSpace(int handle, size_t freeBytes, TickType_t timeout);

bool         itsConnected(int handle);
size_t       itsBytesAvailable(int handle);

/** Bytes of free space in the send-direction buffer. In packet mode this
 *  subtracts the 4-byte header overhead — i.e. it returns the largest body
 *  length that itsSend(timeout=0) would accept right now. */
size_t       itsSpacesAvailable(int handle);
bool         itsIsEmpty(int handle);
bool         itsIsFull(int handle);

/** Returns true if the SEND-direction buffer is fully drained by the
 *  peer (mirror of itsIsEmpty, which looks at the recv direction). Use
 *  this to confirm the remote side has read everything we wrote before
 *  closing the connection. */
bool         itsSendIsEmpty(int handle);

/** Block up to `timeoutMs` waiting for the send buffer to drain. Returns
 *  true if fully drained, false on timeout. Useful before an onConnect
 *  reject to let a last-gasp wsSendClose reach the wire. */
bool         itsSendDrain(int handle, uint32_t timeoutMs);
bool         itsReset(int handle);
TaskHandle_t itsRemoteTask(int handle);

#endif /* ITS_H */
