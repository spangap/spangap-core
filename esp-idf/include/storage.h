/**
 * storage — config store + file I/O service.
 *
 * Config: cJSON tree in RAM, backed by JSON on /state.
 * All writes go through a patch tree (RFC 7396 merge-patch format).
 * commit() merges the patch into cfgRoot, fires subscriptions, coalesces
 * WS output, and triggers the save timer — atomically.
 *
 * storageBegin()/storageEnd() bracket explicit transactions. Without them,
 * storageSet() is auto-commit (one patch per call). Reads within a
 * transaction see their own writes.
 *
 * Keys starting with "s." are persisted to /state/settings.json.
 * Keys starting with "secrets." are persisted but never sent to the browser.
 * All other keys are ephemeral (in-memory only, lost on reboot).
 *
 * Browser config WebSocket (root path "/"):
 * - Device→browser: full nested JSON dump on connect, then coalesced merge-patches.
 * - Browser→device: nested JSON merge-patches. null = delete subtree (silent).
 * - Deletes via storageDeleteTree() do not fire storageSubscribeChanges callbacks.
 *
 * File I/O: POSIX-like API proxied to a DRAM-stack worker task.
 * PSRAM-stack tasks can safely call storageFopen/Fread/Fwrite/Fclose.
 */
#ifndef SECCAM_STORAGE_H
#define SECCAM_STORAGE_H

#include <stdint.h>
#include <stddef.h>
#include <sys/stat.h>
#include <cJSON.h>

/** Storage task's ITS server port for the /epl config WebSocket. */
static constexpr uint16_t STORAGE_EPL_PORT = 0;

/** Storage task's ITS aux ports. */
static constexpr uint16_t STORAGE_SAVE_PORT   = 43;  /* save-now signal */
static constexpr uint16_t STORAGE_CHANGE_PORT = 42;  /* change dispatch on subscriber tasks */

/** fs worker's ITS aux port: one-shot file ops (open/read/write/stat/...). */
static constexpr uint16_t FS_FILE_PORT = 1;

/* ---- Config types ---- */

enum cfg_type_t : uint8_t {
  CFG_INT = 'I',
  CFG_STR = 'S',
};

/* ---- Config API ---- */

/** Create storage task. Call after webInit(). */
void storageInit();

/** Load config: factory defaults from /fixed, then user overrides from /state. */
void storageLoad();

bool   storageExists(const char* key);
int    storageGetInt(const char* key, int def = 0);
void   storageGetStr(const char* key, char* out, size_t outLen, const char* def = "");
void   storageSet(const char* key, int val);
void   storageSet(const char* key, const char* val);
/** Set an arbitrary cJSON node (array, object, etc.) at a dot-notation key.
 *  Takes ownership of val — caller must not free it.
 *  Uses patch/commit: fires subscriptions and WS broadcast. */
void   storageSetTree(const char* key, cJSON* val);
/** Delete a single key via patch/commit. Fires storageSubscribeChanges with val="". */
void   storageUnset(const char* key);
/** Delete a key/subtree directly. No change callbacks. Sends null on WS. */
void   storageDeleteTree(const char* keyOrPrefix);
void   storageSave();                   /** Force immediate JSON write. */

/** Begin a transaction. All storageSet/Unset calls accumulate in a patch tree.
 *  storageEnd() commits atomically: one merge, one WS message, subscriptions
 *  fire once per key (final state only). Nestable. */
void   storageBegin();
void   storageEnd();

/** Copy all keys matching srcPrefix to dstPrefix.
 *  e.g., storageCopy("s.camera.", "camera.") copies s.camera.img.quality → camera.img.quality.
 *  If onlyIfTargetKeyExists, only overwrites keys that already exist at the destination.
 *  Uses begin/end internally for atomic commit. */
void   storageCopy(const char* srcPrefix, const char* dstPrefix, bool onlyIfTargetKeyExists = false);

/** Same as storageCopy but merges directly into cfgRoot without firing
 *  subscriptions or WS output. */
void   storageCopyNoNotify(const char* srcPrefix, const char* dstPrefix, bool onlyIfTargetKeyExists = false);
cfg_type_t storageGetType(const char* key);

/** Count consecutive numbered entries (0, 1, 2, ...) under a path.
 *  Works with both JSON arrays and objects with numeric keys.
 *  Prefix should end with dot, e.g., "s.web.map." → checks .0, .1, ... */
int    storageArrayCount(const char* prefix);

void   storageForEach(const char* prefix, void (*cb)(const char* key, const char* val));

/** Output function type for CLI output routing. */
typedef void (*cli_write_fn)(const char* data, size_t len);

void storageList(cli_write_fn write);
void storageRegisterCmds();

/* ---- Config change subscriptions ---- */

/** Callback for config changes. key is the full dot-notation key, val is the new value. */
typedef void (*storage_change_cb_t)(const char* key, const char* val);

/** Subscribe to config changes matching a scope prefix.
 *  Callback fires on the calling task's own stack (via itsPoll).
 *  Scope is prefix-matched: "s.camera.img" matches "s.camera.img.quality", etc.
 *  Empty scope "" matches all changes.
 *  Call from the task that should receive the callback (during init). */
void storageSubscribeChanges(const char* scope, storage_change_cb_t cb);

/** Convenience: lambda-friendly callback type */
#define ON_CHANGE [](const char* key, const char* val)

/* ---- File I/O API ---- */

/** Open a file via storage task. Returns handle (>= 0) or -1 on error. */
int    storageFopen(const char* path, const char* mode);

/** Read from an open file handle. Returns bytes read, 0 on EOF. */
size_t storageFread(void* buf, size_t maxLen, int f);

/** Write to an open file handle. Returns bytes written. */
size_t storageFwrite(const void* buf, size_t len, int f);

/** Get current file position. */
long   storageFtell(int f);

/** Close a file handle. For writes, blocks until data is flushed. */
void   storageFclose(int f);

/** stat() via storage task. Returns 0 on success, -1 on error. */
int    storageStat(const char* path, struct stat* st);

/** rename() via storage task. */
int    storageRename(const char* oldPath, const char* newPath);

/** remove() via storage task. */
int    storageRemove(const char* path);

#endif
