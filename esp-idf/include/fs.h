/**
 * fs — Unified file I/O, PSRAM-safe.
 *
 * All file access should go through this API. Calls on flash-backed paths
 * (/state/, /fixed/) from PSRAM-stack tasks are automatically proxied through
 * a DRAM-stack worker task (SPI flash ops disable the PSRAM cache). SD card
 * paths and DRAM-stack callers use direct POSIX.
 */
#ifndef SECCAM_FS_H
#define SECCAM_FS_H

#include <sys/stat.h>
#include <cstddef>
#include <string>
#include "esp_err.h"

/* ---- Filesystem layout ---- */

#define FS_FIXED  "/fixed"
#define FS_STATE  "/state"
#define FS_SDCARD "/sdcard"

/** Filesystem mount descriptor. */
struct fs_mount_t {
    const char* path;           /**< VFS mount point */
    const char* partition;      /**< partition label in partitions.csv (resolved at fs_init) */
    bool        readonly;       /**< mount read-only */
    bool        needsDramStack; /**< SPI flash — PSRAM cache disabled during access */
    bool        formatOnFail;   /**< format if mount fails (writable partitions) */
};

/* The "fixed" partition label is resolved at fs_init() to fixed_a or fixed_b
 * based on the running app slot (app0 → fixed_a, app1 → fixed_b). The pair
 * is selected together by the bootloader's otadata, so rollback automatically
 * pairs the matching fixed slot. */
extern const fs_mount_t FS_MOUNTS[];
extern const int        FS_MOUNT_COUNT;

/** Partition label for the inactive fixed slot — OTA writes here. */
const char* fs_inactive_fixed_label();

/** Partition label for the inactive app slot — OTA writes here. */
const char* fs_inactive_app_label();

/** Partition label for the running fixed slot. */
const char* fs_active_fixed_label();

/* ---- Init ---- */

/** Mount flash filesystems, init NVS, start the DRAM-stack fs worker.
 *  Call early in app_main after basic hardware init, before storageLoad(). */
void fs_init();

/** True after fs_init() if this was a first boot (factory defaults were copied). */
bool fs_first_boot();

/** Copy factory defaults from /fixed/factory_state/ to the active state
 *  store (fsStateDir()). */
void fs_factory_reset();

/** Choose the active state store and seed it on first boot. Call ONCE,
 *  AFTER fs_mount_sd() and BEFORE storageLoad(): if the SD mounted and
 *  /sdcard/state is a directory it becomes the active store, else it stays
 *  the on-flash /state; an empty store gets factory defaults copied in.
 *  (Kept out of fs_init() so SD is probed at the proven-reliable time.) */
void fsSelectStateStore(void);

/** The active state store for this boot: "/state" (on-flash, always
 *  mounted) or "/sdcard/state" (when /sdcard/state existed at boot).
 *  There is NO path rewriting — both locations are real and always
 *  available; this is just which one holds settings/certs/etc. Every
 *  consumer of the state store MUST build its paths from this, e.g.
 *  fs_open(fsStatePath("/storage/root.json").c_str(), ...), instead of
 *  hard-coding FS_STATE. The returned pointer is stable for the process
 *  lifetime. */
const char* fsStateDir();

/** Convenience: fsStateDir() + sub. `sub` must start with '/'. */
std::string fsStatePath(const char* sub);

/** True iff the active state store is the SD one (derived from
 *  fsStateDir(); the string is the source of truth). For callers that need
 *  the predicate, e.g. the `reset factory` guard. */
bool fsStateOnSd();

/** Unmount, reformat, and remount the on-flash `state` partition (always
 *  mounted at /state, even when the active store is on SD). Run on a DRAM
 *  stack. Used by `format flash` and `reset factory` (which reboots after). */
void fsFormatFlash(void);

/** Reformat the SD card in place (FAT); it stays mounted at /sdcard.
 *  Returns false if no card is mounted or SD support is compiled out.
 *  Run on a DRAM stack. Used by `format sd`. */
bool fsFormatSd(void);

/** LittleFS usage for a partition `label` ("state", "webroot", "fixed_*").
 *  Proxied through the DRAM-stack fs worker — esp_littlefs_info walks
 *  metadata via SPI-flash reads that disable the PSRAM cache, so a direct
 *  call from a PSRAM-stack task asserts. Returns ESP_OK on success. */
esp_err_t fsLittlefsInfo(const char* label, size_t* total, size_t* used);

/* ---- Optional SD card mount ---- */

/** Mount an SD card at /sdcard (FAT on SDMMC slot). Pin numbers and bus
 *  width are read from Kconfig:
 *    CONFIG_DIPTYCH_SDCARD            — gate (no-op return when n)
 *    CONFIG_DIPTYCH_SDCARD_PIN_CLK    — required
 *    CONFIG_DIPTYCH_SDCARD_PIN_CMD    — required
 *    CONFIG_DIPTYCH_SDCARD_PIN_D0     — required
 *    CONFIG_DIPTYCH_SDCARD_4BIT       — enable 4-bit bus
 *    CONFIG_DIPTYCH_SDCARD_PIN_D1/D2/D3 — required when 4BIT
 *  Returns true on success. After this returns true, sdAvailable() is true.
 *  Call after fs_init(). One-shot: subsequent calls are no-ops. */
bool fs_mount_sd(void);

/** True iff the SD card is mounted at /sdcard. Modules that may write to
 *  /sdcard/... should gate on this. False until fs_mount_sd() succeeds. */
bool sdAvailable();

/* ---- Handle-based file operations ---- */

/** Open a file. Returns handle >= 0 on success, -1 on error. */
int     fs_open(const char* path, const char* mode);

/** Read up to nmemb elements of size bytes. Returns elements read. */
size_t  fs_read(void* buf, size_t size, size_t nmemb, int f);

/** Write nmemb elements of size bytes. Returns elements written. */
size_t  fs_write(const void* buf, size_t size, size_t nmemb, int f);

/** Get current file position. */
long    fs_tell(int f);

/** Seek within file. Returns 0 on success. */
int     fs_seek(int f, long offset, int whence);

/** Flush stdio buffer. Returns 0 on success. */
int     fs_flush(int f);

/** Flush stdio buffer + sync to storage. Returns 0 on success. */
int     fs_sync(int f);

/** Truncate file to length bytes. Returns 0 on success. */
int     fs_truncate(int f, long length);

/** Close a file handle. */
void    fs_close(int f);

/* ---- Path operations (stateless) ---- */

int     fs_stat(const char* path, struct stat* st);
int     fs_rename(const char* from, const char* to);
int     fs_remove(const char* path);
int     fs_mkdir(const char* path);

/** Create directory and all parent components (mkdir -p). */
void    fs_mkdirp(const char* path);

/* ---- Directory iteration ---- */

struct fs_dirent_t {
    char name[64];
    int  d_type;   /* DT_REG, DT_DIR, DT_UNKNOWN */
};

/** Open a directory. Returns handle >= 0 on success, -1 on error. */
int     fs_opendir(const char* path);

/** Read one entry. Returns true if entry written, false at end. */
bool    fs_readdir(int d, fs_dirent_t* out);

/** Close a directory handle. */
void    fs_closedir(int d);

/** Bulk directory listing with per-entry stat — single proxy round-trip.
 *  Caller allocates out[max]. Returns entry count, or -1 on error.
 *  Includes '.' / '..' / dotfiles — caller filters as desired. */
struct fs_listing_t {
    char     name[64];
    uint32_t size;
    time_t   mtime;
    bool     isDir;
};

int     fs_listdir(const char* path, fs_listing_t* out, int max);

/* ---- HTTP-style file serving (single round-trip body delivery) ---- */

struct fs_file_info_t {
    bool   gzipped;
    size_t size;
    time_t mtime;
};

/** Discover a file. If tryGz, tries `path.gz` first (sets gzipped=true) then `path`.
 *  Returns 0 on found, -1 if missing. Single round-trip. */
int     fs_file_info(const char* path, bool tryGz, fs_file_info_t* out);

/* ---- Streaming read API (ITS-stream backed) ----
 * Open a file for reading as an ITS connection. A dedicated pump task on
 * the fs side fread's into the server→client stream buffer; caller pulls
 * via itsRecv(handle, buf, len, timeout). At EOF the pump exits but the
 * buffer keeps delivering remaining bytes until itsRecv returns 0 and
 * itsConnected(handle) is false — caller then calls itsDisconnect(handle).
 *
 * triggerLevel — client-side: don't wake us until >= N bytes queued.
 * freeNotify   — server-side: pump arms a wake when >= N bytes freed
 *                in the send buffer (low-water mark for the pump). 0 =
 *                default (the pump uses its internal low-water mark). */
int     fs_open_stream_read(const char* path, size_t bufMinSize,
                            size_t triggerLevel, size_t freeNotify);

/* ---- Streaming write API (ITS-stream backed) ----
 * Open a file as an ITS connection to the fs worker. Caller writes via
 * itsSend(handle, data, len, timeout). The fs worker drains the connection's
 * client-side stream buffer when fill crosses `triggerLevel` and bursts the
 * data to disk in one fs_write — eliminating per-line SD round-trips.
 *
 * Use itsDisconnect(handle) to close. Returns ITS handle >= 0 on success. */
int     fs_open_stream(const char* path, const char* mode,
                       size_t bufMinSize, size_t triggerLevel);

/** Force-drain an fs_open_stream handle: flush the ITS stream buffer into
 *  fwrite + fflush + fsync. Blocks until the sync completes.
 *  Returns 0 on success, -1 on error or timeout. */
int     fs_stream_sync(int handle);

#endif
