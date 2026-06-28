# Unified file I/O — `fs.cpp/h`

POSIX-style file API for all filesystems (`/state/`, `/fixed/`, `/sdcard/`), safe from any task stack type.

## Why

- **PSRAM-stack safety.** On the ESP32-S3, SPI flash operations disable the PSRAM cache. A task with a PSRAM stack that calls `fopen`/`fread`/`fwrite` on a LittleFS path (`/state/`, `/fixed/`) crashes with `esp_task_stack_is_sane_cache_disabled()`.
- **SDMMC DMA serialization.** The SD driver needs internal DMA buffers; concurrent writers from rec/log/CLI without serialization would hit `ESP_ERR_NO_MEM`.

`fs.cpp` solves both by routing all I/O through dedicated DRAM-stack worker tasks.

## Workers

- **`fs`** — POSIX aux-op worker. One ITS aux port (`FS_OP_PORT=1`) with a single `fs_op_t`-pointer message per call. Caller blocks on `ITS_WAIT_PICKUP` until the worker finishes.
- **`fs_strm`** — streaming read/write server. Three ITS ports:
  - `FS_STREAM_PORT=2` — streaming write connections (client writes bytes via `itsSend`; worker drains when fill ≥ `triggerLevel` and bursts with `fwrite`).
  - `FS_READ_PORT=3` — streaming read connections (worker pumps `fread` into the server→client buffer; client pulls with `itsRecv`).
  - `FS_STREAM_SYNC_PORT=4` — aux to force a drain + `fflush` + `fsync` on a write-stream.

All fs ops currently go through the worker even for DRAM-stack callers. This also serializes SDMMC DMA access — `/sdcard/` is safe from PSRAM stacks directly, but the worker keeps rec/log/play from colliding on DMA.

## API

Handle-based (`int` slot index, 12 concurrent):

```c
int     fs_open(const char* path, const char* mode);
size_t  fs_read(void* buf, size_t size, size_t nmemb, int f);
size_t  fs_write(const void* buf, size_t size, size_t nmemb, int f);
long    fs_tell(int f);
int     fs_seek(int f, long offset, int whence);
int     fs_flush(int f);
int     fs_sync(int f);              // fflush + fsync
int     fs_truncate(int f, long length);
void    fs_close(int f);
```

Path operations (stateless):

```c
int     fs_stat(const char* path, struct stat* st);
int     fs_rename(const char* from, const char* to);   // overwrite-correct on FAT
int     fs_remove(const char* path);
int     fs_mkdir(const char* path);
void    fs_mkdirp(const char* path);   // recursive
int     fs_file_info(const char* path, bool tryGz, fs_file_info_t* out);
int     fs_listdir(const char* path, fs_listing_t* out, int max);
```

Directory iteration (4 concurrent):

```c
int     fs_opendir(const char* path);
bool    fs_readdir(int d, fs_dirent_t* out);
void    fs_closedir(int d);
```

Streaming (returns ITS handle, use `itsSend` / `itsRecv` / `itsDisconnect`):

```c
int     fs_open_stream(const char* path, const char* mode,
                       size_t bufMinSize, size_t triggerLevel);
int     fs_open_stream_read(const char* path, size_t bufMinSize,
                            size_t triggerLevel, size_t freeNotify);
int     fs_stream_sync(int handle);
```

## Active state store (flash or SD)

The state store is **`/state`** (on-flash, always mounted) **or `/sdcard/state`** (if that directory exists at boot). No path rewriting — both are real; one is "active". Build state paths from these, never hard-code `FS_STATE`:

```c
const char* fsStateDir();                 // "/state" | "/sdcard/state", stable
std::string fsStatePath(const char* sub); // fsStateDir() + sub  (sub starts '/')
bool        fsStateOnSd();                // derived predicate
void        fsSelectStateStore();         // pick store + first-boot seed (see below)
void        fsFormatFlash();              // unmount/format/remount flash `state`; DRAM stack
bool        fsFormatSd();                 // reformat SD in place, stays mounted; DRAM stack
```

`fs_rename` is **overwrite-correct on FAT**: `f_rename` won't replace an existing destination (unlike LittleFS/POSIX), so the worker removes the dest and retries. This is what makes `atomicWriteJson` (`<file>.new` + rename) actually persist on an SD-backed store.

## Init

`fs_init()` (called early in `app_main`/`spangapInit`):

1. `nvs_flash_init()` (ESP-IDF internals only).
2. Mount `/fixed` (read-only) and the on-flash `/state` (read-write, format on fail) from the `FS_MOUNTS` table in `fs.h`. **`/state` is always mounted.**
3. Start the `fs` and `fs_strm` worker tasks.

`fs_init()` no longer probes SD or does the first-boot copy (mounting SD that early raced the bus/power bring-up). `spangapInit()` then calls, in order: `fs_mount_sd()` → **`fsSelectStateStore()`** → `storageLoad()`.

`fsSelectStateStore()`: choose `/state` vs `/sdcard/state`; if the chosen store has no non-dot entries, run `fs_factory_reset()` (copy `/fixed/factory_state/*`) + `applyAdditionalState()` (`/fixed/additional_state/` overlay) and set the first-boot flag. `fs_factory_reset()` re-copies factory defaults into `fsStateDir()`; `fs_first_boot()` reports whether the first-boot copy ran this session.
