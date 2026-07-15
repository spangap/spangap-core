/**
 * storage — config store (an ACTOR).
 *
 * Config: cJSON tree in RAM, backed by JSON on /state.
 * WRITES are messages: storageSet and friends serialize the change into an op
 * list and hand it to the storage task, which applies it (build patch tree →
 * RFC 7396 deepMerge into cfgRoot → notify subscribers → save timer) — the
 * message boundary is the transaction, so a list applies atomically. Writes are
 * synchronous (the caller blocks until applied, so read-your-writes holds);
 * when the caller IS the storage task, or storage hasn't spawned yet, the write
 * applies directly (the fast path). READS stay direct under cfgMux.
 *
 * storageBegin()/storageEnd() bracket a task-local op accumulator: the writes
 * between them ship as one atomic message. (Read-your-writes INSIDE an open
 * bracket is gone — reads see committed state, not the bracket's pending ops.)
 *
 * Thread safety: a recursive mutex (cfgMux) guards readers vs the single
 * actor-writer on cfgRoot, plus the externals bookkeeping. The subscription
 * table is storage-task-owned (mutated only via SUB/UNSUB ops), so it never
 * races. There is no write lock and no transaction accumulator any more.
 *
 * Browser config DataChannel (`storage:1`, packet-mode over WebRTC):
 * - Device→browser: full dump on connect, then coalesced merge-patches.
 * - Browser→device: nested JSON merge-patches. null = delete subtree.
 * - Secrets (secrets.*) are never sent to browser and browser writes are ignored.
 *
 * Authentication and BUSY/takeover gating live entirely at the /webrtc
 * signaling WS — by the time a DC arrives here the peer is authenticated.
 */
#include "storage.h"
#include "storage_db.h"
#include "fs.h"
#include "log.h"
#include "cli.h"
#include "its.h"
#include "compat.h"
#include "mem.h"

#include <string>
#include <vector>
#include <deque>
#include <set>
#include <atomic>
#include <algorithm>
#include <functional>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>
#include "cJSON.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "miniz.h"        /* ROM tdefl/tinfl — gz-compressed storage files */
#include "esp_rom_crc.h"  /* esp_rom_crc32_le == zlib crc32 for the gzip footer */

/* ---- External storage files ----
 *
 * /state/storage/<mode>/<key.path>.json holds a sub-tree at the given prefix.
 * The first-level subdir under /state/storage/ is the "mode" — only "external"
 * is used today (own file, lives in cfgRoot at runtime, saved to its own file
 * instead of in root.json). New modes (e.g. "flash-only") can be added later
 * by handling more subdirs in scanExternals(). Drop a file in the build tree
 * and it just appears — no compile-time registration. */
struct external_t {
  std::string prefix;          /* dot-path key prefix, e.g. "s.time.zones" */
  std::string path;            /* on-disk file, e.g. "/state/storage/external/s.time.zones.json" */
  bool        dirty = false;   /* sub-tree at prefix changed since last flush */
  bool        pendingDelete = false; /* file should be removed + unregistered on next flush */
};
static std::vector<external_t> externals;
static bool rootDirty = false;        /* root.json needs rewrite */

/* ---- Config tree state ---- */

static cJSON* cfgRoot = nullptr;        /* committed config (the truth) */
static SemaphoreHandle_t cfgMux = nullptr;  /* recursive mutex: readers vs the single
                                             * actor-writer on cfgRoot, plus externals */

#define CFG_LOCK()   xSemaphoreTakeRecursive(cfgMux, portMAX_DELAY)
#define CFG_UNLOCK() xSemaphoreGiveRecursive(cfgMux)

/* ── phase-0 latency instrumentation (temporary; see plans/storage-needs-work) ─
 * Surface the two ways storage can starve the browser DC's 2 s liveness ping:
 * a CFG_LOCK snapshot hold (whole-tree/subtree cJSON_Duplicate) and an actor
 * poll-loop iteration that runs long (a heavy op-apply, patch serialize, or
 * dump pump). Both warns are self-throttling — they fire only past threshold. */
#define CFG_HOLD_WARN_US    50000    /* 50 ms  — a lock-held duplicate/print   */
#define ACTOR_STALL_WARN_US 250000   /* 250 ms — a single poll-loop iteration  */
#define ACTOR_STALL_LOUD_US 500000   /* > 500 ms: warn; 250-500 ms is debug-only */

static bool savePending = false;
static esp_timer_handle_t saveTimer = nullptr;

/* Save-now semaphores handed to the persist worker by the actor; the worker
 * gives each after a flush completes (storageSave waits on its own). */
static std::vector<SemaphoreHandle_t> pendingSaveSems;

/* ---- Task state ---- */

static TaskHandle_t storageHandle = nullptr;
/* Persist worker: owns the blocking fs writes (writeSettingsFile). The storage
 * task runs itsPoll and must NEVER do fs I/O around its poll loop — each
 * proxyOp parks the caller in xSemaphoreTake(pickupSem, portMAX_DELAY) for the
 * whole op, and writeSettingsFile chains one per dirty external + root.json. On
 * the storage task that blocks its inbox drain → "notify drop … → [storage]"
 * floods (worst with many lxmf externals). So saves run here, off the loop. */
static TaskHandle_t saveWorkerHandle = nullptr;
/* Change-notification dispatch worker: owns the BLOCKING remote-subscriber
 * CHANGED sends (itsSendAuxOwnedByTaskHandle → inboxEnqueue waits up to
 * STORAGE_NOTIFY_TIMEOUT_MS for space in a slow subscriber's inbox). Running
 * that on the storage actor let ONE flooded subscriber stall the actor's own
 * op-inbox drain: every task's port-44 config write then backed up (the "owned
 * aux to port 44 … receiver stuck?" burst — self-sustaining once an unreachable
 * RNS link floods a subscriber). The actor now only BUILDS the per-subscriber
 * buffers (it owns the sub table, iterated on-task, so no race) and hands them
 * here; this worker absorbs the inbox/pickup wait off the poll loop. Mirrors the
 * persist worker: the actor never blocks on a subscriber again. */
static TaskHandle_t notifyWorkerHandle = nullptr;
static int dcHandle = -1;               /* single packet-mode DC client */
static cJSON* dcPendingPatch = nullptr; /* outgoing coalescing */
/* The structured-DB instance prefix the single client currently has "open"
 * (set by a {"fetch":...} request). Record bodies never ride the connect dump
 * (they aren't in cfgRoot); on fetch we ship the instance's records once, then
 * mirror only that instance's live changes as ordinary patches. */
static std::string dcOpenPrefix;

/* File I/O moved to fs.cpp/h — unified PSRAM-safe API. */

/* ---- Path navigation ---- */

static bool isAllDigits(const char* s, size_t len) {
  if (len == 0) return false;
  for (size_t i = 0; i < len; i++)
    if (s[i] < '0' || s[i] > '9') return false;
  return true;
}

/** Navigate a cJSON tree by dot-path. Returns the node or NULL.
 *
 *  Segment buffer is sized to 96 chars to accommodate SHA-256 hex segments
 *  (64 chars) that lxmf uses for inbound-message keys, e.g.
 *  `s.lxmf.id.0.msgs.<64-hex>.<field>`. The previous 48-byte cap silently
 *  rejected writes with longer segments — storageSet returned without
 *  setting anything, and inbound LXMs never persisted. Outbound keys (mids
 *  like `o_<ts>_<id>`) escaped notice because they're short. Subscriber
 *  change notifications carry keys at full length (variable-size
 *  messages, see notifyChange); this keeps the path parser from being
 *  the binding constraint. */
static cJSON* navigatePath(cJSON* root, const char* dotPath) {
  if (!root || !dotPath || !*dotPath) return nullptr;
  cJSON* node = root;
  const char* p = dotPath;
  while (*p) {
    const char* dot = strchr(p, '.');
    size_t segLen = dot ? (size_t)(dot - p) : strlen(p);
    if (segLen == 0) { p = dot + 1; continue; }
    char seg[96];
    if (segLen >= sizeof(seg)) {
      /* Used to silently fail and burn cycles in callers (lxmf's inbound
       * msg persistence depended on 64-char SHA-256 hex segments). If you
       * see this warn, bump seg[] and the matching leaf[] buffers above. */
      warn("storage: segment too long in key '%s' (%zu B, max %zu)",
           dotPath, segLen, sizeof(seg) - 1);
      return nullptr;
    }
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
    char seg[96];  /* see navigatePath: SHA-256 hex segments are 64 chars */
    if (segLen >= sizeof(seg)) {
      warn("storage: segment too long in key '%s' (%zu B, max %zu)",
           dotPath, segLen, sizeof(seg) - 1);
      return nullptr;
    }
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

/** Resolve a key directly in the committed tree. Reads never consult a pending
 *  patch now (writes are applied by the actor, not accumulated in a tx). */
static cJSON* resolveKey(const char* key) {
  return navigatePath(cfgRoot, key);
}

/** Strip trailing dots from a prefix string. */
static std::string stripDots(const char* s) {
  std::string r(s);
  while (!r.empty() && r.back() == '.') r.pop_back();
  return r;
}

/* ---- Deep merge (RFC 7396, with array-element extension) ---- */

static void deepMerge(cJSON* dst, const cJSON* src);
static bool markExternalsDeletedUnder(const char* keyOrPrefix);  /* defined below */
static void startSaveTimer();                                    /* defined below */
static void routePatchDirty(const cJSON* node, char* path, size_t cap, size_t len);
static bool isSecret(const char* key);
static cfg_type_t inferType(const char* val);                    /* defined below */
static void storageDbFlushDirty();                               /* structured-DB flush (defined below) */

/** True if every (named) child of obj has an all-digits name. Empty objects
 *  return false — we don't want to interpret `{}` as "merge nothing into the
 *  array"; that case never happens for a real edit. */
static bool allNumericKeys(const cJSON* obj) {
  if (!cJSON_IsObject(obj) || !obj->child) return false;
  for (const cJSON* it = obj->child; it; it = it->next) {
    if (!it->string) return false;
    if (!isAllDigits(it->string, strlen(it->string))) return false;
  }
  return true;
}

/** Merge a numeric-keyed object patch into an existing array, element-wise.
 *  patch[i]=null removes the element (subsequent indices shift down — same
 *  semantics as object-key delete). patch[i]=object recursively merges into
 *  the existing element if it's an object, otherwise replaces. patch[i] at
 *  an out-of-bounds index extends the array (padding with null if sparse).
 *
 *  This is what makes `set s.net.wifi.nets.3.pass=foo` work: the patch tree
 *  always builds nested objects (numeric segments become object keys), and
 *  without this routine the existing array would be wholesale replaced. */
static void deepMergeIntoArray(cJSON* dstArr, const cJSON* patchObj) {
  /* Apply deletions first, in descending order, so earlier indices stay valid. */
  std::vector<int> deletions;
  for (const cJSON* it = patchObj->child; it; it = it->next) {
    if (cJSON_IsNull(it)) deletions.push_back(atoi(it->string));
  }
  std::sort(deletions.begin(), deletions.end(), std::greater<int>());
  for (int idx : deletions)
    if (idx >= 0 && idx < cJSON_GetArraySize(dstArr))
      cJSON_DeleteItemFromArray(dstArr, idx);

  for (const cJSON* it = patchObj->child; it; it = it->next) {
    if (cJSON_IsNull(it)) continue;
    int idx = atoi(it->string);
    int sz = cJSON_GetArraySize(dstArr);
    cJSON* dstElem = (idx < sz) ? cJSON_GetArrayItem(dstArr, idx) : nullptr;

    if (cJSON_IsObject(it) && dstElem && cJSON_IsObject(dstElem)) {
      deepMerge(dstElem, it);
    } else if (idx < sz) {
      cJSON_ReplaceItemInArray(dstArr, idx, cJSON_Duplicate(it, true));
    } else {
      while (cJSON_GetArraySize(dstArr) < idx)
        cJSON_AddItemToArray(dstArr, cJSON_CreateNull());
      cJSON_AddItemToArray(dstArr, cJSON_Duplicate(it, true));
    }
  }
}

/** Merge src into dst in place. Objects recurse; arrays receive element-wise
 *  patches when src is a numeric-keyed object; everything else replaces.
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
      } else if (dstChild && cJSON_IsArray(dstChild) && allNumericKeys(item)) {
        deepMergeIntoArray(dstChild, item);
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

/** Walk leaves for CLI output (show / storageList), appending "key = val\n"
 *  lines to `out`. The tree is snapshotted into `out` UNDER CFG_LOCK and the
 *  caller emits `out` only after releasing the lock — a blocking transport
 *  write must never run while CFG_LOCK is held. The CLI's own output drains
 *  over a DataChannel whose consumer (the LCD task) itself takes CFG_LOCK on a
 *  timer (status-bar clock, wifi/battery subscriptions); holding the lock
 *  across the back-pressured send therefore deadlocks the drain and wedges both
 *  tasks into a watchdog reboot once the output is large enough to span a tick. */
static void walkTreeCollect(cJSON* node, const char* prefix, std::string& out) {
  walkLeaves(node, prefix, [](const char* key, const char* val, void* ctx) {
    auto* out = (std::string*)ctx;
    char line[192];
    int n = snprintf(line, sizeof(line), "%s = %s\n", key, val);
    if (n > 0) out->append(line, (size_t)(n < (int)sizeof(line) ? n : (int)sizeof(line) - 1));
  }, &out);
}

/** Append one "key = val\n" leaf line (string/number values only) to `out`. */
static void appendLeaf(std::string& out, const char* key, cJSON* node) {
  char vb[32];
  const char* v = nullptr;
  if (cJSON_IsString(node)) v = node->valuestring;
  else if (cJSON_IsNumber(node)) { snprintf(vb, sizeof(vb), "%d", node->valueint); v = vb; }
  if (!v) return;
  char line[192];
  int n = snprintf(line, sizeof(line), "%s = %s\n", key, v);
  if (n > 0) out.append(line, (size_t)(n < (int)sizeof(line) ? n : (int)sizeof(line) - 1));
}

/** Emit a collected multi-line buffer to the CLI with NO lock held, one line
 *  per write() so a packet-mode transport (the DC ports) never sees a body over
 *  its per-message cap. */
static void emitLines(const std::string& out, cli_write_fn write) {
  size_t i = 0, n = out.size();
  while (i < n) {
    size_t nl = out.find('\n', i);
    size_t end = (nl == std::string::npos) ? n : nl + 1;
    write(out.data() + i, end - i);
    i = end;
  }
}

/* ---- Settings file read/write ---- */

static bool isSecret(const char* key) {
  return strncmp(key, "secrets.", 8) == 0;
}

/* The fw.* subtree is immutable firmware identity (CONFIG_SPANGAP_FW_*),
 * synthesized into the browser dump (dcBuildDump) straight from ROM — it
 * never lives in cfgRoot, is never persisted, and is read-only: inbound
 * patches and `set fw.* ...` are rejected. */
static bool isFw(const char* key) {
  return strcmp(key, "fw") == 0 || strncmp(key, "fw.", 3) == 0;
}

/* The `storage.db` structured-DB registry (schemas, patterns) is ephemeral,
 * browser-synced (for record decoding) but FIRMWARE-WRITABLE ONLY — a browser
 * patch must not register a store or make storage create files. Gated like
 * fw.*: skipped on inbound merges. */
static bool isStorageDb(const char* key) {
  return strcmp(key, "storage") == 0 || strncmp(key, "storage.", 8) == 0;
}

/** Read entire file into malloc'd buffer (NUL-terminated). Uses fs API.
 *  Optionally reports the byte length (excluding the NUL) via outLen. */
static char* readFileStr(const char* path, size_t* outLen = nullptr) {
  struct stat st;
  if (fs_stat(path, &st) != 0 || st.st_size <= 0) return nullptr;
  int f = fs_open(path, "rb");
  if (f < 0) return nullptr;
  char* buf = (char*)gp_alloc(st.st_size + 1);
  if (!buf) { fs_close(f); return nullptr; }
  fs_read(buf, 1, st.st_size, f);
  buf[st.st_size] = '\0';
  fs_close(f);
  if (outLen) *outLen = (size_t)st.st_size;
  return buf;
}

/* Flush writes go to flash in chunks this size with a tick's sleep between
 * them. Each flash program/erase op disables the PSRAM cache, and a single
 * large fs_write keeps those windows back-to-back for the whole file — every
 * PSRAM-stack task (the storage actor, rnsd, lxmf, the ifaces) is starved for
 * the duration. A multi-hundred-KB conversation external held the actor deaf
 * for 5+ s: every task's sync port-44 write timed out ("owned aux … receiver
 * stuck?") and the browser's 2 s DC liveness tripped. The inter-chunk tick
 * lets PSRAM-stack tasks drain between windows; a 256 KB file costs ~32 extra
 * ticks on the background flush. */
#define STORAGE_FLUSH_CHUNK 8192

/** Atomic write of `data[len]` to `path` via `<path>.new` + rename. */
static bool atomicWriteFile(const char* path, const void* data, size_t len) {
  std::string tmp = std::string(path) + ".new";
  int f = fs_open(tmp.c_str(), "w");
  if (f < 0) return false;
  bool ok = true;
  for (size_t off = 0; off < len; ) {
    size_t n = len - off;
    if (n > STORAGE_FLUSH_CHUNK) n = STORAGE_FLUSH_CHUNK;
    if (fs_write((const char*)data + off, 1, n, f) != n) { ok = false; break; }
    off += n;
    if (off < len) vTaskDelay(1);
  }
  fs_close(f);
  if (ok) fs_rename(tmp.c_str(), path);
  else    fs_remove(tmp.c_str());
  return ok;
}

/* ---- gzip storage files (ROM miniz) ----
 *
 * Storage JSON is persisted gzip-compressed: JSON compresses ~5-10×, and the
 * single fs worker + SD/flash write windows are the system's stall source —
 * a smaller file shortens the blocking write far more than deflate costs on
 * the (background) saving task. Reads accept BOTH <base>.gz and plain <base>
 * (legacy files, hand-placed files, factory seeds); writes only ever produce
 * <base>.gz and remove the plain sibling so the stale format can't shadow
 * newer data on the next read.
 *
 * The ROM one-shot entry points (tdefl/tinfl *_mem_to_mem/_to_heap) are
 * deliberately avoided: they place an ~11 KB tinfl_decompressor on the caller
 * stack, or MZ_MALLOC state from the ROM heap — wrong on our 8 KB PSRAM-stack
 * tasks. State is gp_alloc'd (PSRAM) instead: ~160 KB tdefl_compressor per
 * save, ~11 KB tinfl_decompressor per load, both transient. */

#define GZ_HDR_LEN   10
#define GZ_FOOT_LEN  8
/* Fast-and-good-enough deflate: greedy parsing, 32 dictionary probes. JSON
 * still compresses ~5× here at roughly twice the speed of the 128-probe
 * default; the save worker pays the CPU, the fs worker saves the I/O. */
#define GZ_TDEFL_FLAGS (32 | TDEFL_GREEDY_PARSING_FLAG)

/** Compress `in[inLen]` into a malloc'd gzip stream. NULL on failure. */
static uint8_t* gzDeflate(const void* in, size_t inLen, size_t* outLen) {
  /* Raw-deflate worst case is a hair over input (stored blocks: 5 B/64 KB);
   * inLen/8 + 192 covers it plus header/footer with a wide margin. */
  size_t cap = GZ_HDR_LEN + GZ_FOOT_LEN + inLen + inLen / 8 + 192;
  uint8_t* out = (uint8_t*)gp_alloc(cap);
  if (!out) return nullptr;
  tdefl_compressor* comp = (tdefl_compressor*)gp_alloc(sizeof(tdefl_compressor));
  if (!comp) { free(out); return nullptr; }

  static const uint8_t hdr[GZ_HDR_LEN] =
      { 0x1f, 0x8b, 8, 0, 0, 0, 0, 0, 0, 0xff };  /* deflate, no name, OS=unknown */
  memcpy(out, hdr, GZ_HDR_LEN);

  size_t inSz = inLen, outSz = cap - GZ_HDR_LEN - GZ_FOOT_LEN;
  tdefl_init(comp, nullptr, nullptr, GZ_TDEFL_FLAGS);
  tdefl_status st = tdefl_compress(comp, in, &inSz,
                                   out + GZ_HDR_LEN, &outSz, TDEFL_FINISH);
  free(comp);
  if (st != TDEFL_STATUS_DONE || inSz != inLen) { free(out); return nullptr; }

  uint32_t crc = esp_rom_crc32_le(0, (const uint8_t*)in, inLen);
  uint32_t isz = (uint32_t)inLen;
  memcpy(out + GZ_HDR_LEN + outSz,     &crc, 4);   /* LE target, direct copy */
  memcpy(out + GZ_HDR_LEN + outSz + 4, &isz, 4);
  *outLen = GZ_HDR_LEN + outSz + GZ_FOOT_LEN;
  return out;
}

/** Inflate a gzip stream into a malloc'd NUL-terminated string. NULL on any
 *  malformation (bad magic, truncation, CRC mismatch, size lie). */
static char* gzInflate(const uint8_t* gz, size_t n) {
  if (n < GZ_HDR_LEN + GZ_FOOT_LEN || gz[0] != 0x1f || gz[1] != 0x8b || gz[2] != 8)
    return nullptr;
  uint8_t flg = gz[3];
  size_t off = GZ_HDR_LEN;
  if (flg & 0x04) {                       /* FEXTRA */
    if (off + 2 > n) return nullptr;
    uint16_t xlen; memcpy(&xlen, gz + off, 2);
    off += 2 + xlen;
  }
  if (flg & 0x08) {                       /* FNAME: NUL-terminated */
    while (off < n && gz[off]) off++;
    off++;
  }
  if (flg & 0x10) {                       /* FCOMMENT */
    while (off < n && gz[off]) off++;
    off++;
  }
  if (flg & 0x02) off += 2;               /* FHCRC */
  if (off + GZ_FOOT_LEN > n) return nullptr;

  uint32_t crc, isz;
  memcpy(&crc, gz + n - 8, 4);
  memcpy(&isz, gz + n - 4, 4);
  /* Sanity bound: storage files are hundreds of KB; a multi-MB ISIZE means
   * corruption — don't let a flipped bit demand a 4 GB allocation. */
  if (isz > 8 * 1024 * 1024) return nullptr;

  char* out = (char*)gp_alloc((size_t)isz + 1);
  if (!out) return nullptr;
  tinfl_decompressor* inf = (tinfl_decompressor*)gp_alloc(sizeof(tinfl_decompressor));
  if (!inf) { free(out); return nullptr; }

  size_t inSz = n - off - GZ_FOOT_LEN, outSz = isz;
  tinfl_init(inf);
  tinfl_status st = tinfl_decompress(inf, gz + off, &inSz,
                                     (mz_uint8*)out, (mz_uint8*)out, &outSz,
                                     TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
  free(inf);
  if (st != TINFL_STATUS_DONE || outSz != isz ||
      esp_rom_crc32_le(0, (const uint8_t*)out, outSz) != crc) {
    free(out);
    return nullptr;
  }
  out[outSz] = '\0';
  return out;
}

/** Read a storage JSON file: try `<path>.gz` first, then plain `<path>`.
 *  `path` is always the base .json path — callers never name the .gz. */
static char* readJsonFile(const char* path) {
  std::string gzPath = std::string(path) + ".gz";
  size_t n = 0;
  char* raw = readFileStr(gzPath.c_str(), &n);
  if (raw) {
    char* text = gzInflate((const uint8_t*)raw, n);
    free(raw);
    if (text) return text;
    warn("storage: corrupt %s, trying plain\n", gzPath.c_str());
  }
  return readFileStr(path);
}

/** Write a storage JSON file: gzip to `<path>.gz` (atomic), falling back to
 *  plain `<path>` if compression fails. On success remove the other-format
 *  sibling so a stale copy can't shadow this write on the next read. */
static bool atomicWriteJsonGz(const char* path, const char* text) {
  std::string gzPath = std::string(path) + ".gz";
  size_t gzLen = 0;
  uint8_t* gz = gzDeflate(text, strlen(text), &gzLen);
  if (gz) {
    bool ok = atomicWriteFile(gzPath.c_str(), gz, gzLen);
    free(gz);
    if (ok) { fs_remove(path); return true; }
    return false;
  }
  warn("storage: gzip failed, writing %s uncompressed\n", path);
  if (!atomicWriteFile(path, text, strlen(text))) return false;
  fs_remove(gzPath.c_str());
  return true;
}

/** Resolve a prefix-or-leaf path inside an object. Returns the parent of the
 *  leaf and writes the leaf name to `outLeaf`. Used to (de)attach an
 *  external sub-tree. Returns nullptr if the path is empty/invalid. */
static cJSON* navigateLeaf(cJSON* root, const char* dotPath,
                           char* outLeaf, size_t leafLen) {
  if (!root || !dotPath || !*dotPath) return nullptr;
  cJSON* node = root;
  const char* p = dotPath;
  for (;;) {
    const char* dot = strchr(p, '.');
    size_t segLen = dot ? (size_t)(dot - p) : strlen(p);
    if (segLen == 0) return nullptr;
    if (segLen >= 96) {
      warn("storage: segment too long in key '%s' (%zu B, max 95)",
           dotPath, segLen);
      return nullptr;
    }
    if (!dot) {
      memcpy(outLeaf, p, segLen);
      outLeaf[segLen] = '\0';
      return node;
    }
    char seg[96];  /* see navigatePath */
    memcpy(seg, p, segLen);
    seg[segLen] = '\0';
    node = cJSON_GetObjectItem(node, seg);
    if (!node) return nullptr;
    p = dot + 1;
  }
}

/** Detach all external sub-trees from cfgRoot, run `fn`, then reattach.
 *  Used so the root.json snapshot omits external blobs without copying
 *  the whole tree first. Caller holds CFG_LOCK. */
static void withExternalsDetached(std::function<void()> fn) {
  struct save_t { cJSON* parent; std::string leaf; cJSON* item; };
  std::vector<save_t> saved;
  for (auto& ext : externals) {
    /* A pendingDelete external is no longer a real external — whatever still
     * lives at its prefix must persist via root.json (this is what lets the
     * lxmf monolith split move contacts/identity into root.json while the
     * conversation subtrees leave through their own — detached — externals).
     * For storageDeleteTree-marked entries the subtree is already gone from
     * cfgRoot, so skipping is a no-op there. */
    if (ext.pendingDelete) continue;
    char leaf[96];  /* see navigatePath */
    cJSON* parent = navigateLeaf(cfgRoot, ext.prefix.c_str(), leaf, sizeof(leaf));
    if (!parent) continue;
    cJSON* item = cJSON_DetachItemFromObject(parent, leaf);
    if (item) saved.push_back({parent, leaf, item});
  }
  fn();
  /* Reattach in reverse order — preserves original child positions
   * if multiple externals share a parent. */
  for (auto it = saved.rbegin(); it != saved.rend(); ++it)
    cJSON_AddItemToObject(it->parent, it->leaf.c_str(), it->item);
}

/** Serialize one external's sub-tree at its prefix to its own file.
 *
 *  Only the cJSON_Duplicate snapshot holds CFG_LOCK; serialization runs
 *  lock-free (the dump-builder shape). Printing a large conversation external
 *  under the lock blocked the actor's storageApplyOps — and with it the
 *  port-44 op drain and the browser ping→pong — for the whole serialize.
 *  Unformatted: nothing on-device reads these files by eye, and the compact
 *  form shrinks the flash write (fewer PSRAM-cache-disable windows). */
static void writeExternalFile(const external_t& ext) {
  int64_t t0 = esp_timer_get_time();
  CFG_LOCK();
  cJSON* node = navigatePath(cfgRoot, ext.prefix.c_str());
  cJSON* snap = node ? cJSON_Duplicate(node, true) : nullptr;
  CFG_UNLOCK();
  int64_t held = esp_timer_get_time() - t0;
  if (held > CFG_HOLD_WARN_US)
    warn("CFG_LOCK hold %lldms: dup external %s\n", held / 1000, ext.prefix.c_str());
  if (!snap) return;
  char* text = cJSON_PrintUnformatted(snap);
  cJSON_Delete(snap);
  if (!text) return;
  atomicWriteJsonGz(ext.path.c_str(), text);
  cJSON_free(text);
}

/* The non-external config tree lives at <stateDir>/storage/root.json,
 * alongside the per-prefix blobs in <stateDir>/storage/external/ — so
 * everything persisted is under storage/. */
#define ROOT_JSON_PATH "/storage/root.json"

/** Write root.json (cfgRoot minus external sub-trees).
 *  Same lock discipline as writeExternalFile: snapshot under CFG_LOCK,
 *  serialize lock-free, compact form. */
static void writeSettingsFileOnly() {
  cJSON* out = cJSON_CreateObject();
  if (!out) return;
  int64_t t0 = esp_timer_get_time();
  CFG_LOCK();
  withExternalsDetached([&]() {
    cJSON* s = cJSON_GetObjectItem(cfgRoot, "s");
    cJSON* sec = cJSON_GetObjectItem(cfgRoot, "secrets");
    if (s)   cJSON_AddItemToObject(out, "s",       cJSON_Duplicate(s, true));
    if (sec) cJSON_AddItemToObject(out, "secrets", cJSON_Duplicate(sec, true));
  });
  CFG_UNLOCK();
  int64_t held = esp_timer_get_time() - t0;
  if (held > CFG_HOLD_WARN_US)
    warn("CFG_LOCK hold %lldms: dup root.json\n", held / 1000);
  char* text = cJSON_PrintUnformatted(out);
  cJSON_Delete(out);
  if (!text) return;
  atomicWriteJsonGz(fsStatePath(ROOT_JSON_PATH).c_str(), text);
  cJSON_free(text);
}

static void writeSettingsFile() {
  /* Order is load-bearing for crash safety: write dirty externals, then
     root.json, and only THEN remove pendingDelete files — data must never be
     deleted before its replacement is durable. The lxmf monolith split leans
     on this: its conversations flush to per-conv files, its remainder to
     root.json (withExternalsDetached skips pendingDelete), and the monolith
     file is removed last; a crash anywhere in between leaves the monolith on
     disk and the split simply re-runs next boot (per-conv files, being the
     deeper prefixes, load after it and win).

     Index-walk re-checking size() each step so a concurrent storageNewTreeFile
     push_back can't invalidate us; fs I/O stays OUTSIDE CFG_LOCK (writeExternalFile
     snapshots under the lock then writes lock-free), matching prior behaviour. */
  /* Clear the pending flag up front (not at the end): a write that dirties state
     while this flush is in flight then re-arms the timer via startSaveTimer, so
     its change flushes next window instead of waiting for an unrelated later
     write to re-arm. */
  savePending = false;
  for (size_t i = 0; ; ) {
    bool doWrite = false;
    external_t work;
    CFG_LOCK();
    if (i >= externals.size()) { CFG_UNLOCK(); break; }
    external_t& ext = externals[i];
    if (ext.dirty && !ext.pendingDelete) {
      ext.dirty = false;
      work.prefix = ext.prefix;
      work.path   = ext.path;
      doWrite = true;
    }
    i++;
    CFG_UNLOCK();
    if (doWrite) writeExternalFile(work);
  }

  if (rootDirty) {
    writeSettingsFileOnly();
    rootDirty = false;
  }

  /* Structured-DB instances: independent per-conversation files, same
   * snapshot-under-lock / write-lock-free discipline as externals. */
  storageDbFlushDirty();

  /* Deletes last (see above). Erase re-checks from index 0 each round since
     the erase shifts the vector. */
  for (;;) {
    external_t work;
    bool doDelete = false;
    CFG_LOCK();
    for (size_t i = 0; i < externals.size(); i++) {
      if (externals[i].pendingDelete) {
        work.path = externals[i].path;
        externals.erase(externals.begin() + i);
        doDelete = true;
        break;
      }
    }
    CFG_UNLOCK();
    if (!doDelete) break;
    fs_remove(work.path.c_str());
    fs_remove((work.path + ".gz").c_str());
  }
  /* savePending was cleared at the top so mid-flush changes re-arm the timer. */
}

/* Persist worker loop: block until poked, then flush. ulTaskNotifyTake(pdTRUE)
 * coalesces any pokes that arrived during a flush into one extra pass. Not an
 * ITS task — blocking on fs I/O here harms nothing. */
static void saveWorkerFn(void*) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    writeSettingsFile();
    /* Release any save-now semaphores queued by SAVE ops while we flushed. */
    CFG_LOCK();
    std::vector<SemaphoreHandle_t> sems = std::move(pendingSaveSems);
    pendingSaveSems.clear();
    CFG_UNLOCK();
    for (auto s : sems) if (s) xSemaphoreGive(s);
  }
}

static void requestSave() {
  if (saveWorkerHandle) xTaskNotifyGive(saveWorkerHandle);
}

static void saveTimerCb(void* /*arg*/) {
  requestSave();
}

static void startSaveTimer() {
  if (!saveTimer) {
    esp_timer_create_args_t args = {};
    args.callback = saveTimerCb;
    args.name = "storage_save";
    esp_timer_create(&args, &saveTimer);
  }
  /* Arm ONCE: the first unsaved change starts the clock; later changes ride the
     same window and must NOT reset it. Resetting on every write (the old
     behaviour) starves the flush on a busy device — background traffic (rnsd
     link stats, the announce catalogue, lxmf stats, per-message directory
     bumps) writes more often than flash_delay, so the deadline was pushed out
     forever and nothing ever persisted despite ample wall-clock time. Arm-once
     guarantees a flush within flash_delay of the first dirty write while still
     coalescing the whole window into one rewrite. `savePending` is cleared at
     the start of writeSettingsFile, so a change during a flush re-arms. */
  if (savePending) return;
  esp_timer_stop(saveTimer);   /* clear any stale arm left by a direct requestSave; harmless if idle */
  int delaySec = storageGetInt("s.storage.flash_delay", 60);
  if (delaySec < 1) delaySec = 1;
  esp_timer_start_once(saveTimer, (int64_t)delaySec * 1000000);
  savePending = true;
}

/* ---- Config change subscriptions ---- */

#define STORAGE_MAX_SUBS     128
/* STORAGE_CHANGE_PORT in storage.h */

struct storage_sub_t {
  TaskHandle_t        task;
  storage_change_cb_t cb;
  std::string         scope;   /* unbounded — lxmf's 64-hex segments fit now */
};

/* The subscription table is OWNED BY THE STORAGE ACTOR: it is only ever mutated
 * on the storage task (via SUB/UNSUB ops) or at boot before the task spawns, so
 * the old unguarded subCount++ race is gone by construction. */
static storage_sub_t subs[STORAGE_MAX_SUBS];
static int           subCount = 0;

/* ========================================================================
 * The storage actor
 *
 * All config WRITES funnel through a serialized op list applied by
 * storageApplyOps on the storage task; READS stay direct under cfgMux. A write
 * either applies directly (FAST PATH, D1: the caller IS the storage task, or
 * storage hasn't spawned yet) or is sent to the storage task as one aux message
 * and the caller blocks until applied (sync write — read-your-writes holds).
 * The message boundary is the transaction: a whole op list applies atomically.
 *
 * Op format (one heap block, freeable with a single free()):
 *   [u8 flags][op]*          flags bit0 = SILENT (suppress subscriptions)
 *   'S' SET     key\0 vtype value      vtype 'I': int32 LE
 *   'D' DELETE  key\0                        'S': u32 len + bytes (string)
 *   'd' DEFAULT key\0 vtype value            'J': u32 len + printed JSON (subtree)
 *   '+' SUB     scope\0 cb(void*)
 *   '-' UNSUB   scope\0 cb(void*)      cb NULL = all of sender's subs on scope
 *   'W' SAVE    sem(SemaphoreHandle_t)
 * Keys/scopes are NUL-terminated and unbounded. The whole list is validated
 * before anything is applied; a malformed list is rejected whole.
 * ===================================================================== */

#define STORAGE_OP_PORT 44    /* aux port on the storage task carrying op lists */
#define OP_F_SILENT     0x01

/* ---- op encoding (append to a std::string buffer) ---- */
static inline void opPutStr(std::string& b, const char* s) { b.append(s ? s : ""); b.push_back('\0'); }
static inline void opPutU32(std::string& b, uint32_t v) { b.append((const char*)&v, sizeof(v)); }
static inline void opPutPtr(std::string& b, const void* p) { b.append((const char*)&p, sizeof(p)); }

/* opcode is 'S' (SET) or 'd' (DEFAULT). */
static void opAppendKV(std::string& b, char opcode, const char* key, const char* val) {
  b.push_back(opcode); opPutStr(b, key);
  if (inferType(val) == CFG_INT) {
    b.push_back('I'); int32_t v = atoi(val); b.append((const char*)&v, 4);
  } else {
    uint32_t n = (uint32_t)strlen(val); b.push_back('S'); opPutU32(b, n); b.append(val, n);
  }
}
static void opAppendKVInt(std::string& b, char opcode, const char* key, int val) {
  b.push_back(opcode); opPutStr(b, key); b.push_back('I'); int32_t v = val; b.append((const char*)&v, 4);
}
static void opAppendKVJson(std::string& b, char opcode, const char* key, const char* json, size_t jlen) {
  b.push_back(opcode); opPutStr(b, key); b.push_back('J'); opPutU32(b, (uint32_t)jlen); b.append(json, jlen);
}
static void opAppendDelete(std::string& b, const char* key) { b.push_back('D'); opPutStr(b, key); }

/* ---- per-task bracket accumulator (storageBegin/End sugar, D4) ---- */
struct op_accum_t { TaskHandle_t task; std::string ops; int depth; bool silent; volatile bool active; };
#define STORAGE_MAX_ACCUM 24
static op_accum_t   accums[STORAGE_MAX_ACCUM];
static portMUX_TYPE accumMux = portMUX_INITIALIZER_UNLOCKED;

static op_accum_t* accumFind(TaskHandle_t t) {
  for (int i = 0; i < STORAGE_MAX_ACCUM; i++)
    if (accums[i].active && accums[i].task == t) return &accums[i];
  return nullptr;
}
static op_accum_t* accumFindOrCreate(TaskHandle_t t) {
  op_accum_t* e = accumFind(t);
  if (e) return e;
  portENTER_CRITICAL(&accumMux);
  e = accumFind(t);
  if (!e) for (int i = 0; i < STORAGE_MAX_ACCUM; i++) {
    if (!accums[i].active) { accums[i].task = t; accums[i].depth = 0; accums[i].silent = false;
                             accums[i].active = true; e = &accums[i]; break; }
  }
  portEXIT_CRITICAL(&accumMux);
  return e;
}

/* ---- subscription table mutation (storage-task-owned; under CFG_LOCK) ---- */
static void subAdd(TaskHandle_t task, storage_change_cb_t cb, const char* scope) {
  const char* sc = scope ? scope : "";
  /* Idempotent: re-subscribing the same (task, scope, cb) is a no-op, not a new
   * row. A captureless ON_CHANGE lambda has a stable function pointer per site,
   * so this triple identifies one subscription exactly. Without this, any caller
   * that re-runs a subscribe on a repeating event — a program layer rebuilt
   * after eviction, a handler re-entered on wake — appends a duplicate every
   * time until the table fills and real subscriptions are silently dropped
   * ("subscription table full"). Generalises the hand-rolled !have guard in
   * lcd_settings.cpp so no caller has to remember it. */
  for (int i = 0; i < subCount; i++)
    if (subs[i].task == task && subs[i].cb == cb && subs[i].scope == sc) return;
  if (subCount >= STORAGE_MAX_SUBS) { warn("storage: subscription table full\n"); return; }
  subs[subCount].task = task; subs[subCount].cb = cb; subs[subCount].scope = sc;
  subCount++;
}
static void subRemove(TaskHandle_t task, storage_change_cb_t cb, const char* scope) {
  for (int i = 0; i < subCount; ) {
    bool match = subs[i].task == task &&
                 (scope == nullptr || subs[i].scope == scope) &&
                 (cb == nullptr || subs[i].cb == cb);
    if (match) subs[i] = subs[--subCount];   /* swap-with-last; scope is a std::string move */
    else i++;
  }
}

/* Task-death cleanup. Called from ITS's vTaskPreDeletionHook (the one global
 * task-deletion hook) in a scheduler critical section — so NO locks/alloc/
 * logging, and no concurrent subAdd/subRemove (scheduler suspended). Null the
 * owner handle of any subscription on the dead task so notifyChange stops trying
 * to deliver to a freed TCB: a notify into a dead handle is a UAF, and reading
 * its name for the warn() reads freed memory ("notify drop → [garbage]"). The
 * slot + its scope string stay allocated but inert (compacted by a later matching
 * subRemove), mirroring the ITS s_tasks table's append-only discipline. Canonical
 * case: a module that subscribed from main_task during init, which then
 * self-deletes when app_main returns. */
extern "C" void storageOnTaskDeath(void* tcb) {
  TaskHandle_t dead = (TaskHandle_t)tcb;
  for (int i = 0; i < subCount; i++)
    if (subs[i].task == dead) subs[i].task = nullptr;
}

/* Collect every changed leaf in the patch as {key, printed-value} (deletes →
 * empty value), replacing the inline firePatchSubscriptions walk. */
static void collectChanges(cJSON* node, char* path, size_t pathSize, size_t pathLen,
                           std::vector<std::pair<std::string,std::string>>& out) {
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

    if (cJSON_IsNull(item))        out.emplace_back(path, "");
    else if (cJSON_IsObject(item)) collectChanges(item, path, pathSize, newLen, out);
    else if (cJSON_IsArray(item))  out.emplace_back(path, "");
    else if (cJSON_IsString(item)) out.emplace_back(path, item->valuestring);
    else if (cJSON_IsNumber(item)) { char vb[32]; snprintf(vb, sizeof(vb), "%d", item->valueint); out.emplace_back(path, vb); }

    path[pathLen] = '\0';
  }
}

/* Sanitized, length-capped preview of a value for the serial log. Storage
 * values can be arbitrary attacker-influenced bytes (e.g. a fetched NomadNet
 * page in nomad.page.body) carrying raw control/escape sequences. Echoing
 * those verbatim lets a remote page drive the operator's terminal — a query
 * sequence (cursor/DSR) makes the terminal reply, and that reply lands on the
 * CLI prompt. Fold C0 controls + DEL to '.' (matches nomad's logPage) and cap
 * the length so a multi-KB page can't flood the log. UTF-8 (>=0x80) is kept. */
static std::string logSafe(const char* s, size_t cap = 96) {
  std::string r;
  for (size_t i = 0; s[i] && i < cap; i++) {
    uint8_t c = (uint8_t)s[i];
    r += (c < 0x20 || c == 0x7f) ? '.' : (char)c;
  }
  if (s[r.size()]) r += "…";
  return r;
}

/* Deliver one change to every matching subscriber. The storage task's own subs
 * (the "" browser-sync) are invoked DIRECTLY — no self-send, and since the DC
 * handler re-reads by key the value isn't copied (D6). Remote subscribers get a
 * variable-length CHANGED message {cb, key\0, val\0}, bounded send (D2). */
/* Cross-task notifies carry at most this much of the VALUE. Notifications
 * are change SIGNALS, not value transport: a handler that needs the full
 * value re-reads storage by key (they all do for large values). Without the
 * cap, a big write (a Nomad page body) made the notify exceed subscriber
 * inbox guards and got dropped ("notify drop ... → [lcd]") — and copying
 * kilobytes per subscriber per write is waste even when it fits. Same-task
 * subscribers (the storage task's own, called directly) still see the full
 * value. */
#define STORAGE_NOTIFY_VAL_MAX 512

/* One built CHANGED message awaiting delivery to `task` on STORAGE_CHANGE_PORT.
 * `buf` is a gp_alloc block owned by the queue until the worker sends it (ITS
 * adopts it on a successful send; on failure the worker frees it). */
struct notify_job_t { TaskHandle_t task; uint8_t* buf; size_t n; };
static std::deque<notify_job_t> notifyQueue;
static SemaphoreHandle_t         notifyQueueMux = nullptr;

/* Bound the backlog: a permanently-stuck subscriber drains at ~1 per
 * STORAGE_NOTIFY_TIMEOUT_MS, so a sustained change flood could grow the queue
 * without limit. Past the cap, drop the OLDEST pending notify (free its buffer).
 * A CHANGED message is a signal, not value transport — every handler re-reads by
 * key — so a coalesced drop only costs a redundant re-read, never a lost value. */
#define STORAGE_NOTIFY_QUEUE_MAX 256
static uint32_t notifyDropped = 0;

/* Send one built CHANGED buffer to `task`, blocking up to the notify timeout on
 * its inbox. Frees `buf` on failure (ITS owns it on success). */
static void notifySend(TaskHandle_t task, uint8_t* buf, size_t n) {
  if (!itsSendAuxOwnedByTaskHandle(task, STORAGE_CHANGE_PORT, buf, n,
                                   pdMS_TO_TICKS(CONFIG_SPANGAP_STORAGE_NOTIFY_TIMEOUT_MS))) {
    /* buf layout {cb ptr, key\0, val\0}; recover the key for the log before we
     * free the block (the send did not adopt it). */
    const char* key = (n > sizeof(void*)) ? (const char*)(buf + sizeof(void*)) : "?";
    const char* tn = pcTaskGetName(task);
    warn("notify drop: %s → [%s]\n", logSafe(key).c_str(), tn ? tn : "?");
    free(buf);
  }
}

/* Hand a built CHANGED buffer to the dispatch worker (called on the storage
 * actor). Takes ownership of `buf`. Before the worker spawns (early boot) or if
 * the queue mux is absent, send inline — same blocking behaviour as before. */
static void notifyEnqueue(TaskHandle_t task, uint8_t* buf, size_t n) {
  if (!notifyWorkerHandle || !notifyQueueMux) { notifySend(task, buf, n); return; }
  uint8_t* stale = nullptr;
  xSemaphoreTake(notifyQueueMux, portMAX_DELAY);
  if (notifyQueue.size() >= STORAGE_NOTIFY_QUEUE_MAX) {
    stale = notifyQueue.front().buf; notifyQueue.pop_front(); notifyDropped++;
  }
  notifyQueue.push_back({task, buf, n});
  xSemaphoreGive(notifyQueueMux);
  if (stale) {
    free(stale);
    if (notifyDropped % 64 == 1)
      warn("notify backlog full, dropping oldest (total %u)\n", (unsigned)notifyDropped);
  }
  xTaskNotifyGive(notifyWorkerHandle);
}

/* Dispatch worker loop: drain the queue and do the blocking sends here, off the
 * storage actor's poll loop. ulTaskNotifyTake(pdTRUE) coalesces pokes. */
static void notifyWorkerFn(void*) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    for (;;) {
      notify_job_t job;
      xSemaphoreTake(notifyQueueMux, portMAX_DELAY);
      if (notifyQueue.empty()) { xSemaphoreGive(notifyQueueMux); break; }
      job = notifyQueue.front();
      notifyQueue.pop_front();
      xSemaphoreGive(notifyQueueMux);
      notifySend(job.task, job.buf, job.n);
    }
  }
}

static void notifyChange(const char* key, const char* val) {
  for (int i = 0; i < subCount; i++) {
    if (!subs[i].task) continue;   /* owner died — nulled by storageOnTaskDeath */
    size_t sl = subs[i].scope.size();
    if (sl != 0 && strncmp(key, subs[i].scope.c_str(), sl) != 0) continue;
    if (subs[i].task == storageHandle) {
      if (subs[i].cb) subs[i].cb(key, val);
      continue;
    }
    size_t klen = strlen(key), vlen = strlen(val);
    if (vlen > STORAGE_NOTIFY_VAL_MAX) vlen = STORAGE_NOTIFY_VAL_MAX;
    size_t n = sizeof(void*) + klen + 1 + vlen + 1;
    uint8_t* buf = (uint8_t*)gp_alloc(n);
    if (!buf) continue;
    void* cb = (void*)subs[i].cb;
    memcpy(buf, &cb, sizeof(cb));
    memcpy(buf + sizeof(cb), key, klen + 1);
    memcpy(buf + sizeof(cb) + klen + 1, val, vlen);   /* may be truncated */
    buf[sizeof(cb) + klen + 1 + vlen] = '\0';
    /* Hand off to the dispatch worker: the blocking send must not run on the
     * actor (see notifyWorkerHandle). Ownership of buf transfers to the queue. */
    notifyEnqueue(subs[i].task, buf, n);
  }
}

/* ======================================================================
 * Structured-DB routing (storage_db.h; plans/storage-structured-db.md)
 *
 * A registered store claims a dot-path pattern with `$` wildcards
 * ("s.lxmf.id.$.msgs.$"). A key that matches, with a two-segment
 * <record>.<field> tail, is served from the packed record store instead of
 * cfgRoot: reads resolve against it (this task or a reader, under CFG_LOCK),
 * writes are applied in the actor's pass 2, and both synthesize the SAME
 * dot-path change events a cfgRoot write would — so the LCD subscription and
 * (later) the browser keep updating. Message bodies never enter cfgRoot, so
 * they cost nothing in the whole-tree dup/dump/save/mirror paths.
 *
 * All store access is serialized by CFG_LOCK (the same recursive mutex that
 * guards cfgRoot): the resident block only mutates with the lock held, so a
 * reader holding it is safe from the actor's writes and from block relocation.
 * ==================================================================== */

struct sdb_reg {
  std::string              name;
  std::vector<std::string> pat;        /* pattern segments; "$" = wildcard bind */
  std::string              litPrefix;  /* pattern up to the first wildcard + '.'; a
                                        * cheap strncmp reject so ordinary config reads
                                        * never pay the splitDots allocation */
  size_t                   patLen = 0;
  std::string              filePat;    /* relative path pattern w/ $1..; "" = ephemeral */
  const sdb_schema*        schema = nullptr;
  bool                     persist = false;
  storage_db_evict         evict = STORAGE_DB_RELOAD;
  uint32_t                 cap = 0;
  bool                     browserMirror = false;   /* mirror every change to the browser (directory/
                                                      * catalogue), not just while the instance is open */
  std::unordered_map<std::string, sdb_store*> insts;  /* joined-binds -> instance */
};
static std::vector<sdb_reg*> g_sdbRegs;
/* Instance files to unlink, drained on the persist worker — fs I/O must never
 * run on the storage actor's poll loop. */
static std::vector<std::string> g_sdbPendingDeletes;

static std::vector<std::string> splitDots(const char* key) {
  std::vector<std::string> out;
  const char* p = key;
  while (*p) {
    const char* dot = strchr(p, '.');
    if (!dot) { out.emplace_back(p); break; }
    out.emplace_back(p, (size_t)(dot - p));
    p = dot + 1;
  }
  if (*key && key[strlen(key) - 1] == '.') out.emplace_back("");
  return out;
}

/* Substitute $1..$9 in a file pattern with the bound wildcard values. */
static std::string sdbSubst(const std::string& pat, const std::vector<std::string>& binds) {
  std::string out;
  for (size_t i = 0; i < pat.size(); i++) {
    if (pat[i] == '$' && i + 1 < pat.size() && pat[i + 1] >= '1' && pat[i + 1] <= '9') {
      size_t idx = (size_t)(pat[i + 1] - '1');
      if (idx < binds.size()) out += binds[idx];
      i++;
    } else out += pat[i];
  }
  return out;
}

static std::string joinBinds(const std::vector<std::string>& b) {
  std::string k;
  for (size_t i = 0; i < b.size(); i++) { if (i) k += '\x1f'; k += b[i]; }
  return k;
}

struct sdb_match {
  sdb_reg*                 reg = nullptr;
  std::vector<std::string> binds;
  std::vector<std::string> tail;   /* segments after the matched pattern (0=instance,1=record,2=field) */
};

/* Match a full key against the registry. Returns false (no match) fast when
 * there are no registrations or the key is too short / literal segs differ. */
static bool sdbRoute(const char* key, sdb_match& m) {
  if (g_sdbRegs.empty()) return false;
  /* Fast reject before the splitDots allocation: an ordinary config read only
   * pays a couple of strncmp against each store's literal prefix. Compare over
   * min(key, litPrefix) length: a key that IS the bare instance prefix (a
   * zero-wildcard store queried at exactly its pattern, e.g. storageForEach over
   * "lxmf.announces") is one char shorter than litPrefix's trailing dot and must
   * still be a candidate. Over-matching is harmless — the full segment compare
   * below rejects any real mismatch. */
  size_t klen = strlen(key);
  bool cand = false;
  for (auto* r : g_sdbRegs) {
    if (r->litPrefix.empty()) { cand = true; break; }
    size_t n = klen < r->litPrefix.size() ? klen : r->litPrefix.size();
    if (strncmp(key, r->litPrefix.c_str(), n) == 0) { cand = true; break; }
  }
  if (!cand) return false;
  std::vector<std::string> segs = splitDots(key);
  for (auto* r : g_sdbRegs) {
    if (segs.size() < r->patLen) continue;
    bool ok = true;
    std::vector<std::string> binds;
    for (size_t i = 0; i < r->patLen; i++) {
      if (r->pat[i] == "$") binds.push_back(segs[i]);
      else if (r->pat[i] != segs[i]) { ok = false; break; }
    }
    if (!ok) continue;
    m.reg = r;
    m.binds = std::move(binds);
    m.tail.assign(segs.begin() + r->patLen, segs.end());
    return true;
  }
  return false;
}

/* True if a write/read to this key belongs to a store (so cfgRoot / browser JSON
 * mirror must not also handle it). */
static bool sdbRoutes(const char* key) { sdb_match m; return sdbRoute(key, m); }

static void mkdirpParent(const std::string& filePath) {
  size_t slash = filePath.find_last_of('/');
  if (slash != std::string::npos) fs_mkdirp(filePath.substr(0, slash).c_str());
}

/* Find or create the instance for these binds. Caller holds CFG_LOCK. */
static sdb_store* sdbInstance(sdb_reg* r, const std::vector<std::string>& binds) {
  std::string ik = joinBinds(binds);
  auto it = r->insts.find(ik);
  if (it != r->insts.end()) return it->second;
  sdb_store* st = new sdb_store();
  st->schema = r->schema;
  st->cap_records = (r->evict == STORAGE_DB_DROP) ? r->cap : 0;
  if (r->persist) {
    st->path = fsStatePath(("/" + sdbSubst(r->filePat, binds)).c_str());
    mkdirpParent(st->path);
  }
  r->insts[ik] = st;
  return st;
}

/* Resolve + load the instance addressed by a match's binds. Caller holds CFG_LOCK. */
static sdb_store* sdbResolveLoaded(sdb_match& m) {
  sdb_store* st = sdbInstance(m.reg, m.binds);
  if (!st->loaded) sdbLoad(st);
  return st;
}

/* ---- read side (caller holds CFG_LOCK) ---- */

/* Resolve a routed key's value. Returns false if not routed / record or field
 * absent. Caller holds CFG_LOCK. */
static bool sdbGetLocked(const char* key, std::string& out) {
  sdb_match m;
  if (!sdbRoute(key, m)) return false;
  if (m.tail.size() != 2) return false;            /* only a record.field leaf has a value */
  if (!m.reg->schema->find(m.tail[1].c_str())) return false;  /* unknown field (e.g. peer) */
  sdb_store* st = sdbResolveLoaded(m);
  return sdbGetField(st, m.tail[0].c_str(), m.tail[1].c_str(), out);
}

/* Existence of a routed leaf/record. Caller holds CFG_LOCK. */
static bool sdbExistsLocked(const char* key, bool& exists) {
  sdb_match m;
  if (!sdbRoute(key, m)) return false;
  sdb_store* st = sdbResolveLoaded(m);
  if (m.tail.size() == 2) {
    if (!m.reg->schema->find(m.tail[1].c_str())) { exists = false; return true; }
    std::string v; exists = sdbGetField(st, m.tail[0].c_str(), m.tail[1].c_str(), v);
  } else if (m.tail.size() == 1) {
    exists = sdbHasRecord(st, m.tail[0].c_str());
  } else exists = false;
  return true;
}

/* ---- iterate a subtree that spans one or more instances (caller holds CFG_LOCK) ----
 * Handles storageForEach over a store prefix: a full instance prefix (tail 0),
 * or a shorter prefix that leaves the last wildcard unbound (all conversations
 * of an identity). The latter enumerates instances from disk + resident set. */
static void sdbForEachInstance(sdb_store* st, const std::string& fullPrefix,
                               void (*cb)(const char* key, const char* val), std::vector<std::string>& out) {
  struct ctx_t { const std::string* pre; std::vector<std::string>* out; } c{ &fullPrefix, &out };
  sdbForEach(st, [](const char* rec, const char* field, const char* val, void* vp) {
    auto* c = (ctx_t*)vp;
    std::string k = *c->pre + "." + rec + "." + field;
    c->out->push_back(k);
    c->out->push_back(val);
  }, &c);
  (void)cb;
}

/* Returns true if the prefix was handled by a store (values collected into
 * `out` as alternating key,val). Caller holds CFG_LOCK. */
static bool sdbForEachUnderLocked(const char* prefix, std::vector<std::string>& out) {
  if (g_sdbRegs.empty()) return false;
  std::vector<std::string> pre = splitDots(prefix);
  for (auto* r : g_sdbRegs) {
    /* Case A: prefix is at/below a full instance (>= patLen segments). */
    if (pre.size() >= r->patLen) {
      sdb_match m;
      if (!sdbRoute(prefix, m) || m.reg != r) continue;
      sdb_store* st = sdbResolveLoaded(m);
      /* the instance's full dot-prefix is the first patLen key segments */
      std::string instPrefix;
      for (size_t i = 0; i < r->patLen; i++) { if (i) instPrefix += '.'; instPrefix += pre[i]; }
      sdbForEachInstance(st, instPrefix, nullptr, out);
      return true;
    }
    /* Case B: prefix matches a leading portion, last wildcard unbound —
     * enumerate every instance (disk + resident) consistent with the binds. */
    if (pre.size() < r->patLen) {
      bool ok = true;
      std::vector<std::string> binds;
      for (size_t i = 0; i < pre.size(); i++) {
        if (r->pat[i] == "$") binds.push_back(pre[i]);
        else if (r->pat[i] != pre[i]) { ok = false; break; }
      }
      if (!ok) continue;
      /* Only the simple case is supported: exactly one trailing unbound wildcard
       * that is the file stem (covers "s.lxmf.id.N.msgs" over per-peer files). */
      if (pre.size() != r->patLen - 1 || r->pat[r->patLen - 1] != "$") continue;
      std::set<std::string> stems;   /* the unbound $ values */
      /* resident instances */
      for (auto& kv : r->insts) {
        /* instance key = joinBinds; the last bind is the unbound stem */
        std::vector<std::string> ib;
        size_t s = 0;
        for (size_t i = 0; i <= kv.first.size(); i++)
          if (i == kv.first.size() || kv.first[i] == '\x1f') { ib.emplace_back(kv.first.substr(s, i - s)); s = i + 1; }
        if (ib.size() != binds.size() + 1) continue;
        bool consistent = true;
        for (size_t i = 0; i < binds.size(); i++) if (ib[i] != binds[i]) { consistent = false; break; }
        if (consistent) stems.insert(ib.back());
      }
      /* disk files */
      if (r->persist) {
        std::string dir = fsStatePath(("/" + sdbSubst(r->filePat, binds)).c_str());
        size_t slash = dir.find_last_of('/');
        if (slash != std::string::npos) dir = dir.substr(0, slash);
        int dh = fs_opendir(dir.c_str());
        if (dh >= 0) {
          fs_dirent_t ent;
          while (fs_readdir(dh, &ent)) {
            size_t nl = strlen(ent.name);
            if (nl > 6 && strcmp(ent.name + nl - 6, ".db.gz") == 0)
              stems.insert(std::string(ent.name, nl - 6));
          }
          fs_closedir(dh);
        }
      }
      for (const std::string& stem : stems) {
        std::vector<std::string> ib = binds; ib.push_back(stem);
        sdb_store* st = sdbInstance(r, ib);
        if (!st->loaded) sdbLoad(st);
        std::string instPrefix(prefix); instPrefix += '.'; instPrefix += stem;
        sdbForEachInstance(st, instPrefix, nullptr, out);
      }
      return true;
    }
  }
  return false;
}

/* ---- write side (runs in the actor's pass 2, under CFG_LOCK) ----
 * Apply one routed op to its store and record the synthesized change (for the
 * post-unlock notify fan-out). Returns true if the op was routed (and must not
 * enter the cfgRoot patch). `vs` is the op's value as a string ("" for delete). */
static bool sdbApplyOpLocked(char op, const char* key, const std::string& vs, bool silent,
                             std::vector<std::pair<std::string,std::string>>& changes,
                             bool& routedDirty) {
  sdb_match m;
  if (!sdbRoute(key, m)) return false;

  if (m.tail.size() == 2) {                     /* <record>.<field> */
    const sdb_field* fd = m.reg->schema->find(m.tail[1].c_str());
    if (!fd) return true;                        /* swallow unknown field (e.g. redundant peer) */
    sdb_store* st = sdbResolveLoaded(m);
    const char* rec = m.tail[0].c_str();
    const char* field = m.tail[1].c_str();
    if (op == 'D') { return true; }              /* single-field delete: unused, swallow */
    std::string cur;
    bool have = sdbGetField(st, rec, field, cur);
    if (op == 'd' && have && !cur.empty()) return true;   /* DEFAULT: keep existing */
    if (have && cur == vs) return true;          /* dedup (load-bearing vs notify floods) */
    sdbSetField(st, rec, field, vs.c_str());
    routedDirty = true;
    if (!silent) changes.emplace_back(key, vs);
    return true;
  }

  if (m.tail.size() == 1) {                      /* whole record */
    sdb_store* st = sdbResolveLoaded(m);
    if (op == 'D') {
      sdbDeleteRecord(st, m.tail[0].c_str());
      routedDirty = true;
      if (!silent) changes.emplace_back(key, "");   /* record subtree gone */
    }
    return true;
  }

  /* tail 0: whole instance. A DELETE drops the instance; handled by
   * storageDbDropInstance via markExternals path below, so just swallow SETs. */
  if (op == 'D') {
    sdb_store* st = sdbInstance(m.reg, m.binds);
    if (st->loaded) sdbEvict(st);
    if (!st->path.empty()) g_sdbPendingDeletes.push_back(st->path);  /* unlink on the worker */
    delete st;
    m.reg->insts.erase(joinBinds(m.binds));
    routedDirty = true;
    if (!silent) changes.emplace_back(key, "");
  }
  return true;
}

/* Flush every dirty resident instance. Snapshot the raw block under CFG_LOCK,
 * then deflate + write lock-free (the writeExternalFile discipline). Runs on the
 * persist worker. */
static void storageDbFlushDirty() {
  struct job_t { std::string path; uint8_t* raw; size_t len; };
  std::vector<job_t> jobs;
  std::vector<std::string> deletes;
  CFG_LOCK();
  for (auto* r : g_sdbRegs) {
    if (!r->persist) continue;
    for (auto& kv : r->insts) {
      sdb_store* st = kv.second;
      if (st->loaded && st->dirty && !st->path.empty()) {
        size_t len = 0;
        uint8_t* raw = sdbSnapshotRaw(st, &len);
        st->dirty = false;
        if (raw) jobs.push_back({ st->path, raw, len });
      }
    }
  }
  deletes = std::move(g_sdbPendingDeletes);
  g_sdbPendingDeletes.clear();
  CFG_UNLOCK();
  for (auto& p : deletes) fs_remove(p.c_str());
  for (auto& j : jobs) {
    size_t gzLen = 0;
    uint8_t* gz = sdbGzDeflate(j.raw, j.len, &gzLen);
    free(j.raw);
    if (!gz) { warn("storage_db: deflate %s failed\n", j.path.c_str()); continue; }
    std::string tmp = j.path + ".new";
    if (atomicWriteFile(tmp.c_str(), gz, gzLen)) fs_rename(tmp.c_str(), j.path.c_str());
    free(gz);
  }
}

/* True if any resident instance needs a flush (arms the boot save kick). */
static bool storageDbAnyDirty() {
  if (!g_sdbPendingDeletes.empty()) return true;
  for (auto* r : g_sdbRegs)
    for (auto& kv : r->insts)
      if (kv.second->dirty) return true;
  return false;
}

/* The apply pipeline — runs on the storage task (aux handler) and on the D1
 * fast path. `sender` owns any SUB/UNSUB registered by this message. */
static void storageApplyOps(const uint8_t* p, size_t len, TaskHandle_t sender) {
  if (len < 1) return;
  bool silent = (p[0] & OP_F_SILENT);
  size_t pos = 1;

  struct ParsedOp { char op; std::string key; cJSON* val; void* ptr; };
  std::vector<ParsedOp> ops;
  std::vector<SemaphoreHandle_t> saves;
  bool bad = false;

  /* Pass 1: validate + parse, no side effects on cfgRoot/externals. */
  while (pos < len && !bad) {
    char op = (char)p[pos++];
    if (op == 'S' || op == 'd' || op == 'D') {
      const char* key = (const char*)(p + pos);
      size_t klen = strnlen(key, len - pos);
      if (klen >= len - pos) { bad = true; break; }   /* no NUL within bounds */
      pos += klen + 1;
      ParsedOp po{op, std::string(key, klen), nullptr, nullptr};
      if (op != 'D') {
        if (pos >= len) { bad = true; break; }
        char vt = (char)p[pos++];
        if (vt == 'I') {
          if (pos + 4 > len) { bad = true; break; }
          int32_t v; memcpy(&v, p + pos, 4); pos += 4;
          po.val = cJSON_CreateNumber(v);
        } else if (vt == 'S' || vt == 'J') {
          if (pos + 4 > len) { bad = true; break; }
          uint32_t vl; memcpy(&vl, p + pos, 4); pos += 4;
          if (pos + vl > len) { bad = true; break; }
          if (vt == 'S') po.val = cJSON_CreateString(std::string((const char*)(p + pos), vl).c_str());
          else           po.val = cJSON_ParseWithLength((const char*)(p + pos), vl);
          pos += vl;
        } else { bad = true; break; }
      }
      ops.push_back(std::move(po));
    } else if (op == '+' || op == '-') {
      const char* sc = (const char*)(p + pos);
      size_t scl = strnlen(sc, len - pos);
      if (scl >= len - pos) { bad = true; break; }
      pos += scl + 1;
      if (pos + sizeof(void*) > len) { bad = true; break; }
      void* cb; memcpy(&cb, p + pos, sizeof(void*)); pos += sizeof(void*);
      ops.push_back(ParsedOp{op, std::string(sc, scl), nullptr, cb});
    } else if (op == 'W') {
      if (pos + sizeof(void*) > len) { bad = true; break; }
      SemaphoreHandle_t sem; memcpy(&sem, p + pos, sizeof(sem)); pos += sizeof(sem);
      saves.push_back(sem);
    } else { bad = true; break; }
  }

  if (bad) {
    warn("storage: malformed op list (rejected)\n");
    for (auto& o : ops) if (o.val) cJSON_Delete(o.val);
    for (auto s : saves) if (s) xSemaphoreGive(s);   /* don't hang storageSave callers */
    return;
  }

  /* Pass 2: apply under the config mutex. */
  cJSON* patch = cJSON_CreateObject();
  std::vector<std::pair<std::string,std::string>> changes;
  bool routedDirty = false;

  CFG_LOCK();
  for (auto& o : ops) {
    if (o.op == '+' || o.op == '-') continue;

    /* Route structured-DB keys to their record store instead of cfgRoot. The
     * store applies the op and appends the synthesized change to `changes`, so
     * the notify fan-out below is identical to a cfgRoot write. */
    if (!g_sdbRegs.empty()) {
      std::string vs;
      if (o.op != 'D' && o.val) {
        if (cJSON_IsString(o.val))      vs = o.val->valuestring;
        else if (cJSON_IsNumber(o.val)) vs = std::to_string(o.val->valueint);
        else { char* j = cJSON_PrintUnformatted(o.val); if (j) { vs = j; cJSON_free(j); } }
      }
      if (sdbApplyOpLocked(o.op, o.key.c_str(), vs, silent, changes, routedDirty)) continue;
    }

    char leaf[96];  /* see navigatePath */
    if (o.op == 'D') {
      cJSON* parent = navigateOrCreate(patch, o.key.c_str(), leaf, sizeof(leaf));
      if (parent) { cJSON_DeleteItemFromObject(parent, leaf); cJSON_AddNullToObject(parent, leaf); }
      markExternalsDeletedUnder(o.key.c_str());
      continue;
    }
    if (!o.val) continue;   /* failed parse/alloc — skip this leaf */
    if (o.op == 'd') {
      cJSON* cur = navigatePath(cfgRoot, o.key.c_str());   /* DEFAULT: skip if present */
      if (cur && !cJSON_IsObject(cur) && !cJSON_IsArray(cur)) continue;
    } else {
      cJSON* cur = navigatePath(cfgRoot, o.key.c_str());   /* SET dedup (load-bearing vs notify floods) */
      if (cur) {
        bool same = (cJSON_IsString(o.val) && cJSON_IsString(cur) && strcmp(cur->valuestring, o.val->valuestring) == 0)
                 || (cJSON_IsNumber(o.val) && cJSON_IsNumber(cur) && cur->valueint == o.val->valueint);
        if (same) continue;
      }
    }
    cJSON* parent = navigateOrCreate(patch, o.key.c_str(), leaf, sizeof(leaf));
    if (parent) {
      cJSON_DeleteItemFromObject(parent, leaf);
      cJSON_AddItemToObject(parent, leaf, o.val);
      o.val = nullptr;   /* ownership moved into the patch */
    }
  }

  deepMerge(cfgRoot, patch);
  if (!silent) { char pb[128] = ""; collectChanges(patch, pb, sizeof(pb), 0, changes); }
  if (cJSON_GetObjectItem(patch, "s") || cJSON_GetObjectItem(patch, "secrets")) {
    char rb[128] = "";
    routePatchDirty(patch, rb, sizeof(rb), 0);
    startSaveTimer();
  }
  if (routedDirty) startSaveTimer();   /* a structured-DB instance went dirty */
  cJSON_Delete(patch);

  /* SUB/UNSUB: mutate the actor-owned table under the lock (race-free). */
  for (auto& o : ops) {
    if (o.op == '+')      subAdd(sender, (storage_change_cb_t)o.ptr, o.key.c_str());
    else if (o.op == '-') subRemove(sender, (storage_change_cb_t)o.ptr, o.key.c_str());
  }
  CFG_UNLOCK();

  for (auto& o : ops) if (o.val) cJSON_Delete(o.val);   /* any skipped/leftover values */

  /* Notify AFTER the unlock (deliberate ordering: callbacks see committed state
   * and direct-called ones may re-lock the recursive mutex). */
  for (auto& ch : changes) notifyChange(ch.first.c_str(), ch.second.c_str());

  /* SAVE: queue the semaphores and poke the persist worker. */
  if (!saves.empty()) {
    CFG_LOCK();
    for (auto s : saves) pendingSaveSems.push_back(s);
    CFG_UNLOCK();
    requestSave();
  }
}

/* Aux handler on the storage task: one op message arrived. */
static void onStorageOp(TaskHandle_t sender, const void* data, size_t len) {
  storageApplyOps((const uint8_t*)data, len, sender);
}

/* Submit an op buffer (leading flags byte already present). Fast path applies
 * directly; otherwise hand the buffer to the storage task and (for sync) block
 * until applied. */
static void storageSubmit(std::string&& ops, bool sync) {
  TaskHandle_t me = xTaskGetCurrentTaskHandle();
  if (storageHandle == nullptr || me == storageHandle) {
    storageApplyOps((const uint8_t*)ops.data(), ops.size(), me);
    return;
  }
  size_t n = ops.size();
  void* buf = gp_alloc(n);
  if (!buf) { warn("storage: op alloc failed (%u B)\n", (unsigned)n); return; }
  memcpy(buf, ops.data(), n);
  if (!itsSendAuxOwnedByTaskHandle(storageHandle, STORAGE_OP_PORT, buf, n,
                                   pdMS_TO_TICKS(CONFIG_SPANGAP_STORAGE_OP_TIMEOUT_MS),
                                   sync ? ITS_WAIT_PICKUP : ITS_WAIT_DELIVERY)) {
    free(buf);
    warn("storage: op send timed out (storage task stuck?)\n");
  }
}

/* Emit one already-built op: into the calling task's open bracket if it has
 * one, else as a single auto-committed (sync) message. */
static void storageEmitOp(const std::string& op, bool silentSingle) {
  op_accum_t* a = accumFind(xTaskGetCurrentTaskHandle());
  if (a && a->depth > 0) { a->ops.append(op); return; }
  std::string buf;
  buf.push_back(silentSingle ? OP_F_SILENT : 0);
  buf.append(op);
  storageSubmit(std::move(buf), /*sync=*/true);
}

/* Aux handler installed on each subscribing task — unpacks the variable-length
 * CHANGED message {cb, key\0, val\0} and invokes the callback. */
static void storageChangeDispatch(TaskHandle_t /*sender*/, const void* data, size_t len) {
  if (len < sizeof(void*) + 2) return;
  const uint8_t* r = (const uint8_t*)data;
  void* cbp; memcpy(&cbp, r, sizeof(cbp));
  storage_change_cb_t cb = (storage_change_cb_t)cbp;
  const char* key = (const char*)(r + sizeof(cbp));
  size_t rem = len - sizeof(cbp);
  size_t klen = strnlen(key, rem);
  if (klen + 1 >= rem) return;
  const char* val = key + klen + 1;
  if (cb) cb(key, val);
}

void storageSubscribeChanges(const char* scope, storage_change_cb_t cb) {
  /* Register the receive handler on THIS task, then send a SUB op so the actor
   * adds us to its table. Sync, so the subscription is live on return (an
   * immediate NOW_AND_ON_CHANGE read then sees consistent state). */
  itsOnAux(STORAGE_CHANGE_PORT, storageChangeDispatch);
  std::string buf;
  buf.push_back(0);
  buf.push_back('+'); opPutStr(buf, scope); opPutPtr(buf, (void*)cb);
  storageSubmit(std::move(buf), /*sync=*/true);
}

void storageUnsubscribe(const char* scope) {
  if (!scope) return;
  std::string buf;
  buf.push_back(0);
  buf.push_back('-'); opPutStr(buf, scope); opPutPtr(buf, nullptr);  /* cb NULL = all on scope */
  storageSubmit(std::move(buf), /*sync=*/true);
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

/* ---- Patch routing (the apply pipeline lives in the actor, above) ---- */

/** Walk a patch tree and route dirty flags. If a sub-tree's path equals an
 *  external prefix, mark that external dirty (don't descend further). Any
 *  primitive/array leaf reached under "s." or "secrets." marks rootDirty. */
static void routePatchDirty(const cJSON* node, char* path, size_t cap, size_t len) {
  /* Check whole-prefix match before descending. */
  if (len > 0) {
    for (auto& ext : externals) {
      if (ext.prefix.size() == len && strcmp(ext.prefix.c_str(), path) == 0) {
        ext.dirty = true;
        return;
      }
    }
  }
  if (cJSON_IsObject(node)) {
    for (cJSON* child = node->child; child; child = child->next) {
      const char* name = child->string;
      if (!name) continue;
      size_t nameLen = strlen(name);
      size_t addLen = (len > 0 ? 1 : 0) + nameLen;
      if (len + addLen >= cap) continue;
      if (len > 0) path[len] = '.';
      memcpy(path + len + (len > 0 ? 1 : 0), name, nameLen + 1);
      routePatchDirty(child, path, cap, len + addLen);
      path[len] = '\0';
    }
    return;
  }
  if (len > 0 &&
      (strncmp(path, "s.", 2) == 0 || strncmp(path, "secrets.", 8) == 0))
    rootDirty = true;
}

/* storageBegin/End are sugar over the per-task op accumulator (D4): writes
 * between them collect into one op list, submitted atomically at the outer
 * storageEnd. NOTE: read-your-writes INSIDE an open bracket is gone — reads go
 * straight to cfgRoot, which has not yet seen the bracket's pending writes.
 * Audited callers restructure around this (read before the bracket). */
void storageBegin() {
  op_accum_t* a = accumFindOrCreate(xTaskGetCurrentTaskHandle());
  if (a && a->depth++ == 0) { a->ops.clear(); a->silent = false; }
  /* If the accumulator table is full, a is null: writes simply auto-commit
   * individually (atomicity lost, never data lost). */
}

void storageEnd() {
  op_accum_t* a = accumFind(xTaskGetCurrentTaskHandle());
  if (!a || a->depth <= 0) return;
  if (--a->depth == 0 && !a->ops.empty()) {
    std::string buf;
    buf.push_back(a->silent ? OP_F_SILENT : 0);
    buf.append(a->ops);
    a->ops.clear();
    storageSubmit(std::move(buf), /*sync=*/true);
  }
}

/* Internal: open a SILENT bracket (storageDefaultTree / storageCopyNoNotify). */
static void storageBeginSilent() {
  op_accum_t* a = accumFindOrCreate(xTaskGetCurrentTaskHandle());
  if (a && a->depth++ == 0) { a->ops.clear(); a->silent = true; }
}

/* ---- JSON deep merge (RFC 7396) ---- */

/** Merge src into dst in place. Objects recurse. Arrays/primitives replace. Null deletes. */
static void jsonDeepMerge(cJSON* dst, const cJSON* src) {
  const cJSON* item = src->child;
  while (item) {
    const cJSON* next = item->next;
    const char* name = item->string;
    if (!name) { item = next; continue; }
    if (cJSON_IsNull(item)) {
      cJSON_DeleteItemFromObject(dst, name);
    } else if (cJSON_IsObject(item)) {
      cJSON* dstChild = cJSON_GetObjectItem(dst, name);
      if (dstChild && cJSON_IsObject(dstChild)) {
        jsonDeepMerge(dstChild, item);
      } else {
        if (dstChild) cJSON_DeleteItemFromObject(dst, name);
        cJSON_AddItemToObject(dst, name, cJSON_Duplicate(item, true));
      }
    } else {
      cJSON* existing = cJSON_DetachItemFromObject(dst, name);
      if (existing) cJSON_Delete(existing);
      cJSON_AddItemToObject(dst, name, cJSON_Duplicate(item, true));
    }
    item = next;
  }
}

/* ---- Public Config API ---- */

/** Insert `subtree` into cfgRoot at `dotPath`. Replaces any existing node at
 *  that path. Takes ownership of `subtree`. */
static void attachAtPath(const char* dotPath, cJSON* subtree) {
  if (!subtree) return;
  char leaf[96];  /* see navigatePath */
  cJSON* parent = navigateOrCreate(cfgRoot, dotPath, leaf, sizeof(leaf));
  if (!parent) { cJSON_Delete(subtree); return; }
  cJSON_DeleteItemFromObject(parent, leaf);
  cJSON_AddItemToObject(parent, leaf, subtree);
}

/** Scan /state/storage/MODE/ for .json and .json.gz files; register each as
 *  an external. Filename's stem (sans extension) is the dot-path prefix where
 *  its content lives in cfgRoot; ext.path is always the BASE .json path (the
 *  read/write helpers handle the .gz sibling). Subdir under storage/ is the
 *  "mode" — only "external" today. */
static void scanExternals() {
  externals.clear();
  const char* modes[] = { "external" };
  for (const char* mode : modes) {
    char dirPath[64];
    snprintf(dirPath, sizeof(dirPath), "%s/storage/%s", fsStateDir(), mode);
    int dh = fs_opendir(dirPath);
    if (dh < 0) continue;
    fs_dirent_t ent;
    while (fs_readdir(dh, &ent)) {
      size_t nl = strlen(ent.name);
      size_t stem;
      if (nl > 8 && strcmp(ent.name + nl - 8, ".json.gz") == 0) stem = nl - 8;
      else if (nl > 5 && strcmp(ent.name + nl - 5, ".json") == 0) stem = nl - 5;
      else continue;
      std::string prefix(ent.name, stem);
      /* Both formats may exist for one prefix (e.g. crash between the .gz
       * write and the plain-sibling remove) — register once. */
      bool dup = false;
      for (auto& e : externals) if (e.prefix == prefix) { dup = true; break; }
      if (dup) continue;
      external_t ext;
      ext.prefix = std::move(prefix);
      ext.path   = std::string(dirPath) + "/" + ext.prefix + ".json";
      ext.dirty  = false;
      externals.push_back(std::move(ext));
    }
    fs_closedir(dh);
  }
  /* Shortest prefix first — the LOAD order invariant: a parent external
   * ("s.lxmf") must attach before any deeper one ("s.lxmf.id.0.msgs.<peer>")
   * so the deeper file's newer content overwrites its slice of the parent,
   * not the other way around (attachAtPath replaces the whole node at the
   * prefix). This is what makes the monolith→per-conversation split below
   * crash-safe: if both generations coexist on disk, the split files win.
   * Dirty routing is unaffected — routePatchDirty exact-matches the patch
   * path at each depth, so vector order never changes which external a
   * write dirties. */
  std::sort(externals.begin(), externals.end(),
            [](const external_t& a, const external_t& b) {
              return a.prefix.size() < b.prefix.size();
            });
}

/** Read each external's file and attach its content to cfgRoot. Iterates in
 *  scanExternals' shortest-prefix-first order (see the sort comment there). */
static void loadExternals() {
  for (auto& ext : externals) {
    char* text = readJsonFile(ext.path.c_str());
    if (!text) continue;
    cJSON* node = cJSON_Parse(text);
    free(text);
    if (node) attachAtPath(ext.prefix.c_str(), node);
  }
}

/** One-time split of a legacy monolithic "s.lxmf" external into the
 *  per-conversation externals current lxmf uses ("s.lxmf.id.<n>.msgs.<peer>").
 *  Runs at load, pre-tasks, with cfgRoot fully assembled. Registers a dirty
 *  external per conversation, marks the monolith pendingDelete and rootDirty;
 *  the flush happens on the save worker once storageInit kicks it. */
static void migrateLxmfMonolith() {
  external_t* mono = nullptr;
  for (auto& e : externals)
    if (e.prefix == "s.lxmf") { mono = &e; break; }
  if (!mono) return;

  int registered = 0, skipped = 0;
  cJSON* ids = navigatePath(cfgRoot, "s.lxmf.id");
  if (ids && cJSON_IsObject(ids)) {
    for (cJSON* id = ids->child; id; id = id->next) {
      if (!id->string) continue;
      cJSON* msgs = cJSON_GetObjectItem(id, "msgs");
      if (!msgs || !cJSON_IsObject(msgs)) continue;
      for (cJSON* conv = msgs->child; conv; conv = conv->next) {
        if (!conv->string || !conv->child) continue;   /* skip empty convs */
        char prefix[96];
        int n = snprintf(prefix, sizeof(prefix), "s.lxmf.id.%s.msgs.%s",
                         id->string, conv->string);
        /* The basename must survive fs_dirent_t's name[64] on the rescan
         * next boot — a too-long prefix would be re-registered truncated
         * (i.e. wrong). Leave such a conversation unsplit: with no external
         * covering it, it simply persists into root.json via rootDirty. */
        if (n < 0 || (size_t)n >= sizeof(prefix) || n + 8 >= 64) {
          skipped++;
          continue;
        }
        bool dup = false;
        for (auto& e : externals) if (e.prefix == prefix) { dup = true; break; }
        if (dup) continue;
        external_t ext;
        ext.prefix = prefix;
        ext.path   = std::string(fsStateDir()) + "/storage/external/"
                     + prefix + ".json";
        ext.dirty  = true;
        externals.push_back(std::move(ext));
        registered++;
      }
    }
  }

  /* push_back may reallocate — re-find the monolith instead of trusting the
   * pointer captured before the loop. */
  for (auto& e : externals)
    if (e.prefix == "s.lxmf") { e.pendingDelete = true; e.dirty = false; break; }
  rootDirty = true;
  info("splitting legacy s.lxmf external: %d conversation file(s)%s\n",
       registered,
       skipped ? " (some left in root.json: name too long)" : "");
}

void storageLoad() {
  if (!cfgMux) cfgMux = xSemaphoreCreateRecursiveMutex();

  /* The config tree (cfgRoot) is large config DATA (not ISR/lock-touched), so it
   * follows the default allocation policy — route cJSON through gp_alloc (mem.h):
   * PSRAM on PSRAM targets, internal on no-PSRAM ones, in one place. Set once. */
  static bool cjsonHooked = false;
  if (!cjsonHooked) {
    cJSON_Hooks h = { gp_alloc, free };
    cJSON_InitHooks(&h);
    cjsonHooked = true;
  }

  if (cfgRoot) cJSON_Delete(cfgRoot);

  /* Ensure <stateDir>/storage/ (and storage/external/) exist before any
   * read/write — a freshly-seeded SD store may only have what factory_state
   * shipped, and atomicWriteFile() does not create parent dirs. */
  fs_mkdirp(fsStatePath("/storage/external").c_str());

  /* Load <stateDir>/storage/root.json(.gz) — the single source of truth. */
  char* text = readJsonFile(fsStatePath(ROOT_JSON_PATH).c_str());
  if (text) {
    cfgRoot = cJSON_Parse(text);
    free(text);
  }
  if (!cfgRoot) cfgRoot = cJSON_CreateObject();

  /* External files: scan /state/storage/<mode>/, register, then load each
   * file's contents into cfgRoot at its prefix. Externals overwrite anything
   * at the same path that may have been in root.json. */
  scanExternals();
  loadExternals();

  /* Split a legacy monolithic s.lxmf external into the per-conversation
   * externals current lxmf registers (see ensureConvFile in lxmf.cpp). The
   * monolith predates per-conv registration and would otherwise persist
   * forever: scanExternals resurrects registrations from filenames, so
   * nothing ever retires it, every message write rewrites the whole file,
   * and — worse — once per-conv files appear next to it, the load-order
   * rule above means the shallower monolith loads FIRST specifically so it
   * cannot clobber them. All content is already in cfgRoot at this point;
   * the split is pure bookkeeping (register + mark dirty), flushed by the
   * save kicked in storageInit. Non-msgs lxmf state (contacts, identity)
   * falls through to root.json via rootDirty. Crash-safe: until the flush
   * deletes the monolith file, a reboot just re-runs this split. */
  migrateLxmfMonolith();

  /* First boot only: deep-merge additional_state/settings.json overlay.
   * fs_init() copies plain files from additional_state/; settings.json is
   * handled here because it requires cJSON knowledge. The merged result is
   * written to /state/ by the first storageSave(), so subsequent boots
   * just load the already-merged file. */
  if (fs_first_boot()) {
    char* overlay = readFileStr(FS_FIXED "/additional_state/settings.json");
    if (overlay) {
      cJSON* ov = cJSON_Parse(overlay);
      if (ov) {
        jsonDeepMerge(cfgRoot, ov);
        cJSON_Delete(ov);
      }
      free(overlay);
    }
  }

  /* Drop a stale temp from a crash between write and rename (atomicWriteFile
   * writes "<path>.new"). With the FAT-safe rename this is rare, but cheap
   * to clear. */
  fs_remove(fsStatePath(ROOT_JSON_PATH ".new").c_str());
  fs_remove(fsStatePath(ROOT_JSON_PATH ".gz.new").c_str());
}

bool storageExists(const char* key) {
  CFG_LOCK();
  bool exists;
  if (!sdbExistsLocked(key, exists)) {
    cJSON* node = resolveKey(key);
    exists = node && !cJSON_IsObject(node) && !cJSON_IsArray(node);
  }
  CFG_UNLOCK();
  return exists;
}

int storageGetInt(const char* key, int def) {
  CFG_LOCK();
  int result = def;
  std::string sv;
  if (sdbGetLocked(key, sv)) {
    result = atoi(sv.c_str());
  } else {
    cJSON* node = resolveKey(key);
    if (node) {
      if (cJSON_IsNumber(node)) result = node->valueint;
      else if (cJSON_IsString(node)) result = atoi(node->valuestring);
    }
  }
  CFG_UNLOCK();
  return result;
}

void storageGetStr(const char* key, char* out, size_t outLen, const char* def) {
  if (outLen == 0) return;
  CFG_LOCK();
  std::string sv;
  if (sdbGetLocked(key, sv)) { safeStrncpy(out, sv.c_str(), outLen); CFG_UNLOCK(); return; }
  cJSON* node = resolveKey(key);
  if (!node) { CFG_UNLOCK(); safeStrncpy(out, def, outLen); return; }
  if (cJSON_IsString(node)) { safeStrncpy(out, node->valuestring, outLen); CFG_UNLOCK(); return; }
  if (cJSON_IsNumber(node)) { snprintf(out, outLen, "%d", node->valueint); CFG_UNLOCK(); return; }
  CFG_UNLOCK();
  safeStrncpy(out, def, outLen);
}

std::string storageGetStr(const char* key, const char* def) {
  CFG_LOCK();
  std::string out;
  if (sdbGetLocked(key, out)) { CFG_UNLOCK(); return out; }
  cJSON* node = resolveKey(key);
  if (node && cJSON_IsString(node))      out = node->valuestring;
  else if (node && cJSON_IsNumber(node)) out = std::to_string(node->valueint);
  else                                   out = def ? def : "";
  CFG_UNLOCK();
  return out;
}

void storageSet(const char* key, int val) {
  std::string op;
  opAppendKVInt(op, 'S', key, val);
  storageEmitOp(op, /*silentSingle=*/false);
}

void storageSet(const char* key, const char* val) {
  /* Dedup against the committed value now happens in the actor (load-bearing
   * against notify floods, but moved so it sees the real committed state). */
  std::string op;
  opAppendKV(op, 'S', key, val);
  storageEmitOp(op, /*silentSingle=*/false);
}

/* Default writes don't fire change subscriptions: by definition the value
 * was absent before, and these are firmware-bundled defaults being seeded
 * once (typically dozens at first boot), not real config changes. Without
 * this, broad subscriptions like net's "s." flood inboxes during install. */
bool storageDefault(const char* key, const char* val) {
  /* DEFAULT op: the actor sets the key only if it is currently unset, and the
   * SILENT flag suppresses notification (defaults are not real changes). The
   * return value is a pre-check (benign TOCTOU on the return only; the
   * conditional itself is resolved race-free inside the actor). */
  bool wasUnset = !storageExists(key);
  std::string op;
  opAppendKV(op, 'd', key, val);
  storageEmitOp(op, /*silentSingle=*/true);
  return wasUnset;
}

bool storageDefault(const char* key, int val) {
  char buf[16];
  snprintf(buf, sizeof(buf), "%d", val);
  return storageDefault(key, buf);
}

/** Internal: does any node exist at this path (any type, including arrays)? */
static bool pathPresent(const char* key) {
  CFG_LOCK();
  cJSON* node = navigatePath(cfgRoot, key);
  CFG_UNLOCK();
  return node != nullptr;
}

static bool defaultTreeImpl(const char* fullKey, const cJSON* node, bool atRoot) {
  if (!node || cJSON_IsNull(node)) return false;

  if (cJSON_IsObject(node)) {
    bool wrote = false;
    for (cJSON* item = node->child; item; item = item->next) {
      if (!item->string) continue;
      char childKey[128];
      if (atRoot)
        safeStrncpy(childKey, item->string, sizeof(childKey));
      else
        snprintf(childKey, sizeof(childKey), "%s.%s", fullKey, item->string);
      wrote |= defaultTreeImpl(childKey, item, false);
    }
    return wrote;
  }

  if (atRoot) return false;  /* a non-object at the bare prefix has no key to set */

  if (cJSON_IsArray(node)) {
    if (pathPresent(fullKey)) return false;
    storageSetTree(fullKey, cJSON_Duplicate(node, true));
    return true;
  }
  if (cJSON_IsString(node))
    return storageDefault(fullKey, node->valuestring);
  if (cJSON_IsNumber(node))
    return storageDefault(fullKey, node->valueint);
  return false;
}

bool storageDefaultTree(const char* prefix, const cJSON* json) {
  if (!json) return false;
  bool prefixEmpty = (!prefix || !*prefix);
  storageBeginSilent();
  bool wrote = defaultTreeImpl(prefixEmpty ? "" : prefix, json, prefixEmpty);
  storageEnd();
  return wrote;
}

bool storageDefaultTree(const char* prefix, const char* jsonStr) {
  if (!jsonStr) return false;
  cJSON* j = cJSON_Parse(jsonStr);
  if (!j) return false;
  bool ret = storageDefaultTree(prefix, j);
  cJSON_Delete(j);
  return ret;
}

void storageUnset(const char* key) {
  /* Unified with storageDeleteTree (D12): one DELETE op, which the actor turns
   * into a null-in-patch (whole subtree removed) plus markExternalsDeletedUnder. */
  std::string op;
  opAppendDelete(op, key);
  storageEmitOp(op, /*silentSingle=*/false);
}

void storageSetTree(const char* key, cJSON* val) {
  if (!val) return;
  /* Serialize the subtree into the op as a 'J' value, then delete the caller's
   * tree (ownership contract unchanged for callers). */
  char* json = cJSON_PrintUnformatted(val);
  cJSON_Delete(val);
  if (!json) return;
  std::string op;
  opAppendKVJson(op, 'S', key, json, strlen(json));
  cJSON_free(json);
  storageEmitOp(op, /*silentSingle=*/false);
}

/** Flag every external file at or below `keyOrPrefix` for deletion (the
 *  external IS the key, or lives under it). The actual fs_remove + unregister
 *  happens on the next writeSettingsFile flush, keeping all external file I/O
 *  on the saving task. NOT the reverse direction: when the key is a sub-key
 *  *under* an external (e.g. deleting one message), no external matches here
 *  and routePatchDirty just marks that file dirty for rewrite. Caller holds
 *  CFG_LOCK. Returns true if any external was flagged. */
static bool markExternalsDeletedUnder(const char* keyOrPrefix) {
  std::string arg = stripDots(keyOrPrefix);
  if (arg.empty()) return false;
  std::string argDot = arg + ".";   /* trailing dot: id.0 must not match id.01 */
  bool any = false;
  for (auto& ext : externals) {
    bool match = (ext.prefix == arg) ||
                 (ext.prefix.size() >= argDot.size() &&
                  ext.prefix.compare(0, argDot.size(), argDot) == 0);
    if (match) { ext.pendingDelete = true; any = true; }
  }
  return any;
}

void storageDeleteTree(const char* keyOrPrefix) {
  if (!keyOrPrefix || !*keyOrPrefix) return;
  /* Unified with storageUnset on one DELETE op (D12): the actor adds a null to
   * the patch (RFC 7396 whole-subtree removal via deepMerge) AND calls
   * markExternalsDeletedUnder so an owning external file is dropped on the next
   * flush. Routes through the coalesced, back-pressure-retried browser patch. */
  std::string op;
  opAppendDelete(op, keyOrPrefix);
  storageEmitOp(op, /*silentSingle=*/false);
}

void storageSave() {
  /* Force a flush now and block until it completes. A SAVE op carries a
   * semaphore the persist worker gives once writeSettingsFile returns (D:
   * never call from the storage task — it must not wait on its own actor). */
  if (xTaskGetCurrentTaskHandle() == storageHandle) {
    warn("storage: storageSave from the storage task is unsupported\n");
    return;
  }
  SemaphoreHandle_t sem = xSemaphoreCreateBinary();
  if (!sem) return;
  std::string buf;
  buf.push_back(0);
  buf.push_back('W'); opPutPtr(buf, sem);
  storageSubmit(std::move(buf), /*sync=*/false);  /* delivery only; we wait on sem */
  if (xSemaphoreTake(sem, pdMS_TO_TICKS(30000)) != pdTRUE)
    warn("storage: save timed out\n");
  vSemaphoreDelete(sem);
}

void storageNewTreeFile(const char* prefix) {
  std::string p = stripDots(prefix);
  if (p.empty()) return;
  /* The basename must survive fs_dirent_t's name[64] on the next boot's
   * rescan — a longer name would be re-registered truncated (wrong prefix).
   * Refuse; the subtree just persists into root.json instead. */
  if (p.size() + 8 /* ".json.gz" */ >= 64) {
    warn("storage: external prefix too long for dirent (%u B): %s\n",
         (unsigned)p.size(), p.c_str());
    return;
  }

  CFG_LOCK();
  /* Idempotent. If a just-deleted prefix is being re-created before the flush
     processed its pendingDelete, cancel that delete and reuse the entry. */
  for (auto& ext : externals) {
    if (ext.prefix == p) {
      ext.pendingDelete = false;
      CFG_UNLOCK();
      return;
    }
  }
  external_t ext;
  ext.prefix = p;
  ext.path   = std::string(fsStateDir()) + "/storage/external/" + p + ".json";
  externals.push_back(std::move(ext));
  CFG_UNLOCK();

  /* No fs I/O here: the physical file is created on the next flush by
     writeExternalFile (on the saving task) as soon as a key under `p` marks it
     dirty — which the caller does immediately after this returns. Registration
     alone routes those writes to the file, so we keep blocking fs I/O off the
     caller's itsPoll loop. A reboot before the first flush simply loses the
     (also-unsaved) registration, re-created on the conversation's next write —
     no orphaned file, no new data-loss window beyond the existing save delay. */
}

/* ---- Structured record store registration ---- */

void storageStructuredDB(const char* name, const char* keyPattern,
                         const sdb_schema* schema, const storage_db_opts& opts) {
  if (!name || !keyPattern || !schema) return;

  CFG_LOCK();
  for (auto* r : g_sdbRegs) if (r->name == name) { CFG_UNLOCK(); return; }  /* idempotent */
  sdb_reg* r  = new sdb_reg();
  r->name     = name;
  r->pat      = splitDots(keyPattern);
  r->patLen   = r->pat.size();
  for (auto& seg : r->pat) {                 /* literal prefix up to the first wildcard */
    if (seg == "$") break;
    if (!r->litPrefix.empty()) r->litPrefix += '.';
    r->litPrefix += seg;
  }
  if (!r->litPrefix.empty()) r->litPrefix += '.';
  r->filePat  = opts.persist ? opts.persist : "";
  r->schema   = schema;
  r->persist  = opts.persist != nullptr;
  r->browserMirror = opts.browserMirror;
  r->evict    = opts.evict;
  r->cap      = opts.cap;
  g_sdbRegs.push_back(r);
  CFG_UNLOCK();

  /* Publish the declaration into the ephemeral, browser-synced, firmware-only
   * `storage.db.<name>` registry so the browser can decode the raw record
   * files it is shipped. Rebuilt from code each boot (never persisted). */
  cJSON* d = cJSON_CreateObject();
  cJSON_AddStringToObject(d, "pattern", keyPattern);
  if (opts.persist) cJSON_AddStringToObject(d, "file", opts.persist);
  cJSON_AddNumberToObject(d, "persist",    opts.persist ? 1 : 0);
  cJSON_AddNumberToObject(d, "schema_id",  schema->schema_id);
  cJSON_AddNumberToObject(d, "schema_ver", schema->schema_ver);
  cJSON_AddNumberToObject(d, "hdr_size",   schema->hdr_size);
  cJSON* farr = cJSON_CreateArray();
  for (auto& f : schema->fields) {
    cJSON* fo = cJSON_CreateObject();
    cJSON_AddStringToObject(fo, "name", f.name.c_str());
    cJSON_AddNumberToObject(fo, "kind", f.kind);
    cJSON_AddNumberToObject(fo, "off",  f.off);
    cJSON_AddNumberToObject(fo, "width", f.width);
    cJSON_AddItemToArray(farr, fo);
  }
  cJSON_AddItemToObject(d, "fields", farr);
  std::string rk = std::string("storage.db.") + name;
  storageSetTree(rk.c_str(), d);   /* ephemeral (bare prefix), synced, takes ownership */
}

/* ---- one-way migration: cfgRoot subtrees -> record stores ---- */

/* Walk cfgRoot following a store's key pattern; at each instance subtree pack
 * every record's schema fields into the store and record the (parent,leaf) to
 * detach afterwards. Caller holds CFG_LOCK. */
static void migrateWalk(cJSON* node, sdb_reg* r, size_t depth,
                        std::vector<std::string>& binds,
                        std::vector<std::pair<cJSON*, std::string>>& detach,
                        int& packed) {
  if (!node) return;
  if (depth == r->patLen) {
    /* `node` is the instance subtree: an object of <record> children. */
    sdb_store* st = sdbInstance(r, binds);
    if (!st->loaded) sdbLoad(st);
    bool any = false;
    for (cJSON* rec = node->child; rec; rec = rec->next) {
      if (!rec->string || !cJSON_IsObject(rec)) continue;
      for (cJSON* fld = rec->child; fld; fld = fld->next) {
        if (!fld->string) continue;
        if (!r->schema->find(fld->string)) continue;   /* skip redundant fields (e.g. peer) */
        std::string v;
        if (cJSON_IsString(fld))      v = fld->valuestring;
        else if (cJSON_IsNumber(fld)) v = std::to_string(fld->valueint);
        else continue;
        sdbSetField(st, rec->string, fld->string, v.c_str());
        any = true;
      }
    }
    if (any) packed++;
    return;
  }
  const std::string& pseg = r->pat[depth];
  if (pseg == "$") {
    for (cJSON* c = node->child; c; c = c->next) {
      if (!c->string) continue;
      binds.push_back(c->string);
      if (depth + 1 == r->patLen) detach.emplace_back(node, c->string);
      migrateWalk(c, r, depth + 1, binds, detach, packed);
      binds.pop_back();
    }
  } else {
    cJSON* c = cJSON_GetObjectItem(node, pseg.c_str());
    if (c) {
      if (depth + 1 == r->patLen) detach.emplace_back(node, pseg);
      migrateWalk(c, r, depth + 1, binds, detach, packed);
    }
  }
}

void storageDbMigrate() {
  std::string oldDir = fsStatePath("/storage/external.old");
  std::string curDir = fsStatePath("/storage/external");
  struct stat st;

  CFG_LOCK();
  if (fs_stat(oldDir.c_str(), &st) == 0) { CFG_UNLOCK(); return; }   /* already migrated */

  int packed = 0;
  std::vector<std::pair<cJSON*, std::string>> detach;
  for (auto* r : g_sdbRegs) {
    if (!r->persist) continue;
    std::vector<std::string> binds;
    migrateWalk(cfgRoot, r, 0, binds, detach, packed);
  }
  /* Detach the migrated subtrees from cfgRoot so they can't re-mirror or fall
   * back into root.json. */
  for (auto& d : detach) cJSON_DeleteItemFromObject(d.first, d.second.c_str());

  /* Collapse every legacy external back into root.json: their message data now
   * lives in the stores; anything else they held (contacts, identity) is still
   * in cfgRoot and, with the externals forgotten, persists via root.json. This
   * is what makes renaming the whole external/ dir safe. */
  externals.clear();
  rootDirty = true;
  CFG_UNLOCK();

  /* Durable before we touch the old data: flush the new stores + root.json. */
  storageSave();

  /* Done-marker + recovery backup. Keep external.old (don't delete); a crash
   * before this rename simply re-runs the migration next boot. */
  fs_rename(curDir.c_str(), oldDir.c_str());
  info("storage_db: migrated %d conversation(s) to record stores\n", packed);
}

void storageDbMigrateStore(const char* name) {
  if (!name || !*name) return;
  std::string marker = fsStatePath((std::string("/storage/migrated-") + name).c_str());
  struct stat st;

  CFG_LOCK();
  if (fs_stat(marker.c_str(), &st) == 0) { CFG_UNLOCK(); return; }   /* already done */

  sdb_reg* reg = nullptr;
  for (auto* r : g_sdbRegs) if (r->name == name) { reg = r; break; }
  if (!reg || !reg->persist) { CFG_UNLOCK(); return; }   /* unknown or RAM-only */

  int packed = 0;
  std::vector<std::pair<cJSON*, std::string>> detach;
  std::vector<std::string> binds;
  migrateWalk(cfgRoot, reg, 0, binds, detach, packed);
  /* Detach the packed subtree so it can't re-mirror or fall back into root.json. */
  for (auto& d : detach) cJSON_DeleteItemFromObject(d.first, d.second.c_str());
  rootDirty = true;
  CFG_UNLOCK();

  /* Durable before we mark done: flush the new store(s) + the shrunk root.json. */
  storageSave();

  /* Per-store done-marker (touch an empty file). A crash before this re-runs. */
  int f = fs_open(marker.c_str(), "w");
  if (f >= 0) fs_close(f);
  info("storage_db: migrated store '%s' (%d instance(s))\n", name, packed);
}

bool storageDbDropInstance(const char* instancePrefix) {
  if (!instancePrefix || !*instancePrefix) return false;
  CFG_LOCK();
  bool routes = sdbRoutes(instancePrefix);
  CFG_UNLOCK();
  if (!routes) return false;
  /* A DELETE op routes through sdbApplyOpLocked (tail 0 → drop the instance,
   * remove its file, synthesize the subtree-gone notification). */
  storageDeleteTree(instancePrefix);
  return true;
}

cfg_type_t storageGetType(const char* key) {
  CFG_LOCK();
  cJSON* node = resolveKey(key);
  cfg_type_t t = (node && cJSON_IsNumber(node)) ? CFG_INT : CFG_STR;
  CFG_UNLOCK();
  return t;
}

/* ---- Bulk operations ---- */

/** Walk src subtree, append a SET op for each leaf at the corresponding dst
 *  path. Reads cfgRoot for the onlyIfExists guard, so the caller holds the
 *  lock; the ops are submitted after the lock is released. */
static void walkAndCopyOps(std::string& buf, cJSON* node, const std::string& srcPre,
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
      walkAndCopyOps(buf, item, srcKey, dstKey, onlyIfExists);
    } else {
      if (onlyIfExists && !navigatePath(cfgRoot, dstKey.c_str())) continue;
      if (cJSON_IsNumber(item))      opAppendKVInt(buf, 'S', dstKey.c_str(), item->valueint);
      else if (cJSON_IsString(item)) opAppendKV(buf, 'S', dstKey.c_str(), item->valuestring);
    }
  }
}

/* Read the source under the lock and compose SET ops into one message; submit
 * after the lock is released (the actor would deadlock taking it again). Loses
 * source-stability during the copy — callers are init-time, acceptable. */
void storageCopy(const char* srcPrefix, const char* dstPrefix, bool onlyIfTargetKeyExists) {
  std::string src = stripDots(srcPrefix);
  std::string dst = stripDots(dstPrefix);
  std::string buf;
  buf.push_back(0);   /* notify */

  CFG_LOCK();
  cJSON* srcNode = navigatePath(cfgRoot, src.c_str());
  if (srcNode) {
    if (!cJSON_IsObject(srcNode) && !cJSON_IsArray(srcNode)) {
      if (!(onlyIfTargetKeyExists && !navigatePath(cfgRoot, dst.c_str()))) {
        if (cJSON_IsNumber(srcNode))      opAppendKVInt(buf, 'S', dst.c_str(), srcNode->valueint);
        else if (cJSON_IsString(srcNode)) opAppendKV(buf, 'S', dst.c_str(), srcNode->valuestring);
      }
    } else {
      walkAndCopyOps(buf, srcNode, src, dst, onlyIfTargetKeyExists);
    }
  }
  CFG_UNLOCK();

  if (buf.size() > 1) storageSubmit(std::move(buf), /*sync=*/true);
}

void storageCopyNoNotify(const char* srcPrefix, const char* dstPrefix, bool onlyIfTargetKeyExists) {
  std::string src = stripDots(srcPrefix);
  std::string dst = stripDots(dstPrefix);
  if (onlyIfTargetKeyExists) return;   /* unchanged: never copies in this mode */

  /* Serialize the whole source subtree under the lock and emit it as one SILENT
   * SET 'J' op — the actor's deepMerge places/merges it at dst exactly as the
   * old direct merge did, but through the actor (no foreign-task write). */
  CFG_LOCK();
  cJSON* srcNode = navigatePath(cfgRoot, src.c_str());
  char* json = srcNode ? cJSON_PrintUnformatted(srcNode) : nullptr;
  CFG_UNLOCK();
  if (!json) return;
  std::string buf;
  buf.push_back(OP_F_SILENT);
  opAppendKVJson(buf, 'S', dst.c_str(), json, strlen(json));
  cJSON_free(json);
  storageSubmit(std::move(buf), /*sync=*/true);
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
  /* A prefix that lands in a record store iterates its instance(s) instead of
   * cfgRoot — collect under the lock, then invoke cb after releasing it (a cb
   * that re-enters storage would re-take the recursive lock; keep parity with
   * the collect-then-emit CLI discipline). */
  std::vector<std::string> routed;
  CFG_LOCK();
  if (sdbForEachUnderLocked(pre.c_str(), routed)) {
    CFG_UNLOCK();
    for (size_t i = 0; i + 1 < routed.size(); i += 2) cb(routed[i].c_str(), routed[i + 1].c_str());
    return;
  }
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
  std::string out;
  CFG_LOCK();
  bool empty = (!cfgRoot || !cfgRoot->child);
  if (!empty) walkTreeCollect(cfgRoot, "", out);
  CFG_UNLOCK();
  if (empty) { write("(empty)\n", 8); return; }
  emitLines(out, write);   /* write outside CFG_LOCK — see walkTreeCollect */
}

/* ---- CLI commands ---- */

static void cmdSet(const char* a) {
    if (cliWantsHelp(a)) { cliPrintf("%-*s set config variable\n", CLI_HELP_COL, "set <key>=<value>  (or: set <key> <value>)"); return; }
    while (*a == ' ') a++;
    /* Accept either `set key=value` or `set key value`: the T-Deck keyboard has
     * no '=' key, so a space is an equally valid key/value separator. Whichever
     * of '=' or ' ' appears first is the separator; the rest is the value (so a
     * value may itself contain '=' or spaces). */
    const char* eq = strchr(a, '=');
    const char* sp = strchr(a, ' ');
    const char* sep = (eq && (!sp || eq < sp)) ? eq : sp;
    if (!sep || sep == a) { cliPrintf("usage: set <key>=<value>  (or: set <key> <value>)\n"); return; }
    /* Generous full-key buffer (change notifies carry keys at any length).
     * Used to be 48 — small enough that `set s.lxmf.id.0.msgs.<64-hex>.<field>=…`
     * was rejected at the CLI before storageSet ever ran. */
    char key[128];
    size_t klen = sep - a;
    while (klen > 0 && a[klen - 1] == ' ') klen--;
    if (klen == 0 || klen >= sizeof(key)) { cliPrintf("err: key empty or too long\n"); return; }
    memcpy(key, a, klen); key[klen] = '\0';
    if (isFw(key)) { cliPrintf("err: fw.* is read-only firmware identity (compile-time)\n"); return; }
    const char* val = sep + 1;
    while (*val == ' ') val++;
    storageSet(key, val);
    if (strncmp(key, "s.log", 5) == 0)
        logApplyLevels();
}

static void cmdUnset(const char* a) {
    if (cliWantsHelp(a)) { cliPrintf("%-*s delete config variable\n", CLI_HELP_COL, "unset <key>"); return; }
    if (!*a) { cliPrintf("usage: unset <key>\n"); return; }
    storageDeleteTree(a);
}

static void cmdShow(const char* a) {
    if (cliWantsHelp(a)) { cliPrintf("%-*s show config variables\n", CLI_HELP_COL, "show [<prefix>]"); return; }
    auto write = [](const char* d, size_t l) { cliPrintf("%.*s", (int)l, d); };

    if (!*a) {
        storageList(write);
        return;
    }

    /* Collect all matched output into `out` UNDER the lock, then emit it after
     * releasing the lock. The blocking transport write must not run while
     * CFG_LOCK is held (see walkTreeCollect). */
    std::string out;
    bool found = false;
    bool prefixTooLong = false;

    CFG_LOCK();
    /* Try exact path first */
    cJSON* node = navigatePath(cfgRoot, a);
    if (node) {
        if (cJSON_IsObject(node) || cJSON_IsArray(node)) walkTreeCollect(node, a, out);
        else                                             appendLeaf(out, a, node);
        found = true;   /* an exact path is a match even if it prints nothing */
    } else if (const char* lastDot = strrchr(a, '.')) {
        /* Partial last-segment match. parentPath matches storage's full-key
         * capacity — was 64, too small for paths with 64-char SHA-256 hex
         * segments. */
        char parentPath[128];
        size_t parentLen = lastDot - a;
        if (parentLen >= sizeof(parentPath)) {
            prefixTooLong = true;
        } else {
            memcpy(parentPath, a, parentLen);
            parentPath[parentLen] = '\0';
            cJSON* parent = navigatePath(cfgRoot, parentPath);
            if (parent && cJSON_IsObject(parent)) {
                const char* partial = lastDot + 1;
                size_t partialLen = strlen(partial);
                cJSON* item;
                cJSON_ArrayForEach(item, parent) {
                    if (item->string && strncmp(item->string, partial, partialLen) == 0) {
                        /* parentPath up to 127 chars + '.' + item->string. cJSON
                         * string keys are bounded by what we've stored (≤ 95
                         * per the navigatePath limit), so 256 is comfortable. */
                        char key[256];
                        snprintf(key, sizeof(key), "%s.%s", parentPath, item->string);
                        if (cJSON_IsObject(item) || cJSON_IsArray(item)) walkTreeCollect(item, key, out);
                        else                                             appendLeaf(out, key, item);
                        found = true;
                    }
                }
            }
        }
    } else {
        /* No dot — match against root children */
        size_t len = strlen(a);
        cJSON* item;
        cJSON_ArrayForEach(item, cfgRoot) {
            if (item->string && strncmp(item->string, a, len) == 0) {
                if (cJSON_IsObject(item) || cJSON_IsArray(item)) walkTreeCollect(item, item->string, out);
                else                                             appendLeaf(out, item->string, item);
                found = true;
            }
        }
    }
    CFG_UNLOCK();

    if (prefixTooLong) { cliPrintf("(prefix too long)\n"); return; }
    emitLines(out, write);
    if (!found) cliPrintf("(no matches)\n");
}

static void cmdSave(const char* a) {
    if (cliWantsHelp(a)) { cliPrintf("%-*s write settings to flash now\n", CLI_HELP_COL, "save"); return; }
    storageSave();
}

void storageRegisterCmds() {
    cliRegisterCmd("set", cmdSet);
    cliRegisterCmd("unset", cmdUnset);
    cliRegisterCmd("show", cmdShow);
    cliRegisterCmd("save", cmdSave);
}

/* ---- Config DataChannel handling (single-session via WebRTC router) ---- */

/* One packet per JSON message. The webrtc router (webrtc_task.cpp) holds
 * the single-session constraint; by the time a DC reaches us, the peer
 * is authenticated. We carry the single client handle, coalesce changes
 * into one patch tree, and flush it as one packet per main-loop pass. */

/* Streamed full dump. The saved-announce stores (rnsd, lxmf) push cfgRoot far
 * past any single ITS packet, so the dump is split into multiple JSON messages,
 * each under DC_DUMP_MAX. The browser deep-merges every message it receives, so
 * each chunk need only carry its subtree at the right place — structure is
 * mirrored as nested objects (NOT dotted paths: announce keys are opaque
 * dest-hashes that may contain '.'). A {"__dump":"b"}/{"__dump":"e"} pair
 * brackets the stream so the browser can defer its post-dump pending-set
 * re-flush until full state has landed. */

/* The server→client ITS buffer is 16K (fromSize at itsServerPortOpen below).
 * Every packet is header(4)+body <= 16384. DC_DUMP_MAX is a conservative body
 * budget for a streamed chunk: it leaves room for the header plus the path
 * wrapper a deep subtree (e.g. s.<...>.<64-char-hash>) adds around its payload,
 * since the per-unit size below is an estimate, not the exact printed length. */
static constexpr size_t DC_DUMP_MAX   = 14000;
static constexpr int    DC_DUMP_DEPTH = 32;     /* cfgRoot nests ~9 deep */

/* The full dump is pre-serialized into chunks (dcBuildDump) and streamed from
 * the storage task loop (dcPumpDump), paced to DC buffer space. It must NOT run
 * inside the connect callback or block on a send: the connect ack is withheld
 * until storageItsConnect returns (its.cpp gives the ackSem only after
 * onConnect), and the browser can't drain the stream until that ack lands — so
 * a blocking in-callback dump deadlocks the ack (client gives up after 3 s) and
 * freezes the inbox drain into a notify-drop storm. Streaming from the loop
 * keeps the ack instant and the inbox draining while back-pressure paces the
 * chunks. */
static std::vector<std::string> dcDumpQueue;        /* pending dump chunks, in order */
static size_t                   dcDumpPos     = 0;  /* next queue index to send */
static bool                     dcDumpPending = false;  /* build queued on next pump */

/* Printed length of a node's value (0 on alloc failure). */
static size_t dcNodePrintLen(cJSON* node) {
    char* s = cJSON_PrintUnformatted(node);
    if (!s) return 0;
    size_t n = strlen(s);
    cJSON_free(s);
    return n;
}

static cJSON* dcGetOrCreateObject(cJSON* parent, const char* name) {
    cJSON* o = cJSON_GetObjectItem(parent, name);
    if (o && cJSON_IsObject(o)) return o;
    o = cJSON_CreateObject();
    cJSON_AddItemToObject(parent, name, o);
    return o;
}

/* Serialize the accumulated batch as one chunk, append it to dcDumpQueue, and
 * reset *batch to empty. No network I/O — dcPumpDump streams the queue out from
 * the task loop. */
/* Where a running dump build emits its chunks. Points at dcDumpQueue for the
 * legacy inline path; the off-actor builder points it at its staging vector.
 * Only one dump build ever runs at a time (s_dumpBuilding guards). */
static std::vector<std::string>* s_dumpOut = nullptr;

static void dcDumpEmit(cJSON** batch, size_t* batchLen) {
    if (*batch && (*batch)->child) {
        char* text = cJSON_PrintUnformatted(*batch);
        if (text) {
            s_dumpOut->emplace_back(text);
            cJSON_free(text);
        }
        cJSON_Delete(*batch);
        *batch = cJSON_CreateObject();
    }
    *batchLen = 0;
}

/* Place a copy of `node` into `batch` at stack[0..depth) + name, creating the
 * mirror objects along the way. */
static void dcBatchAdd(cJSON* batch, const char* const* stack, int depth,
                       const char* name, cJSON* node) {
    cJSON* cur = batch;
    for (int i = 0; i < depth; i++) cur = dcGetOrCreateObject(cur, stack[i]);
    cJSON_AddItemToObject(cur, name, cJSON_Duplicate(node, true));
}

/* Greedily pack subtrees into <=DC_DUMP_MAX chunks, recursing into objects too
 * big to fit as a unit. */
static void dcStreamNode(cJSON** batch, size_t* batchLen,
                         const char** stack, int depth,
                         const char* name, cJSON* node) {
    size_t m   = dcNodePrintLen(node);
    size_t est = m + strlen(name) + 8;          /* "name":{} + slack */

    if (cJSON_IsObject(node) && node->child && est > DC_DUMP_MAX
        && depth < DC_DUMP_DEPTH) {
        stack[depth] = name;
        for (cJSON* c = node->child; c; c = c->next)
            dcStreamNode(batch, batchLen, stack, depth + 1, c->string, c);
        return;
    }

    if (*batchLen && *batchLen + est > DC_DUMP_MAX)
        dcDumpEmit(batch, batchLen);

    dcBatchAdd(*batch, stack, depth, name, node);
    *batchLen += est;

    /* A single unit at/over budget can't share a chunk — emit it now so the
     * next unit starts clean. dcPumpDump skips (with a warn) any chunk that
     * genuinely won't fit the DC buffer. */
    if (*batchLen >= DC_DUMP_MAX)
        dcDumpEmit(batch, batchLen);
}

/* Build the full dump into dcDumpQueue: a {"__dump":"b"} sentinel, the config
 * tree packed into <=DC_DUMP_MAX chunks, then {"__dump":"e"}. Pure RAM/CPU (no
 * network I/O); dcPumpDump streams the queue out paced to buffer space. */
static void dcBuildDumpInto(std::vector<std::string>& out) {
    int64_t t0 = esp_timer_get_time();
    CFG_LOCK();
    cJSON* clone = cJSON_Duplicate(cfgRoot, true);
    CFG_UNLOCK();
    int64_t held = esp_timer_get_time() - t0;
    if (held > CFG_HOLD_WARN_US)
        warn("CFG_LOCK hold %lldms: dup full dump\n", held / 1000);
    if (!clone) return;

    /* Remove secrets from the dump */
    cJSON* secrets = cJSON_DetachItemFromObject(clone, "secrets");
    if (secrets) cJSON_Delete(secrets);

    /* Synthesize the read-only fw.* identity subtree straight from the ROM
       string constants — never resident in cfgRoot, so it costs no steady-state
       RAM and can never be persisted or patched. The browser receives it in the
       initial dump alongside s.* and binds fw.name / fw.banner as plain text. */
    cJSON* fw = cJSON_CreateObject();
    if (fw) {
        cJSON_AddStringToObject(fw, "stub",   CONFIG_SPANGAP_FW_STUB);
        cJSON_AddStringToObject(fw, "name",   CONFIG_SPANGAP_FW_NAME);
        cJSON_AddStringToObject(fw, "banner", CONFIG_SPANGAP_FW_BANNER);
        cJSON_AddItemToObject(clone, "fw", fw);
    }

    /* Bracket the stream so the browser knows when the snapshot is complete. */
    s_dumpOut = &out;
    out.emplace_back("{\"__dump\":\"b\"}");

    cJSON* batch = cJSON_CreateObject();
    size_t batchLen = 0;
    const char* stack[DC_DUMP_DEPTH];
    for (cJSON* c = clone->child; c; c = c->next)
        dcStreamNode(&batch, &batchLen, stack, 0, c->string, c);
    dcDumpEmit(&batch, &batchLen);
    cJSON_Delete(batch);
    cJSON_Delete(clone);

    out.emplace_back("{\"__dump\":\"e\"}");
    s_dumpOut = nullptr;
}

/* ---- Off-actor dump builder ----
 * dcBuildDumpInto is ~1 s of cJSON_Duplicate + serialization on a config tree
 * bloated by saved announces / Nomad pages. Run inline on the storage actor it
 * makes the actor deaf for that second — no ping→pong, no port-44 drain — which
 * trips the browser's 2 s liveness mid-connect and flaps the session in a
 * connect→dump→abort loop. So the build runs on a one-shot worker task instead:
 * only the cJSON_Duplicate holds CFG_LOCK (~half the time, and actor waits on
 * the lock are bounded well under the liveness window); serialization runs
 * lock-free. The actor adopts the finished queue on its next pass. A generation
 * counter discards a build whose session died mid-build. */
static std::vector<std::string> s_dumpStaging;
static std::atomic<bool>     s_dumpBuilding{false};
static std::atomic<bool>     s_dumpDone{false};
static std::atomic<uint32_t> s_dumpGen{0};       /* bumped on connect/disconnect */
static uint32_t              s_dumpBuildGen = 0; /* gen the running build captured */

static void dumpBuilderFn(void*) {
    dcBuildDumpInto(s_dumpStaging);
    s_dumpDone.store(true, std::memory_order_release);
    xTaskNotifyGive(storageHandle);              /* wake the actor's itsPoll */
    vTaskDelete(nullptr);
}

/** Accumulate a changed key into dcPendingPatch for coalesced output. */
static void dcAccumulateChange(const char* key, const char* val) {
    (void)val;
    if (dcHandle < 0) return;
    if (isSecret(key)) return;

    CFG_LOCK();

    /* Structured-DB bodies aren't in cfgRoot. Mirror a body change only for the
     * instance the client currently has open (record-scoped, per the plan) — its
     * value comes from the store, not cfgRoot. Changes to other conversations
     * are represented to the browser only by their maintained directory entry. */
    sdb_match sm;
    if (sdbRoute(key, sm)) {
        bool open = !dcOpenPrefix.empty() &&
                    strncmp(key, dcOpenPrefix.c_str(), dcOpenPrefix.size()) == 0 &&
                    key[dcOpenPrefix.size()] == '.';
        /* A body (message conversation) only mirrors while it is the open instance;
         * a browser-mirrored store (contacts directory, announce catalogue) mirrors
         * every change, exactly as the equivalent cfgRoot subtree used to. */
        if (!open && !sm.reg->browserMirror) { CFG_UNLOCK(); return; }
        std::string sv;
        bool has = sdbGetLocked(key, sv);
        if (!dcPendingPatch) dcPendingPatch = cJSON_CreateObject();
        char leaf[96];
        cJSON* parent = navigateOrCreate(dcPendingPatch, key, leaf, sizeof(leaf));
        if (parent) {
            cJSON_DeleteItemFromObject(parent, leaf);
            if (has) cJSON_AddStringToObject(parent, leaf, sv.c_str());
            else     cJSON_AddNullToObject(parent, leaf);
        }
        CFG_UNLOCK();
        return;
    }

    /* The key may be gone (storageUnset / storageDeleteTree removed it
       before firing callbacks). Previously we skipped — so deletions were
       never echoed and a deleted conversation lingered in open clients
       until a full reload. Instead echo an explicit null at the key: the
       coalesced patch is retried under back-pressure (never dropped), so
       the browser reliably drops the (sub)tree. */
    cJSON* node = navigatePath(cfgRoot, key);

    if (!dcPendingPatch) dcPendingPatch = cJSON_CreateObject();

    char leaf[96];  /* see navigatePath */
    cJSON* parent = navigateOrCreate(dcPendingPatch, key, leaf, sizeof(leaf));
    if (!parent) { CFG_UNLOCK(); return; }

    cJSON_DeleteItemFromObject(parent, leaf);
    if (!node) {
        cJSON_AddNullToObject(parent, leaf);          /* deletion */
    } else {
        bool deep = cJSON_IsObject(node) || cJSON_IsArray(node);
        cJSON_AddItemToObject(parent, leaf, cJSON_Duplicate(node, deep));
    }
    CFG_UNLOCK();
}

/* Ship a structured-DB instance's records to the browser as one merge patch,
 * placed at the instance's dot-path so the browser merges them into its mirror
 * exactly where its message-reading code already looks. Called on {"fetch":...}.
 * Runs on the storage task. */
static void dcShipStorePrefix(const char* prefix) {
    std::vector<std::string> kv;
    CFG_LOCK();
    if (!sdbForEachUnderLocked(prefix, kv)) { CFG_UNLOCK(); return; }
    /* Track the open instance only for on-demand bodies (conversations). A
     * browser-mirrored store is fetched once and then kept live by the change
     * mirror, so it must not displace the open conversation here. */
    sdb_match sm;
    if (!(sdbRoute(prefix, sm) && sm.reg->browserMirror)) dcOpenPrefix = prefix;
    if (!dcPendingPatch) dcPendingPatch = cJSON_CreateObject();
    for (size_t i = 0; i + 1 < kv.size(); i += 2) {
        char leaf[96];
        cJSON* parent = navigateOrCreate(dcPendingPatch, kv[i].c_str(), leaf, sizeof(leaf));
        if (parent) {
            cJSON_DeleteItemFromObject(parent, leaf);
            cJSON_AddStringToObject(parent, leaf, kv[i + 1].c_str());
        }
    }
    CFG_UNLOCK();
}

/** Flush accumulated changes to the browser as one DC packet. On back-
 *  pressure leave the patch intact and retry next pass — never drop.
 *
 *  The per-patch ceiling now derives from the packet link's per-message size
 *  guard (itsSendBufSize == the port's maxMsg, 64 KB), not a fixed 15.5 KB ring
 *  budget — so a 15-32 KB Nomad page reaches the browser instead of being
 *  silently dropped one layer below Nomad's own cap. The drop-and-warn arm
 *  stays as a final backstop but should be unreachable below the guard. */
static void dcFlushPatch() {
    if (!dcPendingPatch || dcHandle < 0) return;

    /* Cheap gate BEFORE serializing: if the DC send link has no room right now
       (a slow or mid-teardown browser has back-pressured us up to the byte
       window), don't pay for cJSON_PrintUnformatted of the whole pending patch
       — which can be a 128 KB Nomad page — on every 10 ms loop pass. A stuck
       browser would otherwise pin the storage actor in repeated large prints,
       starving the STORAGE_OP_PORT drain and the ping→pong reply enough to trip
       the browser's 2 s liveness check (flap) and back up every task's config
       write. The patch stays intact and coalesces further changes; we retry
       once the browser drains or reconnects (a reconnect re-dumps anyway).
       itsSpacesAvailable==0 means no descriptor slot or the window is exhausted;
       a live browser draining normally always reports the full maxMsg here. */
    if (itsSpacesAvailable(dcHandle) == 0) return;

    char* text = cJSON_PrintUnformatted(dcPendingPatch);
    if (!text) return;
    size_t len = strlen(text);

    size_t dcPatchMax = itsSendBufSize(dcHandle);   /* the port's maxMsg guard */

    /* Patch outgrew the per-message guard: drop and warn (now unreachable in
       practice). Incremental UI state may become stale until the next change
       forces a fresh patch; full re-dumps from the storage task would blow the
       stack (cJSON_Duplicate of cfgRoot is deeply recursive). */
    if (len > dcPatchMax) {
        warn("storage: patch %u > %u, dropping (clients may need reload)\n",
             (unsigned)len, (unsigned)dcPatchMax);
        cJSON_free(text);
        cJSON_Delete(dcPendingPatch);
        dcPendingPatch = nullptr;
        return;
    }

    /* Non-blocking packet send: require the whole body + 4-byte packet
       header to fit. Retry next pass on back-pressure. */
    if (itsSpacesAvailable(dcHandle) < len) {
        cJSON_free(text);
        return;
    }
    size_t sent = itsSend(dcHandle, text, len, 0);
    cJSON_free(text);
    if (sent == len) {
        cJSON_Delete(dcPendingPatch);
        dcPendingPatch = nullptr;
    }
}

/** True while a full dump is queued or mid-stream. Patches are held until it
 *  drains (see the task loop) so a post-snapshot change can't be overwritten by
 *  an older dump chunk that lands after it. */
static bool dcDumpInProgress() {
    return dcDumpPending || dcDumpPos < dcDumpQueue.size();
}

/** Stream the pre-built dump from the task loop, paced to buffer space. Builds
 *  the queue lazily on the first pump after a connect, then sends as many
 *  chunks as fit right now and yields — the browser drains the buffer between
 *  passes and the inbox keeps draining, so neither the dump nor the
 *  notification fan-in starves the other. Never blocks. */
static void dcPumpDump() {
    if (dcHandle < 0) return;
    if (dcDumpPending && !s_dumpBuilding.load(std::memory_order_acquire)) {
        /* Kick the off-actor build. Core 0 (it has headroom; core 1 carries the
         * UI + actors); 16 KB stack for cJSON_Duplicate's recursion. */
        s_dumpStaging.clear();
        s_dumpDone.store(false, std::memory_order_relaxed);
        s_dumpBuildGen = s_dumpGen.load(std::memory_order_relaxed);
        s_dumpBuilding.store(true, std::memory_order_release);
        if (!spawnTask(dumpBuilderFn, "storage_dump", 16384, nullptr, 1, 0, STACK_PSRAM)) {
            s_dumpBuilding.store(false, std::memory_order_release);
            warn("dump builder spawn failed — retrying next pass\n");
        }
    }
    if (s_dumpBuilding.load(std::memory_order_acquire) &&
        s_dumpDone.load(std::memory_order_acquire)) {
        if (s_dumpBuildGen == s_dumpGen.load(std::memory_order_relaxed) && dcHandle >= 0) {
            dcDumpQueue = std::move(s_dumpStaging);   /* adopt */
            dcDumpPos = 0;
            dcDumpPending = false;
        }                                             /* else: stale build — discard */
        s_dumpStaging.clear();
        s_dumpStaging.shrink_to_fit();
        s_dumpBuilding.store(false, std::memory_order_release);
    }

    size_t cap = itsSendBufSize(dcHandle);
    while (dcDumpPos < dcDumpQueue.size()) {
        const std::string& chunk = dcDumpQueue[dcDumpPos];
        if (cap && chunk.size() + 4 > cap) {
            /* Larger than the whole DC buffer — can never be enqueued (a single
               leaf value > ~16 KB). Skip it rather than wedge the stream
               forever; the subtree it carried is lost for this client. */
            warn("storage: dump chunk %u exceeds DC buffer %u, skipping\n",
                 (unsigned)chunk.size(), (unsigned)cap);
            dcDumpPos++;
            continue;
        }
        if (itsSpacesAvailable(dcHandle) < chunk.size()) break;   /* drain & retry next pass */
        if (itsSend(dcHandle, chunk.data(), chunk.size(), 0) != chunk.size()) break;
        dcDumpPos++;
    }

    if (dcDumpPos >= dcDumpQueue.size() && !dcDumpQueue.empty()) {
        dcDumpQueue.clear();
        dcDumpQueue.shrink_to_fit();   /* return the dump's RAM promptly */
        dcDumpPos = 0;
    }
}

/** Process incoming JSON from browser. Null = silent delete, values = storageSet. */
static void mergeIncomingPatch(cJSON* obj, const std::string& prefix) {
  cJSON* item;
  cJSON_ArrayForEach(item, obj) {
    std::string key = prefix.empty() ? item->string : prefix + "." + item->string;
    if (isSecret(key.c_str())) continue;
    if (isFw(key.c_str())) continue;   /* read-only firmware identity */
    if (isStorageDb(key.c_str())) continue;   /* read-only structured-DB registry */
    if (cJSON_IsNull(item)) {
      /* Browser-initiated silent delete: a SILENT DELETE op (no subscription
       * callbacks). Runs on the storage task, so this fast-paths straight into
       * storageApplyOps — externals + null-merge + save-timer all handled there. */
      std::string b;
      b.push_back(OP_F_SILENT);
      opAppendDelete(b, key.c_str());
      storageSubmit(std::move(b), /*sync=*/true);
    } else if (cJSON_IsObject(item)) {
      mergeIncomingPatch(item, key);
    } else if (cJSON_IsArray(item)) {
      storageSetTree(key.c_str(), cJSON_Duplicate(item, true));
    } else if (cJSON_IsNumber(item)) {
      storageSet(key.c_str(), item->valueint);
    } else if (cJSON_IsString(item)) {
      storageSet(key.c_str(), item->valuestring);
    }
  }
}

static void dcHandleMessage(int handle, const char* text, size_t len) {
    if (len == 10 && memcmp(text, "{\"ping\":1}", 10) == 0) {
        itsSend(handle, "{\"pong\":1}", 10, 0);
        return;
    }
    if (len == 10 && memcmp(text, "{\"save\":1}", 10) == 0) {
        requestSave();   /* off the storage task — never fs I/O on the poll loop */
        return;
    }
    /* The borrowed packet block carries no NUL terminator — parse by length. */
    cJSON* root = cJSON_ParseWithLength(text, len);
    if (!root) return;
    /* {"fetch":"<store-prefix>"} — the client opened a conversation: ship that
     * instance's records once, and mark it open so its live changes mirror. An
     * empty/absent fetch closes (stops mirroring bodies). */
    cJSON* fetch = cJSON_GetObjectItem(root, "fetch");
    if (fetch && cJSON_IsString(fetch)) {
        if (*fetch->valuestring) dcShipStorePrefix(fetch->valuestring);
        else                     dcOpenPrefix.clear();
        cJSON_Delete(root);
        return;
    }
    mergeIncomingPatch(root, "");
    cJSON_Delete(root);
}

static void dcPollConfig() {
    if (dcHandle < 0) return;
    /* Zero-copy receive: take ownership of each inbound JSON body (no copy, no
       static buffer, no size cap — a >8 KB browser patch now applies), parse it
       by length, then free. */
    void* p = nullptr;
    size_t n = 0;
    while (itsRecvRef(dcHandle, &p, &n, 0)) {
        dcHandleMessage(dcHandle, (const char*)p, n);
        free(p);
    }
}

/* ---- ITS server callbacks ---- */

static int storageItsConnect(int handle, const void* data, size_t len) {
    (void)data; (void)len;
    /* webrtc router already enforces single-session + auth. Just accept. */
    if (dcHandle >= 0) {
        /* Shouldn't happen — router enforces one DC per label, and label
           mapping here is one-to-one. Defensive: reject. */
        warn("storage: unexpected second DC, rejecting\n");
        return -1;
    }
    dcHandle = handle;
    s_dumpGen.fetch_add(1, std::memory_order_relaxed);   /* new session — stale builds discard */
    /* Defer the dump to the task loop so we ack the connect immediately. The
       ack is only sent once this callback returns (its.cpp), and the browser
       can't drain the dump stream until it's acked — so building/streaming here
       would blow the client's 3 s connect timeout and freeze the inbox drain. */
    dcDumpPending = true;
    return 0;
}

static void storageItsDisconnect(int ref) {
    (void)ref;
    dcHandle = -1;
    dcOpenPrefix.clear();
    s_dumpGen.fetch_add(1, std::memory_order_relaxed);   /* orphan any in-flight build */
    dcDumpPending = false;
    dcDumpQueue.clear();
    dcDumpQueue.shrink_to_fit();
    dcDumpPos = 0;
    if (dcPendingPatch) {
        cJSON_Delete(dcPendingPatch);
        dcPendingPatch = nullptr;
    }
}

/* ---- Task function ---- */

static void storageTaskFn(void* arg) {
    /* Re-home the config tree onto this task. storageLoad() parsed cfgRoot on
     * main_task (which self-deletes → the tree shows under (deleted) in `top`).
     * A deep-copy here re-attributes the long-lived tree to `storage`. Pure
     * memory (no fs I/O), so it can't wedge itsPoll the way running storageLoad
     * on this task would. Measured cost: ~300 B stack, depth 9, ~1.4k nodes —
     * far under this task's headroom, despite the dcFlushPatch caveat. */
    {
        CFG_LOCK();
        cJSON* dup = cJSON_Duplicate(cfgRoot, true);
        cJSON* old = nullptr;
        if (dup) { old = cfgRoot; cfgRoot = dup; }
        CFG_UNLOCK();
        if (old) cJSON_Delete(old);
    }

    /* The storage actor's inbox carries op messages (STORAGE_OP_PORT) and the
     * change self-sends from the "" browser-sync sub. Foreign writers block on
     * sync delivery, so the inbox can't truly overflow, but a generous depth
     * keeps a ~1 Hz multi-producer stats burst flowing without back-pressure.
     * Slots are pointers now (~4 B). */
    /* Inbox guard (Kconfig, default 192 KB): foreign-task op lists (a big
     * storageSetTree/storageCopy subtree serialized as one message) AND the
     * largest single published value must fit — Nomad page bodies run up to
     * NOMAD_MAX_PAGE_PUBLISH (128 KB). Browser patches never travel here
     * (they run on this task and fast-path). */
    itsServerInit(CONFIG_SPANGAP_STORAGE_OP_MSG_MAX, CONFIG_SPANGAP_STORAGE_NOTIFY_INBOX);
    itsOnAux(STORAGE_OP_PORT, onStorageOp);   /* receive config write op lists */
    /* Packet-mode: each DC message is one JSON body (dump, patch, ping).
     * toSize=48K holds the largest browser-originated patch — the IANA
     * timezone DB is ~40K when flattened. fromSize=16K: the full dump now
     * STREAMS as multiple chunks (dcBuildDump/dcPumpDump), so the server→client
     * buffer no longer has to hold the whole config tree — the saved-announce
     * stores (rnsd, lxmf) blow past any fixed cap. SCTP fragments both on the
     * wire; the webrtc router reassembles inbound into one packet. */
    /* Packet link (ITS_PACKET): each DC message is one heap-borrowed JSON body
     * (dump chunk, patch, ping). toCap 64K = browser→device patch window;
     * fromCap 256K = device→browser dump/patch window (lazy — idle ≈ 0, not a
     * permanent reservation). maxMsg 256K is the per-message size guard, now at
     * the lifted browser ceiling (SDP a=max-message-size:262144) so a single
     * large config patch (e.g. a big Nomad page) rides one message; the router
     * reassembles inbound SCTP fragments into one packet and sizes its recv
     * buffer from this guard (D7). DC_PATCH_MAX derives from it below. */
    itsServerPortOpen(STORAGE_CONFIG_PORT, ITS_PACKET, 1, 65536, 262144,
                      /*depth=*/0, /*maxMsg=*/262144);
    itsServerOnConnect(STORAGE_CONFIG_PORT, storageItsConnect);
    itsServerOnDisconnect(STORAGE_CONFIG_PORT, storageItsDisconnect);

    /* Subscribe to all config changes for DC coalescing */
    storageSubscribeChanges("", ON_CHANGE {
        dcAccumulateChange(key, val);
    });

    info("ready\n");

    for (;;) {
        int64_t it0 = esp_timer_get_time();
        bool dc_active = (dcHandle >= 0);   /* gate the stall warn on entry state:
                                               a fresh connect unblocks a MAX-delay
                                               poll and would read as a false stall */
        TickType_t timeout = dc_active ? pdMS_TO_TICKS(10) : portMAX_DELAY;
        int drained = 0;
        while (itsPoll(timeout)) { timeout = 0; drained++; }
        int64_t t_poll = esp_timer_get_time();
        dcPollConfig();
        int64_t t_cfg = esp_timer_get_time();
        dcPumpDump();
        int64_t t_dump = esp_timer_get_time();
        /* Hold patches until the dump drains: a chunk carries the snapshot
           value, a patch the newer one, so the patch must land after its
           chunk. dcAccumulateChange keeps coalescing meanwhile. */
        if (!dcDumpInProgress()) dcFlushPatch();
        int64_t t_end = esp_timer_get_time();
        /* Sub-500ms stalls are routine (a flash/SD write burst disables the cache
         * for both cores) and only noise at warn — log them at debug and keep warn
         * for the ones long enough to actually hurt. */
        if (dc_active && (t_end - it0) > ACTOR_STALL_WARN_US) {
            if ((t_end - it0) > ACTOR_STALL_LOUD_US)
                warn("actor stall %lldms: applyPoll=%lld(ops=%d) cfgPoll=%lld dump=%lld patch=%lld\n",
                     (t_end - it0) / 1000, (t_poll - it0) / 1000, drained,
                     (t_cfg - t_poll) / 1000, (t_dump - t_cfg) / 1000,
                     (t_end - t_dump) / 1000);
            else
                dbg("actor stall %lldms: applyPoll=%lld(ops=%d) cfgPoll=%lld dump=%lld patch=%lld\n",
                    (t_end - it0) / 1000, (t_poll - it0) / 1000, drained,
                    (t_cfg - t_poll) / 1000, (t_dump - t_cfg) / 1000,
                    (t_end - t_dump) / 1000);
        }
    }
}

/* Module config version. Bump when adding/changing defaults. See duckdns.cpp. */
#define STORAGE_VERSION 1

void storageInit() {
    int v = storageGetInt("s.storage.version", 0);
    if (v < STORAGE_VERSION) {
        storageBegin();
        storageDefault("s.storage.flash_delay", 60);
        storageSet("s.storage.version", STORAGE_VERSION);
        storageEnd();
    }

    /* All three storage tasks run on CORE 0, off the LCD's core (the UI task is
     * prio-1 core-1). Same priority means same-core storage work — the save
     * worker's gzip deflate above all, but also the actor's op-apply and the
     * notify sends — time-slices with LVGL rendering and stalls the UI for the
     * duration (~0.5 s while a busy conversation flushes). On core 0 (with rnsd
     * prio-2 / lxmf / wifi) the LCD keeps core 1 to itself; the only remaining
     * coupling is the brief CFG_LOCK a UI read takes, which is cross-core. */

    /* persist worker: owns the blocking fs writes + gzip so the storage task's
     * poll loop never stalls on them. Spawn before the storage task so it's ready
     * for the first save poke (the save timer can only fire 60s+ after boot). */
    saveWorkerHandle = spawnTask(saveWorkerFn, "storage_save", 8192, nullptr, 1, 0);

    /* notify worker: owns the blocking remote-subscriber CHANGED sends so a
     * flooded subscriber can't stall the storage actor's op-inbox drain. Spawn
     * before the storage task so its first subscribe/change dispatches here. */
    notifyQueueMux = xSemaphoreCreateMutex();
    notifyWorkerHandle = spawnTask(notifyWorkerFn, "storage_notify", 8192, nullptr, 1, 0);

    /* storage task: PSRAM stack (config WS + ITS, no direct file I/O) */
    storageHandle = spawnTask(storageTaskFn, "storage", 8192, nullptr, 1, 0);

    /* Flush any dirt storageLoad left behind (the lxmf monolith split marks
     * externals dirty + pendingDelete + rootDirty). Normal boots have none —
     * this is a no-op then. Without the kick, migration state would sit
     * unflushed until the first ordinary write arms the save timer, and a
     * reboot before that would just re-run the split (harmless, but the
     * monolith would never actually retire). */
    bool pending;
    CFG_LOCK();
    pending = rootDirty;
    for (auto& e : externals) pending = pending || e.dirty || e.pendingDelete;
    pending = pending || storageDbAnyDirty();
    CFG_UNLOCK();
    if (pending) requestSave();
}
