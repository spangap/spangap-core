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

#define ITS_MAX_POOL 128

struct its_pool_entry_t {
    StreamBufferHandle_t handle;
    size_t               size;
    /* false = a no_pool server's buffer: created fresh on connect, never
     * reused/shared, and deleted (vStreamBufferDeleteWithCaps) when the
     * connection tears down — so a brief/large buffer returns to the heap and
     * stays correctly attributed under per-task heap tracking. true = normal
     * retained pool entry, reused across same-size connections. */
    bool                 pooled;
    size_t               triggerLevel;   /* wake receiver when >= N bytes queued */
    volatile bool        inUse;
    /* Free-space notification: when the remote consumes enough that free
     * space >= freeNotify, the receiver's consume path fires
     *   xSemaphoreGive(spaceFreedSem)     — wakes itsWaitForSpace callers
     *   xTaskNotifyGive(sender_task)      — wakes itsPoll on the sender
     * then clears freeNotify (one-shot). Armed by itsSetFreeNotify and by
     * itsWaitForSpace; the latter drains spaceFreedSem, the former relies
     * on the sender's itsPoll to pick up the task notification. */
    volatile size_t      freeNotify;
    SemaphoreHandle_t    spaceFreedSem;
};

static its_pool_entry_t itsPool[ITS_MAX_POOL];
static int              itsPoolCount = 0;
static portMUX_TYPE     itsPoolMux = portMUX_INITIALIZER_UNLOCKED;

/* Acquire a stream buffer of `size` bytes.
 *  - Pooled (noPool=false): reuse a free same-size entry, else create a
 *    retained one. Buffers live forever and are reused — no fragmentation,
 *    no per-connect alloc. This is the default for every existing caller.
 *  - no_pool (noPool=true): always create a fresh buffer; poolFree deletes it
 *    on disconnect (see poolFree), so transient/large buffers return to the
 *    heap and can't be inherited by another task. The lightweight slot + its
 *    semaphore are kept for reclamation either way (the sem is tiny and
 *    deleting it would race a not-yet-woken sender). */
static int poolGet(size_t size, bool noPool = false) {
    if (size == 0) return -1;

    portENTER_CRITICAL(&itsPoolMux);
    if (!noPool) {
        for (int i = 0; i < itsPoolCount; i++) {
            if (!itsPool[i].inUse && itsPool[i].pooled &&
                itsPool[i].handle && itsPool[i].size == size) {
                itsPool[i].inUse = true;
                itsPool[i].triggerLevel = 1;
                portEXIT_CRITICAL(&itsPoolMux);
                xStreamBufferReset(itsPool[i].handle);
                xStreamBufferSetTriggerLevel(itsPool[i].handle, 1);
                return i;
            }
        }
    }
    /* Need a fresh buffer. Reuse a reclaimed empty slot (handle==null, e.g. a
     * freed no_pool entry whose semaphore we kept) or append a new one. Mark
     * inUse before dropping the spinlock so concurrent searches skip it. */
    int idx = -1;
    for (int i = 0; i < itsPoolCount; i++) {
        if (!itsPool[i].inUse && itsPool[i].handle == nullptr) { idx = i; break; }
    }
    if (idx < 0) {
        if (itsPoolCount >= ITS_MAX_POOL) {
            portEXIT_CRITICAL(&itsPoolMux);
            ITS_LOGE("pool table full (%d entries), cannot add %u-byte stream "
                     "(raise ITS_MAX_POOL)", ITS_MAX_POOL, (unsigned)size);
            return -1;
        }
        idx = itsPoolCount++;
        itsPool[idx].spaceFreedSem = nullptr;
    }
    auto& e = itsPool[idx];
    e.size = size;
    e.inUse = true;
    e.pooled = !noPool;
    e.triggerLevel = 1;
    e.freeNotify = 0;
    e.handle = nullptr;
    portEXIT_CRITICAL(&itsPoolMux);

    e.handle = xStreamBufferCreateWithCaps(size, 1, MALLOC_CAP_SPIRAM);
    if (e.spaceFreedSem == nullptr) e.spaceFreedSem = xSemaphoreCreateBinary();
    if (!e.handle || !e.spaceFreedSem) {
        ITS_LOGE("alloc failed for %u-byte stream entry", (unsigned)size);
        return -1;
    }
    return idx;
}

static void poolFree(int idx) {
    if (idx < 0 || idx >= itsPoolCount) return;
    its_pool_entry_t& e = itsPool[idx];
    if (e.pooled) {
        /* Retain for same-size reuse. */
        if (e.handle) {
            xStreamBufferReset(e.handle);
            xStreamBufferSetTriggerLevel(e.handle, 1);
        }
        portENTER_CRITICAL(&itsPoolMux);
        e.inUse = false;
        e.triggerLevel = 1;
        portEXIT_CRITICAL(&itsPoolMux);
        /* Wake any sender blocked waiting for space — connection is gone, so
         * it should abort. itsSend re-checks the connection after the sem. */
        if (e.spaceFreedSem) xSemaphoreGive(e.spaceFreedSem);
    } else {
        /* no_pool: wake waiters, delete the buffer (return memory to the heap),
         * keep the slot + semaphore for reclamation. Safe because this runs on
         * the disconnect's receiving end, in its dispatch loop — neither end is
         * inside a (non-blocking) buffer op at this point. */
        if (e.spaceFreedSem) xSemaphoreGive(e.spaceFreedSem);
        /* Read the handle and null it under the lock so two concurrent frees
         * of the same slot can't both reach vStreamBufferDeleteWithCaps with
         * the same pointer — the second reads nullptr and skips the delete. */
        portENTER_CRITICAL(&itsPoolMux);
        StreamBufferHandle_t h = e.handle;
        e.handle = nullptr;
        e.size = 0;
        e.inUse = false;
        e.triggerLevel = 1;
        e.freeNotify = 0;
        portEXIT_CRITICAL(&itsPoolMux);
        if (h) vStreamBufferDeleteWithCaps(h);
    }
}

/* Fallback when a no_pool peer can't be notified of disconnect: keep the
 * buffers (flip to pooled) so connFree retains rather than deletes — never
 * worse than the pooled path, and avoids both a UAF and a leak. */
static void poolRetainOnFree(int idx) {
    if (idx >= 0 && idx < itsPoolCount) itsPool[idx].pooled = true;
}

static StreamBufferHandle_t poolHandle(int idx) {
    if (idx < 0 || idx >= itsPoolCount) return nullptr;
    return itsPool[idx].handle;
}

/* ---- Pickup semaphore: per-task (in its_task_t.pickupSem)
 * Each task has its own binary sem. ITS_WAIT_PICKUP callers wait on their
 * own sem; receivers give the sender's sem after processing. A task is
 * single-threaded so it can only be in one outstanding pickup at a time,
 * making per-task safe and race-free.
 *
 * Replaces the previous global pool (pickupSems[ITS_MAX_PICKUP]) which had
 * two SMP races: (1) non-atomic check-then-claim in pickupAcquire let two
 * callers grab the same slot; (2) stale gives from one acquire could be
 * consumed by the next acquire's xSemaphoreTake(sem, 0) before the real
 * waiter reached pickupWait, leaving the waiter stuck forever. */

/* ---- Global connection table ---- */

#define ITS_MAX_CONNS 128

struct its_conn_t {
    volatile bool       active;
    bool                packetBased;
    bool                noPool;       /* server opted out of the buffer pool */
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
    /* Clear the entry (mark inactive) BEFORE releasing the buffers, so a
     * sender woken by poolFree's sem re-checks conn(), sees it gone, and
     * aborts before touching the buffer — required now that no_pool poolFree
     * actually deletes the StreamBuffer.
     *
     * Read the pool indices and clear the entry atomically. Two tasks can
     * race to free the SAME handle on a no_pool conn: a bidirectional close
     * (rnsd's linkFreeSlot disconnects the server side while the consumer
     * detaches the client side) leaves the entry live on both ends, so each
     * end then receives the other's DISCONNECT and calls connFree. Reading
     * the indices outside the lock let both read the same valid indices and
     * double-free the stream buffers (tlsf double-free abort). Inside the
     * lock, the first caller reads the real indices and clears them; the
     * second reads -1 and the poolFree calls below no-op. */
    portENTER_CRITICAL(&connMux);
    int toIdx = c->toPoolIdx, fromIdx = c->fromPoolIdx;
    *c = {};
    c->toPoolIdx = -1;
    c->fromPoolIdx = -1;
    portEXIT_CRITICAL(&connMux);
    poolFree(toIdx);
    poolFree(fromIdx);
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

#define ITS_MAX_TASKS  48

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

    /* ITS_WAIT_PICKUP — sender waits on its own pickupSem; receiver gives
     * this sem after processing the aux. Created on first registration. */
    SemaphoreHandle_t     pickupSem;

    /* Aux callbacks (per-task, per-port) */
    its_aux_entry_t       auxCallbacks[ITS_MAX_PORTS];
    int                   auxCount;

    /* Inbox */
    QueueHandle_t         inbox;
    size_t                inboxItemSize;
    int                   inboxDepth;

    bool                  isServer;
    bool                  isClient;
    /* no_pool: when set, this server's connections bypass the shared buffer
     * pool — each stream buffer is created fresh on connect and deleted on
     * disconnect (by the disconnect's receiving end). Returns transient/large
     * buffers to the heap and keeps per-task heap attribution exact. Off by
     * default; opt in via itsServerInit. */
    bool                  no_pool;
};

static its_task_t s_tasks[ITS_MAX_TASKS];
static int        s_taskCount = 0;

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

    e = &s_tasks[s_taskCount++];
    memset(e, 0, sizeof(*e));
    e->task = task;
    e->ackHandle = -1;
    e->pickupSem = xSemaphoreCreateBinary();

    size_t itemSize = inboxMaxMsgLen > 0 ? (sizeof(its_header_t) + inboxMaxMsgLen)
                                         : ITS_DEFAULT_INBOX_SIZE;
    int depth = inboxDepth > 0 ? (int)inboxDepth : 8;
    e->inboxItemSize = itemSize;
    e->inboxDepth = depth;
    /* PSRAM-backed: ITS is task-context only — no ISR may call
     * itsSend/itsSendAux. ISRs should set a heap flag + use
     * vTaskNotifyGiveFromISR instead (PSRAM is unreachable from ISRs
     * during cache-disabled windows; xQueueSendFromISR on a PSRAM
     * queue would crash). See docs/its.md "ISR safety". The big win
     * is storage's depth-64 inbox: ~22 KB reclaimed from DRAM. */
    e->inbox = xQueueCreateWithCaps(depth, itemSize, MALLOC_CAP_SPIRAM);
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
            c->noPool = me->no_pool;
            bool buffersOk = true;
            if (sp->toSize > 0) {
                c->toPoolIdx = poolGet(sp->toSize, me->no_pool);
                if (c->toPoolIdx < 0) {
                    ITS_LOGE("port %u rejected: no pool stream >= %u for to-buffer "
                             "(client %s -> server %s)",
                             hdr->itsPort, (unsigned)sp->toSize,
                             pcTaskGetName(hdr->sender), pcTaskGetName(me->task));
                    buffersOk = false;
                }
            }
            if (sp->fromSize > 0) {
                c->fromPoolIdx = poolGet(sp->fromSize, me->no_pool);
                if (c->fromPoolIdx < 0) {
                    ITS_LOGE("port %u rejected: no pool stream >= %u for from-buffer "
                             "(client %s -> server %s)",
                             hdr->itsPort, (unsigned)sp->fromSize,
                             pcTaskGetName(hdr->sender), pcTaskGetName(me->task));
                    buffersOk = false;
                }
            }

            if (buffersOk) {
                if (!sp->onConnect) {
                    accepted = true;
                } else {
                    int sRef = sp->onConnect(handle, payload, hdr->len);
                    accepted = sRef >= 0;
                    if (accepted) c->serverRef = (int8_t)sRef;
                }
            }
            if (!accepted) connFree(handle);  /* releases pool entries via poolFree */
        }

        if (cli) {
            cli->ackHandle = accepted ? handle : -1;
            xSemaphoreGive(cli->ackSem);
        }

    } else if (hdr->msg == ITS_MSG_DISCONNECT && me->isServer &&
               /* Payload form distinguishes the disconnect direction so dual
                * server+client tasks (e.g. sshd) route both kinds correctly.
                * Client-side close → len==1 (just serverRef). Server-side
                * close → len==1+sizeof(cb) (clientRef + embedded callback).
                * Without this check, sshd's me->isServer=true short-circuits
                * the dispatch and the client-side cb path at line 540+
                * never runs — silently dropping cli→sshd disconnects. */
               hdr->len == 1) {
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
    if (pickupIdx > 0) {
        /* Release sender's per-task pickup sem. No-op if sender isn't
         * registered (shouldn't happen: ITS_WAIT_PICKUP forces registration). */
        its_task_t* s = taskFind(hdr->sender);
        if (s && s->pickupSem) xSemaphoreGive(s->pickupSem);
    }
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

bool itsServerInit(size_t inboxMaxMsgLen, size_t inboxDepth, bool no_pool) {
    TaskHandle_t me = xTaskGetCurrentTaskHandle();
    its_task_t* entry = taskFindOrCreate(me, inboxMaxMsgLen, inboxDepth);
    if (!entry || entry->isServer) return false;
    entry->isServer = true;
    entry->no_pool = no_pool;
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

its_mem_t itsTaskMem(TaskHandle_t task) {
    its_mem_t m = {};
    /* Stream buffers are pool-allocated by the server task at connect, so
     * attribute a connection's to+from buffers to its server — matches the
     * heap-task-tracking owner. Reflects current in-use, not pool high-water. */
    for (int i = 0; i < ITS_MAX_CONNS; i++) {
        if (!connTable[i].active || connTable[i].serverTask != task) continue;
        if (connTable[i].toPoolIdx >= 0) {
            m.streamBytes += itsPool[connTable[i].toPoolIdx].size; m.streamBufs++;
        }
        if (connTable[i].fromPoolIdx >= 0) {
            m.streamBytes += itsPool[connTable[i].fromPoolIdx].size; m.streamBufs++;
        }
    }
    /* The task's single inbox queue (PSRAM): depth * itemSize storage. */
    its_task_t* t = taskFind(task);
    if (t && t->inbox) m.inboxBytes = (size_t)t->inboxDepth * t->inboxItemSize;
    return m;
}

void itsStatus(int (*print)(const char*, ...)) {
    int active = 0;
    for (int i = 0; i < ITS_MAX_CONNS; i++)
        if (connTable[i].active) active++;

    print("ITS System Status Report\n");
    print("Connections (%d/%d in use)\n", active, ITS_MAX_CONNS);
    for (int i = 0; i < ITS_MAX_CONNS; i++) {
        its_conn_t* c = &connTable[i];
        if (!c->active) continue;
        const char* cn = c->clientTask ? pcTaskGetName(c->clientTask) : "?";
        const char* sn = c->serverTask ? pcTaskGetName(c->serverTask) : "?";
        print("    [%s] -> [%s:%u]\n", cn, sn, c->itsPort);
    }

    print("Streams\n");
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
            print("    %u kB (%d/%d in use)\n",
                  (unsigned)(sz / 1024), used, total);
        else
            print("    %u B (%d/%d in use)\n",
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
    if (!me || !me->isClient) {
        ITS_LOGE("itsConnect: caller [%s] is not initialised as a client (missing itsClientInit?)",
                 pcTaskGetName(nullptr));
        return -1;
    }

    int clientActive = 0;
    for (int i = 0; i < ITS_MAX_CONNS; i++)
        if (connTable[i].active && connTable[i].clientTask == me->task)
            clientActive++;
    if (clientActive >= me->maxConns) {
        ITS_LOGE("itsConnect: [%s] has %d/%d client conns already; raise itsClientInit cap",
                 pcTaskGetName(nullptr), clientActive, me->maxConns);
        return -1;
    }

    its_task_t* se = taskFind(serverTask);
    if (!se || !se->isServer) {
        ITS_LOGE("itsConnect: target task [%s] not initialised as a server",
                 pcTaskGetName(serverTask));
        return -1;
    }

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

    if (!inboxSend(se, buf, totalLen, timeout)) {
        ITS_LOGE("itsConnect: target [%s] inbox full or send timed out",
                 pcTaskGetName(serverTask));
        return -1;
    }

    if (xSemaphoreTake(me->ackSem, timeout) != pdTRUE) {
        ITS_LOGE("itsConnect: target [%s] did not ack within %u ms",
                 pcTaskGetName(serverTask),
                 (unsigned)(timeout * portTICK_PERIOD_MS));
        return -1;
    }

    int handle = me->ackHandle;
    if (handle < 0) {
        ITS_LOGE("itsConnect: target [%s] rejected connection on port %u",
                 pcTaskGetName(serverTask), port);
        return -1;
    }
    its_conn_t* c = &connTable[handle];
    if (ref >= 0)        c->clientRef       = (int8_t)ref;
    if (onRecv)          c->cliRecvCb       = onRecv;
    if (onDisconnect)    c->cliDisconnectCb = onDisconnect;
    return handle;
}

int itsConnect(const char* serverName, uint16_t port,
               const void* data, size_t dataLen, TickType_t timeout, int ref,
               its_recv_cb_t onRecv, its_disconnect_cb_t onDisconnect) {
    TaskHandle_t task = xTaskGetHandle(serverName);
    if (!task) {
        ITS_LOGE("itsConnect: server task [%s] not found (not running?)", serverName);
        return -1;
    }
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

    /* no_pool: defer the buffer free to the disconnect's *receiving* end. The
     * initiator leaves the conn entry live (so its slot can't be reused and the
     * peer's connFree finds the buffers) and just notifies; the peer deletes in
     * its dispatch loop, when neither end is inside a buffer op. Pooled conns
     * keep the original free-now behavior. If the peer can't be notified, the
     * initiator retains the buffers (poolRetainOnFree) — never worse than pooled,
     * no UAF, no leak. */
    bool noPool = c->noPool;
    int  toIdx = c->toPoolIdx, fromIdx = c->fromPoolIdx;

    if (c->serverTask == me) {
        /* Server side closes — embed client cb pointer in the kick message
         * since the conn entry will be freed before the client wakes. */
        TaskHandle_t client = c->clientTask;
        int8_t clientRef = c->clientRef;
        uint16_t port = c->itsPort;
        its_disconnect_cb_t cb = c->cliDisconnectCb;
        if (!noPool) connFree(handle);
        its_task_t* ce = taskFind(client);
        bool notified = false;
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
            /* 100ms blocking — matches the client-side branch below. Earlier
             * timeout=0 here silently dropped the kick under inbox bursts
             * (e.g. cli closing a busy SSH session: sshd's inbox briefly
             * fills with channel-data events, the disconnect kick is the
             * one that gets discarded, sshd never tears the session down,
             * Mac SSH hangs on Ctrl-D forever). Disconnects are rare; a
             * brief block here is the cheap correct fix. */
            notified = inboxSend(ce, buf, sizeof(buf), pdMS_TO_TICKS(100));
        }
        if (noPool && !notified) {
            poolRetainOnFree(toIdx); poolRetainOnFree(fromIdx);
            connFree(handle);
        }
    } else if (c->clientTask == me) {
        /* Client side closes — server callback lives in the per-port
         * table on the server task, no need to embed anything. */
        TaskHandle_t serverTask = c->serverTask;
        int8_t serverRef = c->serverRef;
        uint16_t port = c->itsPort;
        if (!noPool) connFree(handle);
        its_task_t* se = taskFind(serverTask);
        bool notified = false;
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
            notified = inboxSend(se, buf, sizeof(buf), pdMS_TO_TICKS(100));
        }
        if (noPool && !notified) {
            poolRetainOnFree(toIdx); poolRetainOnFree(fromIdx);
            connFree(handle);
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

    /* For ITS_WAIT_PICKUP, ensure the caller has an ITS task entry with its
     * own pickupSem, then drain any stale give on that sem. Receiver will
     * give it after processing the aux. */
    its_task_t* me = nullptr;
    if (wait == ITS_WAIT_PICKUP) {
        me = taskFindOrCreate(xTaskGetCurrentTaskHandle(), 0, 0);
        if (!me || !me->pickupSem) return false;
        xSemaphoreTake(me->pickupSem, 0);
    }

    size_t totalLen = sizeof(its_header_t) + dataLen;
    uint8_t* buf = (uint8_t*)alloca(totalLen);
    auto* hdr = (its_header_t*)buf;
    *hdr = {};
    hdr->sender = xTaskGetCurrentTaskHandle();
    hdr->msg = ITS_MSG_AUX;
    hdr->pickupIdx = (wait == ITS_WAIT_PICKUP) ? 1 : -1;  /* flag only */
    hdr->itsPort = port;
    hdr->len = (uint16_t)dataLen;
    hdr->handle = -1;
    if (data && dataLen > 0)
        memcpy(buf + sizeof(its_header_t), data, dataLen);

    bool delivered = inboxSend(te, buf, totalLen, timeout);
    if (!delivered) return false;

    if (me)
        return xSemaphoreTake(me->pickupSem, timeout) == pdTRUE;

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
/* Receiver-side: consume path fires the sender's pending free-space wake
 * if the threshold is now satisfied. One-shot: clears freeNotify before
 * firing so the wake does not repeat until the sender re-arms. */
static inline void wakeSenderIfReady(int handle, its_pool_entry_t* pe) {
    if (pe->freeNotify == 0) return;
    if (xStreamBufferSpacesAvailable(pe->handle) < pe->freeNotify) return;
    pe->freeNotify = 0;
    xSemaphoreGive(pe->spaceFreedSem);
    TaskHandle_t sender = remoteOf(handle);
    if (sender) xTaskNotifyGive(sender);
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
        if (!itsWaitForSpace(handle, total, timeout)) return 0;

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

        wakeSenderIfReady(handle, pe);
        return got;
    }

    /* Stream mode */
    size_t got = xStreamBufferReceive(sb, buf, maxLen, timeout);
    if (got > 0) wakeSenderIfReady(handle, pe);
    return got;
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

size_t itsRecvBufSize(int handle) {
    its_pool_entry_t* pe = nullptr;
    StreamBufferHandle_t buf = recvBufWithPool(handle, &pe);
    return (buf && pe) ? pe->size : 0;
}

size_t itsSendBufSize(int handle) {
    its_pool_entry_t* pe = nullptr;
    StreamBufferHandle_t buf = sendBufWithPool(handle, &pe);
    return (buf && pe) ? pe->size : 0;
}

bool itsSetTriggerLevel(int handle, size_t triggerLevel) {
    its_pool_entry_t* pe = nullptr;
    StreamBufferHandle_t buf = recvBufWithPool(handle, &pe);
    if (!buf || !pe) return false;
    if (triggerLevel == 0) triggerLevel = 1;
    pe->triggerLevel = triggerLevel;
    return xStreamBufferSetTriggerLevel(buf, triggerLevel);
}

bool itsSetFreeNotify(int handle, size_t freeBytes) {
    its_pool_entry_t* pe = nullptr;
    StreamBufferHandle_t buf = sendBufWithPool(handle, &pe);
    if (!buf || !pe) return false;
    /* Drain any stale sem token from a previous arm so the caller's next
     * itsWaitForSpace / itsPoll sees only fresh wakes. */
    xSemaphoreTake(pe->spaceFreedSem, 0);
    pe->freeNotify = freeBytes;
    /* Close the race where the remote consumed between the caller's last
     * space check and our arm: if the threshold is already met, fire now.
     * Self-notify so a subsequent itsPoll on this task returns promptly. */
    if (freeBytes > 0 && xStreamBufferSpacesAvailable(buf) >= freeBytes) {
        pe->freeNotify = 0;
        xSemaphoreGive(pe->spaceFreedSem);
        xTaskNotifyGive(xTaskGetCurrentTaskHandle());
    }
    return true;
}

bool itsWaitForSpace(int handle, size_t freeBytes, TickType_t timeout) {
    if (freeBytes == 0) return true;
    its_pool_entry_t* pe = nullptr;
    StreamBufferHandle_t buf = sendBufWithPool(handle, &pe);
    if (!buf || !pe) return false;
    if (xStreamBufferSpacesAvailable(buf) >= freeBytes) return true;
    if (timeout == 0) return false;

    TickType_t startTick = xTaskGetTickCount();
    TickType_t remaining = timeout;
    bool success = false;
    for (;;) {
        xSemaphoreTake(pe->spaceFreedSem, 0);   /* clear stale signal */
        pe->freeNotify = freeBytes;
        /* Re-check: remote may have freed space between our last check and
         * our freeNotify set, so could have skipped waking us. */
        if (xStreamBufferSpacesAvailable(buf) >= freeBytes) {
            success = true;
            break;
        }
        BaseType_t got = xSemaphoreTake(pe->spaceFreedSem, remaining);
        /* A disconnect path also gives the sem after poolFree — bail if
         * the connection has gone away under us. Otherwise we'd proceed
         * against a buffer now owned by a different connection. */
        if (!conn(handle)) break;
        if (got != pdTRUE) break;
        if (xStreamBufferSpacesAvailable(buf) >= freeBytes) {
            success = true;
            break;
        }
        if (timeout != portMAX_DELAY) {
            TickType_t elapsed = xTaskGetTickCount() - startTick;
            if (elapsed >= timeout) break;
            remaining = timeout - elapsed;
        }
    }
    pe->freeNotify = 0;
    return success;
}

bool itsIsEmpty(int handle) {
    StreamBufferHandle_t buf = recvBuf(handle);
    return buf ? xStreamBufferIsEmpty(buf) : true;
}

bool itsIsFull(int handle) {
    StreamBufferHandle_t buf = sendBuf(handle);
    return buf ? xStreamBufferIsFull(buf) : true;
}

bool itsSendIsEmpty(int handle) {
    StreamBufferHandle_t buf = sendBuf(handle);
    return buf ? xStreamBufferIsEmpty(buf) : true;
}

bool itsSendDrain(int handle, uint32_t timeoutMs) {
    TickType_t start = xTaskGetTickCount();
    TickType_t deadline = start + pdMS_TO_TICKS(timeoutMs);
    while (!itsSendIsEmpty(handle)) {
        if (!itsConnected(handle)) return false;
        if (xTaskGetTickCount() >= deadline) return false;
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    return true;
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
