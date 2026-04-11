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

/* ---- Filesystem layout ---- */

#define FS_FIXED  "/fixed"
#define FS_STATE  "/state"
#define FS_SDCARD "/sdcard"

/** Filesystem mount descriptor. */
struct fs_mount_t {
    const char* path;           /**< VFS mount point */
    const char* partition;      /**< partition label in partitions.csv */
    bool        readonly;       /**< mount read-only */
    bool        needsDramStack; /**< SPI flash — PSRAM cache disabled during access */
    bool        formatOnFail;   /**< format if mount fails (writable partitions) */
};

static constexpr fs_mount_t FS_MOUNTS[] = {
    { FS_FIXED, "fixed", true,  true, false },
    { FS_STATE, "state", false, true, true  },
};

static constexpr int FS_MOUNT_COUNT = sizeof(FS_MOUNTS) / sizeof(FS_MOUNTS[0]);

/* ---- Init ---- */

/** Mount flash filesystems, init NVS, start the DRAM-stack fs worker.
 *  Call early in app_main after basic hardware init, before storageLoad(). */
void fs_init();

/** True after fs_init() if this was a first boot (factory defaults were copied). */
bool fs_first_boot();

/** Copy factory defaults from /fixed/factory_state/ to /state/. */
void fs_factory_reset();

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

#endif
