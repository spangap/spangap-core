/**
 * fs — Unified file I/O, PSRAM-safe.
 *
 * Transparently proxies flash file ops to a DRAM-stack worker task when the
 * caller has a PSRAM stack.  SD card paths and DRAM-stack callers go direct.
 *
 * Also owns LittleFS mounts and first-boot factory copy.
 */
#include "fs.h"
#include "its.h"
#include "compat.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <dirent.h>
#include <errno.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "log.h"
#include "esp_littlefs.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_vfs_fat.h"
#include "ff.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#if CONFIG_DIPTYCH_SDCARD_BUS_SPI
#include "driver/sdspi_host.h"
#include "driver/spi_master.h"
#include "spi_helper.h"
#endif

/* ---- Handle table ---- */

#define MAX_FILE_SLOTS 12
#define MAX_DIR_SLOTS  4

static struct {
    FILE* fp;
    bool  active;
    bool  flash;      /* path was on a flash-backed filesystem at open time */
} fileSlots[MAX_FILE_SLOTS];

static struct {
    DIR* dir;
    bool active;
} dirSlots[MAX_DIR_SLOTS];

static bool firstBoot = false;

/* ---- OTA-aware mount table (label resolved at fs_init) ---- */

static char fixedLabel[12] = "fixed_a";   /* default: app0 → fixed_a */

const fs_mount_t FS_MOUNTS[] = {
    { FS_FIXED, fixedLabel, true,  true, false },
    { FS_STATE, "state",    false, true, true  },
};
const int FS_MOUNT_COUNT = (int)(sizeof(FS_MOUNTS) / sizeof(FS_MOUNTS[0]));

const char* fs_active_fixed_label()   { return fixedLabel; }
const char* fs_inactive_fixed_label() {
    return strcmp(fixedLabel, "fixed_a") == 0 ? "fixed_b" : "fixed_a";
}
const char* fs_inactive_app_label() {
    const esp_partition_t* run = esp_ota_get_running_partition();
    if (run && run->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_1) return "app0";
    return "app1";
}

static int allocSlot() {
    for (int i = 0; i < MAX_FILE_SLOTS; i++)
        if (!fileSlots[i].active) { fileSlots[i].active = true; return i; }
    return -1;
}

bool fs_first_boot() { return firstBoot; }

/* ---- Flash / proxy detection ---- */

static TaskHandle_t fsWorkerHandle = nullptr;

__attribute__((unused)) static bool isFlashPath(const char* path) {
    for (int i = 0; i < FS_MOUNT_COUNT; i++) {
        auto& m = FS_MOUNTS[i];
        if (m.needsDramStack && strncmp(path, m.path, strlen(m.path)) == 0)
            return true;
    }
    return false;
}

__attribute__((unused)) static bool callerOnPsram() {
    int local;
    return !esp_ptr_internal(&local);
}

/* Route ALL fs access through the worker once it's up.
 * This serializes SD access (sdmmc needs internal DMA buffers; concurrent
 * rec/log/CLI hits ESP_ERR_NO_MEM) and is also PSRAM-stack safe.
 * The isFlashPath / callerOnPsram plumbing is kept but unused — flip back
 * to `isFlashPath(path) && callerOnPsram()` to restore the old routing. */
static bool needsProxy(const char*) {
    return fsWorkerHandle != nullptr;
}

static volatile bool sdReady = false;
bool sdAvailable() { return sdReady; }

/* Where the active state store lives this boot. There is NO path rewriting:
 * "/state" is *always* the on-flash partition (always mounted), and
 * "/sdcard/state" is always the SD directory. This single string just says
 * which one is active — set once at fs_init() and never changed. Every
 * consumer of the state store builds its paths from fsStateDir(); nothing
 * hard-codes FS_STATE for that purpose. Static and unambiguous: the path
 * you see is the path that's used. */
static char s_stateDir[24] = FS_STATE;          /* "/state" | "/sdcard/state" */
const char* fsStateDir() { return s_stateDir; }

/* Convenience: fsStateDir() + sub (sub must start with '/'). */
std::string fsStatePath(const char* sub) {
    return std::string(s_stateDir) + sub;
}

/* Derived, non-authoritative: the active state store is the SD one. Kept
 * for callers that need the predicate (e.g. the `reset factory` guard);
 * the string above is the single source of truth. */
bool fsStateOnSd() { return strcmp(s_stateDir, FS_STATE) != 0; }

/* Register the on-flash `state` LittleFS partition at /state (format on
 * mount failure, matching the FS_MOUNTS table entry). It is ALWAYS mounted,
 * regardless of where the active state store is. Shared by fs_init() and
 * fsFormatFlash(). */
static esp_err_t mountStateLittlefs() {
    esp_vfs_littlefs_conf_t conf = {};
    conf.base_path = FS_STATE;
    conf.partition_label = "state";
    conf.format_if_mount_failed = true;
    return esp_vfs_littlefs_register(&conf);
}

static bool needsProxyHandle(int f) {
    if (f < 0 || f >= MAX_FILE_SLOTS || !fileSlots[f].active) return false;
    return fsWorkerHandle != nullptr;
}


/* ---- Worker task (DRAM stack) ---- */

static constexpr uint16_t FS_OP_PORT = 1;

struct fs_op_t {
    enum Op { OPEN, READ, WRITE, TELL, SEEK, FLUSH, SYNC, TRUNCATE, CLOSE,
              STAT, RENAME, REMOVE, MKDIR,
              OPENDIR, READDIR, CLOSEDIR, LISTDIR,
              FILE_INFO } op;
    const char* path;
    const char* path2;      /* rename: dest; open: mode */
    int     slot;
    void*   buf;
    size_t  size;
    size_t  nmemb;
    long    offset;
    int     whence;
    struct stat* st;
    bool    flag;           /* file_info: tryGz */
    int     result;
};

static SemaphoreHandle_t fsReady = nullptr;

static void handleOp(fs_op_t* req) {
    /* No path rewriting: "/state" and "/sdcard/state" are both real, always-
     * mounted locations. Callers already pass whichever one is active. */
    switch (req->op) {
    case fs_op_t::OPEN: {
        FILE* fp = fopen(req->path, req->path2);
        if (!fp) { req->result = -1; break; }
        fileSlots[req->slot].fp = fp;
        req->result = req->slot;
        break;
    }
    case fs_op_t::READ: {
        auto& s = fileSlots[req->slot];
        req->result = (s.fp) ? (int)fread(req->buf, req->size, req->nmemb, s.fp) : 0;
        break;
    }
    case fs_op_t::WRITE: {
        auto& s = fileSlots[req->slot];
        req->result = (s.fp) ? (int)fwrite(req->buf, req->size, req->nmemb, s.fp) : 0;
        break;
    }
    case fs_op_t::TELL: {
        auto& s = fileSlots[req->slot];
        req->result = (s.fp) ? (int)ftell(s.fp) : -1;
        break;
    }
    case fs_op_t::SEEK: {
        auto& s = fileSlots[req->slot];
        req->result = (s.fp) ? fseek(s.fp, req->offset, req->whence) : -1;
        break;
    }
    case fs_op_t::FLUSH: {
        auto& s = fileSlots[req->slot];
        req->result = (s.fp) ? fflush(s.fp) : -1;
        break;
    }
    case fs_op_t::SYNC: {
        auto& s = fileSlots[req->slot];
        if (s.fp) { fflush(s.fp); req->result = fsync(fileno(s.fp)); }
        else req->result = -1;
        break;
    }
    case fs_op_t::TRUNCATE: {
        auto& s = fileSlots[req->slot];
        if (s.fp) { fflush(s.fp); req->result = ftruncate(fileno(s.fp), (off_t)req->offset); }
        else req->result = -1;
        break;
    }
    case fs_op_t::CLOSE: {
        auto& s = fileSlots[req->slot];
        if (s.fp) { fclose(s.fp); s.fp = nullptr; }
        s.active = false;
        req->result = 0;
        break;
    }
    case fs_op_t::STAT:
        /* "/" isn't a real VFS mount — synthesize a directory entry so cd /,
         * stat checks, etc. work. */
        if (req->path && (strcmp(req->path, "/") == 0 || req->path[0] == '\0')) {
            memset(req->st, 0, sizeof(*req->st));
            req->st->st_mode = S_IFDIR | 0755;
            req->result = 0;
            break;
        }
        req->result = stat(req->path, req->st);
        break;
    case fs_op_t::RENAME:
        req->result = rename(req->path, req->path2);
        if (req->result != 0) {
            /* FATFS f_rename does NOT overwrite an existing destination
             * (returns FR_EXIST); LittleFS/POSIX rename does. Make rename
             * overwrite-correct everywhere (atomicWriteJson, mv, ...): if
             * the destination already exists, drop it and retry. Brief
             * window where dest is absent — unavoidable on FAT, but the
             * source still exists so no data is lost. No-op on LittleFS
             * (the first rename already succeeded). */
            struct stat st;
            if (stat(req->path2, &st) == 0) {
                remove(req->path2);
                req->result = rename(req->path, req->path2);
            }
        }
        break;
    case fs_op_t::REMOVE:
        req->result = remove(req->path);
        break;
    case fs_op_t::MKDIR:
        req->result = mkdir(req->path, 0755);
        break;
    case fs_op_t::OPENDIR: {
        DIR* d = opendir(req->path);
        if (!d) { req->result = -1; break; }
        dirSlots[req->slot].dir = d;
        req->result = req->slot;
        break;
    }
    case fs_op_t::READDIR: {
        auto& s = dirSlots[req->slot];
        auto* out = (fs_dirent_t*)req->buf;
        struct dirent* ent = s.dir ? readdir(s.dir) : nullptr;
        if (!ent) { req->result = 0; break; }
        safeStrncpy(out->name, ent->d_name, sizeof(out->name));
        out->d_type = ent->d_type;
        req->result = 1;
        break;
    }
    case fs_op_t::CLOSEDIR: {
        auto& s = dirSlots[req->slot];
        if (s.dir) { closedir(s.dir); s.dir = nullptr; }
        s.active = false;
        req->result = 0;
        break;
    }
    case fs_op_t::FILE_INFO: {
        auto* out = (fs_file_info_t*)req->buf;
        struct stat st;
        char gzPath[256];
        bool found = false;
        if (req->flag) {
            snprintf(gzPath, sizeof(gzPath), "%.250s.gz", req->path);
            if (stat(gzPath, &st) == 0) { out->gzipped = true; found = true; }
        }
        if (!found) {
            if (stat(req->path, &st) != 0) { req->result = -1; break; }
            out->gzipped = false;
        }
        out->size  = (size_t)st.st_size;
        out->mtime = st.st_mtime;
        req->result = 0;
        break;
    }
    case fs_op_t::LISTDIR: {
        auto* out = (fs_listing_t*)req->buf;
        int max = (int)req->nmemb;
        /* "/" isn't a real VFS mount — synthesize the mount points. */
        if (req->path && (strcmp(req->path, "/") == 0 || req->path[0] == '\0')) {
            int count = 0;
            const char* names[] = { "fixed", "state", "sdcard" };
            const char* paths[] = { FS_FIXED, FS_STATE, FS_SDCARD };
            for (size_t i = 0; i < sizeof(names) / sizeof(names[0]) && count < max; i++) {
                if (strcmp(names[i], "sdcard") == 0 && !sdReady) continue;
                struct stat st;
                safeStrncpy(out[count].name, names[i], sizeof(out[count].name));
                out[count].size  = 0;
                out[count].mtime = (stat(paths[i], &st) == 0) ? st.st_mtime : 0;
                out[count].isDir = true;
                count++;
            }
            req->result = count;
            break;
        }
        DIR* d = opendir(req->path);
        if (!d) { req->result = -1; break; }
        int count = 0;
        struct dirent* ent;
        char fp[256];
        while ((ent = readdir(d)) && count < max) {
            snprintf(fp, sizeof(fp), "%.180s/%.64s", req->path, ent->d_name);
            struct stat st;
            if (stat(fp, &st) != 0) continue;
            safeStrncpy(out[count].name, ent->d_name, sizeof(out[count].name));
            out[count].size  = (uint32_t)st.st_size;
            out[count].mtime = st.st_mtime;
            out[count].isDir = S_ISDIR(st.st_mode);
            count++;
        }
        closedir(d);
        req->result = count;
        break;
    }
    }
}

static volatile uint32_t fsOpCount = 0;
static volatile int fsCurrentOp = -1;
static volatile int fsCurrentSlot = -1;

static void onFsOp(TaskHandle_t, const void* data, size_t len) {
    static uint32_t lastOpExitUs = 0;
    if (len < sizeof(fs_op_t*)) return;
    fs_op_t* op;
    memcpy(&op, data, sizeof(op));
    uint32_t t0 = (uint32_t)esp_timer_get_time();
    /* Time gap since previous op exited — exposes whether fs worker is being
     * starved between picking up messages. */
    uint32_t idleMs = lastOpExitUs ? (t0 - lastOpExitUs) / 1000 : 0;
    if (idleMs > 200) {
        verb("fs worker: idle %ums before op=%d\n", (unsigned)idleMs, (int)op->op);
    }
    fsCurrentOp = (int)op->op;
    fsCurrentSlot = op->slot;
    handleOp(op);
    fsCurrentOp = -1;
    fsCurrentSlot = -1;
    fsOpCount = fsOpCount + 1;  /* C++20 deprecates ++ on volatile */
    uint32_t t1 = (uint32_t)esp_timer_get_time();
    uint32_t took = (t1 - t0) / 1000;
    if (took > 200) {
        ESP_LOGD("fs", "op=%d slot=%d took %ums", (int)op->op, op->slot, (unsigned)took);
    }
    lastOpExitUs = t1;
}

static int proxyOp(fs_op_t& req) {
    fs_op_t* ptr = &req;
    uint32_t t0 = (uint32_t)esp_timer_get_time();
    itsSendAuxByTaskHandle(fsWorkerHandle, FS_OP_PORT, &ptr, sizeof(ptr),
                           portMAX_DELAY, ITS_WAIT_PICKUP);
    uint32_t took = ((uint32_t)esp_timer_get_time() - t0) / 1000;
    if (took > 200) {
        verb("fs proxy: op=%d slot=%d wait %ums\n",
             (int)req.op, req.slot, (unsigned)took);
    }
    return req.result;
}

/* ---- Streaming write server (ITS-stream backed) ---- */

static constexpr uint16_t FS_STREAM_PORT = 2;
#define FS_STREAM_MAX_HANDLES 2
#define FS_STREAM_BUF_SIZE    16384

struct fs_stream_open_t {
    char     path[80];
    char     mode[8];
    uint32_t triggerLevel;
};

static struct {
    FILE* fp;
    int   handle;
} fsStreamSlots[FS_STREAM_MAX_HANDLES];

static int fsStreamSlotFor(int handle) {
    for (int i = 0; i < FS_STREAM_MAX_HANDLES; i++)
        if (fsStreamSlots[i].fp && fsStreamSlots[i].handle == handle) return i;
    return -1;
}

static int fsStreamOnConnect(int handle, const void* data, size_t len) {
    if (len < sizeof(fs_stream_open_t)) return -1;
    fs_stream_open_t req;
    memcpy(&req, data, sizeof(req));
    int slot = -1;
    for (int i = 0; i < FS_STREAM_MAX_HANDLES; i++)
        if (!fsStreamSlots[i].fp) { slot = i; break; }
    if (slot < 0) return -1;
    FILE* fp = fopen(req.path, req.mode);
    if (!fp) return -1;
    fsStreamSlots[slot].fp = fp;
    fsStreamSlots[slot].handle = handle;
    if (req.triggerLevel > 1) itsSetTriggerLevel(handle, req.triggerLevel);
    return slot;
}

static void fsStreamDrain(int slot) {
    FILE* fp = fsStreamSlots[slot].fp;
    int   h  = fsStreamSlots[slot].handle;
    static char drainBuf[2048];
    for (;;) {
        size_t n = itsRecv(h, drainBuf, sizeof(drainBuf), 0);
        if (n == 0) break;
        fwrite(drainBuf, 1, n, fp);
    }
}

static void fsStreamOnRecv(int handle, size_t /*bytesAvail*/) {
    int slot = fsStreamSlotFor(handle);
    if (slot < 0) return;
    fsStreamDrain(slot);
}

static void fsStreamOnDisconnect(int ref) {
    if (ref < 0 || ref >= FS_STREAM_MAX_HANDLES) return;
    auto& s = fsStreamSlots[ref];
    if (!s.fp) return;
    fsStreamDrain(ref);
    fflush(s.fp);
    fsync(fileno(s.fp));
    fclose(s.fp);
    s.fp = nullptr;
    s.handle = -1;
}

static TaskHandle_t fsStreamsHandle = nullptr;

static constexpr uint16_t FS_STREAM_SYNC_PORT = 4;

struct fs_stream_sync_msg_t { int handle; };

static void fsStreamSyncHandler(TaskHandle_t, const void* data, size_t len) {
    if (len < sizeof(fs_stream_sync_msg_t)) return;
    fs_stream_sync_msg_t req;
    memcpy(&req, data, sizeof(req));
    int slot = fsStreamSlotFor(req.handle);
    if (slot < 0) return;
    fsStreamDrain(slot);
    fflush(fsStreamSlots[slot].fp);
    fsync(fileno(fsStreamSlots[slot].fp));
}

int fs_stream_sync(int handle) {
    if (!fsStreamsHandle) return -1;
    fs_stream_sync_msg_t req = { handle };
    bool ok = itsSendAuxByTaskHandle(fsStreamsHandle, FS_STREAM_SYNC_PORT,
                                     &req, sizeof(req), pdMS_TO_TICKS(2000),
                                     ITS_WAIT_PICKUP);
    return ok ? 0 : -1;
}

int fs_open_stream(const char* path, const char* mode,
                   size_t bufMinSize, size_t triggerLevel) {
    if (bufMinSize > FS_STREAM_BUF_SIZE) return -1;
    fs_stream_open_t req = {};
    safeStrncpy(req.path, path, sizeof(req.path));
    safeStrncpy(req.mode, mode, sizeof(req.mode));
    req.triggerLevel = triggerLevel;
    return itsConnectByTaskHandle(fsStreamsHandle, FS_STREAM_PORT,
                                  &req, sizeof(req), pdMS_TO_TICKS(2000));
}

/* ---- Streaming read server ----
 * Pumped inline from the fs worker's main loop (see fsWorkerFn). FAT/SDMMC
 * serializes anyway, so a separate pump task would buy nothing. Each active
 * slot's server→client buffer is topped up with fread chunks between
 * itsPoll calls; client pulls via itsRecv. At EOF we stop topping up —
 * queued bytes keep draining until the client calls itsDisconnect. */

static constexpr uint16_t FS_READ_PORT = 3;
#define FS_READ_MAX_HANDLES 2
#define FS_READ_BUF_SIZE    (512 * 1024)

struct fs_read_open_t {
    char     path[80];
    uint32_t freeNotify;    /* server arms itsSetFreeNotify with this */
    uint32_t _reserved;
};

static struct {
    FILE* fp;
    int   handle;
    bool  eof;
} fsReadSlots[FS_READ_MAX_HANDLES];

static int fsReadOnConnect(int handle, const void* data, size_t len) {
    if (len < sizeof(fs_read_open_t)) return -1;
    fs_read_open_t req;
    memcpy(&req, data, sizeof(req));
    int slot = -1;
    for (int i = 0; i < FS_READ_MAX_HANDLES; i++)
        if (!fsReadSlots[i].fp) { slot = i; break; }
    if (slot < 0) return -1;
    FILE* fp = fopen(req.path, "rb");
    if (!fp) return -1;
    auto& s = fsReadSlots[slot];
    s.fp = fp;
    s.handle = handle;
    s.eof = false;
    /* Client-side triggerLevel is owned by the client (it's a recv-side
     * concept). We set our sender-side freeNotify per the client's
     * request so the pump loop wakes through itsPoll instead of polling. */
    if (req.freeNotify > 0) itsSetFreeNotify(handle, req.freeNotify);
    return slot;
}

static void fsReadOnDisconnect(int ref) {
    if (ref < 0 || ref >= FS_READ_MAX_HANDLES) return;
    auto& s = fsReadSlots[ref];
    if (!s.fp) return;
    fclose(s.fp);
    s.fp = nullptr;
    s.handle = -1;
}

/* Top up all active read slots. For slots that can't accept a useful chunk,
 * re-arm itsSetFreeNotify so the next receiver consume past the low-water
 * mark wakes us via itsPoll. */
#define FS_READ_LOW_WATER 4096

static void fsReadPumpOnce() {
    static char buf[4096];
    for (int i = 0; i < FS_READ_MAX_HANDLES; i++) {
        auto& s = fsReadSlots[i];
        if (!s.fp || s.eof) continue;
        size_t space = itsSpacesAvailable(s.handle);
        if (space < FS_READ_LOW_WATER) {
            itsSetFreeNotify(s.handle, FS_READ_LOW_WATER);
            continue;
        }
        size_t toRead = space < sizeof(buf) ? space : sizeof(buf);
        size_t n = fread(buf, 1, toRead, s.fp);
        if (n == 0) { s.eof = true; continue; }
        itsSend(s.handle, buf, n, 0);
    }
}

int fs_open_stream_read(const char* path, size_t bufMinSize,
                        size_t triggerLevel, size_t freeNotify) {
    if (bufMinSize > FS_READ_BUF_SIZE) return -1;
    fs_read_open_t req = {};
    safeStrncpy(req.path, path, sizeof(req.path));
    req.freeNotify = (uint32_t)freeNotify;
    int h = itsConnectByTaskHandle(fsStreamsHandle, FS_READ_PORT,
                                   &req, sizeof(req), pdMS_TO_TICKS(2000));
    /* triggerLevel is a recv-side concept: owned by the client, set here
     * on the client's own recv-direction pool entry. */
    if (h >= 0 && triggerLevel > 1) itsSetTriggerLevel(h, triggerLevel);
    return h;
}

static void fsWorkerFn(void*) {
    itsServerInit();
    itsOnAux(FS_OP_PORT, onFsOp);
    xSemaphoreGive(fsReady);
    uint32_t lastPulseMs = 0;
    uint32_t lastOpCount = 0;
    for (;;) {
        itsPoll(pdMS_TO_TICKS(1000));
        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        if (now - lastPulseMs >= 1000) {
            uint32_t delta = fsOpCount - lastOpCount;
            if (fsCurrentOp >= 0) {
                ESP_LOGD("fs", "pulse: ops/s=%u in-op=%d slot=%d",
                         (unsigned)delta, fsCurrentOp, fsCurrentSlot);
            } else if (delta > 0) {
                ESP_LOGD("fs", "pulse: ops/s=%u idle", (unsigned)delta);
            }
            lastPulseMs = now;
            lastOpCount = fsOpCount;
        }
    }
}

static SemaphoreHandle_t fsStreamsReady = nullptr;

static void fsStreamsFn(void*) {
    itsServerInit();
    itsServerPortOpen(FS_STREAM_PORT, false, FS_STREAM_MAX_HANDLES,
                      FS_STREAM_BUF_SIZE, 0);
    itsServerOnConnect(FS_STREAM_PORT, fsStreamOnConnect);
    itsServerOnRecv(FS_STREAM_PORT, fsStreamOnRecv);
    itsServerOnDisconnect(FS_STREAM_PORT, fsStreamOnDisconnect);
    itsServerPortOpen(FS_READ_PORT, false, FS_READ_MAX_HANDLES,
                      0, FS_READ_BUF_SIZE);
    itsServerOnConnect(FS_READ_PORT, fsReadOnConnect);
    itsServerOnDisconnect(FS_READ_PORT, fsReadOnDisconnect);
    itsOnAux(FS_STREAM_SYNC_PORT, fsStreamSyncHandler);
    xSemaphoreGive(fsStreamsReady);
    for (;;) {
        fsReadPumpOnce();
        itsPoll(portMAX_DELAY);
    }
}

/* ---- Filesystem mounting ---- */

static bool copyFile(const char* src, const char* dst) {
    FILE* in = fopen(src, "rb");
    if (!in) return false;
    FILE* out = fopen(dst, "wb");
    if (!out) { fclose(in); return false; }
    char buf[256];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
        fwrite(buf, 1, n, out);
    fclose(in);
    fclose(out);
    return true;
}

/** Recursively copy a directory tree. mkdir's intermediate dirs in dst as needed.
 *  settings.json at the top level is skipped — storageLoad merges it. */
static void copyTree(const char* src, const char* dst, bool topLevel) {
    DIR* dir = opendir(src);
    if (!dir) return;
    mkdir(dst, 0777);
    struct dirent* ent;
    while ((ent = readdir(dir)) != nullptr) {
        if (ent->d_name[0] == '.') continue;
        char srcChild[512], dstChild[512];
        snprintf(srcChild, sizeof(srcChild), "%s/%s", src, ent->d_name);
        snprintf(dstChild, sizeof(dstChild), "%s/%s", dst, ent->d_name);
        if (ent->d_type == DT_DIR) {
            copyTree(srcChild, dstChild, false);
        } else if (ent->d_type == DT_REG) {
            if (topLevel && strcmp(ent->d_name, "settings.json") == 0) continue;
            if (copyFile(srcChild, dstChild))
                printf("copied %s\n", srcChild + strlen(FS_FIXED) + 1);
            else
                printf("failed to copy %s\n", srcChild);
        }
    }
    closedir(dir);
}

/** Apply /fixed/additional_state/ overlay on top of /state/.
 *  Files (and subdirs) are copied recursively. settings.json is skipped at the
 *  top level — the JSON merge happens in storageLoad(). */
static void applyAdditionalState() {
    if (opendir(FS_FIXED "/additional_state") == nullptr) return;
    printf("applying additional_state overlay\n");
    copyTree(FS_FIXED "/additional_state", fsStateDir(), true);
}

void fs_factory_reset() {
    DIR* probe = opendir(FS_FIXED "/factory_state");
    if (!probe) { printf("factory_state dir not found\n"); return; }
    closedir(probe);
    copyTree(FS_FIXED "/factory_state", fsStateDir(), false);
}

/* ---- Optional SD card mount (FAT on SDMMC slot) ----
 * Mount logic lives here so /sdcard plumbing (sdAvailable(), IDF log silencing,
 * FATFS type detection) is shared. Pin numbers and bus width come from
 * Kconfig — see fs.h's fs_mount_sd() for the CONFIG_DIPTYCH_SDCARD_* knobs. */

static sdmmc_card_t* sdCard = nullptr;
/* sdReady + sdAvailable() defined earlier so the fs worker (LISTDIR root case)
 * can gate the synthetic /sdcard entry on whether the card mounted. */

bool fs_mount_sd(void) {
#if !CONFIG_DIPTYCH_SDCARD
    return false;
#else
    if (sdReady) return true;  /* one-shot — subsequent calls are no-ops */

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {};
    mount_config.format_if_mount_failed = false;
    mount_config.max_files = 10;
    mount_config.allocation_unit_size = 16 * 1024;

    /* Silence IDF's sdmmc/vfs chatter — empty slot or slow first probe both
     * splatter "send_op_cond" / "init failed" lines. We collapse to one warn
     * after both attempts fail. */
    esp_log_level_set("sdmmc_common",  ESP_LOG_NONE);
    esp_log_level_set("sdmmc_sd",      ESP_LOG_NONE);
    esp_log_level_set("vfs_fat_sdmmc", ESP_LOG_NONE);

    esp_err_t ret;

#if CONFIG_DIPTYCH_SDCARD_BUS_SDMMC
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
#if CONFIG_DIPTYCH_SDCARD_4BIT
    host.flags = SDMMC_HOST_FLAG_4BIT;
#else
    host.flags = SDMMC_HOST_FLAG_1BIT;
#endif
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
#if CONFIG_DIPTYCH_SDCARD_4BIT
    slot.width = 4;
#else
    slot.width = 1;
#endif
    slot.clk = (gpio_num_t)CONFIG_DIPTYCH_SDCARD_PIN_CLK;
    slot.cmd = (gpio_num_t)CONFIG_DIPTYCH_SDCARD_PIN_CMD;
    slot.d0  = (gpio_num_t)CONFIG_DIPTYCH_SDCARD_PIN_D0;
#if CONFIG_DIPTYCH_SDCARD_4BIT
    slot.d1 = (gpio_num_t)CONFIG_DIPTYCH_SDCARD_PIN_D1;
    slot.d2 = (gpio_num_t)CONFIG_DIPTYCH_SDCARD_PIN_D2;
    slot.d3 = (gpio_num_t)CONFIG_DIPTYCH_SDCARD_PIN_D3;
#endif
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    ret = esp_vfs_fat_sdmmc_mount(FS_SDCARD, &host, &slot, &mount_config, &sdCard);

#elif CONFIG_DIPTYCH_SDCARD_BUS_SPI
    /* Bring up the shared SPI bus (idempotent — LoRa, display etc.
     * may have got here first). */
    spi_bus_config_t bus = {};
    bus.sclk_io_num     = CONFIG_DIPTYCH_SDCARD_SPI_PIN_SCK;
    bus.mosi_io_num     = CONFIG_DIPTYCH_SDCARD_SPI_PIN_MOSI;
    bus.miso_io_num     = CONFIG_DIPTYCH_SDCARD_SPI_PIN_MISO;
    bus.quadwp_io_num   = -1;
    bus.quadhd_io_num   = -1;
    bus.max_transfer_sz = 4096;
    /* Kconfig DIPTYCH_SDCARD_SPI_HOST is the peripheral *name* (2 = SPI2/FSPI,
     * 3 = SPI3/HSPI), matching how board headers spell BOARD_*_SPI_HOST. The
     * IDF spi_host_device_t enum is offset by one (SPI1_HOST=0, SPI2_HOST=1,
     * SPI3_HOST=2), so map by name rather than casting the raw int — a straight
     * cast put the SD on SPI3 while a board's shared bus lived on SPI2. */
    static_assert(CONFIG_DIPTYCH_SDCARD_SPI_HOST == 2 ||
                  CONFIG_DIPTYCH_SDCARD_SPI_HOST == 3,
                  "DIPTYCH_SDCARD_SPI_HOST must be 2 (SPI2) or 3 (SPI3)");
    spi_host_device_t host_id =
        (CONFIG_DIPTYCH_SDCARD_SPI_HOST == 2) ? SPI2_HOST : SPI3_HOST;
    esp_err_t br = spiHelperInitBus(host_id, &bus);
    if (br != ESP_OK) {
        warn("SD: SPI bus init failed: %s\n", esp_err_to_name(br));
        esp_log_level_set("sdmmc_common",  ESP_LOG_WARN);
        esp_log_level_set("sdmmc_sd",      ESP_LOG_WARN);
        esp_log_level_set("vfs_fat_sdmmc", ESP_LOG_WARN);
        return false;
    }

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = host_id;
    host.max_freq_khz = CONFIG_DIPTYCH_SDCARD_SPI_FREQ_KHZ;

    sdspi_device_config_t dev = SDSPI_DEVICE_CONFIG_DEFAULT();
    dev.host_id = host_id;
    dev.gpio_cs = (gpio_num_t)CONFIG_DIPTYCH_SDCARD_SPI_PIN_CS;

    ret = esp_vfs_fat_sdspi_mount(FS_SDCARD, &host, &dev, &mount_config, &sdCard);
#else
#error "DIPTYCH_SDCARD enabled but no bus type selected (SDMMC or SPI)"
#endif

    /* Restore IDF log defaults — real I/O errors should be visible from now on. */
    esp_log_level_set("sdmmc_common",  ESP_LOG_WARN);
    esp_log_level_set("sdmmc_sd",      ESP_LOG_WARN);
    esp_log_level_set("vfs_fat_sdmmc", ESP_LOG_WARN);

    if (ret != ESP_OK) {
        warn("no SD card mounted at " FS_SDCARD " (mount rc=%s)\n",
             esp_err_to_name(ret));
        return false;
    }

    /* Detect filesystem type for the info line. */
    FATFS* ffs;
    DWORD free_clust;
    const char* fsName = "FAT?";
    if (f_getfree("0:", &free_clust, &ffs) == FR_OK) {
        switch (ffs->fs_type) {
            case FS_FAT16: fsName = "FAT16"; break;
            case FS_FAT32: fsName = "FAT32"; break;
#if FF_FS_EXFAT
            case FS_EXFAT: fsName = "exFAT"; break;
#endif
        }
    }
    info("SD: %s %llu MB %s\n", sdCard->cid.name,
         ((uint64_t)sdCard->csd.capacity) * sdCard->csd.sector_size / (1024 * 1024), fsName);
    sdReady = true;
    return true;
#endif  /* CONFIG_DIPTYCH_SDCARD */
}

/* Unmount, reformat, and remount the on-flash `state` partition. /state is
 * always mounted (even when the active state store is the SD one), so this
 * always unmounts and remounts it. Must run on a DRAM stack —
 * esp_littlefs_format disables the PSRAM cache during SPI-flash writes.
 * Disruptive: callers either reboot right after (factory reset) or accept
 * that /state is briefly empty. */
void fsFormatFlash(void) {
    esp_vfs_littlefs_unregister("state");
    esp_littlefs_format("state");
    if (mountStateLittlefs() != ESP_OK)
        printf("remount " FS_STATE " after format failed\n");
}

/* Reformat the SD card in place (FAT). The card stays mounted at /sdcard.
 * Returns false if no card is mounted or SD support is compiled out. */
bool fsFormatSd(void) {
#if !CONFIG_DIPTYCH_SDCARD
    return false;
#else
    if (!sdReady || !sdCard) return false;
    esp_err_t e = esp_vfs_fat_sdcard_format(FS_SDCARD, sdCard);
    if (e != ESP_OK) { printf("SD format failed: %s\n", esp_err_to_name(e)); return false; }
    return true;
#endif
}

/* Choose the active state store for this boot and seed it on first boot.
 * Call ONCE from diptychInit(), AFTER fs_mount_sd() and BEFORE storageLoad():
 *   - if the SD mounted and /sdcard/state is a directory, that becomes the
 *     active store; otherwise it stays the on-flash /state;
 *   - if the chosen store is empty (fresh flash, or a freshly `mkdir`'d
 *     /sdcard/state), copy factory defaults + additional_state into it.
 * /state (flash) is always mounted regardless; this never rewrites paths. */
void fsSelectStateStore(void) {
    struct stat st;
    if (sdAvailable() && stat(FS_SDCARD "/state", &st) == 0 && S_ISDIR(st.st_mode))
        safeStrncpy(s_stateDir, FS_SDCARD "/state", sizeof(s_stateDir));
    printf("state: active store is %s (flash %s always mounted)\n",
           fsStateDir(), FS_STATE);

    /* First boot: the active store has no non-dot entries. We can't probe a
     * specific file (settings.json may legitimately not exist now that all
     * defaults self-install), so check for any real entry. */
    DIR* d = opendir(fsStateDir());
    bool empty = true;
    if (d) {
        struct dirent* ent;
        while ((ent = readdir(d)) != nullptr) {
            if (ent->d_name[0] == '.') continue;
            empty = false;
            break;
        }
        closedir(d);
    }
    if (empty) {
        printf("first boot: copying factory defaults to %s\n", fsStateDir());
        firstBoot = true;
        fs_factory_reset();
        applyAdditionalState();
    }
}

void fs_init() {
    /* NVS flash — ESP-IDF internals only (WiFi cal, PHY data) */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    /* Resolve "fixed" partition based on running app slot.
     * app0 → fixed_a, app1 → fixed_b. OTA writes the inactive pair together
     * and otadata flips both atomically by association. */
    {
        const esp_partition_t* run = esp_ota_get_running_partition();
        const char* lbl = (run && run->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_1)
                              ? "fixed_b"
                              : "fixed_a";
        safeStrncpy(fixedLabel, lbl, sizeof(fixedLabel));
    }

    /* Mount all LittleFS partitions from the table, INCLUDING `state`.
     * /state is the on-flash partition and is always mounted, regardless of
     * where the active state store ends up. */
    for (int i = 0; i < FS_MOUNT_COUNT; i++) {
        auto& m = FS_MOUNTS[i];
        esp_vfs_littlefs_conf_t conf = {};
        conf.base_path = m.path;
        conf.partition_label = m.partition;
        conf.format_if_mount_failed = m.formatOnFail;
        if (esp_vfs_littlefs_register(&conf) != ESP_OK)
            printf("mount %s failed\n", m.path);
    }

    /* SD probe + active-state-store selection + first-boot factory copy do
     * NOT happen here. They run in fsSelectStateStore(), called from
     * diptychInit() AFTER fs_mount_sd() — mounting the SD that early (before
     * the bus/card is reliably ready) failed and poisoned the later retry.
     * fs_init() only brings up LittleFS + the workers. */

    /* Start DRAM-stack workers:
     *   fs      — POSIX aux ops (fopen/fread/fwrite-by-handle, stat, …)
     *   fs_strm — streaming write drain + streaming read pump
     * Split so a long streaming read doesn't delay a quick random-access op.
     * FAT/SDMMC still serializes, but FreeRTOS scheduling lets the aux task
     * run between chunks. */
    fsReady = xSemaphoreCreateBinary();
    fsWorkerHandle = spawnTask(fsWorkerFn, "fs", 5120, nullptr, 1, 1, STACK_DRAM);
    xSemaphoreTake(fsReady, portMAX_DELAY);
    vSemaphoreDelete(fsReady);
    fsReady = nullptr;

    fsStreamsReady = xSemaphoreCreateBinary();
    fsStreamsHandle = spawnTask(fsStreamsFn, "fs_strm", 5120, nullptr, 1, 1, STACK_DRAM);
    xSemaphoreTake(fsStreamsReady, portMAX_DELAY);
    vSemaphoreDelete(fsStreamsReady);
    fsStreamsReady = nullptr;

    /* SD card mount is optional and project-specific (board pins). Call
     * fs_mount_sd() from main.cpp / project init if the board has SDMMC. */
}

/* ---- Public API: handle-based ---- */

int fs_open(const char* path, const char* mode) {
    int slot = allocSlot();
    if (slot < 0) return -1;
    fileSlots[slot].flash = isFlashPath(path);

    /* Always go through the fs worker once it's up — matches the rest of fs_*
     * and keeps FatFS/SDMMC access strictly single-threaded. Only the pre-boot
     * settings.json read (done before the worker exists) uses direct fopen. */
    if (fsWorkerHandle != nullptr) {
        fs_op_t req = {};
        req.op = fs_op_t::OPEN;
        req.path = path;
        req.path2 = mode;
        req.slot = slot;
        if (proxyOp(req) < 0) { fileSlots[slot].active = false; return -1; }
    } else {
        FILE* fp = fopen(path, mode);
        if (!fp) { fileSlots[slot].active = false; return -1; }
        fileSlots[slot].fp = fp;
    }
    return slot;
}

size_t fs_read(void* buf, size_t size, size_t nmemb, int f) {
    if (f < 0 || f >= MAX_FILE_SLOTS || !fileSlots[f].active) return 0;
    if (needsProxyHandle(f)) {
        fs_op_t req = {};
        req.op = fs_op_t::READ;
        req.slot = f;
        req.buf = buf;
        req.size = size;
        req.nmemb = nmemb;
        int r = proxyOp(req);
        return r > 0 ? (size_t)r : 0;
    }
    uint32_t t0 = (uint32_t)(esp_timer_get_time() / 1000);
    size_t n = fileSlots[f].fp ? fread(buf, size, nmemb, fileSlots[f].fp) : 0;
    uint32_t took = (uint32_t)(esp_timer_get_time() / 1000) - t0;
    if (took > 500) {
        verb("fs_read %ums (%u×%u)\n",
             (unsigned)took, (unsigned)size, (unsigned)nmemb);
    }
    return n;
}

size_t fs_write(const void* buf, size_t size, size_t nmemb, int f) {
    if (f < 0 || f >= MAX_FILE_SLOTS || !fileSlots[f].active) return 0;
    if (needsProxyHandle(f)) {
        fs_op_t req = {};
        req.op = fs_op_t::WRITE;
        req.slot = f;
        req.buf = (void*)buf;
        req.size = size;
        req.nmemb = nmemb;
        int r = proxyOp(req);
        return r > 0 ? (size_t)r : 0;
    }
    uint32_t t0 = (uint32_t)(esp_timer_get_time() / 1000);
    uint32_t tick0 = xTaskGetTickCount();
    size_t n = fileSlots[f].fp ? fwrite(buf, size, nmemb, fileSlots[f].fp) : 0;
    uint32_t took = (uint32_t)(esp_timer_get_time() / 1000) - t0;
    uint32_t tookTicks = (xTaskGetTickCount() - tick0) * portTICK_PERIOD_MS;
    if (took > 500) {
        verb("fs_write %ums (ticks=%ums, %u×%u)\n",
             (unsigned)took, (unsigned)tookTicks,
             (unsigned)size, (unsigned)nmemb);
    }
    return n;
}

long fs_tell(int f) {
    if (f < 0 || f >= MAX_FILE_SLOTS || !fileSlots[f].active) return -1;
    if (needsProxyHandle(f)) {
        fs_op_t req = {};
        req.op = fs_op_t::TELL;
        req.slot = f;
        return (long)proxyOp(req);
    }
    /* DIAG: raw microsecond timestamps at entry and exit, always printed when
     * slow. Lets caller correlate gap between its own marker and this. */
    uint32_t tEntryUs = (uint32_t)esp_timer_get_time();
    long r = fileSlots[f].fp ? ftell(fileSlots[f].fp) : -1;
    uint32_t tExitUs = (uint32_t)esp_timer_get_time();
    if (tExitUs - tEntryUs > 200000) {
        verb("fs_tell: entry=%uus exit=%uus dt=%ums\n",
             (unsigned)tEntryUs, (unsigned)tExitUs,
             (unsigned)((tExitUs - tEntryUs) / 1000));
    }
    return r;
}

int fs_seek(int f, long offset, int whence) {
    if (f < 0 || f >= MAX_FILE_SLOTS || !fileSlots[f].active) return -1;
    if (needsProxyHandle(f)) {
        fs_op_t req = {};
        req.op = fs_op_t::SEEK;
        req.slot = f;
        req.offset = offset;
        req.whence = whence;
        return proxyOp(req);
    }
    uint32_t t0 = (uint32_t)(esp_timer_get_time() / 1000);
    int r = fileSlots[f].fp ? fseek(fileSlots[f].fp, offset, whence) : -1;
    uint32_t took = (uint32_t)(esp_timer_get_time() / 1000) - t0;
    if (took > 500) {
        verb("fs_seek %ums (offset=%ld whence=%d)\n",
             (unsigned)took, offset, whence);
    }
    return r;
}

int fs_flush(int f) {
    if (f < 0 || f >= MAX_FILE_SLOTS || !fileSlots[f].active) return -1;
    if (needsProxyHandle(f)) {
        fs_op_t req = {};
        req.op = fs_op_t::FLUSH;
        req.slot = f;
        return proxyOp(req);
    }
    return fileSlots[f].fp ? fflush(fileSlots[f].fp) : -1;
}

int fs_sync(int f) {
    if (f < 0 || f >= MAX_FILE_SLOTS || !fileSlots[f].active) return -1;
    if (needsProxyHandle(f)) {
        fs_op_t req = {};
        req.op = fs_op_t::SYNC;
        req.slot = f;
        return proxyOp(req);
    }
    if (!fileSlots[f].fp) return -1;
    fflush(fileSlots[f].fp);
    return fsync(fileno(fileSlots[f].fp));
}

int fs_truncate(int f, long length) {
    if (f < 0 || f >= MAX_FILE_SLOTS || !fileSlots[f].active) return -1;
    if (needsProxyHandle(f)) {
        fs_op_t req = {};
        req.op = fs_op_t::TRUNCATE;
        req.slot = f;
        req.offset = length;
        return proxyOp(req);
    }
    if (!fileSlots[f].fp) return -1;
    fflush(fileSlots[f].fp);
    return ftruncate(fileno(fileSlots[f].fp), (off_t)length);
}

void fs_close(int f) {
    if (f < 0 || f >= MAX_FILE_SLOTS || !fileSlots[f].active) return;
    if (needsProxyHandle(f)) {
        fs_op_t req = {};
        req.op = fs_op_t::CLOSE;
        req.slot = f;
        proxyOp(req);
        return;
    }
    if (fileSlots[f].fp) { fclose(fileSlots[f].fp); fileSlots[f].fp = nullptr; }
    fileSlots[f].active = false;
}

/* ---- Public API: path operations ---- */

int fs_stat(const char* path, struct stat* st) {
    if (needsProxy(path)) {
        fs_op_t req = {};
        req.op = fs_op_t::STAT;
        req.path = path;
        req.st = st;
        return proxyOp(req);
    }
    return stat(path, st);
}

int fs_rename(const char* from, const char* to) {
    if (needsProxy(from)) {
        fs_op_t req = {};
        req.op = fs_op_t::RENAME;
        req.path = from;
        req.path2 = to;
        return proxyOp(req);
    }
    return rename(from, to);
}

int fs_remove(const char* path) {
    if (needsProxy(path)) {
        fs_op_t req = {};
        req.op = fs_op_t::REMOVE;
        req.path = path;
        return proxyOp(req);
    }
    return remove(path);
}

int fs_mkdir(const char* path) {
    if (needsProxy(path)) {
        fs_op_t req = {};
        req.op = fs_op_t::MKDIR;
        req.path = path;
        return proxyOp(req);
    }
    return mkdir(path, 0755);
}

void fs_mkdirp(const char* path) {
    char tmp[128];
    safeStrncpy(tmp, path, sizeof(tmp));
    for (char* p = tmp + 1; *p; p++) {
        if (*p == '/') { *p = '\0'; fs_mkdir(tmp); *p = '/'; }
    }
    fs_mkdir(tmp);
}

/* ---- Directory iteration ---- */

static int allocDirSlot() {
    for (int i = 0; i < MAX_DIR_SLOTS; i++)
        if (!dirSlots[i].active) { dirSlots[i].active = true; return i; }
    return -1;
}

int fs_opendir(const char* path) {
    int slot = allocDirSlot();
    if (slot < 0) return -1;
    if (needsProxy(path)) {
        fs_op_t req = {};
        req.op = fs_op_t::OPENDIR;
        req.path = path;
        req.slot = slot;
        if (proxyOp(req) < 0) { dirSlots[slot].active = false; return -1; }
    } else {
        DIR* d = opendir(path);
        if (!d) { dirSlots[slot].active = false; return -1; }
        dirSlots[slot].dir = d;
    }
    return slot;
}

bool fs_readdir(int d, fs_dirent_t* out) {
    if (d < 0 || d >= MAX_DIR_SLOTS || !dirSlots[d].active || !out) return false;
    if (fsWorkerHandle != nullptr) {
        fs_op_t req = {};
        req.op = fs_op_t::READDIR;
        req.slot = d;
        req.buf = out;
        return proxyOp(req) == 1;
    }
    struct dirent* ent = dirSlots[d].dir ? readdir(dirSlots[d].dir) : nullptr;
    if (!ent) return false;
    safeStrncpy(out->name, ent->d_name, sizeof(out->name));
    out->d_type = ent->d_type;
    return true;
}

void fs_closedir(int d) {
    if (d < 0 || d >= MAX_DIR_SLOTS || !dirSlots[d].active) return;
    if (fsWorkerHandle != nullptr) {
        fs_op_t req = {};
        req.op = fs_op_t::CLOSEDIR;
        req.slot = d;
        proxyOp(req);
        return;
    }
    if (dirSlots[d].dir) { closedir(dirSlots[d].dir); dirSlots[d].dir = nullptr; }
    dirSlots[d].active = false;
}

int fs_file_info(const char* path, bool tryGz, fs_file_info_t* out) {
    if (!out) return -1;
    if (fsWorkerHandle != nullptr) {
        fs_op_t req = {};
        req.op = fs_op_t::FILE_INFO;
        req.path = path;
        req.buf = out;
        req.flag = tryGz;
        return proxyOp(req);
    }
    struct stat st;
    if (tryGz) {
        char gz[256]; snprintf(gz, sizeof(gz), "%.250s.gz", path);
        if (stat(gz, &st) == 0) {
            out->gzipped = true; out->size = (size_t)st.st_size; out->mtime = st.st_mtime;
            return 0;
        }
    }
    if (stat(path, &st) != 0) return -1;
    out->gzipped = false; out->size = (size_t)st.st_size; out->mtime = st.st_mtime;
    return 0;
}

int fs_listdir(const char* path, fs_listing_t* out, int max) {
    if (!out || max <= 0) return -1;
    if (fsWorkerHandle != nullptr) {
        fs_op_t req = {};
        req.op = fs_op_t::LISTDIR;
        req.path = path;
        req.buf = out;
        req.nmemb = max;
        return proxyOp(req);
    }
    DIR* d = opendir(path);
    if (!d) return -1;
    int count = 0;
    struct dirent* ent;
    char fp[256];
    while ((ent = readdir(d)) && count < max) {
        snprintf(fp, sizeof(fp), "%.180s/%.64s", path, ent->d_name);
        struct stat st;
        if (stat(fp, &st) != 0) continue;
        safeStrncpy(out[count].name, ent->d_name, sizeof(out[count].name));
        out[count].size  = (uint32_t)st.st_size;
        out[count].mtime = st.st_mtime;
        out[count].isDir = S_ISDIR(st.st_mode);
        count++;
    }
    closedir(d);
    return count;
}
