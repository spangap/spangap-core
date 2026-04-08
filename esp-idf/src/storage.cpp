/**
 * storage — config store + file I/O service.
 *
 * Config: cJSON tree in RAM, backed by JSON on /state.
 * All writes go through a patch tree (RFC 7396 merge-patch). commit() merges
 * the patch into cfgRoot, fires subscriptions, coalesces WS output, and
 * triggers the save timer — atomically.
 *
 * storageBegin()/storageEnd() bracket explicit transactions.
 * Without them, storageSet() is auto-commit (one patch per call).
 *
 * File I/O: POSIX-like API. SD card paths → direct calls on caller's thread.
 *   LittleFS paths → proxied to a small DRAM-stack worker (SPI flash ops
 *   disable the PSRAM cache, so the call stack must be in DRAM).
 *
 * Thread safety: a recursive mutex (cfgMux) protects cfgRoot, txPatch, and
 * txDepth.  storageBegin() acquires, storageEnd() releases.  All public
 * readers (storageGetInt, storageGetStr, …) lock around tree access.
 *
 * Browser config WebSocket (path "/epl"):
 * - Device→browser: full dump on connect, then coalesced merge-patches.
 * - Browser→device: nested JSON merge-patches. null = delete subtree.
 * - Secrets (secrets.*) are never sent to browser and browser writes are ignored.
 *
 * ITS server: handle 0 = browser config WS (path "/epl").
 */
#include "storage.h"
#include "auth.h"
#include "log.h"
#include "cli.h"
#include "its.h"
#include "web.h"
#include "net.h"
#include "compat.h"

#include <string>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>
#include "cJSON.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"

/* ---- Config tree state ---- */

static cJSON* cfgRoot = nullptr;        /* committed config (the truth) */
static cJSON* txPatch = nullptr;        /* transaction write accumulator */
static int txDepth = 0;                 /* transaction nesting depth */
static SemaphoreHandle_t cfgMux = nullptr;  /* recursive mutex protecting cfgRoot + txPatch */

#define CFG_LOCK()   xSemaphoreTakeRecursive(cfgMux, portMAX_DELAY)
#define CFG_UNLOCK() xSemaphoreGiveRecursive(cfgMux)

static bool savePending = false;
static esp_timer_handle_t saveTimer = nullptr;

/* ---- Task state ---- */

static TaskHandle_t storageHandle = nullptr;
static int wsHandle = -1;
static cJSON* wsPendingPatch = nullptr; /* WS outgoing coalescing */

/* ---- File I/O ---- */

/*
 * All file ops run on the fs worker task (DRAM stack). Callers block on a
 * semaphore until the worker completes. This is safe for any caller stack
 * type — the worker's DRAM stack is needed because SPI flash ops disable
 * the PSRAM cache. Data buffers in PSRAM are fine (only accessed after
 * cache re-enable).
 */

#define MAX_FILE_SLOTS     6
#define FS_FILE_PORT       1    /* ITS aux port for file ops */
#define FS_STREAM_RD_PORT  2    /* ITS port for streaming read connections */
#define FS_STREAM_WR_PORT  3    /* ITS port for streaming write connections */
#define FS_STREAM_BUFSZ    (256 * 1024)
#define FS_STREAM_CHUNK    (32 * 1024)   /* individual SD read/write unit */
#define FS_READ_PREFILL    (64 * 1024)   /* blocking pre-fill on open/seek */
#define FS_READ_REFILL     (64 * 1024)   /* refill-to-capacity when bytes avail < this */
#define FS_WRITE_DRAIN     (196 * 1024)  /* drain to SD when buffer reaches this */
#define FS_MAX_STREAMS     2

static FILE* fileFps[MAX_FILE_SLOTS];
static bool fileActive[MAX_FILE_SLOTS];
static TaskHandle_t fsWorkerTask = nullptr;
static SemaphoreHandle_t fsReady = nullptr;

struct storage_file_op_t {
  enum Op { OPEN, READ, WRITE, TELL, CLOSE, STAT, RENAME, REMOVE } op;
  const char* path;
  const char* path2;        /* rename: newPath; open: mode */
  int slot;
  void* buf;
  size_t len;
  struct stat* st;
  int result;
};

static void handleFileOp(storage_file_op_t* req) {
  switch (req->op) {
    case storage_file_op_t::OPEN: {
      FILE* f = fopen(req->path, req->path2);
      if (!f) { req->result = -1; break; }
      fileFps[req->slot] = f;
      req->result = req->slot;
      break;
    }
    case storage_file_op_t::READ: {
      int s = req->slot;
      if (s < 0 || s >= MAX_FILE_SLOTS || !fileFps[s]) { req->result = 0; break; }
      req->result = (int)fread(req->buf, 1, req->len, fileFps[s]);
      break;
    }
    case storage_file_op_t::WRITE: {
      int s = req->slot;
      if (s < 0 || s >= MAX_FILE_SLOTS || !fileFps[s]) { req->result = 0; break; }
      req->result = (int)fwrite(req->buf, 1, req->len, fileFps[s]);
      break;
    }
    case storage_file_op_t::TELL: {
      int s = req->slot;
      if (s < 0 || s >= MAX_FILE_SLOTS || !fileFps[s]) { req->result = -1; break; }
      req->result = (int)ftell(fileFps[s]);
      break;
    }
    case storage_file_op_t::CLOSE: {
      int s = req->slot;
      if (s >= 0 && s < MAX_FILE_SLOTS && fileFps[s]) {
        fclose(fileFps[s]);
        fileFps[s] = nullptr;
        fileActive[s] = false;
      }
      req->result = 0;
      break;
    }
    case storage_file_op_t::STAT:
      req->result = stat(req->path, req->st);
      break;
    case storage_file_op_t::RENAME:
      req->result = rename(req->path, req->path2);
      break;
    case storage_file_op_t::REMOVE:
      req->result = remove(req->path);
      break;
  }
}

static void onFileOp(TaskHandle_t sender, uint16_t port,
                     const void* data, size_t len) {
  if (len < sizeof(storage_file_op_t*)) return;
  storage_file_op_t* op;
  memcpy(&op, data, sizeof(op));
  handleFileOp(op);
}

/* fsWorkerFn defined after stream handlers (below) */

static int fsOp(storage_file_op_t& req) {
  storage_file_op_t* ptr = &req;
  itsSendAuxByHandle(fsWorkerTask, &ptr, sizeof(ptr),
                     portMAX_DELAY, FS_FILE_PORT, ITS_WAIT_PICKUP);
  return req.result;
}

static int allocSlot() {
  for (int i = 0; i < MAX_FILE_SLOTS; i++) {
    if (!fileActive[i]) { fileActive[i] = true; return i; }
  }
  return -1;
}

/* ---- Streaming file I/O ---- */

struct stream_state_t {
  int itsHandle = -1;
  FILE* f = nullptr;
  bool atEof = false;
  bool isWrite = false;
  long writePos = 0;          /* tracked file position for write streams (ftell equivalent) */
};
static stream_state_t streams[FS_MAX_STREAMS];

static void drainWriteStream(stream_state_t& s);

static stream_state_t* streamByHandle(int h) {
  for (auto& s : streams)
    if (s.itsHandle == h) return &s;
  return nullptr;
}

static uint8_t* streamFillBuf = nullptr;  /* PSRAM, allocated once in storageInit */

/** Service active streams. Batches reads then writes — sequential SD access
 *  within each phase, one seek when switching. Yields between chunks so the
 *  play task can deliver audio during fill/drain bursts. */
static void serviceStreams() {
  if (!streamFillBuf) return;

  /* Phase 1: fill read streams to capacity (sequential reads, no seeks) */
  for (auto& s : streams) {
    if (s.itsHandle < 0 || !s.f || s.isWrite || s.atEof) continue;
    /* Check outgoing buffer fill (itsSpacesAvailable = send direction = server→client) */
    { size_t sp = itsSpacesAvailable(s.itsHandle);
      if (sp < FS_STREAM_BUFSZ && (FS_STREAM_BUFSZ - sp) >= FS_READ_REFILL) continue; }
    while (!s.atEof && itsSpacesAvailable(s.itsHandle) >= FS_STREAM_CHUNK) {
      size_t got = fread(streamFillBuf, 1, FS_STREAM_CHUNK, s.f);
      if (got > 0) itsSend(s.itsHandle, streamFillBuf, got, portMAX_DELAY);
      if (got < FS_STREAM_CHUNK) s.atEof = true;
      taskYIELD();
    }
  }

  /* Phase 2: drain write streams — but read has right of way */
  for (auto& s : streams) {
    if (s.itsHandle < 0 || !s.f || !s.isWrite) continue;
    size_t avail = itsBytesAvailable(s.itsHandle);
    if (avail < FS_WRITE_DRAIN) continue;
    while (avail > 0) {
      /* Read right of way: if any read buffer is low, stop writing and go refill */
      bool readUrgent = false;
      for (auto& r : streams) {
        if (r.itsHandle >= 0 && r.f && !r.isWrite && !r.atEof) {
          size_t sp = itsSpacesAvailable(r.itsHandle);
          if (sp >= FS_STREAM_BUFSZ || (FS_STREAM_BUFSZ - sp) < FS_READ_REFILL) {
            readUrgent = true;
            break;
          }
        }
      }
      if (readUrgent) break;  /* next serviceStreams call will fill reads first */

      size_t toWrite = avail < FS_STREAM_CHUNK ? avail : FS_STREAM_CHUNK;
      size_t got = itsRecv(s.itsHandle, streamFillBuf, toWrite, 0);
      if (got == 0) break;
      fwrite(streamFillBuf, 1, got, s.f);
      s.writePos += got;
      avail = itsBytesAvailable(s.itsHandle);
      taskYIELD();
    }
  }
}

/* Aux message structs for stream commands */
struct stream_open_msg_t {
  const char* path;
  const char* mode;       /* nullptr defaults to "rb"/"wb" per port */
  int result;             /* set by fs worker: ITS handle or -1 */
};

struct stream_seek_msg_t {
  int itsHandle;
  long offset;
  bool result;
};

static int onStreamConnect(int handle, int itsPort, const void* data, size_t len) {
  if (len < sizeof(stream_open_msg_t*)) return -1;
  stream_open_msg_t* msg;
  memcpy(&msg, data, sizeof(msg));

  /* Find a free stream slot */
  stream_state_t* s = nullptr;
  for (auto& slot : streams)
    if (slot.itsHandle < 0) { s = &slot; break; }
  if (!s) { msg->result = -1; return -1; }

  bool writing = (itsPort == FS_STREAM_WR_PORT);
  const char* mode = msg->mode ? msg->mode : (writing ? "wb" : "rb");
  FILE* f = fopen(msg->path, mode);
  if (!f) { msg->result = -1; return -1; }

  s->itsHandle = handle;
  s->f = f;
  s->atEof = false;
  s->isWrite = writing;
  s->writePos = 0;
  msg->result = handle;
  return 0;  /* serverRef */
}

static void onStreamDisconnect(int ref) {
  /* ref is serverRef (0 from onStreamConnect). Find by scanning. */
  for (auto& s : streams) {
    if (s.itsHandle >= 0 && !itsConnected(s.itsHandle)) {
      if (s.f) fclose(s.f);
      s = {};
      s.itsHandle = -1;
    }
  }
}

static void onStreamNudge(TaskHandle_t, uint16_t, const void*, size_t) {
  /* Just a wake-up — serviceStreams fills based on available space */
}

static void onStreamSeek(TaskHandle_t sender, uint16_t port,
                         const void* data, size_t len) {
  if (len < sizeof(stream_seek_msg_t*)) return;
  stream_seek_msg_t* msg;
  memcpy(&msg, data, sizeof(msg));

  stream_state_t* s = streamByHandle(msg->itsHandle);
  if (!s || !s->f) { msg->result = false; return; }

  if (s->isWrite) {
    /* Write stream: drain buffer before seeking */
    drainWriteStream(*s);
    fflush(s->f);
  }

  itsReset(s->itsHandle);
  msg->result = (fseek(s->f, msg->offset, SEEK_SET) == 0);

  if (s->isWrite) {
    s->writePos = msg->offset;
  } else {
    s->atEof = false;
    /* Pre-fill 64KB before releasing caller — covers first frame + headers */
    if (msg->result && streamFillBuf) {
      size_t filled = 0;
      while (filled < FS_READ_PREFILL && !s->atEof) {
        size_t got = fread(streamFillBuf, 1, FS_STREAM_CHUNK, s->f);
        if (got > 0) { itsSend(s->itsHandle, streamFillBuf, got, portMAX_DELAY); filled += got; }
        if (got < FS_STREAM_CHUNK) s->atEof = true;
      }
    }
  }
}

/** Drain all remaining write buffer data to SD, then fflush + fsync. */
static void drainWriteStream(stream_state_t& s) {
  if (!s.f || !streamFillBuf) return;
  for (;;) {
    size_t avail = itsBytesAvailable(s.itsHandle);
    if (avail == 0) break;
    size_t got = itsRecv(s.itsHandle, streamFillBuf, avail < FS_STREAM_CHUNK ? avail : FS_STREAM_CHUNK, 0);
    if (got == 0) break;
    fwrite(streamFillBuf, 1, got, s.f);
    s.writePos += got;
  }
}

static void onStreamFlush(TaskHandle_t sender, uint16_t port,
                          const void* data, size_t len) {
  if (len < sizeof(int)) return;
  int handle;
  memcpy(&handle, data, sizeof(handle));
  stream_state_t* s = streamByHandle(handle);
  if (!s || !s->f || !s->isWrite) return;
  drainWriteStream(*s);
  fflush(s->f);
  /* Top up read streams before the potentially long fsync — otherwise a
   * 300ms+ SD card sync can empty the read buffer and cause play underruns. */
  serviceStreams();
  fsync(fileno(s->f));
}

struct stream_tell_msg_t {
  int itsHandle;
  long result;
};

static void onStreamTell(TaskHandle_t sender, uint16_t port,
                         const void* data, size_t len) {
  if (len < sizeof(stream_tell_msg_t*)) return;
  stream_tell_msg_t* msg;
  memcpy(&msg, data, sizeof(msg));
  stream_state_t* s = streamByHandle(msg->itsHandle);
  if (!s || !s->f) { msg->result = -1; return; }
  if (s->isWrite) {
    /* Drain first so writePos reflects all buffered data */
    drainWriteStream(*s);
    msg->result = s->writePos;
  } else {
    msg->result = ftell(s->f);
  }
}

/* Aux sub-ports for stream commands */
#define STREAM_AUX_NUDGE 2
#define STREAM_AUX_SEEK  3
#define STREAM_AUX_FLUSH 4
#define STREAM_AUX_TELL  5

static void fsWorkerFn(void*) {
  itsServerInit(4, 0, 0);
  itsServerPortInit(FS_STREAM_RD_PORT, 1, 0, FS_STREAM_BUFSZ);
  itsServerPortInit(FS_STREAM_WR_PORT, 1, FS_STREAM_BUFSZ, 0);
  itsServerOnConnect(onStreamConnect);
  itsServerOnDisconnect(onStreamDisconnect);
  itsOnAux(onFileOp, FS_FILE_PORT);
  itsOnAux(onStreamNudge, STREAM_AUX_NUDGE);
  itsOnAux(onStreamSeek, STREAM_AUX_SEEK);
  itsOnAux(onStreamFlush, STREAM_AUX_FLUSH);
  itsOnAux(onStreamTell, STREAM_AUX_TELL);
  for (auto& s : streams) s.itsHandle = -1;
  xSemaphoreGive(fsReady);
  for (;;) {
    while (itsPoll(0)) {}
    serviceStreams();
    /* If a read stream is low, loop back immediately to refill
     * (write drain may have been interrupted for read priority) */
    bool readLow = false, writeActive = false;
    for (auto& s : streams) {
      if (s.itsHandle < 0 || !s.f) continue;
      /* Clean up stale streams (disconnect not yet processed) */
      if (!itsConnected(s.itsHandle)) {
        if (s.f) fclose(s.f);
        s = {};
        s.itsHandle = -1;
        continue;
      }
      if (!s.isWrite && !s.atEof) {
        /* For read streams, check the OUTGOING buffer (server→client).
         * itsBytesAvailable checks incoming (client→server) which is always 0. */
        size_t space = itsSpacesAvailable(s.itsHandle);
        size_t filled = (space < FS_STREAM_BUFSZ) ? FS_STREAM_BUFSZ - space : 0;
        if (filled < FS_READ_REFILL) readLow = true;
      }
      if (s.isWrite)
        writeActive = true;
    }
    if (readLow) {
      static uint32_t rlCount = 0;
      if (++rlCount == 10000) {
        for (auto& s : streams) {
          if (s.itsHandle < 0 || s.isWrite) continue;
          warn("fs: readLow spin h=%d conn=%d eof=%d f=%p avail=%u space=%u\n",
               s.itsHandle, itsConnected(s.itsHandle), s.atEof, s.f,
               (unsigned)itsBytesAvailable(s.itsHandle),
               (unsigned)itsSpacesAvailable(s.itsHandle));
        }
        rlCount = 0;
      }
      taskYIELD();
    } else if (writeActive) {
      vTaskDelay(pdMS_TO_TICKS(10));
    } else {
      static uint32_t idleCount = 0;
      if (++idleCount == 10000) { warn("fs: idle spin\n"); idleCount = 0; }
      itsPoll();
    }
  }
}

/* ---- Path navigation ---- */

static bool isAllDigits(const char* s, size_t len) {
  if (len == 0) return false;
  for (size_t i = 0; i < len; i++)
    if (s[i] < '0' || s[i] > '9') return false;
  return true;
}

/** Navigate a cJSON tree by dot-path. Returns the node or NULL. */
static cJSON* navigatePath(cJSON* root, const char* dotPath) {
  if (!root || !dotPath || !*dotPath) return nullptr;
  cJSON* node = root;
  const char* p = dotPath;
  while (*p) {
    const char* dot = strchr(p, '.');
    size_t segLen = dot ? (size_t)(dot - p) : strlen(p);
    if (segLen == 0) { p = dot + 1; continue; }
    char seg[48];
    if (segLen >= sizeof(seg)) return nullptr;
    memcpy(seg, p, segLen);
    seg[segLen] = '\0';

    if (cJSON_IsArray(node) && isAllDigits(seg, segLen))
      node = cJSON_GetArrayItem(node, atoi(seg));
    else
      node = cJSON_GetObjectItem(node, seg);
    if (!node) return nullptr;
    p = dot ? dot + 1 : p + segLen;
  }
  return node;
}

/** Navigate a cJSON tree, creating intermediate objects as needed.
 *  Returns the parent of the leaf and writes the leaf name to outLeaf. */
static cJSON* navigateOrCreate(cJSON* root, const char* dotPath,
                                char* outLeaf, size_t leafLen) {
  if (!root || !dotPath || !*dotPath) return nullptr;
  cJSON* node = root;
  const char* p = dotPath;
  while (*p) {
    const char* dot = strchr(p, '.');
    size_t segLen = dot ? (size_t)(dot - p) : strlen(p);
    if (segLen == 0) { p = dot + 1; continue; }
    char seg[48];
    if (segLen >= sizeof(seg)) return nullptr;
    memcpy(seg, p, segLen);
    seg[segLen] = '\0';

    if (!dot) {
      /* Last segment = leaf name */
      safeStrncpy(outLeaf, seg, leafLen);
      return node;
    }

    cJSON* child = cJSON_GetObjectItem(node, seg);
    if (!child) {
      child = cJSON_CreateObject();
      cJSON_AddItemToObject(node, seg, child);
    } else if (!cJSON_IsObject(child) && !cJSON_IsArray(child)) {
      /* Replace leaf with object (path goes deeper) */
      cJSON_DeleteItemFromObject(node, seg);
      child = cJSON_CreateObject();
      cJSON_AddItemToObject(node, seg, child);
    }
    node = child;
    p = dot + 1;
  }
  return nullptr;
}

/** Resolve a key: check txPatch first (for read-your-writes in transactions),
 *  then cfgRoot. Returns NULL if deleted in patch or not found. */
static cJSON* resolveKey(const char* key) {
  if (txPatch) {
    cJSON* node = navigatePath(txPatch, key);
    if (node) return cJSON_IsNull(node) ? nullptr : node;
  }
  return navigatePath(cfgRoot, key);
}

/** Strip trailing dots from a prefix string. */
static std::string stripDots(const char* s) {
  std::string r(s);
  while (!r.empty() && r.back() == '.') r.pop_back();
  return r;
}

/* ---- Deep merge (RFC 7396) ---- */

/** Merge src into dst in place. Objects recurse; arrays and primitives replace;
 *  null deletes. src is not modified. */
static void deepMerge(cJSON* dst, const cJSON* src) {
  const cJSON* item = src->child;
  while (item) {
    const cJSON* next = item->next;
    const char* name = item->string;
    if (!name) { item = next; continue; }  /* skip unnamed (array elems in wrong context) */

    if (cJSON_IsNull(item)) {
      cJSON_DeleteItemFromObject(dst, name);
    } else if (cJSON_IsObject(item)) {
      cJSON* dstChild = cJSON_GetObjectItem(dst, name);
      if (dstChild && cJSON_IsObject(dstChild)) {
        deepMerge(dstChild, item);
      } else {
        if (dstChild) cJSON_DeleteItemFromObject(dst, name);
        cJSON_AddItemToObject(dst, name, cJSON_Duplicate(item, true));
      }
    } else {
      /* Array or primitive: replace entirely */
      cJSON* existing = cJSON_DetachItemFromObject(dst, name);
      if (existing) cJSON_Delete(existing);
      cJSON_AddItemToObject(dst, name, cJSON_Duplicate(item, true));
    }
    item = next;
  }
}

/* ---- Tree walk helpers ---- */

/** Walk all leaves, calling cb(dotKey, valStr) for each.
 *  Uses a fixed char buffer to build dot-paths. */
static void walkLeavesImpl(cJSON* node, char* path, size_t pathSize, size_t pathLen,
                           void (*cb)(const char* key, const char* val, void* ctx),
                           void* ctx) {
  int idx = 0;
  cJSON* item;
  cJSON_ArrayForEach(item, node) {
    char idxBuf[12];
    const char* name = item->string;
    if (!name) { snprintf(idxBuf, sizeof(idxBuf), "%d", idx); name = idxBuf; }
    idx++;

    size_t nameLen = strlen(name);
    size_t dotLen = pathLen > 0 ? 1 : 0;
    size_t newLen = pathLen + dotLen + nameLen;
    if (newLen >= pathSize) continue;
    if (dotLen) path[pathLen] = '.';
    memcpy(path + pathLen + dotLen, name, nameLen + 1);

    if (cJSON_IsObject(item) || cJSON_IsArray(item)) {
      walkLeavesImpl(item, path, pathSize, newLen, cb, ctx);
    } else {
      char valBuf[32];
      const char* val = nullptr;
      if (cJSON_IsString(item))
        val = item->valuestring;
      else if (cJSON_IsNumber(item)) {
        snprintf(valBuf, sizeof(valBuf), "%d", item->valueint);
        val = valBuf;
      }
      if (val) cb(path, val, ctx);
    }
    path[pathLen] = '\0';
  }
}

static void walkLeaves(cJSON* node, const char* prefix,
                       void (*cb)(const char* key, const char* val, void* ctx),
                       void* ctx) {
  char pathBuf[128];
  size_t prefixLen = prefix ? strlen(prefix) : 0;
  if (prefixLen >= sizeof(pathBuf)) return;
  if (prefixLen > 0) memcpy(pathBuf, prefix, prefixLen);
  pathBuf[prefixLen] = '\0';
  walkLeavesImpl(node, pathBuf, sizeof(pathBuf), prefixLen, cb, ctx);
}

/** Walk leaves for CLI output (show / storageList). */
static void walkTreePrint(cJSON* node, const char* prefix, cli_write_fn write) {
  walkLeaves(node, prefix, [](const char* key, const char* val, void* ctx) {
    auto write = (cli_write_fn)ctx;
    char line[192];
    int n = snprintf(line, sizeof(line), "  %s = %s\n", key, val);
    if (n > 0) write(line, (size_t)n);
  }, (void*)write);
}

/* ---- Settings file read/write ---- */

static bool isSaved(const char* key) {
  return (key[0] == 's' && key[1] == '.')
      || strncmp(key, "secrets.", 8) == 0;
}

static bool isSecret(const char* key) {
  return strncmp(key, "secrets.", 8) == 0;
}

/* readFileLocal: only called from storageLoad() which runs on main's DRAM stack at boot */
static char* readFileLocal(const char* path) {
  FILE* f = fopen(path, "rb");
  if (!f) return nullptr;
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (sz <= 0) { fclose(f); return nullptr; }
  char* buf = (char*)malloc(sz + 1);
  if (!buf) { fclose(f); return nullptr; }
  fread(buf, 1, sz, f);
  buf[sz] = '\0';
  fclose(f);
  return buf;
}

static void writeSettingsFile() {
  CFG_LOCK();
  cJSON* out = cJSON_CreateObject();
  cJSON* s = cJSON_GetObjectItem(cfgRoot, "s");
  cJSON* sec = cJSON_GetObjectItem(cfgRoot, "secrets");
  if (s) cJSON_AddItemToObject(out, "s", cJSON_Duplicate(s, true));
  if (sec) cJSON_AddItemToObject(out, "secrets", cJSON_Duplicate(sec, true));
  CFG_UNLOCK();
  char* text = cJSON_Print(out);
  cJSON_Delete(out);
  if (!text) return;

  int f = storageFopen("/state/settings.new", "w");
  if (f >= 0) {
    storageFwrite(text, strlen(text), f);
    storageFclose(f);
    storageRename("/state/settings.new", "/state/settings.json");
  }
  cJSON_free(text);
  savePending = false;
}

#define STORAGE_SAVE_PORT 43

static void saveTimerCb(void* arg) {
  if (!storageHandle) return;
  itsSendAuxByHandle(storageHandle, nullptr, 0, 0, STORAGE_SAVE_PORT);
}

static void startSaveTimer() {
  if (!saveTimer) {
    esp_timer_create_args_t args = {};
    args.callback = saveTimerCb;
    args.name = "storage_save";
    esp_timer_create(&args, &saveTimer);
  }
  esp_timer_stop(saveTimer);
  int delaySec = storageGetInt("s.storage.flash_delay", 60);
  if (delaySec < 1) delaySec = 1;
  esp_timer_start_once(saveTimer, (int64_t)delaySec * 1000000);
  savePending = true;
}

/* ---- Config change subscriptions ---- */

#define STORAGE_MAX_SUBS     96
#define STORAGE_CHANGE_PORT  42

struct storage_sub_t {
  TaskHandle_t        task;
  storage_change_cb_t cb;
  char                scope[40];
};

static storage_sub_t subs[STORAGE_MAX_SUBS];
static int           subCount = 0;

/* Payload for aux message on STORAGE_CHANGE_PORT */
struct storage_change_msg_t {
  storage_change_cb_t cb;
  char                key[48];
  char                val[44];
};

/* Aux handler installed on each subscribing task — unpacks and calls */
static void storageChangeDispatch(TaskHandle_t sender, uint16_t port,
                                  const void* data, size_t len) {
  if (len < sizeof(storage_change_msg_t)) return;
  auto* msg = (const storage_change_msg_t*)data;
  if (msg->cb) msg->cb(msg->key, msg->val);
}

void storageSubscribeChanges(const char* scope, storage_change_cb_t cb) {
  if (subCount >= STORAGE_MAX_SUBS) return;

  TaskHandle_t me = xTaskGetCurrentTaskHandle();

  /* Register aux handler on this task if first subscription */
  bool needsHandler = true;
  for (int i = 0; i < subCount; i++) {
    if (subs[i].task == me) { needsHandler = false; break; }
  }
  if (needsHandler)
    itsOnAux(storageChangeDispatch, STORAGE_CHANGE_PORT);

  auto& s = subs[subCount++];
  s.task = me;
  s.cb = cb;
  safeStrncpy(s.scope, scope, sizeof(s.scope));
}

static void fireSubscriptions(const char* key, const char* val) {
  storage_change_msg_t msg = {};
  safeStrncpy(msg.key, key, sizeof(msg.key));
  strncpy(msg.val, val, sizeof(msg.val) - 1);
  msg.val[sizeof(msg.val) - 1] = '\0';

  for (int i = 0; i < subCount; i++) {
    size_t scopeLen = strlen(subs[i].scope);
    if (scopeLen == 0 || strncmp(key, subs[i].scope, scopeLen) == 0) {
      msg.cb = subs[i].cb;
      itsSendAuxByHandle(subs[i].task, &msg, sizeof(msg),
                         pdMS_TO_TICKS(10), STORAGE_CHANGE_PORT);
    }
  }
}

/* ---- Type inference ---- */

static cfg_type_t inferType(const char* val) {
  if (!val || !*val) return CFG_STR;
  const char* p = val;
  if (*p == '-') p++;
  if (!*p) return CFG_STR;
  while (*p) {
    if (*p < '0' || *p > '9') return CFG_STR;
    p++;
  }
  return CFG_INT;
}

/* ---- Transactions ---- */

/** Walk patch leaves firing subscriptions (null → val="").
 *  Uses a fixed char buffer to build dot-paths (no std::string on stack). */
static void firePatchSubscriptions(cJSON* node, char* path, size_t pathSize, size_t pathLen) {
  int idx = 0;
  cJSON* item;
  cJSON_ArrayForEach(item, node) {
    char idxBuf[12];
    const char* name = item->string;
    if (!name) { snprintf(idxBuf, sizeof(idxBuf), "%d", idx); name = idxBuf; }
    idx++;

    /* Append ".name" to path (or just "name" if path is empty) */
    size_t nameLen = strlen(name);
    size_t dotLen = pathLen > 0 ? 1 : 0;
    size_t newLen = pathLen + dotLen + nameLen;
    if (newLen >= pathSize) continue;  /* path too long, skip */
    if (dotLen) path[pathLen] = '.';
    memcpy(path + pathLen + dotLen, name, nameLen + 1);

    if (cJSON_IsNull(item)) {
      fireSubscriptions(path, "");
    } else if (cJSON_IsObject(item)) {
      firePatchSubscriptions(item, path, pathSize, newLen);
    } else if (cJSON_IsArray(item)) {
      fireSubscriptions(path, "");
    } else {
      char valBuf[32];
      const char* val;
      if (cJSON_IsString(item)) val = item->valuestring;
      else if (cJSON_IsNumber(item)) {
        snprintf(valBuf, sizeof(valBuf), "%d", item->valueint);
        val = valBuf;
      } else { path[pathLen] = '\0'; continue; }
      fireSubscriptions(path, val);
    }

    path[pathLen] = '\0';
  }
}

static void commitPatch() {
  /* Caller holds CFG_LOCK — protects cfgRoot, txPatch, txDepth from
     concurrent access.  ITS aux sends (10ms timeout) are bounded. */
  if (!txPatch) return;

  deepMerge(cfgRoot, txPatch);

  char pathBuf[128] = "";
  firePatchSubscriptions(txPatch, pathBuf, sizeof(pathBuf), 0);

  if (cJSON_GetObjectItem(txPatch, "s") || cJSON_GetObjectItem(txPatch, "secrets"))
    startSaveTimer();

  cJSON_Delete(txPatch);
  txPatch = nullptr;
}

void storageBegin() {
  CFG_LOCK();
  if (txDepth++ == 0)
    txPatch = cJSON_CreateObject();
}

void storageEnd() {
  if (txDepth <= 0) { CFG_UNLOCK(); return; }
  if (--txDepth == 0)
    commitPatch();
  CFG_UNLOCK();
}

/* ---- Public Config API ---- */

void storageLoad() {
  if (!cfgMux) cfgMux = xSemaphoreCreateRecursiveMutex();
  if (cfgRoot) cJSON_Delete(cfgRoot);

  /* /state/settings.json is the single source of truth.
   * First boot copies factory defaults to /state/; factory reset does the same.
   * No merging — just load what's there. */
  char* text = readFileLocal("/state/settings.json");
  if (text) {
    cfgRoot = cJSON_Parse(text);
    free(text);
  }
  if (!cfgRoot) cfgRoot = cJSON_CreateObject();
  remove("/state/settings.new");
}

bool storageExists(const char* key) {
  CFG_LOCK();
  cJSON* node = resolveKey(key);
  bool exists = node && !cJSON_IsObject(node) && !cJSON_IsArray(node);
  CFG_UNLOCK();
  return exists;
}

int storageGetInt(const char* key, int def) {
  CFG_LOCK();
  cJSON* node = resolveKey(key);
  int result = def;
  if (node) {
    if (cJSON_IsNumber(node)) result = node->valueint;
    else if (cJSON_IsString(node)) result = atoi(node->valuestring);
  }
  CFG_UNLOCK();
  return result;
}

void storageGetStr(const char* key, char* out, size_t outLen, const char* def) {
  if (outLen == 0) return;
  CFG_LOCK();
  cJSON* node = resolveKey(key);
  if (!node) { CFG_UNLOCK(); safeStrncpy(out, def, outLen); return; }
  if (cJSON_IsString(node)) { safeStrncpy(out, node->valuestring, outLen); CFG_UNLOCK(); return; }
  if (cJSON_IsNumber(node)) { snprintf(out, outLen, "%d", node->valueint); CFG_UNLOCK(); return; }
  CFG_UNLOCK();
  safeStrncpy(out, def, outLen);
}

void storageSet(const char* key, int val) {
  CFG_LOCK();
  bool autoCommit = (txDepth == 0);
  if (autoCommit) storageBegin();  /* takes lock again (recursive) */

  char leaf[48];
  cJSON* parent = navigateOrCreate(txPatch, key, leaf, sizeof(leaf));
  if (parent) {
    cJSON_DeleteItemFromObject(parent, leaf);
    cJSON_AddNumberToObject(parent, leaf, val);
  }

  if (autoCommit) storageEnd();  /* commits + releases inner lock */
  CFG_UNLOCK();
}

void storageSet(const char* key, const char* val) {
  CFG_LOCK();
  bool autoCommit = (txDepth == 0);
  if (autoCommit) storageBegin();

  char leaf[48];
  cJSON* parent = navigateOrCreate(txPatch, key, leaf, sizeof(leaf));
  if (parent) {
    cJSON_DeleteItemFromObject(parent, leaf);
    if (inferType(val) == CFG_INT)
      cJSON_AddNumberToObject(parent, leaf, atoi(val));
    else
      cJSON_AddStringToObject(parent, leaf, val);
  }

  if (autoCommit) storageEnd();
  CFG_UNLOCK();
}

void storageUnset(const char* key) {
  CFG_LOCK();
  bool autoCommit = (txDepth == 0);
  if (autoCommit) storageBegin();

  char leaf[48];
  cJSON* parent = navigateOrCreate(txPatch, key, leaf, sizeof(leaf));
  if (parent) {
    cJSON_DeleteItemFromObject(parent, leaf);
    cJSON_AddNullToObject(parent, leaf);
  }

  if (autoCommit) storageEnd();
  CFG_UNLOCK();
}

/** Build nested JSON with a null at the given dot-path. */
static cJSON* buildNullJson(const char* dotPath) {
  cJSON* root = cJSON_CreateObject();
  char leaf[48];
  cJSON* parent = navigateOrCreate(root, dotPath, leaf, sizeof(leaf));
  if (parent)
    cJSON_AddNullToObject(parent, leaf);
  return root;
}

/** Delete a node from a tree at the given dot-path. */
static bool deleteFromTree(cJSON* root, const char* dotPath) {
  const char* lastDot = strrchr(dotPath, '.');
  if (!lastDot) {
    cJSON* item = cJSON_DetachItemFromObject(root, dotPath);
    if (!item) return false;
    cJSON_Delete(item);
    return true;
  }
  std::string parentPath(dotPath, lastDot - dotPath);
  cJSON* parent = navigatePath(root, parentPath.c_str());
  if (!parent) return false;
  cJSON* item = cJSON_DetachItemFromObject(parent, lastDot + 1);
  if (!item) return false;
  cJSON_Delete(item);
  return true;
}

void storageDeleteTree(const char* keyOrPrefix) {
  if (!keyOrPrefix || !*keyOrPrefix) return;

  CFG_LOCK();
  bool removed = deleteFromTree(cfgRoot, keyOrPrefix);
  CFG_UNLOCK();
  if (!removed) return;

  if (strncmp(keyOrPrefix, "s.", 2) == 0)
    startSaveTimer();

  /* Notify browser WS with {path: null} */
  if (wsHandle >= 0) {
    cJSON* json = buildNullJson(keyOrPrefix);
    if (json) {
      char* text = cJSON_PrintUnformatted(json);
      cJSON_Delete(json);
      if (text) {
        wsSendText(wsHandle, text, strlen(text));
        cJSON_free(text);
      }
    }
  }
}

void storageSave() {
  if (saveTimer) esp_timer_stop(saveTimer);
  writeSettingsFile();
}

cfg_type_t storageGetType(const char* key) {
  CFG_LOCK();
  cJSON* node = resolveKey(key);
  cfg_type_t t = (node && cJSON_IsNumber(node)) ? CFG_INT : CFG_STR;
  CFG_UNLOCK();
  return t;
}

/* ---- Bulk operations ---- */

/** Walk src subtree, storageSet each leaf at the corresponding dst path. */
static void walkAndCopy(cJSON* node, const std::string& srcPre,
                        const std::string& dstPre, bool onlyIfExists) {
  int idx = 0;
  cJSON* item;
  cJSON_ArrayForEach(item, node) {
    char idxBuf[12];
    const char* name = item->string;
    if (!name) { snprintf(idxBuf, sizeof(idxBuf), "%d", idx); name = idxBuf; }
    idx++;
    std::string srcKey = srcPre + "." + name;
    std::string dstKey = dstPre + "." + name;
    if (cJSON_IsObject(item) || cJSON_IsArray(item)) {
      walkAndCopy(item, srcKey, dstKey, onlyIfExists);
    } else {
      if (onlyIfExists && !navigatePath(cfgRoot, dstKey.c_str())) continue;
      if (cJSON_IsNumber(item))
        storageSet(dstKey.c_str(), item->valueint);
      else if (cJSON_IsString(item))
        storageSet(dstKey.c_str(), item->valuestring);
    }
  }
}

void storageCopy(const char* srcPrefix, const char* dstPrefix, bool onlyIfTargetKeyExists) {
  std::string src = stripDots(srcPrefix);
  std::string dst = stripDots(dstPrefix);

  CFG_LOCK();
  cJSON* srcNode = navigatePath(cfgRoot, src.c_str());
  if (!srcNode) { CFG_UNLOCK(); return; }

  if (!cJSON_IsObject(srcNode) && !cJSON_IsArray(srcNode)) {
    /* Single leaf copy */
    if (onlyIfTargetKeyExists && !navigatePath(cfgRoot, dst.c_str())) { CFG_UNLOCK(); return; }
    if (cJSON_IsNumber(srcNode))
      storageSet(dst.c_str(), srcNode->valueint);
    else if (cJSON_IsString(srcNode))
      storageSet(dst.c_str(), srcNode->valuestring);
    CFG_UNLOCK();
    return;
  }

  storageBegin();
  walkAndCopy(srcNode, src, dst, onlyIfTargetKeyExists);
  storageEnd();
  CFG_UNLOCK();
}

void storageCopyNoNotify(const char* srcPrefix, const char* dstPrefix, bool onlyIfTargetKeyExists) {
  std::string src = stripDots(srcPrefix);
  std::string dst = stripDots(dstPrefix);

  CFG_LOCK();
  cJSON* srcNode = navigatePath(cfgRoot, src.c_str());
  if (!srcNode) { CFG_UNLOCK(); return; }

  if (onlyIfTargetKeyExists) { CFG_UNLOCK(); return; }

  /* Clone src subtree, deep-merge at dst */
  cJSON* clone = cJSON_Duplicate(srcNode, true);
  if (!clone) { CFG_UNLOCK(); return; }

  /* Build a wrapper so deepMerge places the clone at the right path */
  cJSON* wrapper = cJSON_CreateObject();
  char leaf[48];
  cJSON* parent = navigateOrCreate(wrapper, dst.c_str(), leaf, sizeof(leaf));
  if (parent) {
    cJSON_AddItemToObject(parent, leaf, clone);
    deepMerge(cfgRoot, wrapper);
  } else {
    cJSON_Delete(clone);
  }
  CFG_UNLOCK();
  cJSON_Delete(wrapper);

  if (isSaved(dst.c_str())) startSaveTimer();
}

int storageArrayCount(const char* prefix) {
  std::string pre = stripDots(prefix);
  CFG_LOCK();
  cJSON* node = navigatePath(cfgRoot, pre.c_str());
  if (!node) { CFG_UNLOCK(); return 0; }
  int count;
  if (cJSON_IsArray(node)) {
    count = cJSON_GetArraySize(node);
  } else {
    count = 0;
    char key[12];
    for (;;) {
      snprintf(key, sizeof(key), "%d", count);
      if (!cJSON_GetObjectItem(node, key)) break;
      count++;
    }
  }
  CFG_UNLOCK();
  return count;
}

void storageForEach(const char* prefix, void (*cb)(const char* key, const char* val)) {
  std::string pre = stripDots(prefix);
  CFG_LOCK();
  cJSON* node = pre.empty() ? cfgRoot : navigatePath(cfgRoot, pre.c_str());
  if (!node) { CFG_UNLOCK(); return; }
  if (cJSON_IsObject(node) || cJSON_IsArray(node)) {
    walkLeaves(node, pre.c_str(), [](const char* key, const char* val, void* ctx) {
      auto cb = (void (*)(const char*, const char*))ctx;
      cb(key, val);
    }, (void*)cb);
  } else {
    char valBuf[32];
    const char* val;
    if (cJSON_IsString(node)) val = node->valuestring;
    else if (cJSON_IsNumber(node)) { snprintf(valBuf, sizeof(valBuf), "%d", node->valueint); val = valBuf; }
    else { CFG_UNLOCK(); return; }
    cb(pre.c_str(), val);
  }
  CFG_UNLOCK();
}

void storageList(cli_write_fn write) {
  CFG_LOCK();
  if (!cfgRoot || !cfgRoot->child) {
    CFG_UNLOCK();
    write("(empty)\n", 8);
    return;
  }
  walkTreePrint(cfgRoot, "", write);
  CFG_UNLOCK();
}

/* ---- Public File I/O API ---- */

int storageFopen(const char* path, const char* mode) {
  int slot = allocSlot();
  if (slot < 0) return -1;
  storage_file_op_t req = {};
  req.op = storage_file_op_t::OPEN;
  req.path = path;
  req.path2 = mode;
  req.slot = slot;
  if (fsOp(req) < 0) { fileActive[slot] = false; return -1; }
  return slot;
}

size_t storageFread(void* buf, size_t maxLen, int f) {
  if (f < 0 || f >= MAX_FILE_SLOTS || !fileActive[f]) return 0;
  storage_file_op_t req = {};
  req.op = storage_file_op_t::READ;
  req.slot = f;
  req.buf = buf;
  req.len = maxLen;
  int r = fsOp(req);
  return r > 0 ? (size_t)r : 0;
}

size_t storageFwrite(const void* buf, size_t len, int f) {
  if (f < 0 || f >= MAX_FILE_SLOTS || !fileActive[f]) return 0;
  storage_file_op_t req = {};
  req.op = storage_file_op_t::WRITE;
  req.slot = f;
  req.buf = (void*)buf;
  req.len = len;
  int r = fsOp(req);
  return r > 0 ? (size_t)r : 0;
}

long storageFtell(int f) {
  if (f < 0 || f >= MAX_FILE_SLOTS || !fileActive[f]) return -1;
  storage_file_op_t req = {};
  req.op = storage_file_op_t::TELL;
  req.slot = f;
  return (long)fsOp(req);
}

void storageFclose(int f) {
  if (f < 0 || f >= MAX_FILE_SLOTS || !fileActive[f]) return;
  storage_file_op_t req = {};
  req.op = storage_file_op_t::CLOSE;
  req.slot = f;
  fsOp(req);
}

int storageStat(const char* path, struct stat* st) {
  storage_file_op_t req = {};
  req.op = storage_file_op_t::STAT;
  req.path = path;
  req.st = st;
  return fsOp(req);
}

int storageRename(const char* oldPath, const char* newPath) {
  storage_file_op_t req = {};
  req.op = storage_file_op_t::RENAME;
  req.path = oldPath;
  req.path2 = newPath;
  return fsOp(req);
}

int storageRemove(const char* path) {
  storage_file_op_t req = {};
  req.op = storage_file_op_t::REMOVE;
  req.path = path;
  return fsOp(req);
}

/* ---- Streaming file API (consumer side, runs on caller's task) ---- */

/* Consumer-side state per stream type (single of each for now) */
static int    streamHandle = -1;
static int    streamWriteHandle = -1;
static long   streamWritePos = 0;    /* client-side position tracking (avoids round-trip for tell) */
static size_t streamWriteSinceNudge = 0;  /* bytes written since last drain nudge */

int storageStreamOpen(const char* path) {
  itsClientInit(2);

  stream_open_msg_t msg = { path, nullptr, -1 };
  stream_open_msg_t* ptr = &msg;

  int handle = itsConnect("fs", FS_STREAM_RD_PORT, &ptr, sizeof(ptr),
                          pdMS_TO_TICKS(5000));
  if (handle < 0) return -1;

  /* onStreamConnect wrote msg.result — check if file opened */
  if (msg.result < 0) {
    itsDisconnect(handle);
    return -1;
  }
  streamHandle = handle;
  return handle;
}

size_t storageStreamRead(int handle, void* buf, size_t len) {
  size_t got = itsRecv(handle, buf, len, pdMS_TO_TICKS(500));

  /* Nudge fs worker to refill when buffer is getting low */
  if (itsBytesAvailable(handle) < FS_READ_REFILL) {
    int h = handle;
    itsSendAuxByHandle(fsWorkerTask, &h, sizeof(h), 0, STREAM_AUX_NUDGE);
  }
  return got;
}

bool storageStreamSeek(int handle, long offset) {
  stream_seek_msg_t msg = { handle, offset, false };
  stream_seek_msg_t* ptr = &msg;
  itsSendAuxByHandle(fsWorkerTask, &ptr, sizeof(ptr),
                     portMAX_DELAY, STREAM_AUX_SEEK, ITS_WAIT_PICKUP);
  if (msg.result && handle == streamWriteHandle) streamWritePos = offset;
  return msg.result;
}

int storageStreamOpenWrite(const char* path, const char* mode) {
  itsClientInit(2);

  stream_open_msg_t msg = { path, mode, -1 };
  stream_open_msg_t* ptr = &msg;

  int handle = itsConnect("fs", FS_STREAM_WR_PORT, &ptr, sizeof(ptr),
                          pdMS_TO_TICKS(5000));
  if (handle < 0) return -1;
  if (msg.result < 0) { itsDisconnect(handle); return -1; }
  streamWriteHandle = handle;
  streamWritePos = 0;
  streamWriteSinceNudge = 0;
  return handle;
}

size_t storageStreamWrite(int handle, const void* data, size_t len) {
  size_t sent = itsSendSilent(handle, data, len, portMAX_DELAY);
  streamWritePos += (long)sent;
  /* Nudge fs worker only when enough data has accumulated for a drain.
   * Avoids waking it on every small write (which caused 97% CPU spin). */
  streamWriteSinceNudge += sent;
  if (streamWriteSinceNudge >= FS_WRITE_DRAIN) {
    streamWriteSinceNudge = 0;
    int h = handle;
    itsSendAuxByHandle(fsWorkerTask, &h, sizeof(h), 0, STREAM_AUX_NUDGE);
  }
  return sent;
}

bool storageStreamFlush(int handle) {
  int h = handle;
  itsSendAuxByHandle(fsWorkerTask, &h, sizeof(h),
                     portMAX_DELAY, STREAM_AUX_FLUSH, ITS_WAIT_PICKUP);
  return true;
}

long storageStreamTell(int handle) {
  /* Write streams: return client-tracked position (no round-trip) */
  if (handle == streamWriteHandle) return streamWritePos;
  /* Read streams: ask fs worker */
  stream_tell_msg_t msg = { handle, -1 };
  stream_tell_msg_t* ptr = &msg;
  itsSendAuxByHandle(fsWorkerTask, &ptr, sizeof(ptr),
                     portMAX_DELAY, STREAM_AUX_TELL, ITS_WAIT_PICKUP);
  return msg.result;
}

void storageStreamClose(int handle) {
  /* Flush write data if this is a write stream */
  if (handle == streamWriteHandle) storageStreamFlush(handle);
  itsDisconnect(handle);
  if (handle == streamHandle) streamHandle = -1;
  if (handle == streamWriteHandle) { streamWriteHandle = -1; streamWritePos = 0; streamWriteSinceNudge = 0; }
}

/* ---- CLI commands ---- */

static void cmdSet(const char* a) {
    if (strcmp(a, "help") == 0) { cliPrintf("  %-*s set config variable\n", CLI_HELP_COL, "set <key>=<value>"); return; }
    const char* eq = strchr(a, '=');
    if (!eq || eq == a) { cliPrintf("usage: set <key>=<value>\n"); return; }
    char key[48];
    size_t klen = eq - a;
    while (klen > 0 && a[klen - 1] == ' ') klen--;
    if (klen == 0 || klen >= sizeof(key)) { cliPrintf("err: key empty or too long\n"); return; }
    memcpy(key, a, klen); key[klen] = '\0';
    const char* val = eq + 1;
    while (*val == ' ') val++;
    storageSet(key, val);
    if (strncmp(key, "s.log", 5) == 0)
        logApplyLevels();
}

static void cmdUnset(const char* a) {
    if (strcmp(a, "help") == 0) { cliPrintf("  %-*s delete config variable\n", CLI_HELP_COL, "unset <key>"); return; }
    if (!*a) { cliPrintf("usage: unset <key>\n"); return; }
    storageDeleteTree(a);
}

static void cmdShow(const char* a) {
    if (strcmp(a, "help") == 0) { cliPrintf("  %-*s show config variables\n", CLI_HELP_COL, "show [<prefix>]"); return; }
    auto write = [](const char* d, size_t l) { cliPrintf("%.*s", (int)l, d); };

    if (!*a) {
        storageList(write);
        return;
    }

    CFG_LOCK();
    /* Try exact path first */
    cJSON* node = navigatePath(cfgRoot, a);
    if (node) {
        if (cJSON_IsObject(node) || cJSON_IsArray(node)) {
            walkTreePrint(node, a, write);
        } else {
            char valBuf[32];
            const char* val = nullptr;
            if (cJSON_IsString(node)) val = node->valuestring;
            else if (cJSON_IsNumber(node)) { snprintf(valBuf, sizeof(valBuf), "%d", node->valueint); val = valBuf; }
            if (val) cliPrintf("  %s = %s\n", a, val);
        }
        CFG_UNLOCK();
        return;
    }

    /* Partial last-segment match */
    bool found = false;
    const char* lastDot = strrchr(a, '.');
    if (lastDot) {
        char parentPath[64];
        size_t parentLen = lastDot - a;
        if (parentLen >= sizeof(parentPath)) { CFG_UNLOCK(); cliPrintf("  (prefix too long)\n"); return; }
        memcpy(parentPath, a, parentLen);
        parentPath[parentLen] = '\0';
        cJSON* parent = navigatePath(cfgRoot, parentPath);
        if (parent && cJSON_IsObject(parent)) {
            const char* partial = lastDot + 1;
            size_t partialLen = strlen(partial);
            cJSON* item;
            cJSON_ArrayForEach(item, parent) {
                if (item->string && strncmp(item->string, partial, partialLen) == 0) {
                    char key[128];
                    snprintf(key, sizeof(key), "%s.%s", parentPath, item->string);
                    if (cJSON_IsObject(item) || cJSON_IsArray(item))
                        walkTreePrint(item, key, write);
                    else {
                        char vb[32];
                        const char* v = nullptr;
                        if (cJSON_IsString(item)) v = item->valuestring;
                        else if (cJSON_IsNumber(item)) { snprintf(vb, sizeof(vb), "%d", item->valueint); v = vb; }
                        if (v) cliPrintf("  %s = %s\n", key, v);
                    }
                    found = true;
                }
            }
        }
    } else {
        /* No dot — match against root children */
        size_t len = strlen(a);
        cJSON* item;
        cJSON_ArrayForEach(item, cfgRoot) {
            if (item->string && strncmp(item->string, a, len) == 0) {
                if (cJSON_IsObject(item) || cJSON_IsArray(item))
                    walkTreePrint(item, item->string, write);
                else {
                    char vb[32];
                    const char* v = nullptr;
                    if (cJSON_IsString(item)) v = item->valuestring;
                    else if (cJSON_IsNumber(item)) { snprintf(vb, sizeof(vb), "%d", item->valueint); v = vb; }
                    if (v) cliPrintf("  %s = %s\n", item->string, v);
                }
                found = true;
            }
        }
    }
    CFG_UNLOCK();
    if (!found) cliPrintf("  (no matches)\n");
}

static void cmdSave(const char* a) {
    if (strcmp(a, "help") == 0) { cliPrintf("  %-*s write settings to flash now\n", CLI_HELP_COL, "save"); return; }
    storageSave();
}

void storageRegisterCmds() {
    cliRegisterCmd("set", cmdSet);
    cliRegisterCmd("unset", cmdUnset);
    cliRegisterCmd("show", cmdShow);
    cliRegisterCmd("save", cmdSave);
}

/* ---- Config WebSocket handling (multi-client) ---- */

#define WS_MAX_CLIENTS 4
static int wsHandles[WS_MAX_CLIENTS] = {-1, -1, -1, -1};

static bool wsAnyClient() {
    for (int i = 0; i < WS_MAX_CLIENTS; i++)
        if (wsHandles[i] >= 0) return true;
    return false;
}

static void wsSendFullDump(int handle) {
    CFG_LOCK();
    cJSON* clone = cJSON_Duplicate(cfgRoot, true);
    CFG_UNLOCK();
    if (!clone) return;

    /* Remove secrets from the dump */
    cJSON* secrets = cJSON_DetachItemFromObject(clone, "secrets");
    if (secrets) cJSON_Delete(secrets);

    char* text = cJSON_PrintUnformatted(clone);
    cJSON_Delete(clone);
    if (!text) return;
    wsSendText(handle, text, strlen(text));
    cJSON_free(text);
}

/** Accumulate a changed key into wsPendingPatch for coalesced WS output.
 *  Called from storage task's "" subscription callback. */
static void wsAccumulateChange(const char* key, const char* val) {
    if (!wsAnyClient()) return;
    if (isSecret(key)) return;

    /* Check cfgRoot — storageUnset removes the key before firing callbacks,
       so if the key is gone we skip (matches current behavior: unset is not
       echoed to browser). */
    CFG_LOCK();
    cJSON* node = navigatePath(cfgRoot, key);
    if (!node) { CFG_UNLOCK(); return; }

    if (!wsPendingPatch) wsPendingPatch = cJSON_CreateObject();

    char leaf[48];
    cJSON* parent = navigateOrCreate(wsPendingPatch, key, leaf, sizeof(leaf));
    if (!parent) { CFG_UNLOCK(); return; }

    cJSON_DeleteItemFromObject(parent, leaf);
    cJSON_AddItemToObject(parent, leaf, cJSON_Duplicate(node, false));
    CFG_UNLOCK();
}

/** Flush accumulated WS changes to all connected browsers. */
static void wsFlushPatch() {
    if (!wsPendingPatch || !wsAnyClient()) return;
    char* text = cJSON_PrintUnformatted(wsPendingPatch);
    cJSON_Delete(wsPendingPatch);
    wsPendingPatch = nullptr;
    if (!text) return;
    size_t len = strlen(text);
    for (int i = 0; i < WS_MAX_CLIENTS; i++)
        if (wsHandles[i] >= 0) wsSendText(wsHandles[i], text, len);
    cJSON_free(text);
}

/** Process incoming JSON from browser. Null = silent delete, values = storageSet. */
static void mergeIncomingWs(cJSON* obj, const std::string& prefix) {
  cJSON* item;
  cJSON_ArrayForEach(item, obj) {
    std::string key = prefix.empty() ? item->string : prefix + "." + item->string;
    if (isSecret(key.c_str())) continue;
    if (cJSON_IsNull(item)) {
      /* Silent delete (no subscription callbacks) */
      CFG_LOCK();
      deleteFromTree(cfgRoot, key.c_str());
      CFG_UNLOCK();
      if (strncmp(key.c_str(), "s.", 2) == 0) startSaveTimer();
    } else if (cJSON_IsObject(item)) {
      mergeIncomingWs(item, key);
    } else if (cJSON_IsNumber(item)) {
      storageSet(key.c_str(), item->valueint);
    } else if (cJSON_IsString(item)) {
      storageSet(key.c_str(), item->valuestring);
    }
  }
}

static void wsHandleMessage(int handle, const char* text, size_t len) {
    if (len == 10 && memcmp(text, "{\"ping\":1}", 10) == 0) {
        wsSendText(handle, "{\"pong\":1}", 10);
        return;
    }
    if (len == 10 && memcmp(text, "{\"save\":1}", 10) == 0) {
        storageSave();
        return;
    }
    cJSON* root = cJSON_Parse(text);
    if (!root) return;
    mergeIncomingWs(root, "");
    cJSON_Delete(root);
}

static void wsPollConfig() {
    uint8_t buf[1024];
    for (int i = 0; i < WS_MAX_CLIENTS; i++) {
        if (wsHandles[i] < 0) continue;
        size_t outLen = 0;
        int op = wsReadFrame(wsHandles[i], buf, sizeof(buf), &outLen);
        if (op == 1) {
            wsHandleMessage(wsHandles[i], (const char*)buf, outLen);
        } else if (op < 0) {
            wsHandles[i] = -1;
        }
    }
}

/* ---- ITS server callbacks ---- */

static int storageItsConnect(int handle, int itsPort, const void* data, size_t len) {
    if (len < sizeof(net_connect_t)) return -1;
    auto* cd = (const net_connect_t*)data;
    if (!cd->ws) return -1;

    /* Read headers once — used for both auth check and WS upgrade */
    char hdr[1024];
    int hdrLen = webGetHeader(handle, hdr, sizeof(hdr));
    if (hdrLen <= 0) return -1;

    if (!wsUpgrade(handle, hdr, hdrLen)) return -1;

    if (authEnabled()) {
        char cookie[64] = {};
        webExtractCookie(hdr, hdrLen, "session", cookie, sizeof(cookie));
        if (authCheck(cookie).empty()) {
            wsSendClose(handle, 4401);
            return -1;
        }
    }

    /* Find a free slot */
    int slot = -1;
    for (int i = 0; i < WS_MAX_CLIENTS; i++)
        if (wsHandles[i] < 0) { slot = i; break; }
    if (slot < 0) {
        wsSendClose(handle, 1013);  /* try again later */
        return -1;
    }
    wsHandles[slot] = handle;
    wsSendFullDump(handle);
    return slot;
}

static void storageItsDisconnect(int ref) {
    if (ref >= 0 && ref < WS_MAX_CLIENTS)
        wsHandles[ref] = -1;
    /* Discard pending patch if no clients left */
    if (!wsAnyClient() && wsPendingPatch) {
        cJSON_Delete(wsPendingPatch);
        wsPendingPatch = nullptr;
    }
}

/* ---- Task function ---- */

static void storageSaveAux(TaskHandle_t, uint16_t, const void*, size_t) {
    writeSettingsFile();
}

static void storageTaskFn(void* arg) {
    itsServerInit(WS_MAX_CLIENTS, 2048, 4096);
    itsServerOnConnect(storageItsConnect);
    itsServerOnDisconnect(storageItsDisconnect);
    itsOnAux(storageSaveAux, STORAGE_SAVE_PORT);

    /* Subscribe to all config changes for WS coalescing */
    storageSubscribeChanges("", ON_CHANGE {
        wsAccumulateChange(key, val);
    });
    storageSubscribeChanges("net.up", ON_CHANGE {
        if (atoi(val) == 0) {
            for (int i = 0; i < WS_MAX_CLIENTS; i++) {
                if (wsHandles[i] >= 0) {
                    itsServerKick(wsHandles[i]);
                    wsHandles[i] = -1;
                }
            }
        }
    });

    web_path_msg_t reg = {};
    reg.itsPort = 0;
    safeStrncpy(reg.path, "epl", sizeof(reg.path));
    while (!itsSendAux("web", &reg, sizeof(reg), pdMS_TO_TICKS(500)))
        vTaskDelay(pdMS_TO_TICKS(100));

    info("ready\n");

    for (;;) {
        TickType_t timeout = wsAnyClient() ? pdMS_TO_TICKS(10) : portMAX_DELAY;
        while (itsPoll(timeout)) { timeout = 0; }
        wsPollConfig();
        wsFlushPatch();
    }
}

void storageInit() {
    /* fs worker: small DRAM-stack task for LittleFS/SD file I/O via ITS */
    streamFillBuf = (uint8_t*)heap_caps_malloc(FS_STREAM_CHUNK, MALLOC_CAP_SPIRAM);
    fsReady = xSemaphoreCreateBinary();
    xTaskCreatePinnedToCore(fsWorkerFn, "fs", 3072, nullptr, 1, &fsWorkerTask, 1);
    xSemaphoreTake(fsReady, portMAX_DELAY);  /* wait until ITS server is up */
    vSemaphoreDelete(fsReady);
    fsReady = nullptr;

    /* storage task: PSRAM stack (config WS + ITS, no direct file I/O) */
    xTaskCreatePinnedToCoreWithCaps(storageTaskFn, "storage", 4096, NULL, 1, &storageHandle, 1, MALLOC_CAP_SPIRAM);
}
