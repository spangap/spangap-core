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
#include <dirent.h>
#include <errno.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "esp_littlefs.h"
#include "nvs_flash.h"

/* ---- Handle table ---- */

#define MAX_FILE_SLOTS 12

static struct {
    FILE* fp;
    bool  active;
    bool  flash;      /* path was on a flash-backed filesystem at open time */
} fileSlots[MAX_FILE_SLOTS];

static bool firstBoot = false;

static int allocSlot() {
    for (int i = 0; i < MAX_FILE_SLOTS; i++)
        if (!fileSlots[i].active) { fileSlots[i].active = true; return i; }
    return -1;
}

bool fs_first_boot() { return firstBoot; }

/* ---- Flash / proxy detection ---- */

static bool isFlashPath(const char* path) {
    for (int i = 0; i < FS_MOUNT_COUNT; i++) {
        auto& m = FS_MOUNTS[i];
        if (m.needsDramStack && strncmp(path, m.path, strlen(m.path)) == 0)
            return true;
    }
    return false;
}

static bool callerOnPsram() {
    int local;
    return !esp_ptr_internal(&local);
}

static bool needsProxy(const char* path) {
    return isFlashPath(path) && callerOnPsram();
}

static bool needsProxyHandle(int f) {
    if (f < 0 || f >= MAX_FILE_SLOTS || !fileSlots[f].active) return false;
    return fileSlots[f].flash && callerOnPsram();
}

/* ---- Worker task (DRAM stack) ---- */

static constexpr uint16_t FS_OP_PORT = 1;

struct fs_op_t {
    enum Op { OPEN, READ, WRITE, TELL, SEEK, FLUSH, SYNC, CLOSE,
              STAT, RENAME, REMOVE, MKDIR } op;
    const char* path;
    const char* path2;      /* rename: dest; open: mode */
    int     slot;
    void*   buf;
    size_t  size;
    size_t  nmemb;
    long    offset;
    int     whence;
    struct stat* st;
    int     result;
};

static TaskHandle_t fsWorkerHandle = nullptr;
static SemaphoreHandle_t fsReady = nullptr;

static void handleOp(fs_op_t* req) {
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
    case fs_op_t::CLOSE: {
        auto& s = fileSlots[req->slot];
        if (s.fp) { fclose(s.fp); s.fp = nullptr; }
        s.active = false;
        req->result = 0;
        break;
    }
    case fs_op_t::STAT:
        req->result = stat(req->path, req->st);
        break;
    case fs_op_t::RENAME:
        req->result = rename(req->path, req->path2);
        break;
    case fs_op_t::REMOVE:
        req->result = remove(req->path);
        break;
    case fs_op_t::MKDIR:
        req->result = mkdir(req->path, 0755);
        break;
    }
}

static void onFsOp(TaskHandle_t, const void* data, size_t len) {
    if (len < sizeof(fs_op_t*)) return;
    fs_op_t* op;
    memcpy(&op, data, sizeof(op));
    handleOp(op);
}

static int proxyOp(fs_op_t& req) {
    fs_op_t* ptr = &req;
    itsSendAuxByTaskHandle(fsWorkerHandle, FS_OP_PORT, &ptr, sizeof(ptr),
                           portMAX_DELAY, ITS_WAIT_PICKUP);
    return req.result;
}

static void fsWorkerFn(void*) {
    itsServerInit();
    itsOnAux(FS_OP_PORT, onFsOp);
    xSemaphoreGive(fsReady);
    for (;;) itsPoll();
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

/** Apply /fixed/additional_state/ overlay on top of /state/.
 *  Plain files are copied. settings.json is skipped here — the JSON merge
 *  happens in storageLoad() which understands the config format. */
static void applyAdditionalState() {
    DIR* dir = opendir(FS_FIXED "/additional_state");
    if (!dir) return;
    printf("applying additional_state overlay\n");
    struct dirent* ent;
    while ((ent = readdir(dir)) != nullptr) {
        if (ent->d_type != DT_REG) continue;
        if (strcmp(ent->d_name, "settings.json") == 0) continue;  /* handled by storageLoad */
        char src[128], dst[128];
        snprintf(src, sizeof(src), FS_FIXED "/additional_state/%.100s", ent->d_name);
        snprintf(dst, sizeof(dst), FS_STATE "/%.100s", ent->d_name);
        if (copyFile(src, dst))
            printf("copied %s\n", ent->d_name);
        else
            printf("failed to copy %s\n", ent->d_name);
    }
    closedir(dir);
}

void fs_factory_reset() {
    DIR* dir = opendir(FS_FIXED "/factory_state");
    if (!dir) { printf("factory_state dir not found\n"); return; }
    struct dirent* ent;
    while ((ent = readdir(dir)) != nullptr) {
        if (ent->d_type != DT_REG) continue;
        char src[128], dst[128];
        snprintf(src, sizeof(src), FS_FIXED "/factory_state/%.100s", ent->d_name);
        snprintf(dst, sizeof(dst), FS_STATE "/%.100s", ent->d_name);
        if (copyFile(src, dst))
            printf("copied %s\n", ent->d_name);
        else
            printf("failed to copy %s\n", ent->d_name);
    }
    closedir(dir);
}

void fs_init() {
    /* NVS flash — ESP-IDF internals only (WiFi cal, PHY data) */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    /* Mount LittleFS partitions from table */
    for (int i = 0; i < FS_MOUNT_COUNT; i++) {
        auto& m = FS_MOUNTS[i];
        esp_vfs_littlefs_conf_t conf = {};
        conf.base_path = m.path;
        conf.partition_label = m.partition;
        conf.format_if_mount_failed = m.formatOnFail;
        if (esp_vfs_littlefs_register(&conf) != ESP_OK)
            printf("mount %s failed\n", m.path);
    }

    /* First boot: copy factory defaults to /state */
    FILE* f = fopen(FS_STATE "/settings.json", "r");
    if (f) {
        fclose(f);
    } else {
        printf("first boot: copying factory defaults to " FS_STATE "\n");
        firstBoot = true;
        fs_factory_reset();
        applyAdditionalState();
    }

    /* Start DRAM-stack worker for proxied flash I/O */
    fsReady = xSemaphoreCreateBinary();
    xTaskCreatePinnedToCore(fsWorkerFn, "fs", 3072, nullptr, 1, &fsWorkerHandle, 1);
    xSemaphoreTake(fsReady, portMAX_DELAY);
    vSemaphoreDelete(fsReady);
    fsReady = nullptr;
}

/* ---- Public API: handle-based ---- */

int fs_open(const char* path, const char* mode) {
    int slot = allocSlot();
    if (slot < 0) return -1;
    fileSlots[slot].flash = isFlashPath(path);

    if (fileSlots[slot].flash && callerOnPsram()) {
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
    return fileSlots[f].fp ? fread(buf, size, nmemb, fileSlots[f].fp) : 0;
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
    return fileSlots[f].fp ? fwrite(buf, size, nmemb, fileSlots[f].fp) : 0;
}

long fs_tell(int f) {
    if (f < 0 || f >= MAX_FILE_SLOTS || !fileSlots[f].active) return -1;
    if (needsProxyHandle(f)) {
        fs_op_t req = {};
        req.op = fs_op_t::TELL;
        req.slot = f;
        return (long)proxyOp(req);
    }
    return fileSlots[f].fp ? ftell(fileSlots[f].fp) : -1;
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
    return fileSlots[f].fp ? fseek(fileSlots[f].fp, offset, whence) : -1;
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
    if (f < 0 || f >= MAX_FILE_SLOTS || !fileSlots[f].active || !fileSlots[f].fp) return -1;
    /* ftruncate is always direct — only used on SD card paths */
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
