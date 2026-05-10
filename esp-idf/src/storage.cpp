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
 * instead of settings.json). New modes (e.g. "flash-only") can be added later
 * by handling more subdirs in scanExternals(). Drop a file in the build tree
 * and it just appears — no compile-time registration. */
struct external_t {
  std::string prefix;   /* dot-path key prefix, e.g. "s.time.zones" */
  std::string path;     /* on-disk file, e.g. "/state/storage/external/s.time.zones.json" */
  bool        dirty;    /* sub-tree at prefix changed since last flush */
};
static std::vector<external_t> externals;
static bool rootDirty = false;        /* settings.json needs rewrite */

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
    if (segLen == 0 || segLen >= 48) return nullptr;
    if (!dot) {
      memcpy(outLeaf, p, segLen);
      outLeaf[segLen] = '\0';
      return node;
    }
    char seg[48];
    memcpy(seg, p, segLen);
    seg[segLen] = '\0';
    node = cJSON_GetObjectItem(node, seg);
    if (!node) return nullptr;
    p = dot + 1;
  }
}

/** Detach all external sub-trees from cfgRoot, run `fn`, then reattach.
 *  Used so cJSON_Print of cfgRoot omits external blobs from settings.json
 *  without copying the whole tree. Caller holds CFG_LOCK. */
static void withExternalsDetached(std::function<void()> fn) {
  struct save_t { cJSON* parent; std::string leaf; cJSON* item; };
  std::vector<save_t> saved;
  for (auto& ext : externals) {
    char leaf[48];
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

/** Write settings.json (cfgRoot minus external sub-trees). */
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
  atomicWriteJson(FS_STATE "/settings.json", text);
  cJSON_free(text);
}

static void writeSettingsFile() {
  for (auto& ext : externals) {
    if (!ext.dirty) continue;
    writeExternalFile(ext);
    ext.dirty = false;
  }
  if (rootDirty) {
    writeSettingsFileOnly();
    rootDirty = false;
  }
  savePending = false;
}

/* STORAGE_SAVE_PORT in storage.h */

static void saveTimerCb(void* arg) {
  if (!storageHandle) return;
  itsSendAuxByTaskHandle(storageHandle, STORAGE_SAVE_PORT, nullptr, 0, 0);
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

/* Payload for aux message on STORAGE_CHANGE_PORT */
struct storage_change_msg_t {
  storage_change_cb_t cb;
  char                key[48];
  char                val[44];
};

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
  char leaf[48];
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
    snprintf(dirPath, sizeof(dirPath), FS_STATE "/storage/%s", mode);
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

  /* Load /state/settings.json — the single source of truth. */
  char* text = readFileStr(FS_STATE "/settings.json");
  if (text) {
    cfgRoot = cJSON_Parse(text);
    free(text);
  }
  if (!cfgRoot) cfgRoot = cJSON_CreateObject();

  /* External files: scan /state/storage/<mode>/, register, then load each
   * file's contents into cfgRoot at its prefix. Externals overwrite anything
   * at the same path that may have been in settings.json. */
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

  fs_remove(FS_STATE "/settings.new");
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

  char leaf[48];
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

  char leaf[48];
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

  /* Notify browser with {path: null} — one packet, non-blocking. */
  if (dcHandle >= 0) {
    cJSON* json = buildNullJson(keyOrPrefix);
    if (json) {
      char* text = cJSON_PrintUnformatted(json);
      cJSON_Delete(json);
      if (text) {
        itsSend(dcHandle, text, strlen(text), 0);
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

/* ---- Config DataChannel handling (single-session via WebRTC router) ---- */

/* One packet per JSON message. The webrtc router (webrtc_task.cpp) holds
 * the single-session constraint; by the time a DC reaches us, the peer
 * is authenticated. We carry the single client handle, coalesce changes
 * into one patch tree, and flush it as one packet per main-loop pass. */

static void dcSendFullDump(int handle) {
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
    /* Generous timeout — first dump must land even if the router is
       momentarily backed up. */
    itsSend(handle, text, strlen(text), pdMS_TO_TICKS(500));
    cJSON_free(text);
}

/** Accumulate a changed key into dcPendingPatch for coalesced output. */
static void dcAccumulateChange(const char* key, const char* val) {
    (void)val;
    if (dcHandle < 0) return;
    if (isSecret(key)) return;

    /* Check cfgRoot — storageUnset removes the key before firing callbacks,
       so if the key is gone we skip (matches current behavior: unset is not
       echoed to browser). */
    CFG_LOCK();
    cJSON* node = navigatePath(cfgRoot, key);
    if (!node) { CFG_UNLOCK(); return; }

    if (!dcPendingPatch) dcPendingPatch = cJSON_CreateObject();

    char leaf[48];
    cJSON* parent = navigateOrCreate(dcPendingPatch, key, leaf, sizeof(leaf));
    if (!parent) { CFG_UNLOCK(); return; }

    cJSON_DeleteItemFromObject(parent, leaf);
    bool deep = cJSON_IsObject(node) || cJSON_IsArray(node);
    cJSON_AddItemToObject(parent, leaf, cJSON_Duplicate(node, deep));
    CFG_UNLOCK();
}

/** Flush accumulated changes to the browser as one DC packet. On back-
 *  pressure leave the patch intact and retry next pass — never drop. */
static constexpr size_t DC_PATCH_MAX = 60000;  /* within SCTP 64KB max-message-size */

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

/** Process incoming JSON from browser. Null = silent delete, values = storageSet. */
static void mergeIncomingPatch(cJSON* obj, const std::string& prefix) {
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
        storageSave();
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
    dcSendFullDump(handle);
    return 0;
}

static void storageItsDisconnect(int ref) {
    (void)ref;
    dcHandle = -1;
    if (dcPendingPatch) {
        cJSON_Delete(dcPendingPatch);
        dcPendingPatch = nullptr;
    }
}

/* ---- Task function ---- */

static void storageSaveAux(TaskHandle_t, const void*, size_t) {
    writeSettingsFile();
}

static void storageTaskFn(void* arg) {
    /* "" subscription for browser sync means every storageSet fires an aux
     * into our inbox — including changes we ourselves push when processing
     * browser-originated patches. UI bursts (page load, opening cli/log
     * windows) generate many writes back-to-back; default depth 8 overflows.
     * Drops show up as "[storage] notify drop" warns when too small. */
    itsServerInit(0, 64);
    /* Packet-mode: each DC message is one JSON body (dump, patch, ping).
     * toSize=48K holds the largest browser-originated patch — the IANA
     * timezone DB is ~40K when flattened. fromSize=64K accommodates full
     * config dumps + coalesced multi-key patches. SCTP fragments both on
     * the wire; the webrtc router reassembles inbound into one packet. */
    itsServerPortOpen(STORAGE_CONFIG_PORT, /*packetBased=*/true, 1, 49152, 65536);
    itsServerOnConnect(STORAGE_CONFIG_PORT, storageItsConnect);
    itsServerOnDisconnect(STORAGE_CONFIG_PORT, storageItsDisconnect);
    itsOnAux(STORAGE_SAVE_PORT, storageSaveAux);

    /* Subscribe to all config changes for DC coalescing */
    storageSubscribeChanges("", ON_CHANGE {
        dcAccumulateChange(key, val);
    });

    info("ready\n");

    for (;;) {
        TickType_t timeout = (dcHandle >= 0) ? pdMS_TO_TICKS(10) : portMAX_DELAY;
        while (itsPoll(timeout)) { timeout = 0; }
        dcPollConfig();
        dcFlushPatch();
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

    /* storage task: PSRAM stack (config WS + ITS, no direct file I/O) */
    storageHandle = spawnTask(storageTaskFn, "storage", 8192, nullptr, 1, 1);
}
