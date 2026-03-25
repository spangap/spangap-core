/**
 * ITS — Inter-Task Streaming
 *
 * Point-to-point connections between FreeRTOS tasks.
 * Global connection table — handles are indices, same number on both sides.
 * Stream buffers from a pre-allocated PSRAM pool.
 *
 * itsPort: 16-bit endpoint identifier passed from connect through to onConnect.
 * A server could use different itsPort values to distinguish multiple endpoints
 * (e.g. TCP ports, URL paths). When proxying incoming TCP connections to a task,
 * one could use the TCP port number (80, 443, 554) as the itsPort. Or just 0.
 * Aux messages also carry a port, dispatched to the matching registered callback.
 *
 * Signaling goes through per-task inbox (message buffer, configurable size).
 * itsPoll() reads one inbox message, dispatches to callback, and gives the
 * pickup semaphore if the sender requested PICKUP acknowledgment.
 *
 * Data flows through per-connection stream buffers (SPSC, partial send/recv).
 */

#ifndef SECCAM_ITS_H
#define SECCAM_ITS_H

#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define ITS_MAX_MSG_DATA      64

/** Timeout waits for delivery into inbox (default) or pickup by receiver's itsPoll. */
enum its_wait_t {
  ITS_WAIT_DELIVERY = 0,   /* return when message is in the inbox */
  ITS_WAIT_PICKUP,         /* return when receiver has processed the message */
};

/* ---- Stream buffer pool ---- */

/** Pre-allocate stream buffers into a systemwide pool.
 *  Call multiple times for different sizes. Called from main at boot. */
void          itsReserveStreams(int count, size_t size);

/* ---- Callbacks ---- */

/** Server: called on incoming connection. Return >= 0 to accept (value = serverRef),
 *  return < 0 to reject. */
typedef int (*its_connect_cb_t)(int handle, int itsPort, const void* data, size_t len);

/** Server: called when active connections == maxHandles and a new client wants in.
 *  Return true to reject. Kick a handle (itsServerKick) and return false
 *  to let ITS retry. */
typedef bool (*its_busy_cb_t)(int itsPort, const void* data, size_t len);

/** Either side: called when connection is closed by remote.
 *  ref is the serverRef (for servers) or clientRef (for clients). */
typedef void (*its_disconnect_cb_t)(int ref);

/** Called when an aux message arrives on the registered port. */
typedef void (*its_aux_cb_t)(TaskHandle_t sender, uint16_t port, const void* data, size_t len);

/* ---- Server API ---- */

/** Init this task as a server. One server per task.
 *  maxHandles:     max concurrent connections this server accepts.
 *  toSize:         client→server stream buffer per connection (0 = send-only server)
 *  fromSize:       server→client stream buffer per connection (0 = recv-only server)
 *  inboxMaxMsgLen: max message size for this task's inbox (0 = default)
 *  inboxDepth:     inbox buffer size in bytes (0 = default, single message) */
bool          itsServerInit(int maxHandles, size_t toSize, size_t fromSize,
                            size_t inboxMaxMsgLen = 0, size_t inboxDepth = 0);

/** Register callbacks individually. Can be called before or after init. */
void          itsServerOnConnect(its_connect_cb_t cb);
void          itsServerOnBusy(its_busy_cb_t cb);
void          itsServerOnDisconnect(its_disconnect_cb_t cb);

/** Register an aux callback for a specific port. Up to ITS_MAX_AUX_CALLBACKS per task.
 *  Port 0 is the default catch-all (matches aux messages with no specific port). */
void          itsOnAux(its_aux_cb_t cb, uint16_t port = 0);

void          itsServerKick(int handle);
int           itsServerActive(void);
int           itsActiveTotal(void);

/** Write data into a connection's client→server buffer.
 *  Use before itsServerForward to inject consumed HTTP headers. */
size_t        itsServerInject(int handle, const void* data, size_t len);

/** Forward a connection to another server task.
 *  Buffers stay, serverTask changes. Target's onConnect fires.
 *  Handle number stays the same. Returns handle on success, -1 on failure. */
int           itsServerForward(int handle, const char* targetServer, int itsPort,
                               const void* data, size_t dataLen);
int           itsServerForwardByHandle(int handle, TaskHandle_t targetTask, int itsPort,
                                       const void* data, size_t dataLen);

/** Get the itsPort of an active connection. */
int           itsServerPort(int handle);

/* ---- Client API ---- */

/** Init this task as a client.
 *  inboxMaxMsgLen/inboxDepth: same as server (0 = default). Ignored if already set. */
void          itsClientInit(int maxConns,
                            its_disconnect_cb_t onDisconnect = nullptr,
                            size_t inboxMaxMsgLen = 0, size_t inboxDepth = 0);

int           itsConnect(const char* serverName, int itsPort,
                         const void* data, size_t dataLen, TickType_t timeout,
                         int ref = -1);
int           itsConnectByHandle(TaskHandle_t serverTask, int itsPort,
                                 const void* data, size_t dataLen,
                                 TickType_t timeout, int ref = -1);
void          itsDisconnect(int handle);

/** Get this side's ref for a connection (serverRef or clientRef depending
 *  on whether the caller is the server or client for this handle). */
int           itsRef(int handle);

/* ---- Aux messages ---- */

/** Send an aux message. Port dispatches to the matching itsOnAux callback on the receiver. */
bool          itsSendAux(const char* taskName,
                         const void* data, size_t dataLen,
                         TickType_t timeout, uint16_t port = 0,
                         its_wait_t wait = ITS_WAIT_DELIVERY);
bool          itsSendAuxByHandle(TaskHandle_t task,
                                 const void* data, size_t dataLen,
                                 TickType_t timeout, uint16_t port = 0,
                                 its_wait_t wait = ITS_WAIT_DELIVERY);

/* ---- Poll ---- */

/** Read one inbox message, dispatch to callback, ACK pickup if requested.
 *  If no message is pending and timeout > 0, blocks (ulTaskNotifyTake)
 *  until notified, then retries. Default portMAX_DELAY = sleep until work.
 *  Returns true if a message was processed.
 *  Typical loop: while (itsPoll()) {} — drains all, blocks on last call. */
bool          itsPoll(TickType_t timeout = portMAX_DELAY);

/* ---- Data (handle from either side) ---- */

size_t        itsSend(int handle, const void* data, size_t len,
                      TickType_t timeout);
size_t        itsRecv(int handle, void* buf, size_t maxLen,
                      TickType_t timeout);
bool          itsConnected(int handle);
size_t        itsBytesAvailable(int handle);
size_t        itsSpacesAvailable(int handle);
bool          itsSetTriggerLevel(int handle, size_t triggerLevel);
bool          itsIsEmpty(int handle);
bool          itsIsFull(int handle);
bool          itsReset(int handle);

/** Get the remote task for a handle. */
TaskHandle_t  itsRemoteTask(int handle);

#endif /* SECCAM_ITS_H */
