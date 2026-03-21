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

/* ---- Callbacks ---- */

/** Server: called on incoming connection. Return true to accept. */
typedef bool (*its_connect_cb_t)(int handle, const void* data, size_t len);

/** Server: called when all handles are busy and a new client wants in.
 *  Return true to reject. Free a handle (itsServerKick) and return false
 *  to let ITS retry — the freed slot will be used for the new client. */
typedef bool (*its_busy_cb_t)(const void* data, size_t len);

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

/* ---- Client API ---- */

void          itsClientInit(int maxConns,
                            its_disconnect_cb_t onDisconnect,
                            its_aux_cb_t onAux);

int           itsConnect(const char* serverName,
                         const void* data, size_t dataLen, TickType_t timeout);
int           itsConnectByHandle(TaskHandle_t serverTask,
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

#endif /* SECCAM_ITS_H */
