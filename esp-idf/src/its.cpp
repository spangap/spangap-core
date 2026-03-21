#include "its.h"
#include "freertos/stream_buffer.h"
#include "freertos/message_buffer.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include <string.h>

/* ---- Signaling protocol (over per-task message buffer inbox) ---- */

enum {
    ITS_MSG_CONNECT,
    ITS_MSG_DISCONNECT,
    ITS_MSG_AUX,
};

typedef struct {
    TaskHandle_t sender;
    uint8_t      msg;
    uint8_t      _pad;
    uint16_t     len;       /* payload bytes after header */
} its_header_t;             /* 8 bytes, naturally aligned */

#define ITS_INBOX_SIZE  (sizeof(its_header_t) + ITS_MAX_MSG_DATA + 4)

/* ---- Server slot ---- */

typedef struct {
    volatile bool        active;
    TaskHandle_t         client;
    StreamBufferHandle_t toServer;
    StreamBufferHandle_t fromServer;
} its_slot_t;

/* ---- Server ---- */

struct ItsServer {
    int                   maxHandles;
    its_slot_t*           slots;
    TaskHandle_t          task;
    size_t                toSize;
    size_t                fromSize;
    its_connect_cb_t      onConnect;
    its_busy_cb_t         onBusy;
    its_disconnect_cb_t   onDisconnect;
};

/* ---- Client connection ---- */

typedef struct {
    bool             active;
    its_slot_t*      slot;
    TaskHandle_t     server;
} its_client_conn_t;

/* ---- Client ---- */

struct ItsClient {
    int                   maxConns;
    its_client_conn_t*    conns;
    TaskHandle_t          task;
    SemaphoreHandle_t     ackSem;
    its_slot_t*           ackSlot;     /* written by server during handshake */
    its_disconnect_cb_t   onDisconnect;
};

/* ---- Global registry ---- */

#define ITS_MAX_TASKS  8

typedef struct {
    TaskHandle_t          task;
    ItsServer*            server;
    ItsClient*            client;
    its_aux_cb_t          onAux;
    MessageBufferHandle_t inbox;
    SemaphoreHandle_t     inboxReady;   /* binary sem: given after message read */
} its_task_entry_t;

static its_task_entry_t s_tasks[ITS_MAX_TASKS];
static int              s_taskCount = 0;

static its_task_entry_t* taskFind(TaskHandle_t task) {
    for (int i = 0; i < s_taskCount; i++)
        if (s_tasks[i].task == task) return &s_tasks[i];
    return nullptr;
}

static its_task_entry_t* taskFindOrCreate(TaskHandle_t task) {
    its_task_entry_t* e = taskFind(task);
    if (e) return e;
    if (s_taskCount >= ITS_MAX_TASKS) return nullptr;
    e = &s_tasks[s_taskCount++];
    e->task = task;
    e->server = nullptr;
    e->client = nullptr;
    e->onAux = nullptr;
    e->inbox = xMessageBufferCreate(ITS_INBOX_SIZE);
    e->inboxReady = xSemaphoreCreateBinary();
    xSemaphoreGive(e->inboxReady);
    return e;
}

static its_task_entry_t* myEntry() {
    return taskFind(xTaskGetCurrentTaskHandle());
}

static ItsServer* myServer() {
    its_task_entry_t* e = myEntry();
    return e ? e->server : nullptr;
}

static ItsClient* myClient() {
    its_task_entry_t* e = myEntry();
    return e ? e->client : nullptr;
}

/* ---- Inbox helpers ---- */

static bool inboxSend(its_task_entry_t* target, const void* data, size_t len,
                      TickType_t timeout) {
    if (xSemaphoreTake(target->inboxReady, timeout) != pdTRUE) return false;
    size_t sent = xMessageBufferSend(target->inbox, data, len, timeout);
    if (sent == 0) {
        xSemaphoreGive(target->inboxReady);
        return false;
    }
    xTaskNotifyGive(target->task);
    return true;
}

/* ---- Handle resolution ---- */

static its_client_conn_t* clientConn(ItsClient* cli, int handle) {
    int idx = handle - ITS_CLIENT_BASE;
    if (!cli || idx < 0 || idx >= cli->maxConns) return nullptr;
    its_client_conn_t* c = &cli->conns[idx];
    if (!c->active) return nullptr;
    if (!c->slot->active) {
        c->active = false;
        c->slot = nullptr;
        c->server = nullptr;
        return nullptr;
    }
    return c;
}

static StreamBufferHandle_t sendBuf(int handle) {
    if (handle >= ITS_CLIENT_BASE) {
        its_client_conn_t* c = clientConn(myClient(), handle);
        return c ? c->slot->toServer : nullptr;
    }
    ItsServer* s = myServer();
    if (!s || handle < 0 || handle >= s->maxHandles) return nullptr;
    return s->slots[handle].active ? s->slots[handle].fromServer : nullptr;
}

static StreamBufferHandle_t recvBuf(int handle) {
    if (handle >= ITS_CLIENT_BASE) {
        its_client_conn_t* c = clientConn(myClient(), handle);
        return c ? c->slot->fromServer : nullptr;
    }
    ItsServer* s = myServer();
    if (!s || handle < 0 || handle >= s->maxHandles) return nullptr;
    return s->slots[handle].active ? s->slots[handle].toServer : nullptr;
}

static TaskHandle_t remoteTask(int handle) {
    if (handle >= ITS_CLIENT_BASE) {
        its_client_conn_t* c = clientConn(myClient(), handle);
        return c ? c->server : nullptr;
    }
    ItsServer* s = myServer();
    if (!s || handle < 0 || handle >= s->maxHandles) return nullptr;
    return s->slots[handle].active ? s->slots[handle].client : nullptr;
}

/* ---- itsPoll — single inbox dispatcher ---- */

bool itsPoll(void) {
    its_task_entry_t* entry = myEntry();
    if (!entry) return false;

    uint8_t buf[sizeof(its_header_t) + ITS_MAX_MSG_DATA];
    size_t n = xMessageBufferReceive(entry->inbox, buf, sizeof(buf), 0);
    if (n < sizeof(its_header_t)) return false;

    auto* hdr = (its_header_t*)buf;
    void* payload = buf + sizeof(its_header_t);

    if (hdr->msg == ITS_MSG_CONNECT && entry->server) {
        ItsServer* srv = entry->server;

        /* Find free slot */
        int slot = -1;
        for (int i = 0; i < srv->maxHandles; i++) {
            if (!srv->slots[i].active) { slot = i; break; }
        }

        /* All slots busy — ask server if it wants to evict */
        if (slot < 0 && srv->onBusy) {
            if (!srv->onBusy(payload, hdr->len)) {
                /* Server freed a slot — retry */
                for (int i = 0; i < srv->maxHandles; i++) {
                    if (!srv->slots[i].active) { slot = i; break; }
                }
            }
        }

        /* Find client for ACK */
        its_task_entry_t* ce = taskFind(hdr->sender);
        ItsClient* cli = ce ? ce->client : nullptr;

        bool accepted = false;
        if (slot >= 0 && cli) {
            accepted = !srv->onConnect ||
                       srv->onConnect(slot, payload, hdr->len);
        }

        if (accepted) {
            its_slot_t* s = &srv->slots[slot];
            s->active = true;
            s->client = hdr->sender;
            if (s->toServer) xStreamBufferReset(s->toServer);
            if (s->fromServer) xStreamBufferReset(s->fromServer);
            cli->ackSlot = s;
        } else if (cli) {
            cli->ackSlot = nullptr;
        }
        if (cli) xSemaphoreGive(cli->ackSem);

    } else if (hdr->msg == ITS_MSG_DISCONNECT && entry->server) {
        ItsServer* srv = entry->server;
        for (int i = 0; i < srv->maxHandles; i++) {
            if (srv->slots[i].active && srv->slots[i].client == hdr->sender) {
                srv->slots[i].active = false;
                srv->slots[i].client = nullptr;
                if (srv->slots[i].toServer) xStreamBufferReset(srv->slots[i].toServer);
                if (srv->slots[i].fromServer) xStreamBufferReset(srv->slots[i].fromServer);
                if (srv->onDisconnect) srv->onDisconnect(i);
                break;
            }
        }

    } else if (hdr->msg == ITS_MSG_AUX) {
        if (entry->onAux) entry->onAux(hdr->sender, payload, hdr->len);

    }

    xSemaphoreGive(entry->inboxReady);
    return true;
}

/* ---- Server API ---- */

bool itsServerInit(int maxHandles, size_t toSize, size_t fromSize,
                   its_connect_cb_t onConnect,
                   its_busy_cb_t onBusy,
                   its_disconnect_cb_t onDisconnect,
                   its_aux_cb_t onAux) {
    TaskHandle_t me = xTaskGetCurrentTaskHandle();
    its_task_entry_t* entry = taskFindOrCreate(me);
    if (!entry) return false;
    if (entry->server) return false;

    auto* srv = (ItsServer*)heap_caps_calloc(1, sizeof(ItsServer), MALLOC_CAP_DEFAULT);
    if (!srv) return false;

    srv->slots = (its_slot_t*)heap_caps_calloc(maxHandles, sizeof(its_slot_t), MALLOC_CAP_DEFAULT);
    if (!srv->slots) { free(srv); return false; }

    srv->maxHandles = maxHandles;
    srv->task = me;
    srv->toSize = toSize;
    srv->fromSize = fromSize;
    srv->onConnect = onConnect;
    srv->onBusy = onBusy;
    srv->onDisconnect = onDisconnect;

    for (int i = 0; i < maxHandles; i++) {
        its_slot_t* slot = &srv->slots[i];
        slot->active = false;
        slot->client = nullptr;
        if (toSize > 0)
            slot->toServer = xStreamBufferCreateWithCaps(toSize, 1, MALLOC_CAP_SPIRAM);
        if (fromSize > 0)
            slot->fromServer = xStreamBufferCreateWithCaps(fromSize, 1, MALLOC_CAP_SPIRAM);
    }

    entry->server = srv;
    entry->onAux = onAux;
    return true;
}

void itsServerKick(int handle) {
    ItsServer* srv = myServer();
    if (!srv || handle < 0 || handle >= srv->maxHandles) return;

    its_slot_t* slot = &srv->slots[handle];
    if (!slot->active) return;

    TaskHandle_t client = slot->client;
    slot->active = false;
    slot->client = nullptr;
    if (slot->toServer) xStreamBufferReset(slot->toServer);
    if (slot->fromServer) xStreamBufferReset(slot->fromServer);

    if (client) xTaskNotifyGive(client);
}

int itsServerActive(void) {
    ItsServer* srv = myServer();
    if (!srv) return 0;
    int count = 0;
    for (int i = 0; i < srv->maxHandles; i++)
        if (srv->slots[i].active) count++;
    return count;
}

/* ---- Client API ---- */

void itsClientInit(int maxConns,
                   its_disconnect_cb_t onDisconnect,
                   its_aux_cb_t onAux) {
    TaskHandle_t me = xTaskGetCurrentTaskHandle();
    its_task_entry_t* entry = taskFindOrCreate(me);
    if (!entry || entry->client) return;

    auto* cli = (ItsClient*)heap_caps_calloc(1, sizeof(ItsClient), MALLOC_CAP_DEFAULT);
    if (!cli) return;

    cli->conns = (its_client_conn_t*)heap_caps_calloc(maxConns, sizeof(its_client_conn_t), MALLOC_CAP_DEFAULT);
    if (!cli->conns) { free(cli); return; }

    cli->maxConns = maxConns;
    cli->task = me;
    cli->ackSem = xSemaphoreCreateBinary();
    cli->ackSlot = nullptr;
    cli->onDisconnect = onDisconnect;

    entry->client = cli;
    entry->onAux = onAux;
}

int itsConnectByHandle(TaskHandle_t serverTask,
                       const void* data, size_t dataLen, TickType_t timeout) {
    ItsClient* cli = myClient();
    if (!cli) return -1;

    int idx = -1;
    for (int i = 0; i < cli->maxConns; i++) {
        if (!cli->conns[i].active) { idx = i; break; }
    }
    if (idx < 0) return -1;

    its_task_entry_t* se = taskFind(serverTask);
    if (!se || !se->server) return -1;

    xSemaphoreTake(cli->ackSem, 0);
    cli->ackSlot = nullptr;

    if (dataLen > ITS_MAX_MSG_DATA) dataLen = ITS_MAX_MSG_DATA;
    uint8_t buf[sizeof(its_header_t) + ITS_MAX_MSG_DATA];
    auto* hdr = (its_header_t*)buf;
    hdr->sender = cli->task;
    hdr->msg = ITS_MSG_CONNECT;
    hdr->_pad = 0;
    hdr->len = dataLen;
    if (data && dataLen > 0)
        memcpy(buf + sizeof(its_header_t), data, dataLen);

    if (!inboxSend(se, buf, sizeof(its_header_t) + dataLen, timeout))
        return -1;

    if (xSemaphoreTake(cli->ackSem, timeout) != pdTRUE) return -1;
    if (!cli->ackSlot) return -1;

    cli->conns[idx].active = true;
    cli->conns[idx].slot = cli->ackSlot;
    cli->conns[idx].server = serverTask;
    cli->ackSlot = nullptr;

    return ITS_CLIENT_BASE + idx;
}

int itsConnect(const char* serverName,
               const void* data, size_t dataLen, TickType_t timeout) {
    TaskHandle_t task = xTaskGetHandle(serverName);
    if (!task) return -1;
    return itsConnectByHandle(task, data, dataLen, timeout);
}

void itsDisconnect(int handle) {
    ItsClient* cli = myClient();
    if (!cli) return;
    int idx = handle - ITS_CLIENT_BASE;
    if (idx < 0 || idx >= cli->maxConns) return;
    if (!cli->conns[idx].active) return;

    TaskHandle_t serverTask = cli->conns[idx].server;

    cli->conns[idx].active = false;
    cli->conns[idx].slot = nullptr;
    cli->conns[idx].server = nullptr;

    its_task_entry_t* se = taskFind(serverTask);
    if (se) {
        its_header_t hdr = {};
        hdr.sender = xTaskGetCurrentTaskHandle();
        hdr.msg = ITS_MSG_DISCONNECT;
        hdr.len = 0;
        inboxSend(se, &hdr, sizeof(hdr), pdMS_TO_TICKS(100));
    }
}

/* ---- Aux messages ---- */

bool itsSendAuxByHandle(TaskHandle_t task,
                        const void* data, size_t dataLen, TickType_t timeout) {
    its_task_entry_t* te = taskFind(task);
    if (!te) return false;
    if (dataLen > ITS_MAX_MSG_DATA) dataLen = ITS_MAX_MSG_DATA;

    uint8_t buf[sizeof(its_header_t) + ITS_MAX_MSG_DATA];
    auto* hdr = (its_header_t*)buf;
    hdr->sender = xTaskGetCurrentTaskHandle();
    hdr->msg = ITS_MSG_AUX;
    hdr->_pad = 0;
    hdr->len = dataLen;
    if (data && dataLen > 0)
        memcpy(buf + sizeof(its_header_t), data, dataLen);

    return inboxSend(te, buf, sizeof(its_header_t) + dataLen, timeout);
}

bool itsSendAux(const char* taskName,
                const void* data, size_t dataLen, TickType_t timeout) {
    TaskHandle_t task = xTaskGetHandle(taskName);
    if (!task) return false;
    return itsSendAuxByHandle(task, data, dataLen, timeout);
}

/* ---- Data API ---- */

size_t itsSend(int handle, const void* data, size_t len, TickType_t timeout) {
    StreamBufferHandle_t buf = sendBuf(handle);
    if (!buf) return 0;
    size_t sent = xStreamBufferSend(buf, data, len, timeout);
    TaskHandle_t remote = remoteTask(handle);
    if (remote && sent > 0) xTaskNotifyGive(remote);
    return sent;
}

size_t itsRecv(int handle, void* buf, size_t maxLen, TickType_t timeout) {
    StreamBufferHandle_t sb = recvBuf(handle);
    if (!sb) return 0;
    return xStreamBufferReceive(sb, buf, maxLen, timeout);
}

bool itsConnected(int handle) {
    if (handle >= ITS_CLIENT_BASE) {
        return clientConn(myClient(), handle) != nullptr;
    }
    ItsServer* s = myServer();
    if (!s || handle < 0 || handle >= s->maxHandles) return false;
    return s->slots[handle].active;
}

size_t itsBytesAvailable(int handle) {
    StreamBufferHandle_t buf = recvBuf(handle);
    return buf ? xStreamBufferBytesAvailable(buf) : 0;
}

size_t itsSpacesAvailable(int handle) {
    StreamBufferHandle_t buf = sendBuf(handle);
    return buf ? xStreamBufferSpacesAvailable(buf) : 0;
}

bool itsSetTriggerLevel(int handle, size_t triggerLevel) {
    StreamBufferHandle_t buf = recvBuf(handle);
    return buf ? xStreamBufferSetTriggerLevel(buf, triggerLevel) : false;
}

bool itsIsEmpty(int handle) {
    StreamBufferHandle_t buf = recvBuf(handle);
    return buf ? xStreamBufferIsEmpty(buf) : true;
}

bool itsIsFull(int handle) {
    StreamBufferHandle_t buf = sendBuf(handle);
    return buf ? xStreamBufferIsFull(buf) : true;
}

bool itsReset(int handle) {
    StreamBufferHandle_t sb;
    sb = recvBuf(handle);
    if (sb) xStreamBufferReset(sb);
    sb = sendBuf(handle);
    if (sb) xStreamBufferReset(sb);
    return true;
}
