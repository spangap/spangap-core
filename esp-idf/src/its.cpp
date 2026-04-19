#include "its.h"
#include "esp_log.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include <string.h>

static const char* TAG = "its";

/* Prepend the calling task's name to every error log so consumers without
 * a custom log reformatter still see WHO triggered the ITS error. ITS by
 * definition runs on the stack of whichever task is using it, so the
 * caller-task identity is the most important breadcrumb. */
#define ITS_LOGE(fmt, ...) \
    ESP_LOGE(TAG, "[%s] " fmt, pcTaskGetName(NULL), ##__VA_ARGS__)
#define ITS_LOGW(fmt, ...) \
    ESP_LOGW(TAG, "[%s] " fmt, pcTaskGetName(NULL), ##__VA_ARGS__)

static inline uint32_t millis() { return (uint32_t)(esp_timer_get_time() / 1000); }

/* ---- Stream buffer pool ---- */

#define ITS_MAX_POOL 32

struct its_pool_entry_t {
    StreamBufferHandle_t handle;
    size_t               size;
    size_t               triggerLevel;   /* notify-on-send threshold (bytes) */
    volatile bool        inUse;
    /* Packet-mode flow control. senderWaiting > 0 means a packet sender is
     * blocked on spaceFreedSem because spaces_available was less than
     * senderWaiting. The receiver gives the sem after consuming a packet if
     * spaces_available has reached senderWaiting. */
    volatile size_t      senderWaiting;
    SemaphoreHandle_t    spaceFreedSem;
};

static its_pool_entry_t itsPool[ITS_MAX_POOL];
static int              itsPoolCount = 0;
static portMUX_TYPE     itsPoolMux = portMUX_INITIALIZER_UNLOCKED;

void itsReserveStreams(int count, size_t size) {
    for (int i = 0; i < count && itsPoolCount < ITS_MAX_POOL; i++) {
        auto& e = itsPool[itsPoolCount++];
        e.handle = xStreamBufferCreateWithCaps(size, 1, MALLOC_CAP_SPIRAM);
        e.size = size;
        e.triggerLevel = 1;
        e.inUse = false;
        e.senderWaiting = 0;
        e.spaceFreedSem = xSemaphoreCreateBinary();
    }
}

static int poolGet(size_t minSize) {
    if (minSize == 0) return -1;
    portENTER_CRITICAL(&itsPoolMux);
    int best = -1;
    size_t bestSize = SIZE_MAX;
    for (int i = 0; i < itsPoolCount; i++) {
        if (!itsPool[i].inUse && itsPool[i].size >= minSize && itsPool[i].size < bestSize) {
            best = i;
            bestSize = itsPool[i].size;
        }
    }
    if (best >= 0) {
        itsPool[best].inUse = true;
        itsPool[best].triggerLevel = 1;
    }
    portEXIT_CRITICAL(&itsPoolMux);
    if (best >= 0) {
        xStreamBufferReset(itsPool[best].handle);
        xStreamBufferSetTriggerLevel(itsPool[best].handle, 1);
    }
    return best;
}

static void poolFree(int idx) {
    if (idx < 0 || idx >= itsPoolCount) return;
    xStreamBufferReset(itsPool[idx].handle);
    xStreamBufferSetTriggerLevel(itsPool[idx].handle, 1);
    portENTER_CRITICAL(&itsPoolMux);
    itsPool[idx].inUse = false;
    itsPool[idx].triggerLevel = 1;
    portEXIT_CRITICAL(&itsPoolMux);
    /* Wake any sender that was blocked waiting for space on this buffer —
       the connection is gone, so they should abort rather than keep waiting
       forever. itsSend re-checks the connection after the sem fires. */
    xSemaphoreGive(itsPool[idx].spaceFreedSem);
}

static StreamBufferHandle_t poolHandle(int idx) {
    if (idx < 0 || idx >= itsPoolCount) return nullptr;
    return itsPool[idx].handle;
}

/* ---- Pickup semaphore pool ---- */

#define ITS_MAX_PICKUP 16

static SemaphoreHandle_t pickupSems[ITS_MAX_PICKUP];
static volatile uint32_t pickupStamps[ITS_MAX_PICKUP];   /* 0 = free, else millis() */

static void pickupInit() {
    for (int i = 0; i < ITS_MAX_PICKUP; i++) {
        pickupSems[i] = xSemaphoreCreateBinary();
        pickupStamps[i] = 0;
    }
}

static int pickupAcquire() {
    for (int i = 0; i < ITS_MAX_PICKUP; i++) {
        if (pickupStamps[i] == 0) {
            xSemaphoreTake(pickupSems[i], 0);
            pickupStamps[i] = millis() | 1;
            return i;
        }
    }
    int oldest = 0;
    uint32_t oldestStamp = pickupStamps[0];
    for (int i = 1; i < ITS_MAX_PICKUP; i++) {
        if (pickupStamps[i] && pickupStamps[i] < oldestStamp) {
            oldest = i;
            oldestStamp = pickupStamps[i];
        }
    }
    xSemaphoreTake(pickupSems[oldest], 0);
    pickupStamps[oldest] = millis() | 1;
    return oldest;
}

static void pickupRelease(int idx) {
    if (idx < 0 || idx >= ITS_MAX_PICKUP) return;
    xSemaphoreGive(pickupSems[idx]);
    pickupStamps[idx] = 0;
}

static bool pickupWait(int idx, TickType_t timeout) {
    if (idx < 0 || idx >= ITS_MAX_PICKUP) return false;
    bool ok = xSemaphoreTake(pickupSems[idx], timeout) == pdTRUE;
    pickupStamps[idx] = 0;
    return ok;
}

/* ---- Global connection table ---- */

#define ITS_MAX_CONNS 64

struct its_conn_t {
    volatile bool       active;
    bool                packetBased;
    TaskHandle_t        clientTask;
    TaskHandle_t        serverTask;
    int                 toPoolIdx;
    int                 fromPoolIdx;
    uint16_t            itsPort;
    int8_t              clientRef;
    int8_t              serverRef;
    /* Per-connection client-side callbacks. Set by itsConnect, used by
     * the client task only. The server side uses per-port callbacks. */
    its_recv_cb_t       cliRecvCb;
    its_disconnect_cb_t cliDisconnectCb;
};

static its_conn_t    connTable[ITS_MAX_CONNS];
static int           connCounter = 0;
static portMUX_TYPE  connMux = portMUX_INITIALIZER_UNLOCKED;

static int connAlloc() {
    portENTER_CRITICAL(&connMux);
    int start = connCounter;
    for (int i = 0; i < ITS_MAX_CONNS; i++) {
        int idx = (start + i) % ITS_MAX_CONNS;
        if (!connTable[idx].active) {
            connTable[idx] = {};
            connTable[idx].active = true;
            connTable[idx].toPoolIdx = -1;
            connTable[idx].fromPoolIdx = -1;
            connTable[idx].clientRef = -1;
            connTable[idx].serverRef = -1;
            connCounter = (idx + 1) % ITS_MAX_CONNS;
            portEXIT_CRITICAL(&connMux);
            return idx;
        }
    }
    portEXIT_CRITICAL(&connMux);
    return -1;
}

static void connFree(int handle) {
    if (handle < 0 || handle >= ITS_MAX_CONNS) return;
    its_conn_t* c = &connTable[handle];
    poolFree(c->toPoolIdx);
    poolFree(c->fromPoolIdx);
    portENTER_CRITICAL(&connMux);
    *c = {};
    c->toPoolIdx = -1;
    c->fromPoolIdx = -1;
    portEXIT_CRITICAL(&connMux);
}

static its_conn_t* conn(int handle) {
    if (handle < 0 || handle >= ITS_MAX_CONNS) return nullptr;
    return connTable[handle].active ? &connTable[handle] : nullptr;
}

/* ---- Signaling protocol ---- */

enum {
    ITS_MSG_CONNECT,
    ITS_MSG_DISCONNECT,
    ITS_MSG_AUX,
    ITS_MSG_FORWARD,
};

typedef struct {
    TaskHandle_t sender;
    uint8_t      msg;
    int8_t       pickupIdx;
    uint16_t     itsPort;
    uint16_t     len;
    int16_t      handle;
} its_header_t;

#define ITS_DEFAULT_INBOX_SIZE  (sizeof(its_header_t) + ITS_MAX_MSG_DATA + 4)

/* ---- Per-task registry ---- */

#define ITS_MAX_TASKS  24

struct its_aux_entry_t {
    bool         active;
    uint16_t     port;
    its_aux_cb_t cb;
};

struct its_server_port_t {
    bool                  active;
    bool                  packetBased;
    uint16_t              port;
    int                   maxHandles;
    size_t                toSize;
    size_t                fromSize;
    its_connect_cb_t      onConnect;
    its_busy_cb_t         onBusy;
    its_disconnect_cb_t   onDisconnect;
    its_recv_cb_t         onRecv;
};

struct its_task_t {
    TaskHandle_t          task;

    /* Server side */
    its_server_port_t     serverPorts[ITS_MAX_PORTS];
    int                   serverPortCount;

    /* Client side */
    int                   maxConns;
    SemaphoreHandle_t     ackSem;
    int                   ackHandle;

    /* Aux callbacks (per-task, per-port) */
    its_aux_entry_t       auxCallbacks[ITS_MAX_PORTS];
    int                   auxCount;

    /* Inbox */
    QueueHandle_t         inbox;
    size_t                inboxItemSize;

    bool                  isServer;
    bool                  isClient;
};

static its_task_t s_tasks[ITS_MAX_TASKS];
static int        s_taskCount = 0;
static bool       pickupInited = false;

static its_task_t* taskFind(TaskHandle_t task) {
    for (int i = 0; i < s_taskCount; i++)
        if (s_tasks[i].task == task) return &s_tasks[i];
    return nullptr;
}

static its_task_t* taskFindOrCreate(TaskHandle_t task, size_t inboxMaxMsgLen, size_t inboxDepth) {
    its_task_t* e = taskFind(task);
    if (e) return e;
    if (s_taskCount >= ITS_MAX_TASKS) {
        ITS_LOGE("task table full (max %d)", ITS_MAX_TASKS);
        return nullptr;
    }
    if (!pickupInited) { pickupInit(); pickupInited = true; }

    e = &s_tasks[s_taskCount++];
    memset(e, 0, sizeof(*e));
    e->task = task;
    e->ackHandle = -1;

    size_t itemSize = inboxMaxMsgLen > 0 ? (sizeof(its_header_t) + inboxMaxMsgLen)
                                         : ITS_DEFAULT_INBOX_SIZE;
    int depth = inboxDepth > 0 ? (int)inboxDepth : 8;
    e->inboxItemSize = itemSize;
    e->inbox = xQueueCreate(depth, itemSize);
    return e;
}

static its_task_t* myTask() {
    return taskFind(xTaskGetCurrentTaskHandle());
}

static its_server_port_t* portFind(its_task_t* t, uint16_t port) {
    for (int i = 0; i < t->serverPortCount; i++)
        if (t->serverPorts[i].active && t->serverPorts[i].port == port)
            return &t->serverPorts[i];
    return nullptr;
}

/* ---- Inbox helpers ---- */

static bool inboxSend(its_task_t* target, const void* data, size_t len,
                      TickType_t timeout) {
    if (!target->inbox || len > target->inboxItemSize) return false;
    /* xQueueSend reads exactly inboxItemSize bytes. Pad if needed. */
    const void* item = data;
    uint8_t* padBuf = nullptr;
    if (len < target->inboxItemSize) {
        padBuf = (uint8_t*)alloca(target->inboxItemSize);
        memcpy(padBuf, data, len);
        item = padBuf;
    }
    if (xQueueSend(target->inbox, item, timeout) != pdTRUE) return false;
    xTaskNotifyGive(target->task);
    return true;
}

/* ---- Handle resolution ---- */

static StreamBufferHandle_t sendBufWithPool(int handle, its_pool_entry_t** outPe) {
    its_conn_t* c = conn(handle);
    if (!c) return nullptr;
    int idx = -1;
    TaskHandle_t me = xTaskGetCurrentTaskHandle();
    if (me == c->clientTask) idx = c->toPoolIdx;
    else if (me == c->serverTask) idx = c->fromPoolIdx;
    if (idx < 0 || idx >= itsPoolCount) return nullptr;
    if (outPe) *outPe = &itsPool[idx];
    return itsPool[idx].handle;
}

static StreamBufferHandle_t sendBuf(int handle) {
    return sendBufWithPool(handle, nullptr);
}

static StreamBufferHandle_t recvBufWithPool(int handle, its_pool_entry_t** outPe) {
    its_conn_t* c = conn(handle);
    if (!c) return nullptr;
    int idx = -1;
    TaskHandle_t me = xTaskGetCurrentTaskHandle();
    if (me == c->clientTask) idx = c->fromPoolIdx;
    else if (me == c->serverTask) idx = c->toPoolIdx;
    if (idx < 0 || idx >= itsPoolCount) return nullptr;
    if (outPe) *outPe = &itsPool[idx];
    return itsPool[idx].handle;
}

static StreamBufferHandle_t recvBuf(int handle) {
    return recvBufWithPool(handle, nullptr);
}

static TaskHandle_t remoteOf(int handle) {
    its_conn_t* c = conn(handle);
    if (!c) return nullptr;
    TaskHandle_t me = xTaskGetCurrentTaskHandle();
    if (me == c->clientTask) return c->serverTask;
    if (me == c->serverTask) return c->clientTask;
    return nullptr;
}

static int serverPortActiveCount(its_task_t* t, uint16_t port) {
    int count = 0;
    for (int i = 0; i < ITS_MAX_CONNS; i++)
        if (connTable[i].active && connTable[i].serverTask == t->task
            && connTable[i].itsPort == port)
            count++;
    return count;
}

/* ---- Inbox message dispatch ---- */

static void processInboxMsg(its_task_t* me, uint8_t* buf) {
    auto* hdr = (its_header_t*)buf;
    void* payload = buf + sizeof(its_header_t);
    int pickupIdx = hdr->pickupIdx;

    if (hdr->msg == ITS_MSG_CONNECT && me->isServer) {
        its_server_port_t* sp = portFind(me, hdr->itsPort);
        if (!sp) {
            ITS_LOGE("connect to unopened port %u (from [%s])",
                     hdr->itsPort, pcTaskGetName(hdr->sender));
            its_task_t* cli = taskFind(hdr->sender);
            if (cli) { cli->ackHandle = -1; xSemaphoreGive(cli->ackSem); }
            goto done;
        }

        bool full = serverPortActiveCount(me, hdr->itsPort) >= sp->maxHandles;
        if (full && sp->onBusy) {
            if (!sp->onBusy(payload, hdr->len))
                full = serverPortActiveCount(me, hdr->itsPort) >= sp->maxHandles;
        }

        its_task_t* cli = taskFind(hdr->sender);

        int handle = -1;
        if (!full && cli) handle = connAlloc();

        bool accepted = false;
        if (handle >= 0) {
            its_conn_t* c = &connTable[handle];
            c->clientTask = hdr->sender;
            c->serverTask = me->task;
            c->itsPort = hdr->itsPort;
            c->packetBased = sp->packetBased;
            if (sp->toSize > 0)   c->toPoolIdx = poolGet(sp->toSize);
            if (sp->fromSize > 0) c->fromPoolIdx = poolGet(sp->fromSize);

            if (!sp->onConnect) {
                accepted = true;
            } else {
                int sRef = sp->onConnect(handle, payload, hdr->len);
                accepted = sRef >= 0;
                if (accepted) c->serverRef = (int8_t)sRef;
            }
            if (!accepted) connFree(handle);
        }

        if (cli) {
            cli->ackHandle = accepted ? handle : -1;
            xSemaphoreGive(cli->ackSem);
        }

    } else if (hdr->msg == ITS_MSG_DISCONNECT && me->isServer) {
        int handle = hdr->handle;
        int8_t ref = -1;
        uint16_t port = hdr->itsPort;
        its_conn_t* c = conn(handle);
        if (c && c->serverTask == me->task && c->clientTask == hdr->sender) {
            ref = c->serverRef;
            port = c->itsPort;
            connFree(handle);
        } else if (hdr->len >= 1) {
            ref = (int8_t)((uint8_t*)payload)[0];
        }
        its_server_port_t* sp = portFind(me, port);
        if (sp && sp->onDisconnect && ref >= 0) sp->onDisconnect(ref);

    } else if (hdr->msg == ITS_MSG_DISCONNECT && me->isClient) {
        int handle = hdr->handle;
        int8_t ref = -1;
        its_disconnect_cb_t cb = nullptr;
        its_conn_t* c = conn(handle);
        if (c && c->clientTask == me->task && c->serverTask == hdr->sender) {
            /* Conn still exists — read cb from connection table */
            ref = c->clientRef;
            cb = c->cliDisconnectCb;
            connFree(handle);
        } else if (hdr->len >= 1 + sizeof(cb)) {
            /* Conn already freed (server disconnected) — cb embedded in payload */
            ref = (int8_t)((uint8_t*)payload)[0];
            memcpy(&cb, (uint8_t*)payload + 1, sizeof(cb));
        } else if (hdr->len >= 1) {
            ref = (int8_t)((uint8_t*)payload)[0];
        }
        if (ref >= 0 && cb) cb(ref);

    } else if (hdr->msg == ITS_MSG_FORWARD && me->isServer) {
        int handle = hdr->handle;
        its_conn_t* c = conn(handle);
        if (c && c->serverTask == me->task) {
            its_server_port_t* sp = portFind(me, hdr->itsPort);
            if (!sp) {
                ITS_LOGE("forwarded conn arrived for unopened port %u (from [%s])",
                         hdr->itsPort, pcTaskGetName(hdr->sender));
                itsDisconnect(handle);
            } else {
                c->packetBased = sp->packetBased;
                if (sp->onConnect && sp->onConnect(handle, payload, hdr->len) < 0)
                    itsDisconnect(handle);
            }
        }

    } else if (hdr->msg == ITS_MSG_AUX) {
        its_aux_cb_t cb = nullptr;
        for (int i = 0; i < me->auxCount; i++) {
            if (me->auxCallbacks[i].active && me->auxCallbacks[i].port == hdr->itsPort) {
                cb = me->auxCallbacks[i].cb;
                break;
            }
        }
        if (cb) cb(hdr->sender, payload, hdr->len);
        else ITS_LOGE("aux to unregistered port %u (from [%s])",
                      hdr->itsPort, pcTaskGetName(hdr->sender));
    }

done:
    if (pickupIdx >= 0) pickupRelease(pickupIdx);
}

/* ---- Receive callback dispatch ---- */

static bool dispatchRecvCallbacks(its_task_t* me) {
    bool any = false;
    for (int i = 0; i < ITS_MAX_CONNS; i++) {
        its_conn_t* c = &connTable[i];
        if (!c->active) continue;
        bool isServer = (c->serverTask == me->task);
        bool isClient = (c->clientTask == me->task);
        if (!isServer && !isClient) continue;

        its_recv_cb_t cb = nullptr;
        StreamBufferHandle_t buf = nullptr;
        if (isServer) {
            its_server_port_t* sp = portFind(me, c->itsPort);
            if (sp) cb = sp->onRecv;
            buf = poolHandle(c->toPoolIdx);
        } else {
            cb = c->cliRecvCb;
            buf = poolHandle(c->fromPoolIdx);
        }
        if (!cb || !buf) continue;
        size_t avail = xStreamBufferBytesAvailable(buf);
        /* Packet mode: avail <= 4 is either empty or a lone in-flight header;
         * avail > 4 guarantees at least one complete packet at the head. */
        size_t threshold = c->packetBased ? 4 : 0;
        if (avail > threshold) { cb(i, avail); any = true; }
    }
    return any;
}

/* ---- itsPoll ---- */

bool itsPoll(TickType_t timeout) {
    its_task_t* me = myTask();
    if (!me) {
        if (timeout > 0) ulTaskNotifyTake(pdTRUE, timeout);
        return false;
    }

    uint8_t* buf = (uint8_t*)alloca(me->inboxItemSize);
    bool any = false;

    if (xQueueReceive(me->inbox, buf, 0) == pdTRUE) {
        processInboxMsg(me, buf);
        any = true;
    }

    if (dispatchRecvCallbacks(me)) any = true;

    if (any) return true;
    if (timeout == 0) return false;

    /* Block until notification, then retry once */
    ulTaskNotifyTake(pdTRUE, timeout);

    if (xQueueReceive(me->inbox, buf, 0) == pdTRUE) {
        processInboxMsg(me, buf);
        any = true;
    }
    if (dispatchRecvCallbacks(me)) any = true;
    return any;
}

/* ---- Server API ---- */

bool itsServerInit(size_t inboxMaxMsgLen, size_t inboxDepth) {
    TaskHandle_t me = xTaskGetCurrentTaskHandle();
    its_task_t* entry = taskFindOrCreate(me, inboxMaxMsgLen, inboxDepth);
    if (!entry || entry->isServer) return false;
    entry->isServer = true;
    return true;
}

bool itsServerPortOpen(uint16_t port, bool packetBased, int maxHandles,
                       size_t toSize, size_t fromSize) {
    its_task_t* e = myTask();
    if (!e || !e->isServer) {
        ITS_LOGE("PortOpen on non-server task");
        return false;
    }
    its_server_port_t* sp = portFind(e, port);
    if (sp) {
        sp->packetBased = packetBased;
        sp->maxHandles = maxHandles;
        sp->toSize = toSize;
        sp->fromSize = fromSize;
        return true;
    }
    for (int i = 0; i < ITS_MAX_PORTS; i++) {
        if (!e->serverPorts[i].active) {
            e->serverPorts[i] = {};
            e->serverPorts[i].active = true;
            e->serverPorts[i].packetBased = packetBased;
            e->serverPorts[i].port = port;
            e->serverPorts[i].maxHandles = maxHandles;
            e->serverPorts[i].toSize = toSize;
            e->serverPorts[i].fromSize = fromSize;
            if (i + 1 > e->serverPortCount) e->serverPortCount = i + 1;
            return true;
        }
    }
    ITS_LOGE("PortOpen %u: no free port slot (max %d)", port, ITS_MAX_PORTS);
    return false;
}

void itsServerPortClose(uint16_t port) {
    its_task_t* e = myTask();
    if (!e) return;
    its_server_port_t* sp = portFind(e, port);
    if (!sp) return;
    /* Disconnect existing connections on this port */
    for (int i = 0; i < ITS_MAX_CONNS; i++) {
        if (connTable[i].active && connTable[i].serverTask == e->task
            && connTable[i].itsPort == port) {
            itsDisconnect(i);
        }
    }
    sp->active = false;
    sp->onConnect = nullptr;
    sp->onBusy = nullptr;
    sp->onDisconnect = nullptr;
    sp->onRecv = nullptr;
}

void itsServerOnConnect(uint16_t port, its_connect_cb_t cb) {
    its_task_t* e = myTask();
    if (!e) return;
    its_server_port_t* sp = portFind(e, port);
    if (!sp) {
        ITS_LOGE("OnConnect for unopened port %u", port);
        return;
    }
    sp->onConnect = cb;
}

void itsServerOnBusy(uint16_t port, its_busy_cb_t cb) {
    its_task_t* e = myTask();
    if (!e) return;
    its_server_port_t* sp = portFind(e, port);
    if (!sp) {
        ITS_LOGE("OnBusy for unopened port %u", port);
        return;
    }
    sp->onBusy = cb;
}

void itsServerOnDisconnect(uint16_t port, its_disconnect_cb_t cb) {
    its_task_t* e = myTask();
    if (!e) return;
    its_server_port_t* sp = portFind(e, port);
    if (!sp) {
        ITS_LOGE("OnDisconnect for unopened port %u", port);
        return;
    }
    sp->onDisconnect = cb;
}

void itsServerOnRecv(uint16_t port, its_recv_cb_t cb) {
    its_task_t* e = myTask();
    if (!e) return;
    its_server_port_t* sp = portFind(e, port);
    if (!sp) {
        ITS_LOGE("OnRecv for unopened port %u", port);
        return;
    }
    sp->onRecv = cb;
}

void itsOnAux(uint16_t port, its_aux_cb_t cb) {
    its_task_t* e = myTask();
    if (!e) {
        e = taskFindOrCreate(xTaskGetCurrentTaskHandle(), 0, 0);
        if (!e) return;
    }
    /* Replace existing handler for same port (idempotent per port) */
    for (int i = 0; i < e->auxCount; i++) {
        if (e->auxCallbacks[i].active && e->auxCallbacks[i].port == port) {
            e->auxCallbacks[i].cb = cb;
            return;
        }
    }
    if (e->auxCount >= ITS_MAX_PORTS) {
        ITS_LOGE("OnAux %u: aux table full (max %d)", port, ITS_MAX_PORTS);
        return;
    }
    e->auxCallbacks[e->auxCount].active = true;
    e->auxCallbacks[e->auxCount].port = port;
    e->auxCallbacks[e->auxCount].cb = cb;
    e->auxCount++;
}

int itsServerActive(int port) {
    its_task_t* me = myTask();
    if (!me) return 0;
    int count = 0;
    for (int i = 0; i < ITS_MAX_CONNS; i++) {
        if (!connTable[i].active) continue;
        if (connTable[i].serverTask != me->task) continue;
        if (port >= 0 && connTable[i].itsPort != (uint16_t)port) continue;
        count++;
    }
    return count;
}

int itsActiveTotal(void) {
    int n = 0;
    for (int i = 0; i < ITS_MAX_CONNS; i++)
        if (connTable[i].active) n++;
    return n;
}

void itsStatus(void) {
    int active = 0;
    for (int i = 0; i < ITS_MAX_CONNS; i++)
        if (connTable[i].active) active++;

    ESP_LOGI(TAG, "ITS System Status Report");
    ESP_LOGI(TAG, "Connections (%d/%d in use)", active, ITS_MAX_CONNS);
    for (int i = 0; i < ITS_MAX_CONNS; i++) {
        its_conn_t* c = &connTable[i];
        if (!c->active) continue;
        const char* cn = c->clientTask ? pcTaskGetName(c->clientTask) : "?";
        const char* sn = c->serverTask ? pcTaskGetName(c->serverTask) : "?";
        ESP_LOGI(TAG, "    [%s] -> [%s:%u]", cn, sn, c->itsPort);
    }

    ESP_LOGI(TAG, "Streams");
    bool seen[ITS_MAX_POOL] = {};
    for (int i = 0; i < itsPoolCount; i++) {
        if (seen[i]) continue;
        size_t sz = itsPool[i].size;
        int total = 0, used = 0;
        for (int j = i; j < itsPoolCount; j++) {
            if (itsPool[j].size == sz) {
                seen[j] = true;
                total++;
                if (itsPool[j].inUse) used++;
            }
        }
        if (sz >= 1024)
            ESP_LOGI(TAG, "    %u kB (%d/%d in use)",
                     (unsigned)(sz / 1024), used, total);
        else
            ESP_LOGI(TAG, "    %u B (%d/%d in use)",
                     (unsigned)sz, used, total);
    }
}

int itsRef(int handle) {
    its_conn_t* c = conn(handle);
    if (!c) return -1;
    TaskHandle_t me = xTaskGetCurrentTaskHandle();
    if (c->serverTask == me) return c->serverRef;
    if (c->clientTask == me) return c->clientRef;
    return -1;
}

size_t itsInject(int handle, bool asServer, const void* data, size_t len,
                 TickType_t timeout) {
    its_conn_t* c = conn(handle);
    if (!c) return 0;
    int idx = asServer ? c->fromPoolIdx : c->toPoolIdx;
    if (idx < 0 || idx >= itsPoolCount) return 0;
    auto* pe = &itsPool[idx];
    if (!pe->handle) return 0;
    size_t sent = xStreamBufferSend(pe->handle, data, len, timeout);
    if (sent > 0) {
        size_t fill = xStreamBufferBytesAvailable(pe->handle);
        size_t trigger = pe->triggerLevel ? pe->triggerLevel : 1;
        if (fill >= trigger) {
            TaskHandle_t remote = asServer ? c->clientTask : c->serverTask;
            if (remote) xTaskNotifyGive(remote);
        }
    }
    return sent;
}

int itsServerForwardByTaskHandle(int handle, TaskHandle_t targetTask, uint16_t port,
                                 const void* data, size_t dataLen) {
    its_conn_t* c = conn(handle);
    if (!c || c->serverTask != xTaskGetCurrentTaskHandle()) return -1;

    its_task_t* te = taskFind(targetTask);
    if (!te || !te->isServer) return -1;

    its_server_port_t* sp = portFind(te, port);
    if (!sp) {
        ITS_LOGE("forward to unopened port %u on [%s]",
                 port, pcTaskGetName(targetTask));
        return -1;
    }

    auto isFull = [&]() {
        return serverPortActiveCount(te, port) >= sp->maxHandles;
    };

    if (isFull() && sp->onBusy) {
        if (sp->onBusy(data, dataLen)) return -1;
        if (isFull()) return -1;
    }

    c->serverTask = targetTask;
    c->itsPort = port;
    c->packetBased = sp->packetBased;

    size_t maxPayload = te->inboxItemSize > sizeof(its_header_t)
                      ? te->inboxItemSize - sizeof(its_header_t) : ITS_MAX_MSG_DATA;
    if (dataLen > maxPayload) {
        ITS_LOGE("forward data truncated: %u > %u",
                 (unsigned)dataLen, (unsigned)maxPayload);
        dataLen = maxPayload;
    }
    size_t totalLen = sizeof(its_header_t) + dataLen;
    uint8_t* buf = (uint8_t*)alloca(totalLen);
    auto* fhdr = (its_header_t*)buf;
    *fhdr = {};
    fhdr->sender = xTaskGetCurrentTaskHandle();
    fhdr->msg = ITS_MSG_FORWARD;
    fhdr->pickupIdx = -1;
    fhdr->itsPort = port;
    fhdr->len = (uint16_t)dataLen;
    fhdr->handle = (int16_t)handle;
    if (data && dataLen > 0)
        memcpy(buf + sizeof(its_header_t), data, dataLen);
    inboxSend(te, buf, totalLen, pdMS_TO_TICKS(100));

    return handle;
}

int itsServerForward(int handle, const char* targetServer, uint16_t port,
                     const void* data, size_t dataLen) {
    TaskHandle_t task = xTaskGetHandle(targetServer);
    if (!task) return -1;
    return itsServerForwardByTaskHandle(handle, task, port, data, dataLen);
}

int itsServerPort(int handle) {
    its_conn_t* c = conn(handle);
    if (!c || c->serverTask != xTaskGetCurrentTaskHandle()) return -1;
    return c->itsPort;
}

/* ---- Client API ---- */

void itsClientInit(int maxConns,
                   size_t inboxMaxMsgLen, size_t inboxDepth) {
    TaskHandle_t me = xTaskGetCurrentTaskHandle();
    its_task_t* entry = taskFindOrCreate(me, inboxMaxMsgLen, inboxDepth);
    if (!entry || entry->isClient) return;

    entry->maxConns = maxConns;
    entry->isClient = true;
    entry->ackSem = xSemaphoreCreateBinary();
    entry->ackHandle = -1;
}

int itsConnectByTaskHandle(TaskHandle_t serverTask, uint16_t port,
                           const void* data, size_t dataLen, TickType_t timeout,
                           int ref,
                           its_recv_cb_t onRecv,
                           its_disconnect_cb_t onDisconnect) {
    its_task_t* me = myTask();
    if (!me || !me->isClient) return -1;

    int clientActive = 0;
    for (int i = 0; i < ITS_MAX_CONNS; i++)
        if (connTable[i].active && connTable[i].clientTask == me->task)
            clientActive++;
    if (clientActive >= me->maxConns) return -1;

    its_task_t* se = taskFind(serverTask);
    if (!se || !se->isServer) return -1;

    /* Pre-flight: ensure the server has actually opened this port. */
    if (!portFind(se, port)) {
        ITS_LOGE("connect to unopened port %u on [%s]",
                 port, pcTaskGetName(serverTask));
        return -1;
    }

    xSemaphoreTake(me->ackSem, 0);
    me->ackHandle = -1;

    /* Cap payload at what the target's inbox actually accepts, not a global
     * compile-time default. Target may have been initialized with a larger
     * inboxMaxMsgLen to carry bigger connect payloads (e.g. JSON args). */
    size_t maxPayload = se->inboxItemSize > sizeof(its_header_t)
                      ? se->inboxItemSize - sizeof(its_header_t) : ITS_MAX_MSG_DATA;
    if (dataLen > maxPayload) {
        ITS_LOGE("connect data truncated: %u > %u",
                 (unsigned)dataLen, (unsigned)maxPayload);
        dataLen = maxPayload;
    }
    size_t totalLen = sizeof(its_header_t) + dataLen;
    uint8_t* buf = (uint8_t*)alloca(totalLen);
    auto* hdr = (its_header_t*)buf;
    *hdr = {};
    hdr->sender = me->task;
    hdr->msg = ITS_MSG_CONNECT;
    hdr->pickupIdx = -1;
    hdr->itsPort = port;
    hdr->len = (uint16_t)dataLen;
    hdr->handle = -1;
    if (data && dataLen > 0)
        memcpy(buf + sizeof(its_header_t), data, dataLen);

    if (!inboxSend(se, buf, totalLen, timeout))
        return -1;

    if (xSemaphoreTake(me->ackSem, timeout) != pdTRUE)
        return -1;

    int handle = me->ackHandle;
    if (handle >= 0) {
        its_conn_t* c = &connTable[handle];
        if (ref >= 0)        c->clientRef       = (int8_t)ref;
        if (onRecv)          c->cliRecvCb       = onRecv;
        if (onDisconnect)    c->cliDisconnectCb = onDisconnect;
    }
    return handle;
}

int itsConnect(const char* serverName, uint16_t port,
               const void* data, size_t dataLen, TickType_t timeout, int ref,
               its_recv_cb_t onRecv, its_disconnect_cb_t onDisconnect) {
    TaskHandle_t task = xTaskGetHandle(serverName);
    if (!task) return -1;
    return itsConnectByTaskHandle(task, port, data, dataLen, timeout, ref,
                                  onRecv, onDisconnect);
}

/* ---- Disconnect (works from either side; -1 = all of this task's conns) ---- */

void itsDisconnect(int handle) {
    TaskHandle_t me = xTaskGetCurrentTaskHandle();

    if (handle == -1) {
        /* Disconnect every connection (server- or client-owned) held by us */
        for (int i = 0; i < ITS_MAX_CONNS; i++) {
            its_conn_t* c = &connTable[i];
            if (!c->active) continue;
            if (c->serverTask == me || c->clientTask == me)
                itsDisconnect(i);
        }
        return;
    }

    its_conn_t* c = conn(handle);
    if (!c) return;

    if (c->serverTask == me) {
        /* Server side closes — embed client cb pointer in the kick message
         * since the conn entry will be freed before the client wakes. */
        TaskHandle_t client = c->clientTask;
        int8_t clientRef = c->clientRef;
        uint16_t port = c->itsPort;
        its_disconnect_cb_t cb = c->cliDisconnectCb;
        connFree(handle);
        its_task_t* ce = taskFind(client);
        if (ce) {
            uint8_t buf[sizeof(its_header_t) + 1 + sizeof(cb)];
            auto* hdr = (its_header_t*)buf;
            *hdr = {};
            hdr->sender = me;
            hdr->msg = ITS_MSG_DISCONNECT;
            hdr->handle = (int16_t)handle;
            hdr->itsPort = port;
            hdr->pickupIdx = -1;
            hdr->len = 1 + sizeof(cb);
            buf[sizeof(its_header_t)] = (uint8_t)clientRef;
            memcpy(buf + sizeof(its_header_t) + 1, &cb, sizeof(cb));
            inboxSend(ce, buf, sizeof(buf), 0);
        }
    } else if (c->clientTask == me) {
        /* Client side closes — server callback lives in the per-port
         * table on the server task, no need to embed anything. */
        TaskHandle_t serverTask = c->serverTask;
        int8_t serverRef = c->serverRef;
        uint16_t port = c->itsPort;
        connFree(handle);
        its_task_t* se = taskFind(serverTask);
        if (se) {
            uint8_t buf[sizeof(its_header_t) + 1];
            auto* hdr = (its_header_t*)buf;
            *hdr = {};
            hdr->sender = me;
            hdr->msg = ITS_MSG_DISCONNECT;
            hdr->pickupIdx = -1;
            hdr->itsPort = port;
            hdr->handle = (int16_t)handle;
            hdr->len = 1;
            buf[sizeof(its_header_t)] = (uint8_t)serverRef;
            inboxSend(se, buf, sizeof(buf), pdMS_TO_TICKS(100));
        }
    }
}

/* ---- Aux messages ---- */

static bool itsSendAuxInternal(TaskHandle_t task, uint16_t port,
                               const void* data, size_t dataLen,
                               TickType_t timeout, its_wait_t wait) {
    its_task_t* te = taskFind(task);
    if (!te) return false;

    /* Pre-flight: ensure the receiver has registered this aux port */
    bool registered = false;
    for (int i = 0; i < te->auxCount; i++) {
        if (te->auxCallbacks[i].active && te->auxCallbacks[i].port == port) {
            registered = true;
            break;
        }
    }
    if (!registered) {
        ITS_LOGE("aux send to unregistered port %u on [%s]",
                 port, pcTaskGetName(task));
        return false;
    }

    /* Max payload = target's inbox item size minus header */
    size_t maxPayload = te->inboxItemSize > sizeof(its_header_t)
                      ? te->inboxItemSize - sizeof(its_header_t) : ITS_MAX_MSG_DATA;
    if (dataLen > maxPayload) {
        ITS_LOGE("aux data truncated: %u > %u",
                 (unsigned)dataLen, (unsigned)maxPayload);
        dataLen = maxPayload;
    }

    int pickupIdx = -1;
    if (wait == ITS_WAIT_PICKUP)
        pickupIdx = pickupAcquire();

    size_t totalLen = sizeof(its_header_t) + dataLen;
    uint8_t* buf = (uint8_t*)alloca(totalLen);
    auto* hdr = (its_header_t*)buf;
    *hdr = {};
    hdr->sender = xTaskGetCurrentTaskHandle();
    hdr->msg = ITS_MSG_AUX;
    hdr->pickupIdx = (int8_t)pickupIdx;
    hdr->itsPort = port;
    hdr->len = (uint16_t)dataLen;
    hdr->handle = -1;
    if (data && dataLen > 0)
        memcpy(buf + sizeof(its_header_t), data, dataLen);

    bool delivered = inboxSend(te, buf, totalLen, timeout);

    if (!delivered) {
        if (pickupIdx >= 0) pickupStamps[pickupIdx] = 0;
        return false;
    }

    if (pickupIdx >= 0)
        return pickupWait(pickupIdx, timeout);

    return true;
}

bool itsSendAuxByTaskHandle(TaskHandle_t task, uint16_t port,
                            const void* data, size_t dataLen, TickType_t timeout,
                            its_wait_t wait) {
    return itsSendAuxInternal(task, port, data, dataLen, timeout, wait);
}

bool itsSendAux(const char* taskName, uint16_t port,
                const void* data, size_t dataLen, TickType_t timeout,
                its_wait_t wait) {
    TaskHandle_t task = xTaskGetHandle(taskName);
    if (!task) return false;
    return itsSendAuxInternal(task, port, data, dataLen, timeout, wait);
}

/* ---- Data API ---- */

/* Wake a sender that's blocked waiting for space, if it asked for an amount
 * we now have free. Caller must have just consumed bytes from `pe`. */
static inline void wakeSenderIfReady(its_pool_entry_t* pe) {
    if (pe->senderWaiting > 0
        && xStreamBufferSpacesAvailable(pe->handle) >= pe->senderWaiting)
        xSemaphoreGive(pe->spaceFreedSem);
}

size_t itsSend(int handle, const void* data, size_t len, TickType_t timeout) {
    its_pool_entry_t* pe = nullptr;
    StreamBufferHandle_t buf = sendBufWithPool(handle, &pe);
    if (!buf || !pe) return 0;
    its_conn_t* c = conn(handle);
    if (!c) return 0;

    if (c->packetBased) {
        if (len == 0 || len > 0xFFFFFF) {
            ITS_LOGE("packet len %u out of range", (unsigned)len);
            return 0;
        }
        size_t total = 4 + len;
        if (total > pe->size) {
            ITS_LOGE("packet (4+%u) exceeds buffer cap %u",
                     (unsigned)len, (unsigned)pe->size);
            return 0;
        }

        /* Wait for the whole packet to fit. Single writer per direction, so
         * once spaces is sufficient nothing else can shrink it; the two
         * xStreamBufferSend calls below will not block. */
        TickType_t startTick = xTaskGetTickCount();
        TickType_t remaining = timeout;
        while (xStreamBufferSpacesAvailable(buf) < total) {
            if (timeout == 0) return 0;

            xSemaphoreTake(pe->spaceFreedSem, 0);   /* clear stale signal */
            pe->senderWaiting = total;
            /* Re-check: receiver may have freed space between our last check
             * and our senderWaiting set, so could have skipped giving us. */
            if (xStreamBufferSpacesAvailable(buf) >= total) {
                pe->senderWaiting = 0;
                break;
            }

            BaseType_t got = xSemaphoreTake(pe->spaceFreedSem, remaining);
            pe->senderWaiting = 0;
            if (got != pdTRUE) return 0;

            /* A disconnect path also gives the sem after poolFree — bail
               out if the connection has gone away under us. Otherwise we'd
               xStreamBufferSend into a buffer now owned by a different
               connection. */
            if (!conn(handle)) return 0;

            if (timeout != portMAX_DELAY) {
                TickType_t elapsed = xTaskGetTickCount() - startTick;
                remaining = (elapsed >= timeout) ? 0 : (timeout - elapsed);
            }
        }

        /* Atomic write: header then body, single notify after body. The
         * "in-flight 4-byte header" state on the wire is transient and
         * dispatchRecvCallbacks ignores avail <= 4 in packet mode. */
        uint8_t hdr[4];
        hdr[0] = 0;
        hdr[1] = (uint8_t)((len >> 16) & 0xFF);
        hdr[2] = (uint8_t)((len >> 8) & 0xFF);
        hdr[3] = (uint8_t)(len & 0xFF);
        if (xStreamBufferSend(buf, hdr, 4, 0) != 4) {
            ITS_LOGE("packet header send failed (space disappeared?)");
            return 0;
        }
        if (xStreamBufferSend(buf, data, len, 0) != len) {
            ITS_LOGE("packet body send failed (space disappeared?)");
            return 0;
        }
        TaskHandle_t remote = remoteOf(handle);
        if (remote) xTaskNotifyGive(remote);
        return len;
    }

    /* Stream mode */
    size_t sent = xStreamBufferSend(buf, data, len, timeout);
    if (sent > 0) {
        size_t fill = xStreamBufferBytesAvailable(buf);
        size_t trigger = pe->triggerLevel ? pe->triggerLevel : 1;
        if (fill >= trigger) {
            TaskHandle_t remote = remoteOf(handle);
            if (remote) xTaskNotifyGive(remote);
        }
    }
    return sent;
}

size_t itsRecv(int handle, void* buf, size_t maxLen, TickType_t timeout) {
    its_pool_entry_t* pe = nullptr;
    StreamBufferHandle_t sb = recvBufWithPool(handle, &pe);
    if (!sb || !pe) return 0;
    its_conn_t* c = conn(handle);
    if (!c) return 0;

    if (c->packetBased) {
        /* Wait for a whole packet (avail > 4 ⇒ at least one complete packet
         * at the head of the buffer; an in-flight header is at most 4 bytes
         * at the tail, never the head). */
        TickType_t startTick = xTaskGetTickCount();
        TickType_t remaining = timeout;
        while (xStreamBufferBytesAvailable(sb) <= 4) {
            if (remaining == 0) return 0;
            ulTaskNotifyTake(pdFALSE, remaining);
            if (xStreamBufferBytesAvailable(sb) > 4) break;
            if (timeout != portMAX_DELAY) {
                TickType_t elapsed = xTaskGetTickCount() - startTick;
                remaining = (elapsed >= timeout) ? 0 : (timeout - elapsed);
            }
        }

        uint8_t hdr[4];
        if (xStreamBufferReceive(sb, hdr, 4, 0) != 4) return 0;
        if (hdr[0] != 0)
            ITS_LOGW("packet reserved byte non-zero: 0x%02x", hdr[0]);
        size_t bodyLen = ((size_t)hdr[1] << 16)
                       | ((size_t)hdr[2] << 8)
                       |  (size_t)hdr[3];

        size_t got;
        if (bodyLen > maxLen) {
            ITS_LOGE("packet body %u > buf %u, dropping",
                     (unsigned)bodyLen, (unsigned)maxLen);
            uint8_t scratch[64];
            size_t left = bodyLen;
            while (left > 0) {
                size_t r = xStreamBufferReceive(sb, scratch,
                    left > sizeof(scratch) ? sizeof(scratch) : left, 0);
                if (r == 0) break;
                left -= r;
            }
            got = 0;
        } else {
            got = xStreamBufferReceive(sb, buf, bodyLen, 0);
        }

        wakeSenderIfReady(pe);
        return got;
    }

    return xStreamBufferReceive(sb, buf, maxLen, timeout);
}

bool itsConnected(int handle) {
    return conn(handle) != nullptr;
}

size_t itsBytesAvailable(int handle) {
    StreamBufferHandle_t buf = recvBuf(handle);
    return buf ? xStreamBufferBytesAvailable(buf) : 0;
}

size_t itsSpacesAvailable(int handle) {
    StreamBufferHandle_t buf = sendBuf(handle);
    if (!buf) return 0;
    size_t spaces = xStreamBufferSpacesAvailable(buf);
    its_conn_t* c = conn(handle);
    if (c && c->packetBased) {
        /* Report max body size that itsSend(timeout=0) would accept. */
        return spaces > 4 ? spaces - 4 : 0;
    }
    return spaces;
}

bool itsSetTriggerLevel(int handle, size_t triggerLevel) {
    its_pool_entry_t* pe = nullptr;
    StreamBufferHandle_t buf = recvBufWithPool(handle, &pe);
    if (!buf || !pe) return false;
    if (triggerLevel == 0) triggerLevel = 1;
    pe->triggerLevel = triggerLevel;
    return xStreamBufferSetTriggerLevel(buf, triggerLevel);
}

bool itsIsEmpty(int handle) {
    StreamBufferHandle_t buf = recvBuf(handle);
    return buf ? xStreamBufferIsEmpty(buf) : true;
}

bool itsIsFull(int handle) {
    StreamBufferHandle_t buf = sendBuf(handle);
    return buf ? xStreamBufferIsFull(buf) : true;
}

TaskHandle_t itsRemoteTask(int handle) {
    return remoteOf(handle);
}

bool itsReset(int handle) {
    StreamBufferHandle_t sb;
    sb = recvBuf(handle);
    if (sb) xStreamBufferReset(sb);
    sb = sendBuf(handle);
    if (sb) xStreamBufferReset(sb);
    return true;
}
