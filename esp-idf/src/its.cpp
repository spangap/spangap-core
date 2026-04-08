#include "its.h"
#include "log.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include <string.h>

static inline uint32_t millis() { return (uint32_t)(esp_timer_get_time() / 1000); }

/* ---- Stream buffer pool ---- */

#define ITS_MAX_POOL 32

struct its_pool_entry_t {
    StreamBufferHandle_t handle;
    size_t               size;
    volatile bool        inUse;
};

static its_pool_entry_t itsPool[ITS_MAX_POOL];
static int              itsPoolCount = 0;
static portMUX_TYPE     itsPoolMux = portMUX_INITIALIZER_UNLOCKED;

void itsReserveStreams(int count, size_t size) {
    for (int i = 0; i < count && itsPoolCount < ITS_MAX_POOL; i++) {
        auto& e = itsPool[itsPoolCount++];
        e.handle = xStreamBufferCreateWithCaps(size, 1, MALLOC_CAP_SPIRAM);
        e.size = size;
        e.inUse = false;
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
    if (best >= 0) itsPool[best].inUse = true;
    portEXIT_CRITICAL(&itsPoolMux);
    if (best >= 0) xStreamBufferReset(itsPool[best].handle);
    return best;
}

static void poolFree(int idx) {
    if (idx < 0 || idx >= itsPoolCount) return;
    xStreamBufferReset(itsPool[idx].handle);
    portENTER_CRITICAL(&itsPoolMux);
    itsPool[idx].inUse = false;
    portEXIT_CRITICAL(&itsPoolMux);
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

/** Acquire a pickup semaphore. Returns pool index or -1. */
static int pickupAcquire() {
    /* Scan for free (stamp == 0) */
    for (int i = 0; i < ITS_MAX_PICKUP; i++) {
        if (pickupStamps[i] == 0) {
            xSemaphoreTake(pickupSems[i], 0);  /* ensure taken state */
            pickupStamps[i] = millis() | 1;    /* never zero (| 1 guards millis()==0) */
            return i;
        }
    }
    /* No free slot — reclaim oldest */
    int oldest = 0;
    uint32_t oldestStamp = pickupStamps[0];
    for (int i = 1; i < ITS_MAX_PICKUP; i++) {
        if (pickupStamps[i] && pickupStamps[i] < oldestStamp) {
            oldest = i;
            oldestStamp = pickupStamps[i];
        }
    }
    xSemaphoreTake(pickupSems[oldest], 0);  /* drain any stale give */
    pickupStamps[oldest] = millis() | 1;
    return oldest;
}

/** Receiver calls this after processing — gives semaphore, zeros stamp. */
static void pickupRelease(int idx) {
    if (idx < 0 || idx >= ITS_MAX_PICKUP) return;
    xSemaphoreGive(pickupSems[idx]);
    pickupStamps[idx] = 0;
}

/** Sender calls this to wait for pickup. */
static bool pickupWait(int idx, TickType_t timeout) {
    if (idx < 0 || idx >= ITS_MAX_PICKUP) return false;
    bool ok = xSemaphoreTake(pickupSems[idx], timeout) == pdTRUE;
    pickupStamps[idx] = 0;   /* free regardless of success */
    return ok;
}

/* ---- Global connection table ---- */

#define ITS_MAX_CONNS 64

struct its_conn_t {
    volatile bool  active;
    TaskHandle_t   clientTask;
    TaskHandle_t   serverTask;
    int            toPoolIdx;
    int            fromPoolIdx;
    int            itsPort;
    int8_t         clientRef;
    int8_t         serverRef;
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
    int8_t       pickupIdx;   /* pickup pool index, -1 = no pickup requested */
    uint16_t     itsPort;
    uint16_t     len;
    int16_t      handle;
} its_header_t;

#define ITS_DEFAULT_INBOX_SIZE  (sizeof(its_header_t) + ITS_MAX_MSG_DATA + 4)

/* ---- Task registry ---- */

#define ITS_MAX_TASKS          24
#define ITS_MAX_AUX_CALLBACKS  8

struct its_aux_entry_t {
    its_aux_cb_t cb;
    uint16_t     port;
};

#define ITS_MAX_PORT_CONFIGS 4

struct its_port_config_t {
    uint16_t port;
    int      maxHandles;
    size_t   toSize;
    size_t   fromSize;
};

struct its_task_t {
    TaskHandle_t          task;
    int                   maxHandles;
    size_t                toSize;
    size_t                fromSize;
    its_connect_cb_t      onConnect;
    its_busy_cb_t         onBusy;
    its_disconnect_cb_t   srvDisconnect;
    int                   maxConns;
    its_disconnect_cb_t   cliDisconnect;
    SemaphoreHandle_t     ackSem;
    int                   ackHandle;
    its_aux_entry_t       auxCallbacks[ITS_MAX_AUX_CALLBACKS];
    int                   auxCount;
    its_port_config_t     portConfigs[ITS_MAX_PORT_CONFIGS];
    int                   portConfigCount;
    QueueHandle_t         inbox;
    size_t                inboxItemSize; /* max message size (header + payload) */
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
    if (s_taskCount >= ITS_MAX_TASKS) return nullptr;

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

static StreamBufferHandle_t sendBuf(int handle) {
    its_conn_t* c = conn(handle);
    if (!c) return nullptr;
    TaskHandle_t me = xTaskGetCurrentTaskHandle();
    if (me == c->clientTask) return poolHandle(c->toPoolIdx);
    if (me == c->serverTask) return poolHandle(c->fromPoolIdx);
    return nullptr;
}

static StreamBufferHandle_t recvBuf(int handle) {
    its_conn_t* c = conn(handle);
    if (!c) return nullptr;
    TaskHandle_t me = xTaskGetCurrentTaskHandle();
    if (me == c->clientTask) return poolHandle(c->fromPoolIdx);
    if (me == c->serverTask) return poolHandle(c->toPoolIdx);
    return nullptr;
}

static TaskHandle_t remoteOf(int handle) {
    its_conn_t* c = conn(handle);
    if (!c) return nullptr;
    TaskHandle_t me = xTaskGetCurrentTaskHandle();
    if (me == c->clientTask) return c->serverTask;
    if (me == c->serverTask) return c->clientTask;
    return nullptr;
}

static int serverActiveCount(its_task_t* t) {
    int count = 0;
    for (int i = 0; i < ITS_MAX_CONNS; i++)
        if (connTable[i].active && connTable[i].serverTask == t->task)
            count++;
    return count;
}

static int serverPortActiveCount(its_task_t* t, int itsPort) {
    int count = 0;
    for (int i = 0; i < ITS_MAX_CONNS; i++)
        if (connTable[i].active && connTable[i].serverTask == t->task
            && connTable[i].itsPort == itsPort)
            count++;
    return count;
}

static const its_port_config_t* portConfigFind(its_task_t* t, int itsPort) {
    for (int i = 0; i < t->portConfigCount; i++)
        if (t->portConfigs[i].port == (uint16_t)itsPort)
            return &t->portConfigs[i];
    return nullptr;
}

/* ---- itsPoll ---- */

bool itsPoll(TickType_t timeout) {
    its_task_t* me = myTask();
    if (!me) {
        /* Not registered yet — just sleep if requested */
        if (timeout > 0) ulTaskNotifyTake(pdTRUE, timeout);
        return false;
    }

    uint8_t* buf = (uint8_t*)alloca(me->inboxItemSize);

    /* Try non-blocking read first */
    if (xQueueReceive(me->inbox, buf, 0) != pdTRUE) {
        if (timeout == 0) return false;
        /* No message pending — block until notification, then retry */
        ulTaskNotifyTake(pdTRUE, timeout);
        if (xQueueReceive(me->inbox, buf, 0) != pdTRUE)
            return false;
    }

    auto* hdr = (its_header_t*)buf;
    void* payload = buf + sizeof(its_header_t);
    int pickupIdx = hdr->pickupIdx;

    if (hdr->msg == ITS_MSG_CONNECT && me->isServer) {
        /* Per-port config: sizes + limit override global defaults */
        const its_port_config_t* pc = portConfigFind(me, hdr->itsPort);
        size_t toSz   = pc ? pc->toSize   : me->toSize;
        size_t fromSz = pc ? pc->fromSize  : me->fromSize;
        int portMax   = pc ? pc->maxHandles : -1;  /* -1 = no per-port limit */

        int active = serverActiveCount(me);
        bool full = active >= me->maxHandles;
        if (!full && portMax >= 0)
            full = serverPortActiveCount(me, hdr->itsPort) >= portMax;

        if (full && me->onBusy) {
            if (!me->onBusy(hdr->itsPort, payload, hdr->len))
                full = (serverActiveCount(me) >= me->maxHandles)
                    || (portMax >= 0 && serverPortActiveCount(me, hdr->itsPort) >= portMax);
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
            if (toSz > 0)   c->toPoolIdx = poolGet(toSz);
            if (fromSz > 0) c->fromPoolIdx = poolGet(fromSz);

            if (!me->onConnect) {
                accepted = true;
            } else {
                int sRef = me->onConnect(handle, hdr->itsPort, payload, hdr->len);
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
        its_conn_t* c = conn(handle);
        if (c && c->serverTask == me->task && c->clientTask == hdr->sender) {
            ref = c->serverRef;
            connFree(handle);
        } else if (hdr->len >= 1) {
            ref = (int8_t)((uint8_t*)payload)[0];
        }
        if (ref >= 0 && me->srvDisconnect) me->srvDisconnect(ref);

    } else if (hdr->msg == ITS_MSG_DISCONNECT && me->isClient) {
        int handle = hdr->handle;
        int8_t ref = -1;
        its_conn_t* c = conn(handle);
        if (c && c->clientTask == me->task && c->serverTask == hdr->sender) {
            ref = c->clientRef;
            connFree(handle);
        } else if (hdr->len >= 1) {
            /* Conn already freed (server kicked) — ref embedded in payload */
            ref = (int8_t)((uint8_t*)payload)[0];
        }
        if (ref >= 0 && me->cliDisconnect) me->cliDisconnect(ref);

    } else if (hdr->msg == ITS_MSG_FORWARD && me->isServer) {
        int handle = hdr->handle;
        its_conn_t* c = conn(handle);
        if (c && c->serverTask == me->task) {
            if (me->onConnect && me->onConnect(handle, hdr->itsPort, payload, hdr->len) < 0)
                itsServerKick(handle);
        }

    } else if (hdr->msg == ITS_MSG_AUX) {
        /* Dispatch to port-matched callback, fall back to port-0 catch-all */
        its_aux_cb_t cb = nullptr;
        its_aux_cb_t catchAll = nullptr;
        for (int i = 0; i < me->auxCount; i++) {
            if (me->auxCallbacks[i].port == hdr->itsPort) { cb = me->auxCallbacks[i].cb; break; }
            if (me->auxCallbacks[i].port == 0) catchAll = me->auxCallbacks[i].cb;
        }
        if (!cb) cb = catchAll;
        if (cb) cb(hdr->sender, hdr->itsPort, payload, hdr->len);
    }

    /* ACK pickup AFTER callback has fully returned */
    if (pickupIdx >= 0) pickupRelease(pickupIdx);

    return true;
}

/* ---- Server API ---- */

bool itsServerInit(int maxHandles, size_t toSize, size_t fromSize,
                   size_t inboxMaxMsgLen, size_t inboxDepth) {
    TaskHandle_t me = xTaskGetCurrentTaskHandle();
    its_task_t* entry = taskFindOrCreate(me, inboxMaxMsgLen, inboxDepth);
    if (!entry || entry->isServer) return false;

    entry->maxHandles = maxHandles;
    entry->toSize = toSize;
    entry->fromSize = fromSize;
    entry->isServer = true;
    return true;
}

bool itsServerPortInit(uint16_t itsPort, int maxHandles,
                       size_t toSize, size_t fromSize) {
    its_task_t* e = myTask();
    if (!e || !e->isServer) return false;
    /* Update existing entry for this port */
    for (int i = 0; i < e->portConfigCount; i++) {
        if (e->portConfigs[i].port == itsPort) {
            e->portConfigs[i] = { itsPort, maxHandles, toSize, fromSize };
            return true;
        }
    }
    if (e->portConfigCount >= ITS_MAX_PORT_CONFIGS) return false;
    e->portConfigs[e->portConfigCount++] = { itsPort, maxHandles, toSize, fromSize };
    return true;
}

void itsServerOnConnect(its_connect_cb_t cb) {
    its_task_t* e = myTask();
    if (e) e->onConnect = cb;
}

void itsServerOnBusy(its_busy_cb_t cb) {
    its_task_t* e = myTask();
    if (e) e->onBusy = cb;
}

void itsServerOnDisconnect(its_disconnect_cb_t cb) {
    its_task_t* e = myTask();
    if (e) e->srvDisconnect = cb;
}

void itsOnAux(its_aux_cb_t cb, uint16_t port) {
    its_task_t* e = myTask();
    if (!e) {
        /* Not yet registered — create entry */
        e = taskFindOrCreate(xTaskGetCurrentTaskHandle(), 0, 0);
        if (!e) return;
    }
    /* Replace existing handler for same port (idempotent per port) */
    for (int i = 0; i < e->auxCount; i++) {
        if (e->auxCallbacks[i].port == port) {
            e->auxCallbacks[i].cb = cb;
            return;
        }
    }
    if (e->auxCount >= ITS_MAX_AUX_CALLBACKS) return;
    e->auxCallbacks[e->auxCount++] = { cb, port };
}

void itsServerKick(int handle) {
    its_conn_t* c = conn(handle);
    if (!c) return;
    if (c->serverTask != xTaskGetCurrentTaskHandle()) return;
    TaskHandle_t client = c->clientTask;
    int8_t clientRef = c->clientRef;
    connFree(handle);
    /* Notify client with their ref embedded in the message */
    its_task_t* ce = taskFind(client);
    if (ce) {
        uint8_t buf[sizeof(its_header_t) + 1];
        auto* hdr = (its_header_t*)buf;
        *hdr = {};
        hdr->sender = xTaskGetCurrentTaskHandle();
        hdr->msg = ITS_MSG_DISCONNECT;
        hdr->handle = (int16_t)handle;
        hdr->pickupIdx = -1;
        hdr->len = 1;
        buf[sizeof(its_header_t)] = (uint8_t)clientRef;
        inboxSend(ce, buf, sizeof(buf), 0);
    }
}

int itsServerActive(void) {
    its_task_t* me = myTask();
    return me ? serverActiveCount(me) : 0;
}

int itsActiveTotal(void) {
    int n = 0;
    for (int i = 0; i < ITS_MAX_CONNS; i++)
        if (connTable[i].active) n++;
    return n;
}

int itsRef(int handle) {
    its_conn_t* c = conn(handle);
    if (!c) return -1;
    TaskHandle_t me = xTaskGetCurrentTaskHandle();
    if (c->serverTask == me) return c->serverRef;
    if (c->clientTask == me) return c->clientRef;
    return -1;
}

size_t itsServerInject(int handle, const void* data, size_t len) {
    its_conn_t* c = conn(handle);
    if (!c || c->serverTask != xTaskGetCurrentTaskHandle()) return 0;
    StreamBufferHandle_t sb = poolHandle(c->toPoolIdx);
    if (!sb) return 0;
    return xStreamBufferSend(sb, data, len, 0);
}

int itsServerForwardByHandle(int handle, TaskHandle_t targetTask, int itsPort,
                             const void* data, size_t dataLen) {
    its_conn_t* c = conn(handle);
    if (!c || c->serverTask != xTaskGetCurrentTaskHandle()) return -1;

    its_task_t* te = taskFind(targetTask);
    if (!te || !te->isServer) return -1;

    const its_port_config_t* pc = portConfigFind(te, itsPort);
    int portMax = pc ? pc->maxHandles : -1;

    auto isFull = [&]() {
        if (serverActiveCount(te) >= te->maxHandles) return true;
        if (portMax >= 0 && serverPortActiveCount(te, itsPort) >= portMax) return true;
        return false;
    };

    if (isFull() && te->onBusy) {
        if (te->onBusy(itsPort, data, dataLen)) return -1;
        if (isFull()) return -1;
    }

    c->serverTask = targetTask;
    c->itsPort = itsPort;

    if (dataLen > ITS_MAX_MSG_DATA) {
        err("forward data truncated: %u > %u", (unsigned)dataLen, ITS_MAX_MSG_DATA);
        dataLen = ITS_MAX_MSG_DATA;
    }
    uint8_t buf[sizeof(its_header_t) + ITS_MAX_MSG_DATA];
    auto* fhdr = (its_header_t*)buf;
    fhdr->sender = xTaskGetCurrentTaskHandle();
    fhdr->msg = ITS_MSG_FORWARD;
    fhdr->pickupIdx = -1;
    fhdr->itsPort = (uint16_t)itsPort;
    fhdr->len = dataLen;
    fhdr->handle = (int16_t)handle;
    if (data && dataLen > 0)
        memcpy(buf + sizeof(its_header_t), data, dataLen);
    inboxSend(te, buf, sizeof(its_header_t) + dataLen, pdMS_TO_TICKS(100));

    return handle;
}

int itsServerForward(int handle, const char* targetServer, int itsPort,
                     const void* data, size_t dataLen) {
    TaskHandle_t task = xTaskGetHandle(targetServer);
    if (!task) return -1;
    return itsServerForwardByHandle(handle, task, itsPort, data, dataLen);
}

int itsServerPort(int handle) {
    its_conn_t* c = conn(handle);
    if (!c || c->serverTask != xTaskGetCurrentTaskHandle()) return -1;
    return c->itsPort;
}

/* ---- Client API ---- */

void itsClientInit(int maxConns,
                   its_disconnect_cb_t onDisconnect,
                   size_t inboxMaxMsgLen, size_t inboxDepth) {
    TaskHandle_t me = xTaskGetCurrentTaskHandle();
    its_task_t* entry = taskFindOrCreate(me, inboxMaxMsgLen, inboxDepth);
    if (!entry || entry->isClient) return;

    entry->maxConns = maxConns;
    entry->cliDisconnect = onDisconnect;
    entry->isClient = true;
    entry->ackSem = xSemaphoreCreateBinary();
    entry->ackHandle = -1;
}

int itsConnectByHandle(TaskHandle_t serverTask, int itsPort,
                       const void* data, size_t dataLen, TickType_t timeout,
                       int ref) {
    its_task_t* me = myTask();
    if (!me || !me->isClient) return -1;

    int clientActive = 0;
    for (int i = 0; i < ITS_MAX_CONNS; i++)
        if (connTable[i].active && connTable[i].clientTask == me->task)
            clientActive++;
    if (clientActive >= me->maxConns) return -1;

    its_task_t* se = taskFind(serverTask);
    if (!se || !se->isServer) return -1;

    xSemaphoreTake(me->ackSem, 0);
    me->ackHandle = -1;

    if (dataLen > ITS_MAX_MSG_DATA) {
        err("connect data truncated: %u > %u", (unsigned)dataLen, ITS_MAX_MSG_DATA);
        dataLen = ITS_MAX_MSG_DATA;
    }
    uint8_t buf[sizeof(its_header_t) + ITS_MAX_MSG_DATA];
    auto* hdr = (its_header_t*)buf;
    hdr->sender = me->task;
    hdr->msg = ITS_MSG_CONNECT;
    hdr->pickupIdx = -1;
    hdr->itsPort = (uint16_t)itsPort;
    hdr->len = dataLen;
    hdr->handle = -1;
    if (data && dataLen > 0)
        memcpy(buf + sizeof(its_header_t), data, dataLen);

    if (!inboxSend(se, buf, sizeof(its_header_t) + dataLen, timeout))
        return -1;

    if (xSemaphoreTake(me->ackSem, timeout) != pdTRUE)
        return -1;
    int handle = me->ackHandle;
    if (handle >= 0 && ref >= 0) connTable[handle].clientRef = (int8_t)ref;
    return handle;
}

int itsConnect(const char* serverName, int itsPort,
               const void* data, size_t dataLen, TickType_t timeout, int ref) {
    TaskHandle_t task = xTaskGetHandle(serverName);
    if (!task) return -1;
    return itsConnectByHandle(task, itsPort, data, dataLen, timeout, ref);
}

void itsDisconnect(int handle) {
    its_conn_t* c = conn(handle);
    if (!c || c->clientTask != xTaskGetCurrentTaskHandle()) return;

    TaskHandle_t serverTask = c->serverTask;
    int8_t serverRef = c->serverRef;
    connFree(handle);

    its_task_t* se = taskFind(serverTask);
    if (se) {
        uint8_t buf[sizeof(its_header_t) + 1];
        auto* hdr = (its_header_t*)buf;
        *hdr = {};
        hdr->sender = xTaskGetCurrentTaskHandle();
        hdr->msg = ITS_MSG_DISCONNECT;
        hdr->pickupIdx = -1;
        hdr->handle = (int16_t)handle;
        hdr->len = 1;
        buf[sizeof(its_header_t)] = (uint8_t)serverRef;
        inboxSend(se, buf, sizeof(buf), pdMS_TO_TICKS(100));
    }
}

/* ---- Aux messages ---- */

static bool itsSendAuxInternal(TaskHandle_t task,
                               const void* data, size_t dataLen,
                               TickType_t timeout, uint16_t port,
                               its_wait_t wait) {
    its_task_t* te = taskFind(task);
    if (!te) return false;

    /* Max payload = target's inbox item size minus header */
    size_t maxPayload = te->inboxItemSize > sizeof(its_header_t)
                      ? te->inboxItemSize - sizeof(its_header_t) : ITS_MAX_MSG_DATA;
    if (dataLen > maxPayload) {
        err("aux data truncated: %u > %u", (unsigned)dataLen, (unsigned)maxPayload);
        dataLen = maxPayload;
    }

    int pickupIdx = -1;
    if (wait == ITS_WAIT_PICKUP)
        pickupIdx = pickupAcquire();

    size_t totalLen = sizeof(its_header_t) + dataLen;
    uint8_t* buf = (uint8_t*)alloca(totalLen);
    auto* hdr = (its_header_t*)buf;
    hdr->sender = xTaskGetCurrentTaskHandle();
    hdr->msg = ITS_MSG_AUX;
    hdr->pickupIdx = (int8_t)pickupIdx;
    hdr->itsPort = port;
    hdr->len = dataLen;
    hdr->handle = -1;
    if (data && dataLen > 0)
        memcpy(buf + sizeof(its_header_t), data, dataLen);

    bool delivered = inboxSend(te, buf, totalLen, timeout);

    if (!delivered) {
        /* Message never entered inbox — free pickup slot */
        if (pickupIdx >= 0) pickupStamps[pickupIdx] = 0;
        return false;
    }

    if (pickupIdx >= 0)
        return pickupWait(pickupIdx, timeout);

    return true;
}

bool itsSendAuxByHandle(TaskHandle_t task,
                        const void* data, size_t dataLen,
                        TickType_t timeout, uint16_t port,
                        its_wait_t wait) {
    return itsSendAuxInternal(task, data, dataLen, timeout, port, wait);
}

bool itsSendAux(const char* taskName,
                const void* data, size_t dataLen,
                TickType_t timeout, uint16_t port,
                its_wait_t wait) {
    TaskHandle_t task = xTaskGetHandle(taskName);
    if (!task) return false;
    return itsSendAuxInternal(task, data, dataLen, timeout, port, wait);
}

/* ---- Data API ---- */

size_t itsSend(int handle, const void* data, size_t len, TickType_t timeout) {
    StreamBufferHandle_t buf = sendBuf(handle);
    if (!buf) return 0;
    size_t sent = xStreamBufferSend(buf, data, len, timeout);
    /* Explicit notify needed: xStreamBufferSend only notifies tasks blocked on
     * xStreamBufferReceive, not tasks waiting in itsPoll/ulTaskNotifyTake. */
    TaskHandle_t remote = remoteOf(handle);
    if (remote && sent > 0) xTaskNotifyGive(remote);
    return sent;
}

size_t itsSendSilent(int handle, const void* data, size_t len, TickType_t timeout) {
    StreamBufferHandle_t buf = sendBuf(handle);
    if (!buf) return 0;
    /* No notify — caller manages wake-up (e.g. threshold nudge for write streams). */
    return xStreamBufferSend(buf, data, len, timeout);
}


size_t itsRecv(int handle, void* buf, size_t maxLen, TickType_t timeout) {
    StreamBufferHandle_t sb = recvBuf(handle);
    if (!sb) return 0;
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
