/**
 * storage — config store.
 *
 * Config: cJSON tree in RAM, backed by JSON on /state. Storage is an ACTOR:
 * writes (storageSet and friends) are op-list messages applied by the storage
 * task — build a patch tree, RFC 7396 deepMerge into cfgRoot, notify
 * subscribers, trigger the save timer, all atomically per message. Writes are
 * synchronous (read-your-writes holds); when the caller is the storage task or
 * storage hasn't spawned yet, the write applies directly. Reads are direct
 * under a recursive mutex (readers vs the single actor-writer).
 *
 * storageBegin()/storageEnd() bracket a task-local op accumulator (one atomic
 * message at the outer End). NOTE: reads INSIDE an open bracket see committed
 * state, not the bracket's own pending writes — read before the bracket.
 *
 * Keys starting with "s." are persisted to <stateDir>/storage/root.json
 * (or a per-prefix blob under <stateDir>/storage/external/ if registered).
 * Keys starting with "secrets." are persisted but never sent to the browser.
 * All other keys are ephemeral (in-memory only, lost on reboot).
 *
 * Browser config DataChannel (`storage:1`, packet-mode):
 * - Device→browser: full nested JSON dump on connect, then coalesced merge-patches.
 * - Browser→device: nested JSON merge-patches. null = delete subtree (silent).
 * - Deletes via storageDeleteTree() do not fire storageSubscribeChanges callbacks.
 *
 * File I/O: use fs.h (unified PSRAM-safe API).
 */
#ifndef SPANGAP_STORAGE_H
#define SPANGAP_STORAGE_H

#include <stdint.h>
#include <stddef.h>
#include <sys/stat.h>
#include <string>
#include <cJSON.h>

/** Storage task's ITS server port for the config DataChannel (`storage:1`).
 *  Packet-mode: each browser→device message is one JSON merge-patch,
 *  each device→browser message is one dump or coalesced patch. */
static constexpr uint16_t STORAGE_CONFIG_PORT = 1;

/** Storage task's ITS aux ports. */
static constexpr uint16_t STORAGE_OP_PORT     = 44;  /* config write op lists → the actor */
static constexpr uint16_t STORAGE_SAVE_PORT   = 43;  /* reserved (was save-now; saves now run on the storage_save worker) */
static constexpr uint16_t STORAGE_CHANGE_PORT = 42;  /* change dispatch on subscriber tasks */

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
/** std::string overload — no fixed buffer, no truncation. */
std::string storageGetStr(const char* key, const char* def = "");
void   storageSet(const char* key, int val);
void   storageSet(const char* key, const char* val);
inline void storageSet(const char* key, const std::string& val) { storageSet(key, val.c_str()); }

/** Set a key only if it does not currently exist. Returns true if written.
 *  Use from module init() to seed config defaults that the module owns. */
bool   storageDefault(const char* key, int val);
bool   storageDefault(const char* key, const char* val);

/** Walk a JSON tree under `prefix` and install only-missing primitive leaves
 *  (each acts like storageDefault). Arrays in the JSON install wholesale only
 *  if the path is absent — never element-wise (would clash with user data).
 *  Existing keys are preserved. nulls in the JSON are skipped.
 *
 *  Wraps the whole walk in storageBegin/End so subscriptions and the WS push
 *  see one coalesced commit. Use to install a module's whole config block:
 *
 *    storageDefaultTree("s.detect", R"({
 *      "motion": {"fps":-2, "pct":5},
 *      "audio":  {"level":500},
 *      "stop_after": 10
 *    })");
 *
 *  The cJSON* form does not take ownership of `json`. Returns true if any
 *  leaf was written. */
bool   storageDefaultTree(const char* prefix, const cJSON* json);
bool   storageDefaultTree(const char* prefix, const char* jsonStr);
/** Set an arbitrary cJSON node (array, object, etc.) at a dot-notation key.
 *  Takes ownership of val — caller must not free it.
 *  Uses patch/commit: fires subscriptions and WS broadcast. */
void   storageSetTree(const char* key, cJSON* val);
/** Delete a single key via patch/commit. Fires storageSubscribeChanges with val="". */
void   storageUnset(const char* key);
/** Delete a key/subtree directly. No change callbacks. Sends null on WS.
 *  If the key (or an ancestor of it) names a registered external file
 *  (see storageNewTreeFile), that file is removed and unregistered on the
 *  next flush — so deleting a contact/identity also drops its own .json. */
void   storageDeleteTree(const char* keyOrPrefix);
void   storageSave();                   /** Force immediate JSON write. */

/** Register a dedicated on-disk file for the subtree at `prefix`, so writes
 *  under it persist to <stateDir>/storage/external/<prefix>.json instead of
 *  bloating root.json (the same mechanism timezones use). Call before writing
 *  the first key under `prefix` — e.g. at contact creation, so a chatty
 *  conversation only rewrites its own small file. Idempotent. The file is
 *  removed + unregistered automatically when `prefix` (or an ancestor) is
 *  passed to storageDeleteTree. Does not evict — the subtree still lives in
 *  cfgRoot and still syncs to the browser. */
void   storageNewTreeFile(const char* prefix);

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

/** Remove all subscriptions on `scope` registered by the calling task.
 *  Exact-string scope match; pair with storageSubscribeChanges. */
void storageUnsubscribe(const char* scope);

/** Convenience: lambda-friendly callback type */
#define ON_CHANGE [](const char* key, const char* val)

/** Subscribe AND apply the current value once, in a single statement:
 *  subscribes like storageSubscribeChanges, then immediately invokes the
 *  body on the calling task with the scope's current value (or "" if unset
 *  / scope is a prefix — handlers normally re-read by `key`). Folds the
 *  boot-apply and the live subscription into one call, which matters because
 *  silent defaults (storageDefault*) don't fire change subscriptions. `key`
 *  and `val` are in scope in the body, same as ON_CHANGE. `scope` is
 *  evaluated more than once — pass a literal. */
#define NOW_AND_ON_CHANGE(scope, ...) do { \
    storage_change_cb_t _naoc = [](const char* key, const char* val) __VA_ARGS__; \
    storageSubscribeChanges((scope), _naoc); \
    _naoc((scope), storageGetStr(scope).c_str()); \
  } while (0)

#endif
