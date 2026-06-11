/**
 * storage — config store.
 *
 * Config: cJSON tree in RAM, backed by JSON on /state.
 * All writes go through a patch tree (RFC 7396 merge-patch). commit() merges
 * the patch into cfgRoot, fires subscriptions, coalesces WS output, and
 * triggers the save timer — atomically.
 *
 * storageBegin()/storageEnd() bracket explicit transactions.
 * Without them, storageSet() is auto-commit (one patch per call).
 *
 * Thread safety: a recursive mutex (cfgMux) protects cfgRoot, txPatch, and
 * txDepth.  storageBegin() acquires, storageEnd() releases.  All public
 * readers (storageGetInt, storageGetStr, …) lock around tree access.
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
#include "fs.h"
#include "log.h"
#include "cli.h"
#include "its.h"
#include "compat.h"

#include <string>
#include <vector>
#include <algorithm>
#include <functional>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>
#include "cJSON.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"

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
static cJSON* txPatch = nullptr;        /* transaction write accumulator */
static int txDepth = 0;                 /* transaction nesting depth */
static int silentDepth = 0;             /* >0 suppresses change subscriptions */
static SemaphoreHandle_t cfgMux = nullptr;  /* recursive mutex protecting cfgRoot + txPatch */

#define CFG_LOCK()   xSemaphoreTakeRecursive(cfgMux, portMAX_DELAY)
#define CFG_UNLOCK() xSemaphoreGiveRecursive(cfgMux)

static bool savePending = false;
static esp_timer_handle_t saveTimer = nullptr;

/* ---- Task state ---- */

static TaskHandle_t storageHandle = nullptr;
/* Persist worker: owns the blocking fs writes (writeSettingsFile). The storage
 * task runs itsPoll and must NEVER do fs I/O around its poll loop — each
 * proxyOp parks the caller in xSemaphoreTake(pickupSem, portMAX_DELAY) for the
 * whole op, and writeSettingsFile chains one per dirty external + root.json. On
 * the storage task that blocks its inbox drain → "notify drop … → [storage]"
 * floods (worst with many lxmf externals). So saves run here, off the loop. */
static TaskHandle_t saveWorkerHandle = nullptr;
static int dcHandle = -1;               /* single packet-mode DC client */
static cJSON* dcPendingPatch = nullptr; /* outgoing coalescing */

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
 *  notification keys are already 128 bytes (see spangap-core CLAUDE.md);
 *  this aligns the path parser with that. */
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

/* ---- Deep merge (RFC 7396, with array-element extension) ---- */

static void deepMerge(cJSON* dst, const cJSON* src);

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

/** Walk leaves for CLI output (show / storageList). */
static void walkTreePrint(cJSON* node, const char* prefix, cli_write_fn write) {
  walkLeaves(node, prefix, [](const char* key, const char* val, void* ctx) {
    auto write = (cli_write_fn)ctx;
    char line[192];
    int n = snprintf(line, sizeof(line), "%s = %s\n", key, val);
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

/* The fw.* subtree is immutable firmware identity (CONFIG_SPANGAP_FW_*),
 * synthesized into the browser dump (dcBuildDump) straight from ROM — it
 * never lives in cfgRoot, is never persisted, and is read-only: inbound
 * patches and `set fw.* ...` are rejected. */
static bool isFw(const char* key) {
  return strcmp(key, "fw") == 0 || strncmp(key, "fw.", 3) == 0;
}

/** Read entire file into malloc'd buffer (NUL-terminated). Uses fs API. */
static char* readFileStr(const char* path) {
  struct stat st;
  if (fs_stat(path, &st) != 0 || st.st_size <= 0) return nullptr;
  int f = fs_open(path, "rb");
  if (f < 0) return nullptr;
  char* buf = (char*)malloc(st.st_size + 1);
  if (!buf) { fs_close(f); return nullptr; }
  fs_read(buf, 1, st.st_size, f);
  buf[st.st_size] = '\0';
  fs_close(f);
  return buf;
}

/** Atomic write of `text` to `path` via `<path>.new` + rename. */
static bool atomicWriteJson(const char* path, const char* text) {
  std::string tmp = std::string(path) + ".new";
  int f = fs_open(tmp.c_str(), "w");
  if (f < 0) return false;
  size_t len = strlen(text);
  bool ok = (fs_write(text, 1, len, f) == len);
  fs_close(f);
  if (ok) fs_rename(tmp.c_str(), path);
  else    fs_remove(tmp.c_str());
  return ok;
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
 *  Used so cJSON_Print of cfgRoot omits external blobs from root.json
 *  without copying the whole tree. Caller holds CFG_LOCK. */
static void withExternalsDetached(std::function<void()> fn) {
  struct save_t { cJSON* parent; std::string leaf; cJSON* item; };
  std::vector<save_t> saved;
  for (auto& ext : externals) {
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

/** Serialize one external's sub-tree at its prefix to its own file. */
static void writeExternalFile(const external_t& ext) {
  CFG_LOCK();
  cJSON* node = navigatePath(cfgRoot, ext.prefix.c_str());
  char* text = node ? cJSON_Print(node) : nullptr;
  CFG_UNLOCK();
  if (!text) return;
  atomicWriteJson(ext.path.c_str(), text);
  cJSON_free(text);
}

/* The non-external config tree lives at <stateDir>/storage/root.json,
 * alongside the per-prefix blobs in <stateDir>/storage/external/ — so
 * everything persisted is under storage/. */
#define ROOT_JSON_PATH "/storage/root.json"

/** Write root.json (cfgRoot minus external sub-trees). */
static void writeSettingsFileOnly() {
  CFG_LOCK();
  char* text = nullptr;
  withExternalsDetached([&]() {
    cJSON* out = cJSON_CreateObject();
    cJSON* s = cJSON_GetObjectItem(cfgRoot, "s");
    cJSON* sec = cJSON_GetObjectItem(cfgRoot, "secrets");
    if (s)   cJSON_AddItemToObject(out, "s",       cJSON_Duplicate(s, true));
    if (sec) cJSON_AddItemToObject(out, "secrets", cJSON_Duplicate(sec, true));
    text = cJSON_Print(out);
    cJSON_Delete(out);
  });
  CFG_UNLOCK();
  if (!text) return;
  atomicWriteJson(fsStatePath(ROOT_JSON_PATH).c_str(), text);
  cJSON_free(text);
}

static void writeSettingsFile() {
  /* Externals: process deletes (fs_remove + unregister) and dirty writes.
     Index-walk re-checking size() each step so a concurrent storageNewTreeFile
     push_back can't invalidate us; fs I/O stays OUTSIDE CFG_LOCK (writeExternalFile
     snapshots under the lock then writes lock-free), matching prior behaviour. */
  for (size_t i = 0; ; ) {
    bool doDelete = false, doWrite = false;
    external_t work;
    CFG_LOCK();
    if (i >= externals.size()) { CFG_UNLOCK(); break; }
    external_t& ext = externals[i];
    if (ext.pendingDelete) {
      work.path = ext.path;
      externals.erase(externals.begin() + i);   /* don't advance i */
      doDelete = true;
    } else if (ext.dirty) {
      ext.dirty = false;
      work.prefix = ext.prefix;
      work.path   = ext.path;
      doWrite = true;
      i++;
    } else {
      i++;
    }
    CFG_UNLOCK();
    if (doDelete)      fs_remove(work.path.c_str());
    else if (doWrite)  writeExternalFile(work);
  }

  if (rootDirty) {
    writeSettingsFileOnly();
    rootDirty = false;
  }
  savePending = false;
}

/* Persist worker loop: block until poked, then flush. ulTaskNotifyTake(pdTRUE)
 * coalesces any pokes that arrived during a flush into one extra pass. Not an
 * ITS task — blocking on fs I/O here harms nothing. */
static void saveWorkerFn(void*) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    writeSettingsFile();
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
  esp_timer_stop(saveTimer);
  int delaySec = storageGetInt("s.storage.flash_delay", 60);
  if (delaySec < 1) delaySec = 1;
  esp_timer_start_once(saveTimer, (int64_t)delaySec * 1000000);
  savePending = true;
}

/* ---- Config change subscriptions ---- */

#define STORAGE_MAX_SUBS     96
/* STORAGE_CHANGE_PORT in storage.h */

struct storage_sub_t {
  TaskHandle_t        task;
  storage_change_cb_t cb;
  char                scope[40];
};

static storage_sub_t subs[STORAGE_MAX_SUBS];
static int           subCount = 0;

/* Payload for aux message on STORAGE_CHANGE_PORT. Sized to fit in
 * ITS_MAX_MSG_DATA with room for hierarchical keys (deep dotted paths)
 * and reasonably long values. */
struct storage_change_msg_t {
  storage_change_cb_t cb;
  char                key[128];
  char                val[128];
};
static_assert(sizeof(storage_change_msg_t) <= ITS_MAX_MSG_DATA,
              "storage_change_msg_t exceeds ITS_MAX_MSG_DATA");

/* Aux handler installed on each subscribing task — unpacks and calls */
static void storageChangeDispatch(TaskHandle_t sender, const void* data, size_t len) {
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
    itsOnAux(STORAGE_CHANGE_PORT, storageChangeDispatch);

  auto& s = subs[subCount++];
  s.task = me;
  s.cb = cb;
  safeStrncpy(s.scope, scope, sizeof(s.scope));
}

void storageUnsubscribe(const char* scope) {
  if (!scope) return;
  TaskHandle_t me = xTaskGetCurrentTaskHandle();
  for (int i = 0; i < subCount; ) {
    if (subs[i].task == me && strcmp(subs[i].scope, scope) == 0) {
      subs[i] = subs[--subCount];  /* swap-with-last */
    } else {
      i++;
    }
  }
  /* Aux handler stays installed for the task — itsOnAux has no off
   * counterpart, and an idle dispatcher costs ~one strcmp per delivery. */
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
      if (!itsSendAuxByTaskHandle(subs[i].task, STORAGE_CHANGE_PORT, &msg, sizeof(msg),
                                  pdMS_TO_TICKS(10))) {
        const char* tn = pcTaskGetName(subs[i].task);
        warn("notify drop: %s=%s → [%s] (inbox full)\n", key, val, tn ? tn : "?");
      }
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

static void commitPatch() {
  /* Caller holds CFG_LOCK — protects cfgRoot, txPatch, txDepth from
     concurrent access.  ITS aux sends (10ms timeout) are bounded. */
  if (!txPatch) return;

  deepMerge(cfgRoot, txPatch);

  if (silentDepth == 0) {
    char pathBuf[128] = "";
    firePatchSubscriptions(txPatch, pathBuf, sizeof(pathBuf), 0);
  }

  if (cJSON_GetObjectItem(txPatch, "s") || cJSON_GetObjectItem(txPatch, "secrets")) {
    char routeBuf[128] = "";
    routePatchDirty(txPatch, routeBuf, sizeof(routeBuf), 0);
    startSaveTimer();
  }

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

/** Scan /state/storage/MODE/ for .json files; register each as an external.
 *  Filename's stem (sans .json) is the dot-path prefix where its content lives
 *  in cfgRoot. Subdir under storage/ is the "mode" — only "external" today. */
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
      const char* dot = strrchr(ent.name, '.');
      if (!dot || strcmp(dot, ".json") != 0) continue;
      external_t ext;
      ext.prefix.assign(ent.name, dot - ent.name);
      ext.path  = std::string(dirPath) + "/" + ent.name;
      ext.dirty = false;
      externals.push_back(std::move(ext));
    }
    fs_closedir(dh);
  }
  /* Longest prefix first — needed for correct dirty routing if two externals
   * ever overlap (e.g. "s.foo" and "s.foo.bar"). */
  std::sort(externals.begin(), externals.end(),
            [](const external_t& a, const external_t& b) {
              return a.prefix.size() > b.prefix.size();
            });
}

/** Read each external's file and attach its content to cfgRoot. */
static void loadExternals() {
  for (auto& ext : externals) {
    char* text = readFileStr(ext.path.c_str());
    if (!text) continue;
    cJSON* node = cJSON_Parse(text);
    free(text);
    if (node) attachAtPath(ext.prefix.c_str(), node);
  }
}

void storageLoad() {
  if (!cfgMux) cfgMux = xSemaphoreCreateRecursiveMutex();
  if (cfgRoot) cJSON_Delete(cfgRoot);

  /* Ensure <stateDir>/storage/ (and storage/external/) exist before any
   * read/write — a freshly-seeded SD store may only have what factory_state
   * shipped, and atomicWriteJson() does not create parent dirs. */
  fs_mkdirp(fsStatePath("/storage/external").c_str());

  /* Load <stateDir>/storage/root.json — the single source of truth. */
  char* text = readFileStr(fsStatePath(ROOT_JSON_PATH).c_str());
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

  /* Drop a stale temp from a crash between write and rename (atomicWriteJson
   * writes "<path>.new"). With the FAT-safe rename this is rare, but cheap
   * to clear. */
  fs_remove(fsStatePath(ROOT_JSON_PATH ".new").c_str());
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

std::string storageGetStr(const char* key, const char* def) {
  CFG_LOCK();
  cJSON* node = resolveKey(key);
  std::string out;
  if (node && cJSON_IsString(node))      out = node->valuestring;
  else if (node && cJSON_IsNumber(node)) out = std::to_string(node->valueint);
  else                                   out = def ? def : "";
  CFG_UNLOCK();
  return out;
}

void storageSet(const char* key, int val) {
  char buf[16];
  snprintf(buf, sizeof(buf), "%d", val);
  storageSet(key, buf);
}

void storageSet(const char* key, const char* val) {
  CFG_LOCK();
  /* Dedup: skip if current committed value equals new value. Saves notify
   * floods when e.g. browser rapid-fires the same signal repeatedly. Compare
   * against the cJSON string directly so we don't truncate long values. */
  {
    cJSON* node = resolveKey(key);
    if (node && cJSON_IsString(node) && strcmp(node->valuestring, val) == 0) {
      CFG_UNLOCK();
      return;
    }
  }
  bool autoCommit = (txDepth == 0);
  if (autoCommit) storageBegin();

  char leaf[96];  /* see navigatePath */
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

/* Default writes don't fire change subscriptions: by definition the value
 * was absent before, and these are firmware-bundled defaults being seeded
 * once (typically dozens at first boot), not real config changes. Without
 * this, broad subscriptions like net's "s." flood inboxes during install. */
bool storageDefault(const char* key, const char* val) {
  CFG_LOCK();
  bool exists = storageExists(key);
  if (!exists) {
    silentDepth++;
    storageSet(key, val);
    silentDepth--;
  }
  CFG_UNLOCK();
  return !exists;
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
  silentDepth++;
  storageBegin();
  bool wrote = defaultTreeImpl(prefixEmpty ? "" : prefix, json, prefixEmpty);
  storageEnd();
  silentDepth--;
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
  CFG_LOCK();
  bool autoCommit = (txDepth == 0);
  if (autoCommit) storageBegin();

  char leaf[96];  /* see navigatePath */
  cJSON* parent = navigateOrCreate(txPatch, key, leaf, sizeof(leaf));
  if (parent) {
    cJSON_DeleteItemFromObject(parent, leaf);
    cJSON_AddNullToObject(parent, leaf);
  }

  if (autoCommit) storageEnd();
  CFG_UNLOCK();
}

void storageSetTree(const char* key, cJSON* val) {
  if (!val) return;
  CFG_LOCK();
  bool autoCommit = (txDepth == 0);
  if (autoCommit) storageBegin();

  char leaf[96];  /* see navigatePath */
  cJSON* parent = navigateOrCreate(txPatch, key, leaf, sizeof(leaf));
  if (parent) {
    cJSON_DeleteItemFromObject(parent, leaf);
    cJSON_AddItemToObject(parent, leaf, val);
  } else {
    cJSON_Delete(val);  /* ownership was transferred, clean up on failure */
  }

  if (autoCommit) storageEnd();
  CFG_UNLOCK();
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

  /* Delete via the same null-in-txPatch → commit path as storageUnset
     (RFC 7396: a null member removes the member, whole subtree included —
     deepMerge() cJSON_DeleteItemFromObject). This routes the deletion
     through firePatchSubscriptions + the coalesced, back-pressure-RETRIED
     browser patch (dcFlushPatch), instead of a fire-and-forget
     itsSend(...,0) whose drop under DC back-pressure left clients showing
     deleted conversations until a reload. commitPatch handles
     persistence (routePatchDirty + startSaveTimer) for s./secrets. */
  CFG_LOCK();
  /* If this key (or an ancestor) owns an external file, drop it on next flush
     — otherwise the on-disk .json survives and resurrects on reboot. */
  markExternalsDeletedUnder(keyOrPrefix);
  bool autoCommit = (txDepth == 0);
  if (autoCommit) storageBegin();

  char leaf[96];  /* see navigatePath */
  cJSON* parent = navigateOrCreate(txPatch, keyOrPrefix, leaf, sizeof(leaf));
  if (parent) {
    cJSON_DeleteItemFromObject(parent, leaf);
    cJSON_AddNullToObject(parent, leaf);
  }

  if (autoCommit) storageEnd();
  CFG_UNLOCK();
}

void storageSave() {
  if (saveTimer) esp_timer_stop(saveTimer);
  writeSettingsFile();
}

void storageNewTreeFile(const char* prefix) {
  std::string p = stripDots(prefix);
  if (p.empty()) return;

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
  char leaf[96];  /* see navigatePath */
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

/* ---- CLI commands ---- */

static void cmdSet(const char* a) {
    if (cliWantsHelp(a)) { cliPrintf("%-*s set config variable\n", CLI_HELP_COL, "set <key>=<value>"); return; }
    const char* eq = strchr(a, '=');
    if (!eq || eq == a) { cliPrintf("usage: set <key>=<value>\n"); return; }
    /* Match storage's full-key capacity (storage_change_msg_t::key is 128B).
     * Used to be 48 — small enough that `set s.lxmf.id.0.msgs.<64-hex>.<field>=…`
     * was rejected at the CLI before storageSet ever ran. */
    char key[128];
    size_t klen = eq - a;
    while (klen > 0 && a[klen - 1] == ' ') klen--;
    if (klen == 0 || klen >= sizeof(key)) { cliPrintf("err: key empty or too long\n"); return; }
    memcpy(key, a, klen); key[klen] = '\0';
    if (isFw(key)) { cliPrintf("err: fw.* is read-only firmware identity (compile-time)\n"); return; }
    const char* val = eq + 1;
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
            if (val) cliPrintf("%s = %s\n", a, val);
        }
        CFG_UNLOCK();
        return;
    }

    /* Partial last-segment match */
    bool found = false;
    const char* lastDot = strrchr(a, '.');
    if (lastDot) {
        /* Match storage's full-key capacity. Was 64 — too small for paths
         * with 64-char SHA-256 hex segments. */
        char parentPath[128];
        size_t parentLen = lastDot - a;
        if (parentLen >= sizeof(parentPath)) { CFG_UNLOCK(); cliPrintf("(prefix too long)\n"); return; }
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
                    if (cJSON_IsObject(item) || cJSON_IsArray(item))
                        walkTreePrint(item, key, write);
                    else {
                        char vb[32];
                        const char* v = nullptr;
                        if (cJSON_IsString(item)) v = item->valuestring;
                        else if (cJSON_IsNumber(item)) { snprintf(vb, sizeof(vb), "%d", item->valueint); v = vb; }
                        if (v) cliPrintf("%s = %s\n", key, v);
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
                    if (v) cliPrintf("%s = %s\n", item->string, v);
                }
                found = true;
            }
        }
    }
    CFG_UNLOCK();
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
static void dcDumpEmit(cJSON** batch, size_t* batchLen) {
    if (*batch && (*batch)->child) {
        char* text = cJSON_PrintUnformatted(*batch);
        if (text) {
            dcDumpQueue.emplace_back(text);
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
static void dcBuildDump() {
    dcDumpQueue.clear();
    dcDumpPos = 0;

    CFG_LOCK();
    cJSON* clone = cJSON_Duplicate(cfgRoot, true);
    CFG_UNLOCK();
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
    dcDumpQueue.emplace_back("{\"__dump\":\"b\"}");

    cJSON* batch = cJSON_CreateObject();
    size_t batchLen = 0;
    const char* stack[DC_DUMP_DEPTH];
    for (cJSON* c = clone->child; c; c = c->next)
        dcStreamNode(&batch, &batchLen, stack, 0, c->string, c);
    dcDumpEmit(&batch, &batchLen);
    cJSON_Delete(batch);
    cJSON_Delete(clone);

    dcDumpQueue.emplace_back("{\"__dump\":\"e\"}");
}

/** Accumulate a changed key into dcPendingPatch for coalesced output. */
static void dcAccumulateChange(const char* key, const char* val) {
    (void)val;
    if (dcHandle < 0) return;
    if (isSecret(key)) return;

    /* The key may be gone (storageUnset / storageDeleteTree removed it
       before firing callbacks). Previously we skipped — so deletions were
       never echoed and a deleted conversation lingered in open clients
       until a full reload. Instead echo an explicit null at the key: the
       coalesced patch is retried under back-pressure (never dropped), so
       the browser reliably drops the (sub)tree. */
    CFG_LOCK();
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

/** Flush accumulated changes to the browser as one DC packet. On back-
 *  pressure leave the patch intact and retry next pass — never drop. */
static constexpr size_t DC_PATCH_MAX = 15500;  /* one 16K fromSize buffer, header(4)+slack */

static void dcFlushPatch() {
    if (!dcPendingPatch || dcHandle < 0) return;
    char* text = cJSON_PrintUnformatted(dcPendingPatch);
    if (!text) return;
    size_t len = strlen(text);

    /* Patch outgrew what we'll send in one packet: drop and warn.
       Incremental UI state may become stale until the next change forces
       a fresh patch; full re-dumps from the storage task would blow the
       stack (cJSON_Duplicate of cfgRoot is deeply recursive). */
    if (len > DC_PATCH_MAX) {
        warn("storage: patch %u > %u, dropping (clients may need reload)\n",
             (unsigned)len, (unsigned)DC_PATCH_MAX);
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
    if (dcDumpPending) { dcBuildDump(); dcDumpPending = false; }

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
    if (cJSON_IsNull(item)) {
      /* Silent delete (no subscription callbacks) */
      CFG_LOCK();
      markExternalsDeletedUnder(key.c_str());   /* drop external file if this is one */
      deleteFromTree(cfgRoot, key.c_str());
      CFG_UNLOCK();
      if (strncmp(key.c_str(), "s.", 2) == 0) startSaveTimer();
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
    cJSON* root = cJSON_Parse(text);
    if (!root) return;
    mergeIncomingPatch(root, "");
    cJSON_Delete(root);
}

static void dcPollConfig() {
    if (dcHandle < 0) return;
    /* Packet-mode itsRecv: one JSON body per call. Size to fit the
       largest browser-originated patch we'd expect. */
    static char buf[8192];
    for (;;) {
        size_t n = itsRecv(dcHandle, buf, sizeof(buf) - 1, 0);
        if (n == 0) break;
        buf[n] = '\0';
        dcHandleMessage(dcHandle, buf, n);
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

    /* "" subscription for browser sync means every storageSet fires an aux
     * into our inbox — including changes we ourselves push when processing
     * browser-originated patches. UI bursts (page load, opening cli/log
     * windows) AND the ~1 Hz multi-producer stats publish (rnsd + every iface
     * + auto + tcp peers + lora + gps, >100 keys at once) generate many writes
     * back-to-back; default depth 8 overflows. Drops show up as "[storage]
     * notify drop … (inbox full)" warns. Depth is a PSRAM-backed Kconfig knob
     * (~344 B/slot) so the worst-case burst fits in one drain window. */
    itsServerInit(0, CONFIG_SPANGAP_STORAGE_NOTIFY_INBOX);
    /* Packet-mode: each DC message is one JSON body (dump, patch, ping).
     * toSize=48K holds the largest browser-originated patch — the IANA
     * timezone DB is ~40K when flattened. fromSize=16K: the full dump now
     * STREAMS as multiple chunks (dcBuildDump/dcPumpDump), so the server→client
     * buffer no longer has to hold the whole config tree — the saved-announce
     * stores (rnsd, lxmf) blow past any fixed cap. SCTP fragments both on the
     * wire; the webrtc router reassembles inbound into one packet. */
    itsServerPortOpen(STORAGE_CONFIG_PORT, /*packetBased=*/true, 1, 49152, 16384);
    itsServerOnConnect(STORAGE_CONFIG_PORT, storageItsConnect);
    itsServerOnDisconnect(STORAGE_CONFIG_PORT, storageItsDisconnect);

    /* Subscribe to all config changes for DC coalescing */
    storageSubscribeChanges("", ON_CHANGE {
        dcAccumulateChange(key, val);
    });

    info("ready\n");

    for (;;) {
        TickType_t timeout = (dcHandle >= 0) ? pdMS_TO_TICKS(10) : portMAX_DELAY;
        while (itsPoll(timeout)) { timeout = 0; }
        dcPollConfig();
        dcPumpDump();
        /* Hold patches until the dump drains: a chunk carries the snapshot
           value, a patch the newer one, so the patch must land after its
           chunk. dcAccumulateChange keeps coalescing meanwhile. */
        if (!dcDumpInProgress()) dcFlushPatch();
    }
}

/* Module config version. Bump when adding/changing defaults. See duckdns.cpp. */
#define STORAGE_VERSION 1

void storageInit() {
    int v = storageGetInt("s.storage.version", 0);
    if (v < STORAGE_VERSION) {
        storageDefault("s.storage.flash_delay", 60);
        storageSet("s.storage.version", STORAGE_VERSION);
    }

    /* persist worker: owns the blocking fs writes so the storage task's poll
     * loop never stalls on them. Spawn before the storage task so it's ready
     * for the first save poke (the save timer can only fire 60s+ after boot). */
    saveWorkerHandle = spawnTask(saveWorkerFn, "storage_save", 8192, nullptr, 1, 1);

    /* storage task: PSRAM stack (config WS + ITS, no direct file I/O) */
    storageHandle = spawnTask(storageTaskFn, "storage", 8192, nullptr, 1, 1);
}
