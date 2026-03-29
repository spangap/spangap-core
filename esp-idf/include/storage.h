/**
 * storage — config store + file I/O service.
 *
 * Config: in-memory std::map with dot-notation keys, backed by JSON on /state.
 * Keys starting with "s." are persisted to /state/settings.json.
 * storageSet() for s.* keys auto-queues a JSON write (500ms coalescing timer).
 *
 * File I/O: POSIX-like API that runs on storage's DRAM stack.
 * PSRAM-stack tasks can safely call storageFopen/Fread/Fwrite/Fclose — the
 * actual SPI flash operations happen on the storage task via ITS streaming.
 *
 * ITS server: owns the browser config WebSocket (root path "/") and file I/O handles.
 */
#ifndef SECCAM_STORAGE_H
#define SECCAM_STORAGE_H

#include <stdint.h>
#include <stddef.h>
#include <sys/stat.h>

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
void   storageUnset(const char* key);
void   storageSave();                   /** Force immediate JSON write. */

/** Copy all keys matching srcPrefix to dstPrefix.
 *  e.g., storageCopy("s.camera.", "camera.") copies s.camera.img.quality → camera.img.quality.
 *  If onlyIfTargetKeyExists, only overwrites keys that already exist at the destination. */
void   storageCopy(const char* srcPrefix, const char* dstPrefix, bool onlyIfTargetKeyExists = false);

/** Same as storageCopy but does not fire storageSubscribeChanges / ITS (avoids flooding tasks). */
void   storageCopyNoNotify(const char* srcPrefix, const char* dstPrefix, bool onlyIfTargetKeyExists = false);
cfg_type_t storageGetType(const char* key);

/** Count consecutive numbered entries (0, 1, 2, ...) with at least one subkey.
 *  Prefix must end with dot, e.g., "s.web.map." → checks .0.*, .1.*, ... */
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
 *  Scope is prefix-matched: "s.camera.img" matches "s.camera.img.quality", "s.camera.img.brightness", etc.
 *  Empty scope "" matches all changes.
 *  Call from the task that should receive the callback (during init, before main loop). */
void storageSubscribeChanges(const char* scope, storage_change_cb_t cb);

/** Convenience: lambda-friendly callback type */
#define ON_CHANGE [](const char* key, const char* val)

/* ---- File I/O API ---- */

/** Open a file via storage task. Returns handle (>= 0) or -1 on error.
 *  Blocks until the file is opened on the storage task's DRAM stack.
 *  Supports any path: /state/, /fixed/, /sdcard/. */
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
