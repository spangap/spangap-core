/**
 * ITS — Inter-Task Streaming
 *
 * Point-to-point connections between FreeRTOS tasks.
 * Server pre-allocates handles with stream buffers at init.
 * Client connects by task name or handle, gets a handle back.
 *
 * All API uses int handles:
 *   Server handles: 0 .. maxHandles-1
 *   Client handles: ITS_CLIENT_BASE .. ITS_CLIENT_BASE+maxConns-1
 *
 * A task can be both server and client — handle ranges don't overlap.
 *
 * itsPort: 16-bit endpoint identifier. Servers register multiple endpoints
 * (TCP ports, URL paths) with different itsPort values. The itsPort flows
 * from connect through to onConnect so the server knows which endpoint
 * the connection is for. Convention: use the TCP port number (80, 443, 554)
 * or 0 if not meaningful.
 *
 * Signaling (connect/disconnect/aux) goes through per-task message buffers.
 * Data flows through per-handle stream buffers (SPSC, partial send/recv).
 * itsPoll() reads one inbox message and dispatches to callbacks.
 */

#ifndef SECCAM_ITS_H
#define SECCAM_ITS_H

#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define ITS_MAX_MSG_DATA      64
#define ITS_CLIENT_BASE       128

/* ---- Stream buffer pool ---- */

/** Pre-allocate stream buffers into a systemwide pool.
 *  Call multiple times for different sizes. Called from main at boot. */
void          itsReserveStreams(int count, size_t size);

/* ---- Callbacks ---- */

/** Server: called on incoming connection. itsPort identifies which endpoint.
 *  Return true to accept. */
typedef bool (*its_connect_cb_t)(int handle, int itsPort, const void* data, size_t len);

/** Server: called when all handles are busy and a new client wants in.
 *  Return true to reject. Free a handle (itsServerKick) and return false
 *  to let ITS retry — the freed slot will be used for the new client. */
typedef bool (*its_busy_cb_t)(int itsPort, const void* data, size_t len);

/** Either side: called when connection is closed by remote. */
typedef void (*its_disconnect_cb_t)(int handle);

/** Called when an aux message arrives. */
typedef void (*its_aux_cb_t)(TaskHandle_t sender, const void* data, size_t len);

/* ---- Server API ---- */

/** Init this task as a server. One server per task.
 *  toSize:   client→server stream buffer per handle (0 = send-only server)
 *  fromSize: server→client stream buffer per handle (0 = recv-only server) */
bool          itsServerInit(int maxHandles, size_t toSize, size_t fromSize,
                            its_connect_cb_t onConnect,
                            its_busy_cb_t onBusy,
                            its_disconnect_cb_t onDisconnect,
                            its_aux_cb_t onAux);

void          itsServerKick(int handle);
int           itsServerActive(void);

/** Write data into a server handle's receive buffer (the buffer the server
 *  reads from). Use before itsServerForward to inject the consumed HTTP
 *  request so the target task can read it. Safe when no concurrent client
 *  writes (true for HTTP: browser awaits response before sending more). */
size_t        itsServerInject(int handle, const void* data, size_t len);

/** Forward a server handle to another server task.
 *  Stream buffers swap from this server's slot to the target's slot.
 *  Target's onConnect fires with itsPort + data. Returns target handle, or -1. */
int           itsServerForward(int handle, const char* targetServer, int itsPort,
                               const void* data, size_t dataLen);
int           itsServerForwardByHandle(int handle, TaskHandle_t targetTask, int itsPort,
                                       const void* data, size_t dataLen);

/** Get the itsPort of an active server handle (set on connect/forward). */
int           itsServerPort(int handle);

/* ---- Client API ---- */

void          itsClientInit(int maxConns,
                            its_disconnect_cb_t onDisconnect,
                            its_aux_cb_t onAux);

int           itsConnect(const char* serverName, int itsPort,
                         const void* data, size_t dataLen, TickType_t timeout);
int           itsConnectByHandle(TaskHandle_t serverTask, int itsPort,
                                 const void* data, size_t dataLen,
                                 TickType_t timeout);
void          itsDisconnect(int handle);

/* ---- Aux messages ---- */

bool          itsSendAux(const char* taskName,
                         const void* data, size_t dataLen, TickType_t timeout);
bool          itsSendAuxByHandle(TaskHandle_t task,
                                 const void* data, size_t dataLen,
                                 TickType_t timeout);

/* ---- Poll (call on task notification or periodically) ---- */

/** Read one inbox message and dispatch to the appropriate callback.
 *  Returns true if a message was processed. */
bool          itsPoll(void);

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

/** Get the remote task for a handle (server handle → client task, or vice versa). */
TaskHandle_t  itsRemoteTask(int handle);

#endif /* SECCAM_ITS_H */
