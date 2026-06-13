#include "its.h"
#include "esp_log.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "pm.h"
#include <string.h>
#include <atomic>
#include "sdkconfig.h"

/* Kconfig defaults (see esp-idf/Kconfig). The 0 cases of the per-task
 * mailbox knobs resolve to these. */
#ifndef CONFIG_SPANGAP_ITS_INBOX_DEPTH
#define CONFIG_SPANGAP_ITS_INBOX_DEPTH   32
#endif
#ifndef CONFIG_SPANGAP_ITS_INBOX_MSG_MAX
#define CONFIG_SPANGAP_ITS_INBOX_MSG_MAX 4096
#endif
#ifndef CONFIG_SPANGAP_ITS_PKT_DEPTH
#define CONFIG_SPANGAP_ITS_PKT_DEPTH     16
#endif
#ifndef CONFIG_SPANGAP_ITS_MSG_MAX
#define CONFIG_SPANGAP_ITS_MSG_MAX       65536
#endif

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
    /* Split allocation (see poolGet): the stream buffer's control block embeds
     * an SMP spinlock that MUST live in internal RAM (its S32C1I atomic is
     * unreliable on PSRAM, and critical sections run during flash cache-disable
     * windows); the ring storage is large and is memcpy'd outside the lock, so
     * it stays in PSRAM. Both freed together in the no_pool path of poolFree. */
    StaticStreamBuffer_t* sbCtrl;    /* control block — internal RAM */
    uint8_t*             sbStore;    /* ring storage — PSRAM */
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
    e.sbCtrl = nullptr;
    e.sbStore = nullptr;
    portEXIT_CRITICAL(&itsPoolMux);

    /* Control block internal (spinlock), ring storage PSRAM — see its_pool_entry_t.
     * Storage area must be xBufferSizeBytes + 1 (FreeRTOS static requirement). */
    StaticStreamBuffer_t* ctrl  = (StaticStreamBuffer_t*)heap_caps_malloc(
                                      sizeof(StaticStreamBuffer_t),
                                      MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    uint8_t*              store = (uint8_t*)heap_caps_malloc(size + 1, MALLOC_CAP_SPIRAM);
    if (ctrl && store) {
        e.sbCtrl  = ctrl;
        e.sbStore = store;
        e.handle  = xStreamBufferCreateStatic(size, 1, store, ctrl);
    } else {
        heap_caps_free(ctrl);
        heap_caps_free(store);
        e.handle = nullptr;
    }
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
        StreamBufferHandle_t  h     = e.handle;
        StaticStreamBuffer_t* ctrl  = e.sbCtrl;
        uint8_t*              store = e.sbStore;
        e.handle  = nullptr;
        e.sbCtrl  = nullptr;
        e.sbStore = nullptr;
        e.size = 0;
        e.inUse = false;
        e.triggerLevel = 1;
        e.freeNotify = 0;
        portEXIT_CRITICAL(&itsPoolMux);
        if (h) {
            vStreamBufferDelete(h);    /* static buffer: tears down, frees nothing itself */
            heap_caps_free(ctrl);      /* control block (internal RAM) */
            heap_caps_free(store);     /* ring storage (PSRAM) */
        }
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

/* ---- Ownership counters (the leak detector) ----
 * Tracked at the ownership boundary, not at malloc: payloads may be adopted
 * from foreign allocators (itsSendAuxOwnedByTaskHandle / itsSendOwned) and the
 * consumer frees with plain free(). Headers and payloads counted separately. */
struct its_ctr_t {
    std::atomic<size_t> liveBlocks{0};
    std::atomic<size_t> liveBytes{0};
    std::atomic<size_t> hwBlocks{0};
    std::atomic<size_t> hwBytes{0};
};
static its_ctr_t hdrCtr;   /* its_msg headers */
static its_ctr_t payCtr;   /* payload blocks (mailbox messages + packet-link payloads) */

static inline void ctrAdd(its_ctr_t& c, size_t bytes) {
    size_t lb = c.liveBlocks.fetch_add(1, std::memory_order_relaxed) + 1;
    size_t by = c.liveBytes.fetch_add(bytes, std::memory_order_relaxed) + bytes;
    /* Best-effort high-water (a stat, not a guard) — a racy lost update only
     * undercounts the peak, never corrupts the live counters. */
    if (lb > c.hwBlocks.load(std::memory_order_relaxed))
        c.hwBlocks.store(lb, std::memory_order_relaxed);
    if (by > c.hwBytes.load(std::memory_order_relaxed))
        c.hwBytes.store(by, std::memory_order_relaxed);
}
static inline void ctrSub(its_ctr_t& c, size_t bytes) {
    c.liveBlocks.fetch_sub(1, std::memory_order_relaxed);
    c.liveBytes.fetch_sub(bytes, std::memory_order_relaxed);
}

/* Payload acquire/release wrappers (the only places payload memory crosses the
 * ownership boundary). payAlloc allocates+adopts (copying sends); payAdopt
 * adopts a foreign block (itsSendOwned); payDrop frees+uncounts; payHandoff
 * uncounts WITHOUT freeing (itsRecvRef — the app now owns the block). */
static void* payAlloc(size_t len) {
    if (len == 0) return nullptr;
    void* p = heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
    if (p) ctrAdd(payCtr, len);
    return p;
}
static inline void payAdopt(size_t len) { ctrAdd(payCtr, len); }
static inline void payDrop(void* p, size_t len) {
    if (!p) return;
    free(p);
    ctrSub(payCtr, len);
}
static inline void payHandoff(size_t len) { ctrSub(payCtr, len); }

/* ---- Packet-link descriptor rings (ITS_PACKET) ----
 *
 * A packet link is a per-connection, per-direction flow of whole messages.
 * Each direction holds a small ring of 8-byte DESCRIPTORS; the payload bytes
 * live in separate heap blocks (borrowed per message, freed on read), so idle
 * cost is ~the ring (depth*8 B) and memory grows only under traffic. This is
 * what reclaims seccam's 256 KB / storage's 48-16 KB permanent reservations.
 *
 * The descriptor ring itself is tiny and POOLED exactly like the stream-buffer
 * pool above: reused/reset across same-depth connections, NEVER deleted. That
 * sidesteps the no_pool deletion hazard entirely — a racing sender can never
 * UAF a freed ring or sem, because nothing is freed. The reclaim comes from
 * the payloads, not the ring. Single writer and single reader per direction
 * (stated invariant — the ring and the byte counter rely on it). */
struct its_desc { uint32_t len; void* ptr; };   /* 8 bytes; one queued message */

struct its_link_t {
    StreamBufferHandle_t ring;          /* descriptor ring: depth*8 bytes of its_desc */
    size_t               depth;         /* descriptor slots (records) */
    size_t               byteCap;       /* backpressure window (payload bytes) */
    size_t               maxMsg;        /* per-message size guard */
    std::atomic<size_t>  outstanding;   /* payload bytes currently queued (D9) */
    volatile size_t      freeNotify;    /* sender's requested free bytes (one-shot) */
    SemaphoreHandle_t    spaceFreedSem; /* wakes a blocked sender on slot-free/window-open */
    volatile bool        inUse;
};

/* Two directions per packet connection, so size for every conn (ITS_MAX_CONNS
 * = 128) being a packet link — a link is then always available whenever a conn
 * slot is, matching the old per-conn stream-buffer pool's effective capacity.
 * itsLinks lives in PSRAM (below), so this costs PSRAM (abundant), not the
 * scarce internal DRAM. */
#define ITS_MAX_LINKS 256
/* PSRAM (task-context-only, like s_tasks): allocated lazily on the first
 * packet-link connection so it costs nothing until a packet port is used. */
static its_link_t*  itsLinks = nullptr;
static int          itsLinkCount = 0;
static portMUX_TYPE itsLinkMux = portMUX_INITIALIZER_UNLOCKED;

/* Acquire a descriptor ring of `depth` records, with this direction's byte cap
 * and per-message guard. Reuses a free same-depth slot (reset) or creates one.
 * Returns an index into itsLinks[], or -1. */
static int linkAcquire(size_t depth, size_t byteCap, size_t maxMsg) {
    if (depth == 0) depth = 1;
    portENTER_CRITICAL(&itsLinkMux);
    for (int i = 0; i < itsLinkCount; i++) {
        if (!itsLinks[i].inUse && itsLinks[i].ring && itsLinks[i].depth == depth) {
            itsLinks[i].inUse = true;
            itsLinks[i].byteCap = byteCap;
            itsLinks[i].maxMsg = maxMsg;
            itsLinks[i].freeNotify = 0;
            itsLinks[i].outstanding.store(0, std::memory_order_relaxed);
            portEXIT_CRITICAL(&itsLinkMux);
            xStreamBufferReset(itsLinks[i].ring);
            return i;
        }
    }
    int idx = -1;
    for (int i = 0; i < itsLinkCount; i++)
        if (!itsLinks[i].inUse && itsLinks[i].ring == nullptr) { idx = i; break; }
    if (idx < 0) {
        if (itsLinkCount >= ITS_MAX_LINKS) {
            portEXIT_CRITICAL(&itsLinkMux);
            ITS_LOGE("link table full (%d), cannot add depth-%u ring (raise ITS_MAX_LINKS)",
                     ITS_MAX_LINKS, (unsigned)depth);
            return -1;
        }
        idx = itsLinkCount++;
        itsLinks[idx].spaceFreedSem = nullptr;
    }
    its_link_t& L = itsLinks[idx];
    L.depth = depth;
    L.byteCap = byteCap;
    L.maxMsg = maxMsg;
    L.freeNotify = 0;
    L.inUse = true;
    L.ring = nullptr;
    L.outstanding.store(0, std::memory_order_relaxed);
    portEXIT_CRITICAL(&itsLinkMux);

    /* (depth+1) records of headroom: a FreeRTOS stream buffer holds one byte
     * short of its size, so size it so `depth` whole 8-byte descriptors always
     * fit. The extra 8 bytes is negligible. */
    /* Internal RAM, not PSRAM: the stream buffer's control block embeds an SMP
     * spinlock (taskENTER_CRITICAL in every send/recv) whose S32C1I atomic is
     * unreliable on PSRAM and is touched during flash cache-disable windows.
     * The ring is small ((depth+1)*8 B of descriptors), so the whole buffer can
     * live internally rather than splitting control/storage like the pool. */
    L.ring = xStreamBufferCreateWithCaps((depth + 1) * sizeof(its_desc), sizeof(its_desc),
                                         MALLOC_CAP_INTERNAL);
    if (L.spaceFreedSem == nullptr) L.spaceFreedSem = xSemaphoreCreateBinary();
    if (!L.ring || !L.spaceFreedSem) {
        ITS_LOGE("link alloc failed for depth-%u ring", (unsigned)depth);
        return -1;
    }
    return idx;
}

/* Release a link: DRAIN every queued descriptor freeing its payload (the
 * teardown arm of the ownership rule), reset the ring for reuse, wake any
 * blocked sender, and mark the slot free. The ring + sem are retained (never
 * deleted) so a racing sender can't UAF them — it re-checks conn() and bails. */
static void linkFree(int idx) {
    if (idx < 0 || idx >= itsLinkCount) return;
    its_link_t& L = itsLinks[idx];
    if (L.ring) {
        its_desc d;
        while (xStreamBufferBytesAvailable(L.ring) >= sizeof(its_desc) &&
               xStreamBufferReceive(L.ring, &d, sizeof(its_desc), 0) == sizeof(its_desc))
            payDrop(d.ptr, d.len);
        xStreamBufferReset(L.ring);
    }
    portENTER_CRITICAL(&itsLinkMux);
    L.inUse = false;
    L.freeNotify = 0;
    L.outstanding.store(0, std::memory_order_relaxed);
    portEXIT_CRITICAL(&itsLinkMux);
    /* Wake a sender blocked waiting for space — conn is gone, so it aborts. */
    if (L.spaceFreedSem) xSemaphoreGive(L.spaceFreedSem);
}

static inline its_link_t* linkAt(int idx) {
    return (idx >= 0 && idx < itsLinkCount) ? &itsLinks[idx] : nullptr;
}

/* Drain a link's queued payloads and reset its ring WITHOUT releasing the slot
 * (the conn stays active). Used by itsReset. */
static void linkPurge(int idx) {
    its_link_t* L = linkAt(idx);
    if (!L || !L->ring) return;
    its_desc d;
    while (xStreamBufferBytesAvailable(L->ring) >= sizeof(its_desc) &&
           xStreamBufferReceive(L->ring, &d, sizeof(its_desc), 0) == sizeof(its_desc))
        payDrop(d.ptr, d.len);
    xStreamBufferReset(L->ring);
    L->outstanding.store(0, std::memory_order_relaxed);
    if (L->spaceFreedSem) xSemaphoreGive(L->spaceFreedSem);
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
    bool                packetBased;  /* true for ITS_PACKET_LEGACY (framed-in-ring) */
    uint8_t             kind;         /* its_port_kind_t: STREAM / PACKET_LEGACY / PACKET */
    bool                noPool;       /* server opted out of the buffer pool (streams) */
    TaskHandle_t        clientTask;
    TaskHandle_t        serverTask;
    int                 toPoolIdx;    /* stream/legacy: client→server ring (pool idx) */
    int                 fromPoolIdx;  /* stream/legacy: server→client ring (pool idx) */
    int                 toLinkIdx;    /* ITS_PACKET: client→server descriptor ring */
    int                 fromLinkIdx;  /* ITS_PACKET: server→client descriptor ring */
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
            connTable[idx].toLinkIdx = -1;
            connTable[idx].fromLinkIdx = -1;
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
    int toLink = c->toLinkIdx, fromLink = c->fromLinkIdx;
    *c = {};
    c->toPoolIdx = -1;
    c->fromPoolIdx = -1;
    c->toLinkIdx = -1;
    c->fromLinkIdx = -1;
    portEXIT_CRITICAL(&connMux);
    poolFree(toIdx);
    poolFree(fromIdx);
    /* Packet links: drain+free both descriptor rings (frees every in-flight
     * payload), same race discipline as the pool (read+null under the lock,
     * so a double close can't double-drain). */
    linkFree(toLink);
    linkFree(fromLink);
}

static its_conn_t* conn(int handle) {
    if (handle < 0 || handle >= ITS_MAX_CONNS) return nullptr;
    return connTable[handle].active ? &connTable[handle] : nullptr;
}

/* ---- Mailbox message header + the one ownership rule ----
 *
 * THE OWNERSHIP RULE (this is the whole discipline; every send, receive and
 * teardown path below is an instance of it):
 *
 *   The transport always frees the header. The payload is freed by whoever
 *   holds it last: by default the transport, immediately after delivery —
 *   unless the receiver takes it. Ownership transfers on SUCCESS only: a
 *   backpressure/timeout return leaves an owned payload with the caller.
 *   Every path that discards a queued message drains it and frees each
 *   payload. Every ownership entry and exit goes through the counting
 *   wrappers (msgAlloc/msgFree, payAlloc/payAdopt/payDrop) so a leak is a
 *   number (see itsStatus), not a hunch.
 *
 * The inbox is a FreeRTOS queue of single `its_msg*` pointers. Each points at
 * a fixed header the transport owns and never looks past; the header carries
 * only what is needed to route, count and free a message. The payload (or
 * none) is one separate heap block, freeable with a single free(); it never
 * carries ownership of other allocations. */

enum its_kind_t : uint8_t {
    ITS_K_CONNECT,            /* payload = handshake, or NULL          */
    ITS_K_DISC_FROM_CLIENT,   /* metadata only (client closed)         */
    ITS_K_DISC_FROM_SERVER,   /* metadata only; cb = client's disc cb  */
    ITS_K_FORWARD,            /* payload = forward data                */
    ITS_K_AUX,                /* payload = aux data                    */
};

#define ITS_F_PICKUP 0x01     /* give sender's pickupSem after dispatch */

struct its_msg {              /* one heap allocation; the mailbox slot points at it */
    uint8_t             kind;
    uint8_t             flags;
    uint16_t            port;
    int16_t             handle;   /* connection handle, or -1                       */
    int8_t              ref;      /* disconnects: peer's ref for the callback; else -1 */
    uint8_t             rsvd;
    TaskHandle_t        sender;   /* for replies / accept-ack / pickup              */
    its_disconnect_cb_t cb;       /* ITS_K_DISC_FROM_SERVER only; else NULL         */
    uint32_t            len;      /* payload CONTENT length (not allocation size)   */
    void*               payload;  /* separate heap block, or NULL                   */
};

/* Header acquire/release. msgAlloc returns a zeroed header with handle/ref
 * defaulted to -1; msgFree frees the payload (if still owned) then the
 * header — the blind teardown drain relies on this freeing ANY message from
 * the header alone. */
static its_msg* msgAlloc() {
    its_msg* m = (its_msg*)heap_caps_malloc(sizeof(its_msg), MALLOC_CAP_SPIRAM);
    if (!m) return nullptr;
    *m = {};
    m->handle = -1;
    m->ref = -1;
    ctrAdd(hdrCtr, sizeof(its_msg));
    return m;
}
static void msgFree(its_msg* m) {
    if (!m) return;
    if (m->payload) payDrop(m->payload, m->len);
    free(m);
    ctrSub(hdrCtr, sizeof(its_msg));
}

/* ---- Per-task registry ---- */

#define ITS_MAX_TASKS  48

struct its_aux_entry_t {
    bool         active;
    uint16_t     port;
    its_aux_cb_t cb;
};

struct its_server_port_t {
    bool                  active;
    bool                  packetBased;  /* derived: kind == ITS_PACKET_LEGACY */
    uint8_t               kind;         /* its_port_kind_t */
    uint16_t              port;
    int                   maxHandles;
    size_t                toSize;       /* stream/legacy: ring bytes; packet: byte window */
    size_t                fromSize;
    size_t                depth;        /* ITS_PACKET: descriptor slots per direction */
    size_t                maxMsg;       /* ITS_PACKET: per-message size guard */
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

    /* Inbox: a queue of its_msg* pointers. inboxMsgMax is the per-task
     * payload size guard (floored at ITS_MAX_MSG_DATA). The in-flight payload
     * bytes currently queued here live in the parallel s_pendingPay[] array
     * (kept out of this memset-initialized struct so the struct stays
     * trivially copyable). */
    QueueHandle_t            inbox;
    size_t                   inboxMsgMax;
    int                      inboxDepth;

    bool                  isServer;
    bool                  isClient;
    /* no_pool: when set, this server's connections bypass the shared buffer
     * pool — each stream buffer is created fresh on connect and deleted on
     * disconnect (by the disconnect's receiving end). Returns transient/large
     * buffers to the heap and keeps per-task heap attribution exact. Off by
     * default; opt in via itsServerInit. */
    bool                  no_pool;
};

/* s_tasks is large (~23 KB: each entry holds serverPorts[8] + auxCallbacks[8]
 * + inbox plumbing) and is task-context-only — never touched from an ISR or a
 * cache-disabled window — so it lives in PSRAM, not internal DRAM, which is the
 * scarce/DMA-capable pool on display+radio boards. Allocated lazily on the
 * first task registration. */
static its_task_t* s_tasks = nullptr;
static int         s_taskCount = 0;

/* In-flight payload bytes queued in each task's inbox, parallel to s_tasks[]
 * (kept out of its_task_t so that struct stays memset-able). Senders increment
 * on enqueue, the receiving task decrements on dispatch — multiple writers, so
 * atomic. Slots are append-only (s_tasks never shrinks), so an index is stable
 * for a task's lifetime. */
static std::atomic<size_t> s_pendingPay[ITS_MAX_TASKS];

static inline std::atomic<size_t>& pendingPayOf(its_task_t* t) {
    return s_pendingPay[t - s_tasks];
}

static its_task_t* taskFind(TaskHandle_t task) {
    if (!s_tasks || !task) return nullptr;        /* no table yet, or a NULL'd
                                                   * (dead-task) slot — never
                                                   * match those. See
                                                   * vPortCleanUpTCB below. */
    for (int i = 0; i < s_taskCount; i++)
        if (s_tasks[i].task == task) return &s_tasks[i];
    return nullptr;
}

static its_task_t* taskFindOrCreate(TaskHandle_t task, size_t inboxMaxMsgLen, size_t inboxDepth) {
    its_task_t* e = taskFind(task);
    if (e) return e;
    if (!s_tasks) {
        /* First registration: allocate the big task-context tables in PSRAM
         * (zeroed). Done here — before any connection can exist — so packet
         * connects find itsLinks ready without a concurrent lazy-alloc race
         * (ITS registration is effectively serialized at boot, same assumption
         * the lock-free s_taskCount already relies on). */
        s_tasks = (its_task_t*)heap_caps_calloc(ITS_MAX_TASKS, sizeof(its_task_t),
                                                MALLOC_CAP_SPIRAM);
        itsLinks = (its_link_t*)heap_caps_calloc(ITS_MAX_LINKS, sizeof(its_link_t),
                                                 MALLOC_CAP_SPIRAM);
        if (!s_tasks || !itsLinks) { ITS_LOGE("ITS table alloc failed"); return nullptr; }
    }
    if (s_taskCount >= ITS_MAX_TASKS) {
        ITS_LOGE("task table full (max %d)", ITS_MAX_TASKS);
        return nullptr;
    }

    e = &s_tasks[s_taskCount++];
    memset(e, 0, sizeof(*e));
    e->task = task;
    e->ackHandle = -1;
    e->pickupSem = xSemaphoreCreateBinary();
    pendingPayOf(e).store(0, std::memory_order_relaxed);

    /* inboxMaxMsgLen is now a per-task PAYLOAD size guard, floored at
     * ITS_MAX_MSG_DATA so small-guard tasks never regress below today's cap
     * (320). 0 = the Kconfig default. The queue item is always one pointer. */
    size_t guard = inboxMaxMsgLen > 0 ? inboxMaxMsgLen
                                      : (size_t)CONFIG_SPANGAP_ITS_INBOX_MSG_MAX;
    if (guard < ITS_MAX_MSG_DATA) guard = ITS_MAX_MSG_DATA;
    int depth = inboxDepth > 0 ? (int)inboxDepth : CONFIG_SPANGAP_ITS_INBOX_DEPTH;
    e->inboxMsgMax = guard;
    e->inboxDepth = depth;
    /* Internal RAM, NOT PSRAM: a FreeRTOS queue's control block embeds an SMP
     * spinlock taken by taskENTER_CRITICAL inside xQueueSend/xQueueReceive. Two
     * reasons that spinlock can't sit in PSRAM, even though ITS is task-context
     * only (no ISR may call itsSend/itsSendAux — ISRs should set a heap flag +
     * vTaskNotifyGiveFromISR; see docs/its.md "ISR safety"):
     *   1. the spinlock acquire uses an S32C1I atomic, which is unreliable on
     *      external PSRAM → owner/count desync → `spinlock_acquire ... count==0`
     *      assert (observed: cli task in itsPoll→xQueueReceive).
     *   2. a flash op on the OTHER core disables the cache, making the PSRAM
     *      queue struct unreadable while this core is in the critical section.
     * Slots are pointers (~4 B), so the whole queue is tiny in internal RAM;
     * payloads stay borrowed per-message in PSRAM (read outside the lock). */
    e->inbox = xQueueCreateWithCaps(depth, sizeof(its_msg*), MALLOC_CAP_INTERNAL);
    return e;
}

/* Task-death hook: the Xtensa FreeRTOS port calls this for every deleted task,
 * inside a scheduler critical section (see port.c prvDeleteTCB). pxTCB IS the
 * TaskHandle_t.
 *
 * Why ITS needs it: taskFindOrCreate appends a permanent s_tasks slot the first
 * time a task touches ITS — including a transient task that registers only to
 * use the connectionless fs/storage pickup proxy (pickupArm), then dies. The
 * canonical case is main_task: it drives proxyOp during boot (tlsInit →
 * fs_stat → itsSendAux) and self-deletes when app_main returns. The table is
 * append-only and never shrinks, so its slot survives with a now-dangling
 * TaskHandle. A later send routed to that slot does xTaskNotifyGive(slot->task)
 * (inboxEnqueue) — writing a notification (ucNotifyState = 2) into the FREED
 * TCB: a use-after-free that corrupts whatever has since reused that internal
 * DRAM (observed downstream as a NULL-owner xTaskRemoveFromEventList assert
 * from the USB-Serial-JTAG RX ISR).
 *
 * Fix: NULL the slot's handle so taskFind() can never match the dead task
 * again. The slot, its pickupSem/ackSem and inbox stay allocated (honoring the
 * append-only invariant — they're never freed, just inert); a cached
 * its_task_t* whose ->task is now NULL degrades to xTaskNotifyGive(NULL), which
 * prvGetTCBFromHandle maps to the *current* task — a harmless spurious notify,
 * not a UAF. Single aligned pointer write: no lock needed, and none allowed
 * here (critical-section context — no FreeRTOS calls, no logging, no malloc).
 *
 * NOTE: this covers connectionless / pickup registrants. A task that dies while
 * still owning live conns leaves stale clientTask/serverTask handles that the
 * link-backpressure notifies (remoteOf) use directly — out of scope here; no
 * such task self-deletes today. */
extern "C" void vPortCleanUpTCB(void* pxTCB) {
    if (!s_tasks) return;
    TaskHandle_t dead = (TaskHandle_t)pxTCB;
    for (int i = 0; i < s_taskCount; i++)
        if (s_tasks[i].task == dead) { s_tasks[i].task = nullptr; break; }
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

/* ---- Inbox helpers ----
 *
 * inboxEnqueue takes a fully-built its_msg* and posts the pointer. On SUCCESS
 * ownership transfers to the receiver (do not touch the message after); on
 * failure ownership stays with the caller (who frees it / returns it). The
 * payload-byte accounting for itsTaskMem follows the same boundary: charged
 * to the target on enqueue, credited back by the target on dispatch. */
static bool inboxEnqueue(its_task_t* target, its_msg* m, TickType_t timeout) {
    if (!target || !target->inbox) return false;
    size_t plen = m->payload ? m->len : 0;
    if (xQueueSend(target->inbox, &m, timeout) != pdTRUE) return false;
    if (plen) pendingPayOf(target).fetch_add(plen, std::memory_order_relaxed);
    xTaskNotifyGive(target->task);
    return true;
}

/* Drain-and-free: the only thing allowed to discard mailbox contents. Frees
 * every queued payload and header through the wrappers. Tasks never actually
 * die today (s_tasks never shrinks), so this is future-proofing for task
 * teardown — but it is the mailbox arm of the discipline at the top of the
 * file, kept here so the rule reads the same for every transport. */
[[maybe_unused]] static void inboxDrain(its_task_t* t) {
    if (!t || !t->inbox) return;
    its_msg* m = nullptr;
    while (xQueueReceive(t->inbox, &m, 0) == pdTRUE) {
        size_t plen = m->payload ? m->len : 0;
        if (plen) pendingPayOf(t).fetch_sub(plen, std::memory_order_relaxed);
        msgFree(m);
    }
}

/* Build a COPYING message: allocate the header and, if data is present, copy
 * it into a fresh payload block. Enforces the target's payload guard by
 * REJECTING (returning nullptr) rather than silently truncating — the old
 * clamp-and-deliver behavior was always a latent corruption. Caller owns the
 * returned message until inboxEnqueue succeeds. */
static its_msg* buildCopyMsg(uint8_t kind, uint16_t port, int handle,
                             const void* data, size_t len, its_task_t* target,
                             const char* what) {
    if (len > target->inboxMsgMax) {
        ITS_LOGE("%s payload %u exceeds [%s] mailbox guard %u (rejected)",
                 what, (unsigned)len, pcTaskGetName(target->task),
                 (unsigned)target->inboxMsgMax);
        return nullptr;
    }
    its_msg* m = msgAlloc();
    if (!m) { ITS_LOGE("%s: header alloc failed", what); return nullptr; }
    m->kind = kind;
    m->port = port;
    m->handle = (int16_t)handle;
    m->sender = xTaskGetCurrentTaskHandle();
    m->len = (uint32_t)len;
    if (data && len > 0) {
        m->payload = payAlloc(len);
        if (!m->payload) {
            ITS_LOGE("%s: payload alloc failed (%u B)", what, (unsigned)len);
            msgFree(m);
            return nullptr;
        }
        memcpy(m->payload, data, len);
    }
    return m;
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

/* Packet-link direction resolvers: the link this task SENDS into / RECVS from
 * for `handle`. nullptr if not a packet link or not an endpoint of it. */
static its_link_t* sendLink(int handle) {
    its_conn_t* c = conn(handle);
    if (!c || c->kind != ITS_PACKET) return nullptr;
    TaskHandle_t me = xTaskGetCurrentTaskHandle();
    if (me == c->clientTask) return linkAt(c->toLinkIdx);
    if (me == c->serverTask) return linkAt(c->fromLinkIdx);
    return nullptr;
}
static its_link_t* recvLink(int handle) {
    its_conn_t* c = conn(handle);
    if (!c || c->kind != ITS_PACKET) return nullptr;
    TaskHandle_t me = xTaskGetCurrentTaskHandle();
    if (me == c->clientTask) return linkAt(c->fromLinkIdx);
    if (me == c->serverTask) return linkAt(c->toLinkIdx);
    return nullptr;
}

/* Wake a packet-link sender blocked on backpressure: the receiver just freed a
 * descriptor slot and dropped `outstanding`, so a send waiting for either may
 * now proceed. One-shot freeNotify mirrors the stream path. */
static inline void linkWakeSender(int handle, its_link_t* L) {
    if (L->spaceFreedSem) xSemaphoreGive(L->spaceFreedSem);
    TaskHandle_t sender = remoteOf(handle);
    if (sender) xTaskNotifyGive(sender);
}

/* Bytes the send-direction link would accept from itsSend(timeout=0) right now:
 * 0 if no descriptor slot is free (the slot clause check-then-send callers
 * depend on), else the open window — and a full maxMsg when the link is idle so
 * a single large message is always admissible. */
static size_t linkSendSpace(its_link_t* L) {
    if (xStreamBufferSpacesAvailable(L->ring) < sizeof(its_desc)) return 0;
    size_t out = L->outstanding.load(std::memory_order_relaxed);
    if (out == 0) return L->maxMsg;
    return out < L->byteCap ? (L->byteCap - out) : 0;
}

/* True if a descriptor slot is free AND the byte window admits `len` (a single
 * message is always admissible into an empty link, even if len > byteCap). */
static bool linkCanSend(its_link_t* L, size_t len) {
    if (xStreamBufferSpacesAvailable(L->ring) < sizeof(its_desc)) return false;
    size_t out = L->outstanding.load(std::memory_order_relaxed);
    return out == 0 || out + len <= L->byteCap;
}

/* Block until the send-direction link can admit `len`, the conn tears down, or
 * timeout. Single writer per direction, so once linkCanSend is true nothing
 * else shrinks the room before we enqueue. Returns false on timeout/teardown. */
static bool linkWaitForSpace(int handle, its_link_t* L, size_t len, TickType_t timeout) {
    if (linkCanSend(L, len)) return true;
    if (timeout == 0) return false;
    TickType_t start = xTaskGetTickCount();
    TickType_t remaining = timeout;
    for (;;) {
        xSemaphoreTake(L->spaceFreedSem, 0);     /* clear stale signal */
        L->freeNotify = len;
        if (linkCanSend(L, len)) { L->freeNotify = 0; return true; }
        BaseType_t got = xSemaphoreTake(L->spaceFreedSem, remaining);
        if (!conn(handle)) { L->freeNotify = 0; return false; }  /* torn down */
        if (got == pdTRUE && linkCanSend(L, len)) { L->freeNotify = 0; return true; }
        if (timeout != portMAX_DELAY) {
            TickType_t elapsed = xTaskGetTickCount() - start;
            if (elapsed >= timeout) break;
            remaining = timeout - elapsed;
        } else if (got != pdTRUE) {
            /* portMAX_DELAY take should not spuriously return without a give;
             * loop and re-check. */
        }
    }
    L->freeNotify = 0;
    return false;
}

/* Enqueue one descriptor (payload already owned/counted). Single writer, and
 * the caller has confirmed a slot via linkWaitForSpace, so the send is
 * non-blocking and cannot fail. Charges the byte window, then notifies recv. */
static void linkEnqueue(int handle, its_link_t* L, void* ptr, size_t len) {
    its_desc d = { (uint32_t)len, ptr };
    L->outstanding.fetch_add(len, std::memory_order_relaxed);
    xStreamBufferSend(L->ring, &d, sizeof(its_desc), 0);
    TaskHandle_t remote = remoteOf(handle);
    if (remote) xTaskNotifyGive(remote);
}

/* Pop one descriptor (or return false if none within timeout). Decrements the
 * byte window and wakes the sender. The payload is handed to the caller; the
 * caller decides whether to copy+free (itsRecv) or hand off (itsRecvRef). */
static bool linkDequeue(int handle, its_link_t* L, its_desc* out, TickType_t timeout) {
    TickType_t start = xTaskGetTickCount();
    TickType_t remaining = timeout;
    while (xStreamBufferBytesAvailable(L->ring) < sizeof(its_desc)) {
        if (remaining == 0) return false;
        ulTaskNotifyTake(pdFALSE, remaining);
        if (!conn(handle)) return false;
        if (xStreamBufferBytesAvailable(L->ring) >= sizeof(its_desc)) break;
        if (timeout != portMAX_DELAY) {
            TickType_t elapsed = xTaskGetTickCount() - start;
            remaining = (elapsed >= timeout) ? 0 : (timeout - elapsed);
        }
    }
    if (xStreamBufferReceive(L->ring, out, sizeof(its_desc), 0) != sizeof(its_desc))
        return false;
    L->outstanding.fetch_sub(out->len, std::memory_order_relaxed);
    linkWakeSender(handle, L);
    return true;
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

static void processInboxMsg(its_task_t* me, its_msg* m) {
    void* payload = m->payload;          /* borrowed for the dispatch; freed below */

    if (m->kind == ITS_K_CONNECT && me->isServer) {
        its_server_port_t* sp = portFind(me, m->port);
        if (!sp) {
            ITS_LOGE("connect to unopened port %u (from [%s])",
                     m->port, pcTaskGetName(m->sender));
            its_task_t* cli = taskFind(m->sender);
            if (cli) { cli->ackHandle = -1; xSemaphoreGive(cli->ackSem); }
            goto done;
        }

        bool full = serverPortActiveCount(me, m->port) >= sp->maxHandles;
        if (full && sp->onBusy) {
            if (!sp->onBusy(payload, m->len))
                full = serverPortActiveCount(me, m->port) >= sp->maxHandles;
        }

        its_task_t* cli = taskFind(m->sender);

        int handle = -1;
        if (!full && cli) handle = connAlloc();

        bool accepted = false;
        if (handle >= 0) {
            its_conn_t* c = &connTable[handle];
            c->clientTask = m->sender;
            c->serverTask = me->task;
            c->itsPort = m->port;
            c->kind = sp->kind;
            c->packetBased = sp->packetBased;
            c->noPool = me->no_pool;
            bool buffersOk = true;
            if (sp->kind == ITS_PACKET) {
                /* Packet link: a descriptor ring per non-zero direction. The
                 * byte caps are lazy windows, not reservations. */
                if (sp->toSize > 0) {
                    c->toLinkIdx = linkAcquire(sp->depth, sp->toSize, sp->maxMsg);
                    if (c->toLinkIdx < 0) buffersOk = false;
                }
                if (buffersOk && sp->fromSize > 0) {
                    c->fromLinkIdx = linkAcquire(sp->depth, sp->fromSize, sp->maxMsg);
                    if (c->fromLinkIdx < 0) buffersOk = false;
                }
                if (!buffersOk)
                    ITS_LOGE("port %u rejected: no link ring (client %s -> server %s)",
                             m->port, pcTaskGetName(m->sender), pcTaskGetName(me->task));
            } else {
                if (sp->toSize > 0) {
                    c->toPoolIdx = poolGet(sp->toSize, me->no_pool);
                    if (c->toPoolIdx < 0) {
                        ITS_LOGE("port %u rejected: no pool stream >= %u for to-buffer "
                                 "(client %s -> server %s)",
                                 m->port, (unsigned)sp->toSize,
                                 pcTaskGetName(m->sender), pcTaskGetName(me->task));
                        buffersOk = false;
                    }
                }
                if (sp->fromSize > 0) {
                    c->fromPoolIdx = poolGet(sp->fromSize, me->no_pool);
                    if (c->fromPoolIdx < 0) {
                        ITS_LOGE("port %u rejected: no pool stream >= %u for from-buffer "
                                 "(client %s -> server %s)",
                                 m->port, (unsigned)sp->fromSize,
                                 pcTaskGetName(m->sender), pcTaskGetName(me->task));
                        buffersOk = false;
                    }
                }
            }

            if (buffersOk) {
                if (!sp->onConnect) {
                    accepted = true;
                } else {
                    int sRef = sp->onConnect(handle, payload, m->len);
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

    } else if (m->kind == ITS_K_DISC_FROM_CLIENT) {
        /* A client closed a connection to me-as-server. The kind (D8) makes
         * the direction unambiguous, so a dual server+client task (sshd)
         * routes this to the server-role path regardless of its client role.
         * ref/port are read from the live conn if it still exists, else from
         * the metadata-only header (conn already freed). */
        int handle = m->handle;
        int8_t ref = m->ref;
        uint16_t port = m->port;
        its_conn_t* c = conn(handle);
        if (c && c->serverTask == me->task && c->clientTask == m->sender) {
            ref = c->serverRef;
            port = c->itsPort;
            connFree(handle);
        }
        its_server_port_t* sp = portFind(me, port);
        if (sp && sp->onDisconnect && ref >= 0) sp->onDisconnect(ref);

    } else if (m->kind == ITS_K_DISC_FROM_SERVER) {
        /* A server closed my client connection. cb travels in the header so
         * the conn can be freed before I wake (no payload allocation). */
        int handle = m->handle;
        int8_t ref = m->ref;
        its_disconnect_cb_t cb = m->cb;
        its_conn_t* c = conn(handle);
        if (c && c->clientTask == me->task && c->serverTask == m->sender) {
            ref = c->clientRef;
            cb = c->cliDisconnectCb;
            connFree(handle);
        }
        if (ref >= 0 && cb) cb(ref);

    } else if (m->kind == ITS_K_FORWARD && me->isServer) {
        int handle = m->handle;
        its_conn_t* c = conn(handle);
        if (c && c->serverTask == me->task) {
            its_server_port_t* sp = portFind(me, m->port);
            if (!sp) {
                ITS_LOGE("forwarded conn arrived for unopened port %u (from [%s])",
                         m->port, pcTaskGetName(m->sender));
                itsDisconnect(handle);
            } else {
                c->kind = sp->kind;
                c->packetBased = sp->packetBased;
                if (sp->onConnect && sp->onConnect(handle, payload, m->len) < 0)
                    itsDisconnect(handle);
            }
        }

    } else if (m->kind == ITS_K_AUX) {
        its_aux_cb_t cb = nullptr;
        for (int i = 0; i < me->auxCount; i++) {
            if (me->auxCallbacks[i].active && me->auxCallbacks[i].port == m->port) {
                cb = me->auxCallbacks[i].cb;
                break;
            }
        }
        if (cb) cb(m->sender, payload, m->len);
        else ITS_LOGE("aux to unregistered port %u (from [%s])",
                      m->port, pcTaskGetName(m->sender));
    }

done:
    if (m->flags & ITS_F_PICKUP) {
        /* Release sender's per-task pickup sem after dispatch. No-op if the
         * sender isn't registered (shouldn't happen: ITS_WAIT_PICKUP forces
         * registration). */
        its_task_t* s = taskFind(m->sender);
        if (s && s->pickupSem) xSemaphoreGive(s->pickupSem);
    }
    /* Ownership exit: credit the in-flight bytes back to this task, then free
     * the payload (the receiver never took it in step 1) and the header. */
    size_t plen = m->payload ? m->len : 0;
    if (plen) pendingPayOf(me).fetch_sub(plen, std::memory_order_relaxed);
    msgFree(m);
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
        if (isServer) {
            its_server_port_t* sp = portFind(me, c->itsPort);
            if (sp) cb = sp->onRecv;
        } else {
            cb = c->cliRecvCb;
        }
        if (!cb) continue;

        if (c->kind == ITS_PACKET) {
            /* Packet link: pending = descriptor count > 0; "made progress" = a
             * descriptor was popped (count decreased) or the conn went away —
             * the same no-spin rule as below, restated on counts. A callback
             * that backpressures without popping is NOT progress. */
            its_link_t* L = linkAt(isServer ? c->toLinkIdx : c->fromLinkIdx);
            if (!L || !L->ring) continue;
            size_t cnt = xStreamBufferBytesAvailable(L->ring) / sizeof(its_desc);
            if (cnt > 0) {
                cb(i, L->outstanding.load(std::memory_order_relaxed));
                if (!c->active ||
                    xStreamBufferBytesAvailable(L->ring) / sizeof(its_desc) < cnt)
                    any = true;
            }
            continue;
        }

        StreamBufferHandle_t buf = isServer ? poolHandle(c->toPoolIdx)
                                            : poolHandle(c->fromPoolIdx);
        if (!buf) continue;
        size_t avail = xStreamBufferBytesAvailable(buf);
        /* Legacy packet mode: avail <= 4 is either empty or a lone in-flight header. */
        size_t threshold = c->packetBased ? 4 : 0;
        if (avail > threshold) {
            cb(i, avail);
            /* Report activity only if the callback made real progress —
             * drained bytes, or tore the conn down. `avail > threshold` is NOT
             * proof a callback can advance: it may apply backpressure and
             * early-return without draining (e.g. iface-tcp drops an inbound
             * rnsd packet while its net side is down). Counting that as
             * activity makes itsPoll skip its blocking wait and spin on the
             * same undrained buffer, starving IDLE and tripping the task WDT.
             * The next real arrival re-notifies us, so blocking loses nothing.
             * `!c->active` short-circuits before touching a buffer the
             * callback may have freed via itsDisconnect. */
            if (!c->active || xStreamBufferBytesAvailable(buf) < avail)
                any = true;
        }
    }
    return any;
}

/* ---- itsPoll ---- */

bool itsPoll(TickType_t timeout) {
    its_task_t* me = myTask();
    if (!me) {
        if (timeout > 0) {
            pmBoostAuto(false);                                  /* floor while parked */
            if (ulTaskNotifyTake(pdTRUE, timeout)) pmBoostAuto(true);  /* notify wake → boost */
        }
        return false;
    }

    its_msg* m = nullptr;
    bool any = false;

    if (xQueueReceive(me->inbox, &m, 0) == pdTRUE) {
        processInboxMsg(me, m);   /* frees the message */
        any = true;
    }

    if (dispatchRecvCallbacks(me)) any = true;

    if (any) return true;
    if (timeout == 0) return false;

    /* Block until notification, then retry once. Drop to the DFS floor while
     * parked; if we woke on a real notify (an event to handle, not a mere
     * timeout tick) boost to 240 for the handling — held until the next block. */
    pmBoostAuto(false);
    if (ulTaskNotifyTake(pdTRUE, timeout)) pmBoostAuto(true);

    if (xQueueReceive(me->inbox, &m, 0) == pdTRUE) {
        processInboxMsg(me, m);
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

bool itsServerPortOpen(uint16_t port, its_port_kind_t kind, int maxHandles,
                       size_t toCap, size_t fromCap, size_t depth, size_t maxMsg) {
    its_task_t* e = myTask();
    if (!e || !e->isServer) {
        ITS_LOGE("PortOpen on non-server task");
        return false;
    }
    if (kind == ITS_PACKET) {
        if (depth == 0)  depth  = CONFIG_SPANGAP_ITS_PKT_DEPTH;
        if (maxMsg == 0) maxMsg = CONFIG_SPANGAP_ITS_MSG_MAX;
    }
    bool packetBased = (kind == ITS_PACKET_LEGACY);
    its_server_port_t* sp = portFind(e, port);
    if (!sp) {
        for (int i = 0; i < ITS_MAX_PORTS && !sp; i++)
            if (!e->serverPorts[i].active) {
                e->serverPorts[i] = {};
                e->serverPorts[i].active = true;
                e->serverPorts[i].port = port;
                if (i + 1 > e->serverPortCount) e->serverPortCount = i + 1;
                sp = &e->serverPorts[i];
            }
        if (!sp) {
            ITS_LOGE("PortOpen %u: no free port slot (max %d)", port, ITS_MAX_PORTS);
            return false;
        }
    }
    sp->kind = kind;
    sp->packetBased = packetBased;
    sp->maxHandles = maxHandles;
    sp->toSize = toCap;
    sp->fromSize = fromCap;
    sp->depth = depth;
    sp->maxMsg = maxMsg;
    return true;
}

/* Transitional bool overload (D5): false→ITS_STREAM, true→ITS_PACKET_LEGACY.
 * Deleted once every packet port has migrated to ITS_PACKET. */
bool itsServerPortOpen(uint16_t port, bool packetBased, int maxHandles,
                       size_t toSize, size_t fromSize) {
    return itsServerPortOpen(port,
                             packetBased ? ITS_PACKET_LEGACY : ITS_STREAM,
                             maxHandles, toSize, fromSize, 0, 0);
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
        its_conn_t& c = connTable[i];
        if (!c.active) continue;
        if (c.serverTask == task) {
            if (c.toPoolIdx >= 0)   { m.streamBytes += itsPool[c.toPoolIdx].size;   m.streamBufs++; }
            if (c.fromPoolIdx >= 0) { m.streamBytes += itsPool[c.fromPoolIdx].size; m.streamBufs++; }
            /* Packet-link descriptor rings are tiny (depth*8); attribute them to
             * the server, like stream buffers. */
            if (its_link_t* L = linkAt(c.toLinkIdx))   { m.streamBytes += L->depth * sizeof(its_desc); m.streamBufs++; }
            if (its_link_t* L = linkAt(c.fromLinkIdx)) { m.streamBytes += L->depth * sizeof(its_desc); m.streamBufs++; }
        }
        /* In-flight packet payloads on the RECV direction are borrowed memory
         * the heap tracker bills to the sender; attribute them here to the
         * receiving task as the corrective lens (mirrors the mailbox below). */
        its_link_t* rl = nullptr;
        if (c.kind == ITS_PACKET) {
            if (c.serverTask == task)      rl = linkAt(c.toLinkIdx);
            else if (c.clientTask == task) rl = linkAt(c.fromLinkIdx);
        }
        if (rl) m.payloadBytes += rl->outstanding.load(std::memory_order_relaxed);
    }
    /* The task's single inbox queue (PSRAM): depth * pointer-slot storage,
     * plus the in-flight mailbox payload bytes currently queued for this task. */
    its_task_t* t = taskFind(task);
    if (t && t->inbox) {
        m.inboxBytes = (size_t)t->inboxDepth * sizeof(its_msg*);
        m.payloadBytes += pendingPayOf(t).load(std::memory_order_relaxed);
    }
    return m;
}

void itsStatus(int (*print)(const char*, ...)) {
    int active = 0;
    for (int i = 0; i < ITS_MAX_CONNS; i++)
        if (connTable[i].active) active++;

    print("ITS System Status Report\n");
    /* Ownership counters — the leak detector. Idle outstanding should be ~0;
     * non-zero residue at idle is an ownership bug, and the high-water marks
     * say where to look. */
    print("Messages: hdr %u live (%u B, hw %u/%u B)  payload %u live (%u B, hw %u/%u B)\n",
          (unsigned)hdrCtr.liveBlocks.load(), (unsigned)hdrCtr.liveBytes.load(),
          (unsigned)hdrCtr.hwBlocks.load(),  (unsigned)hdrCtr.hwBytes.load(),
          (unsigned)payCtr.liveBlocks.load(), (unsigned)payCtr.liveBytes.load(),
          (unsigned)payCtr.hwBlocks.load(),  (unsigned)payCtr.hwBytes.load());
    print("Connections (%d/%d in use)\n", active, ITS_MAX_CONNS);
    for (int i = 0; i < ITS_MAX_CONNS; i++) {
        its_conn_t* c = &connTable[i];
        if (!c->active) continue;
        const char* cn = c->clientTask ? pcTaskGetName(c->clientTask) : "?";
        const char* sn = c->serverTask ? pcTaskGetName(c->serverTask) : "?";
        const char* kn = c->kind == ITS_PACKET ? "pkt"
                       : c->kind == ITS_PACKET_LEGACY ? "lpkt" : "strm";
        if (c->kind == ITS_PACKET) {
            its_link_t* tL = linkAt(c->toLinkIdx);
            its_link_t* fL = linkAt(c->fromLinkIdx);
            unsigned toB = tL ? (unsigned)tL->outstanding.load() : 0;
            unsigned frB = fL ? (unsigned)fL->outstanding.load() : 0;
            unsigned toC = tL ? (unsigned)(tL->byteCap) : 0;
            unsigned frC = fL ? (unsigned)(fL->byteCap) : 0;
            print("    [%s] -> [%s:%u] %s  to %u/%u B  from %u/%u B\n",
                  cn, sn, c->itsPort, kn, toB, toC, frB, frC);
        } else {
            print("    [%s] -> [%s:%u] %s\n", cn, sn, c->itsPort, kn);
        }
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
    if (c->kind == ITS_PACKET) {
        ITS_LOGE("itsInject unsupported on packet link (handle %d)", handle);
        return 0;
    }
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
    c->kind = sp->kind;
    c->packetBased = sp->packetBased;

    its_msg* m = buildCopyMsg(ITS_K_FORWARD, port, handle, data, dataLen, te, "forward");
    if (!m) return -1;
    if (!inboxEnqueue(te, m, pdMS_TO_TICKS(100))) {
        ITS_LOGE("forward to [%s] inbox full", pcTaskGetName(targetTask));
        msgFree(m);   /* ownership stayed with us on failure */
        return -1;
    }

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

/* Cadence at which itsConnect re-checks for the server task / its open port
 * while waiting for it to come up. Pure internal granularity — the caller's
 * `timeout` is the real budget; this only bounds re-check latency. Self-defined
 * so ITS builds standalone outside any particular project; override with -D if
 * an embedder ever wants a different cadence. */
#ifndef ITS_CONNECT_RECHECK_MS
#define ITS_CONNECT_RECHECK_MS 10
#endif

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

    /* Wait — within the caller's `timeout` — for the target to come up as a
     * server with this port open, rather than failing the instant it isn't
     * ready. A client that races the server's boot then connects as soon as the
     * server is ready, with no retry loop of its own. The single `timeout` is
     * the whole budget; the handshake below gets whatever remains. timeout==0
     * keeps the old fail-fast behaviour; portMAX_DELAY waits indefinitely. */
    const bool forever = (timeout == portMAX_DELAY);
    const TickType_t start = xTaskGetTickCount();
    its_task_t* se = taskFind(serverTask);
    while (!se || !se->isServer || !portFind(se, port)) {
        if (!forever && (timeout == 0 || (xTaskGetTickCount() - start) >= timeout)) {
            if (!se || !se->isServer)
                ITS_LOGE("itsConnect: target task [%s] not initialised as a server",
                         pcTaskGetName(serverTask));
            else
                ITS_LOGE("connect to unopened port %u on [%s]",
                         port, pcTaskGetName(serverTask));
            return -1;
        }
        vTaskDelay(pdMS_TO_TICKS(ITS_CONNECT_RECHECK_MS));
        se = taskFind(serverTask);
    }

    /* Budget the remainder of `timeout` for the handshake so the whole connect
     * stays bounded by the caller's timeout, not up to 2×. */
    TickType_t remaining = portMAX_DELAY;
    if (!forever) {
        TickType_t elapsed = xTaskGetTickCount() - start;
        remaining = elapsed < timeout ? timeout - elapsed : 0;
    }

    xSemaphoreTake(me->ackSem, 0);
    me->ackHandle = -1;

    /* The handshake travels as a payload pointer (no copy of a big connect),
     * bounded by the target's mailbox guard — a handshake too large to fit is
     * rejected, never truncated (truncating a handshake would silently corrupt
     * it). The old ~320-byte connect-arg cap is gone. */
    its_msg* m = buildCopyMsg(ITS_K_CONNECT, port, -1, data, dataLen, se, "connect");
    if (!m) return -1;
    if (!inboxEnqueue(se, m, remaining)) {
        ITS_LOGE("itsConnect: target [%s] inbox full or send timed out",
                 pcTaskGetName(serverTask));
        msgFree(m);   /* ownership stayed with us on failure */
        return -1;
    }

    if (xSemaphoreTake(me->ackSem, remaining) != pdTRUE) {
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
    /* Wait — within `timeout` — for the server task to even be created, then
     * hand the remaining budget to the by-handle path (which further waits for
     * it to become a server with the port open). One caller `timeout` thus
     * spans task-creation + ITS init + handshake. timeout==0 = fail-fast. */
    const bool forever = (timeout == portMAX_DELAY);
    const TickType_t start = xTaskGetTickCount();
    TaskHandle_t task = xTaskGetHandle(serverName);
    while (!task) {
        if (!forever && (timeout == 0 || (xTaskGetTickCount() - start) >= timeout)) {
            ITS_LOGE("itsConnect: server task [%s] not found (not running?)", serverName);
            return -1;
        }
        vTaskDelay(pdMS_TO_TICKS(ITS_CONNECT_RECHECK_MS));
        task = xTaskGetHandle(serverName);
    }
    TickType_t remaining = timeout;
    if (!forever) {
        TickType_t elapsed = xTaskGetTickCount() - start;
        remaining = elapsed < timeout ? timeout - elapsed : 0;
    }
    return itsConnectByTaskHandle(task, port, data, dataLen, remaining, ref,
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
     * no UAF, no leak.
     *
     * Packet links are EXCLUDED from the deferral: their descriptor rings are
     * pooled (never deleted, see linkFree), so connFree-now is safe — a racing
     * peer re-checks conn() and bails, and the ring it might touch still exists.
     * connFree drains both rings, freeing every in-flight payload. */
    bool noPool = c->noPool && (c->kind != ITS_PACKET);
    int  toIdx = c->toPoolIdx, fromIdx = c->fromPoolIdx;

    if (c->serverTask == me) {
        /* Server side closes — the client's disconnect cb travels in the
         * header (ITS_K_DISC_FROM_SERVER, metadata only) since the conn entry
         * is freed before the client wakes. */
        TaskHandle_t client = c->clientTask;
        int8_t clientRef = c->clientRef;
        uint16_t port = c->itsPort;
        its_disconnect_cb_t cb = c->cliDisconnectCb;
        if (!noPool) connFree(handle);
        its_task_t* ce = taskFind(client);
        bool notified = false;
        if (ce) {
            its_msg* m = msgAlloc();
            if (m) {
                m->kind = ITS_K_DISC_FROM_SERVER;
                m->sender = me;
                m->handle = (int16_t)handle;
                m->port = port;
                m->ref = clientRef;
                m->cb = cb;
                /* 100ms blocking — disconnects are rare and the kick must not
                 * be dropped under an inbox burst (cli closing a busy SSH
                 * session: sshd's inbox briefly fills, the dropped kick left
                 * Mac SSH hanging on Ctrl-D forever). */
                notified = inboxEnqueue(ce, m, pdMS_TO_TICKS(100));
                if (!notified) msgFree(m);
            }
        }
        if (noPool && !notified) {
            poolRetainOnFree(toIdx); poolRetainOnFree(fromIdx);
            connFree(handle);
        }
    } else if (c->clientTask == me) {
        /* Client side closes — the server's disconnect cb lives in the
         * per-port table on the server task, so ITS_K_DISC_FROM_CLIENT carries
         * only the server's ref (metadata only). */
        TaskHandle_t serverTask = c->serverTask;
        int8_t serverRef = c->serverRef;
        uint16_t port = c->itsPort;
        if (!noPool) connFree(handle);
        its_task_t* se = taskFind(serverTask);
        bool notified = false;
        if (se) {
            its_msg* m = msgAlloc();
            if (m) {
                m->kind = ITS_K_DISC_FROM_CLIENT;
                m->sender = me;
                m->port = port;
                m->handle = (int16_t)handle;
                m->ref = serverRef;
                notified = inboxEnqueue(se, m, pdMS_TO_TICKS(100));
                if (!notified) msgFree(m);
            }
        }
        if (noPool && !notified) {
            poolRetainOnFree(toIdx); poolRetainOnFree(fromIdx);
            connFree(handle);
        }
    }
}

/* ---- Aux messages ---- */

static bool auxRegistered(its_task_t* te, uint16_t port) {
    for (int i = 0; i < te->auxCount; i++)
        if (te->auxCallbacks[i].active && te->auxCallbacks[i].port == port)
            return true;
    return false;
}

/* For ITS_WAIT_PICKUP, ensure the caller has an ITS task entry with its own
 * pickupSem and drain any stale give. Returns the caller's task entry (or
 * nullptr for ITS_WAIT_DELIVERY). On setup failure returns nullptr and sets
 * *ok=false. */
static its_task_t* pickupArm(its_wait_t wait, bool* ok) {
    *ok = true;
    if (wait != ITS_WAIT_PICKUP) return nullptr;
    its_task_t* me = taskFindOrCreate(xTaskGetCurrentTaskHandle(), 0, 0);
    if (!me || !me->pickupSem) { *ok = false; return nullptr; }
    xSemaphoreTake(me->pickupSem, 0);
    return me;
}

static bool itsSendAuxInternal(TaskHandle_t task, uint16_t port,
                               const void* data, size_t dataLen,
                               TickType_t timeout, its_wait_t wait) {
    its_task_t* te = taskFind(task);
    if (!te) return false;
    if (!auxRegistered(te, port)) {
        ITS_LOGE("aux send to unregistered port %u on [%s]",
                 port, pcTaskGetName(task));
        return false;
    }

    bool ok;
    its_task_t* me = pickupArm(wait, &ok);
    if (!ok) return false;

    its_msg* m = buildCopyMsg(ITS_K_AUX, port, -1, data, dataLen, te, "aux");
    if (!m) return false;
    if (wait == ITS_WAIT_PICKUP) m->flags |= ITS_F_PICKUP;

    if (!inboxEnqueue(te, m, timeout)) { msgFree(m); return false; }

    if (me)
        return xSemaphoreTake(me->pickupSem, timeout) == pdTRUE;
    return true;
}

bool itsSendAuxOwnedByTaskHandle(TaskHandle_t task, uint16_t port,
                                 void* ptr, size_t len, TickType_t timeout,
                                 its_wait_t wait) {
    its_task_t* te = taskFind(task);
    if (!te) return false;   /* caller still owns ptr */
    if (!auxRegistered(te, port)) {
        ITS_LOGE("owned aux send to unregistered port %u on [%s]",
                 port, pcTaskGetName(task));
        return false;
    }
    if (len > te->inboxMsgMax) {
        ITS_LOGE("owned aux payload %u exceeds [%s] mailbox guard %u (rejected)",
                 (unsigned)len, pcTaskGetName(task), (unsigned)te->inboxMsgMax);
        return false;
    }

    bool ok;
    its_task_t* me = pickupArm(wait, &ok);
    if (!ok) return false;

    its_msg* m = msgAlloc();
    if (!m) return false;    /* caller still owns ptr */
    m->kind = ITS_K_AUX;
    m->port = port;
    m->sender = xTaskGetCurrentTaskHandle();
    m->len = (uint32_t)len;
    m->payload = ptr;
    if (wait == ITS_WAIT_PICKUP) m->flags |= ITS_F_PICKUP;
    payAdopt(len);           /* ownership boundary: the adopted block is now counted */

    if (!inboxEnqueue(te, m, timeout)) {
        /* Failure → return ownership of ptr to the caller: detach it from the
         * header so msgFree does not free the caller's block, and undo the
         * adopt counter. */
        m->payload = nullptr;
        ctrSub(payCtr, len);
        msgFree(m);
        return false;
    }

    /* Ownership has now transferred to the receiver: the buffer lives in its
     * inbox and will be freed exactly once when it dispatches. A pickup-wait
     * timeout means the receiver is slow/stuck, NOT that the send failed — we
     * must NOT report false here, or the caller (per the ownership contract:
     * "false return → caller still owns ptr") would free a block ITS already
     * owns, double-freeing it once the receiver finally drains the message.
     * Surface the stall as a warning and still return success. */
    if (me && xSemaphoreTake(me->pickupSem, timeout) != pdTRUE)
        ITS_LOGW("owned aux to port %u on [%s] enqueued but not picked up within "
                 "timeout (receiver stuck?)", port, pcTaskGetName(task));
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

/* Packet-link send core, shared by itsSend (copy) and itsSendOwned (adopt).
 * `owned`: ptr is the caller's heap block; on success ITS owns it, on any
 * failure the caller keeps it. !owned: ITS copies `data` into a fresh block. */
static size_t itsSendPacket(int handle, its_link_t* L, const void* data,
                            size_t len, TickType_t timeout, bool owned) {
    if (len == 0 || len > L->maxMsg) {
        ITS_LOGE("packet len %u out of range (maxMsg %u)",
                 (unsigned)len, (unsigned)L->maxMsg);
        return 0;   /* owned: caller keeps ptr */
    }
    /* Wait for a descriptor slot AND byte window. Single writer per direction,
     * so once admissible nothing else shrinks the room before we enqueue. */
    if (!linkWaitForSpace(handle, L, len, timeout)) return 0;  /* owned: caller keeps */

    void* ptr;
    if (owned) {
        ptr = (void*)data;
        payAdopt(len);               /* ownership boundary: foreign block adopted */
    } else {
        ptr = payAlloc(len);
        if (!ptr) return 0;          /* ITS's own OOM — caller retries (don't-drop) */
        memcpy(ptr, data, len);
    }
    linkEnqueue(handle, L, ptr, len);
    return len;
}

size_t itsSendOwned(int handle, void* ptr, size_t len, TickType_t timeout) {
    its_link_t* L = sendLink(handle);
    if (!L) {  /* packet links only */
        ITS_LOGE("itsSendOwned on non-packet-link handle %d", handle);
        return 0;
    }
    return itsSendPacket(handle, L, ptr, len, timeout, /*owned=*/true);
}

size_t itsSend(int handle, const void* data, size_t len, TickType_t timeout) {
    its_conn_t* c0 = conn(handle);
    if (c0 && c0->kind == ITS_PACKET) {
        its_link_t* L = sendLink(handle);
        if (!L) return 0;
        return itsSendPacket(handle, L, data, len, timeout, /*owned=*/false);
    }

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
    its_conn_t* c0 = conn(handle);
    if (c0 && c0->kind == ITS_PACKET) {
        its_link_t* L = recvLink(handle);
        if (!L) return 0;
        its_desc d;
        if (!linkDequeue(handle, L, &d, timeout)) return 0;
        if (d.len > maxLen) {
            /* Caller's buffer is undersized for this port's traffic — same
             * drain-and-drop contract as legacy packet mode. */
            ITS_LOGE("packet body %u > buf %u, dropping", (unsigned)d.len, (unsigned)maxLen);
            payDrop(d.ptr, d.len);
            return 0;
        }
        memcpy(buf, d.ptr, d.len);
        payDrop(d.ptr, d.len);
        return d.len;
    }

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

bool itsRecvRef(int handle, void** out, size_t* len, TickType_t timeout) {
    its_link_t* L = recvLink(handle);
    if (!L) return false;   /* packet links only */
    its_desc d;
    if (!linkDequeue(handle, L, &d, timeout)) return false;
    /* Ownership exits the transport: uncount WITHOUT freeing — the caller now
     * owns the block and frees it with plain free(). The block is deliberately
     * outside the backpressure window from here on (D9). */
    payHandoff(d.len);
    if (out) *out = d.ptr;
    if (len) *len = d.len;
    return true;
}

bool itsConnected(int handle) {
    return conn(handle) != nullptr;
}

size_t itsBytesAvailable(int handle) {
    its_link_t* L = recvLink(handle);
    if (L) return L->outstanding.load(std::memory_order_relaxed);  /* recv payload bytes */
    StreamBufferHandle_t buf = recvBuf(handle);
    return buf ? xStreamBufferBytesAvailable(buf) : 0;
}

size_t itsSpacesAvailable(int handle) {
    its_link_t* L = sendLink(handle);
    if (L) return linkSendSpace(L);   /* 0 if no slot free, else open window */
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
    its_link_t* L = recvLink(handle);
    if (L) return L->maxMsg;          /* packet link: the size guard (D7) */
    its_pool_entry_t* pe = nullptr;
    StreamBufferHandle_t buf = recvBufWithPool(handle, &pe);
    return (buf && pe) ? pe->size : 0;
}

size_t itsSendBufSize(int handle) {
    its_link_t* L = sendLink(handle);
    if (L) return L->maxMsg;          /* packet link: the size guard (D7) */
    its_pool_entry_t* pe = nullptr;
    StreamBufferHandle_t buf = sendBufWithPool(handle, &pe);
    return (buf && pe) ? pe->size : 0;
}

bool itsSetTriggerLevel(int handle, size_t triggerLevel) {
    if (sendLink(handle) || recvLink(handle)) return false;  /* packet links wake per message */
    its_pool_entry_t* pe = nullptr;
    StreamBufferHandle_t buf = recvBufWithPool(handle, &pe);
    if (!buf || !pe) return false;
    if (triggerLevel == 0) triggerLevel = 1;
    pe->triggerLevel = triggerLevel;
    return xStreamBufferSetTriggerLevel(buf, triggerLevel);
}

bool itsSetFreeNotify(int handle, size_t freeBytes) {
    its_link_t* Lp = sendLink(handle);
    if (Lp) {
        xSemaphoreTake(Lp->spaceFreedSem, 0);
        Lp->freeNotify = freeBytes;
        if (freeBytes > 0 && linkSendSpace(Lp) >= freeBytes) {
            Lp->freeNotify = 0;
            xSemaphoreGive(Lp->spaceFreedSem);
            xTaskNotifyGive(xTaskGetCurrentTaskHandle());
        }
        return true;
    }
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
    its_link_t* Lp = sendLink(handle);
    if (Lp) return linkWaitForSpace(handle, Lp, freeBytes, timeout);
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
    its_link_t* L = recvLink(handle);
    if (L) return xStreamBufferBytesAvailable(L->ring) < sizeof(its_desc);
    StreamBufferHandle_t buf = recvBuf(handle);
    return buf ? xStreamBufferIsEmpty(buf) : true;
}

bool itsIsFull(int handle) {
    its_link_t* L = sendLink(handle);
    if (L) return linkSendSpace(L) == 0;   /* no slot or window full */
    StreamBufferHandle_t buf = sendBuf(handle);
    return buf ? xStreamBufferIsFull(buf) : true;
}

bool itsSendIsEmpty(int handle) {
    its_link_t* L = sendLink(handle);
    if (L)
        return xStreamBufferBytesAvailable(L->ring) < sizeof(its_desc) &&
               L->outstanding.load(std::memory_order_relaxed) == 0;
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
    its_conn_t* c = conn(handle);
    if (c && c->kind == ITS_PACKET) {
        /* Drain both directions (frees in-flight payloads), keep the links. */
        linkPurge(c->toLinkIdx);
        linkPurge(c->fromLinkIdx);
        return true;
    }
    StreamBufferHandle_t sb;
    sb = recvBuf(handle);
    if (sb) xStreamBufferReset(sb);
    sb = sendBuf(handle);
    if (sb) xStreamBufferReset(sb);
    return true;
}
