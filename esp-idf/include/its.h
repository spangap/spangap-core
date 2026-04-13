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
 * pre-allocated PSRAM pool) carry payload bytes.
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
 */

#ifndef ITS_H
#define ITS_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define ITS_MAX_MSG_DATA  96
#define ITS_MAX_PORTS      8     /* per task: server ports + aux ports */

/** itsSendAux: wait for inbox delivery (default) or for receiver pickup. */
enum its_wait_t {
    ITS_WAIT_DELIVERY = 0,   /* return when message is in receiver's inbox */
    ITS_WAIT_PICKUP,         /* return when receiver has processed it */
};

/* ---- Stream buffer pool ---- */

/** Pre-allocate stream buffers into a system-wide pool.
 *  Call multiple times for different sizes during boot. */
void itsReserveStreams(int count, size_t size);

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
 *  inboxDepth:     queue depth in messages (0 = default 8). */
bool itsServerInit(size_t inboxMaxMsgLen = 0, size_t inboxDepth = 0);

/** Open a server port. Up to ITS_MAX_PORTS per task (shared with aux ports).
 *  maxHandles: maximum concurrent connections this port accepts.
 *  toSize:     client→server buffer per connection (0 = none/aux-only).
 *  fromSize:   server→client buffer per connection (0 = none/aux-only). */
bool itsServerPortOpen(uint16_t port, int maxHandles,
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

/** Log a complete system status snapshot at INFO level (connection
 *  table + stream pool usage). Useful for diagnostics. */
void itsStatus(void);

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

size_t itsSend(int handle, const void* data, size_t len, TickType_t timeout);
size_t itsRecv(int handle, void* buf, size_t maxLen, TickType_t timeout);

/** Set the trigger level on this side's incoming stream (the buffer
 *  bytes are arriving in, from the caller's perspective). itsSend on
 *  the remote will only wake the local task once the buffer fill
 *  reaches `triggerLevel` bytes. Default is 1 (wake on every send).
 *  Setting to 0 is interpreted as 1. The trigger level is also applied
 *  to the underlying stream buffer so blocking xStreamBufferReceive
 *  callers (if any) see consistent semantics. */
bool itsSetTriggerLevel(int handle, size_t triggerLevel);

bool         itsConnected(int handle);
size_t       itsBytesAvailable(int handle);
size_t       itsSpacesAvailable(int handle);
bool         itsIsEmpty(int handle);
bool         itsIsFull(int handle);
bool         itsReset(int handle);
TaskHandle_t itsRemoteTask(int handle);

#endif /* ITS_H */
