# fs — unified, PSRAM-safe file I/O

`fs` is the device's single file-access API. Every filesystem — the on-flash
LittleFS partitions (`/state`, `/fixed`), the FAT microSD (`/sdcard`) — is
reached through one handle-based POSIX-style interface that is safe to call from
**any** task, regardless of its stack placement. [storage](storage.md), the
logger, the web server, the CLI, and consumer apps all do their file I/O here;
nothing calls `fopen`/`fread` on a flash path directly.

## Why a worker model

Two hardware facts make naive file I/O unsafe on this platform, and `fs` exists
to hide both:

- **PSRAM-stack callers crash on flash ops.** On the ESP32-S3 an SPI-flash
  operation disables the PSRAM cache; a task with a PSRAM stack that calls
  `fread`/`fwrite` on a LittleFS path faults in
  `esp_task_stack_is_sane_cache_disabled()`. Most device tasks run on PSRAM
  stacks.
- **SDMMC needs serialized internal DMA.** Concurrent SD writers from
  record/log/CLI without serialization hit `ESP_ERR_NO_MEM`.

`fs` routes all I/O through dedicated **DRAM-stack worker tasks**, so every
caller is safe and SD access is serialized. The proxy is transparent: callers
just use the `fs_*` functions.

## The two workers

| Task | Stack | Ports | Role |
|---|---|---|---|
| `fs` | DRAM | `FS_OP_PORT=1` | POSIX aux ops: open/read/write/seek/stat/rename/mkdir/listdir/… one `fs_op_t` message per call; the caller blocks on `ITS_WAIT_PICKUP` until the op finishes. Also pumps streaming reads. |
| `fs_strm` | DRAM | `FS_STREAM_PORT=2`, `FS_READ_PORT=3`, `FS_STREAM_SYNC_PORT=4` | Streaming write/read servers: bulk-drain a write stream to disk, pump a read stream from disk, and force a write-stream sync. |

## API

Handle-based (12 file slots, 4 directory slots). Exact signatures and the
streaming contracts are in [`include/fs.h`](../esp-idf/include/fs.h); this is the
map.

| Group | Functions |
|---|---|
| File handles | `fs_open`, `fs_read`, `fs_write`, `fs_tell`, `fs_seek`, `fs_flush`, `fs_sync` (fflush+fsync), `fs_truncate`, `fs_close` |
| Path ops (stateless) | `fs_stat`, `fs_rename`, `fs_remove`, `fs_mkdir`, `fs_mkdirp`, `fs_file_info` (HTTP-style, optional `.gz`), `fs_listdir` (bulk one-round-trip) |
| Directory iteration | `fs_opendir`, `fs_readdir`, `fs_closedir` |
| Streaming | `fs_open_stream` (write), `fs_open_stream_read` (read), `fs_stream_sync` — these return an **ITS handle**; drive it with `itsSend`/`itsRecv`/`itsDisconnect` |
| State store | `fsStateDir`, `fsStatePath`, `fsStateOnSd`, `fsSelectStateStore` |
| Format / SD | `fsFormatFlash`, `fsFormatSd`, `fs_mount_sd`, `sdAvailable`, `fsSdInfo`, `fsLittlefsInfo` |

A minimal consumer (the platform auto-inits fs; never call `fs_init`):

```c
int f = fs_open(fsStatePath("/myapp/notes.txt").c_str(), "wb");
if (f >= 0) {
    fs_write(text, 1, strlen(text), f);
    fs_sync(f);
    fs_close(f);
}
```

`fs_rename` is **overwrite-correct on FAT**: `f_rename` refuses to replace an
existing destination (unlike LittleFS/POSIX), so the worker removes the dest and
retries. This is what makes the `<file>.new` + rename atomic-write pattern (used
by [storage](storage.md) and by any SD-backed persistence) actually replace the
target on an SD store.

## The active state store: flash or SD

Durable device state (settings, certs, boot script) lives in **one** of two
real, always-available locations, chosen once at boot:

- **`/state`** — the on-flash LittleFS partition. **Always mounted**, regardless
  of the choice below.
- **`/sdcard/state`** — a directory on the FAT microSD. Becomes the active store
  **iff it exists** when `fsSelectStateStore()` runs at boot.

There is **no path rewriting and no `/state`↔SD aliasing**. The choice is
exposed as plain strings; every consumer of the state store builds paths from
them and never hard-codes `FS_STATE`:

- `const char* fsStateDir()` — `"/state"` or `"/sdcard/state"`, stable for the
  process.
- `std::string fsStatePath(const char* sub)` — `fsStateDir() + sub` (sub starts
  with `/`).
- `bool fsStateOnSd()` — derived predicate, for the few callers that need it
  (e.g. the `reset factory` guard).

`format flash` is **label-based** (`esp_littlefs_format("state")`) and always
means the on-flash partition, even when the active store is on SD.

Operator flows:

- **Move config to SD:** `format sd; mv /state /sdcard; reboot` — `/sdcard/state`
  now exists, so next boot runs from SD.
- **Fresh SD system:** `format sd; mkdir /sdcard/state; reboot` — the empty dir
  is factory-populated on first boot.

## CLI

All run on-device via `spangap cli "<command>"`. Paths resolve against the
session's cwd.

**Files & directories** (`cli_cmd_fs.cpp`):

```
ls [-la] [path]        list files (default: cwd); -a includes dotfiles, -l long format
cat <file>...          print file(s) to the CLI
cp [-r] <src> <dst>    copy a file or tree (cross-filesystem OK)
mv <src> <dst>         move/rename; same-fs rename fast-path, else copy+delete
rm [-rf] <path>...     remove files or trees
mkdir [-p] <path>      create a directory (-p makes parents)
cd [path]              change cwd (bare cd → s.cli.start_dir)
pwd                    print cwd
df [/path]             disk usage for the filesystem holding path
```

**Mounts** (`cli_cmd_mount.cpp`):

```
mount                  show mount status for /sdcard, /fixed, /state + active state store
mount sd               mount an SD card inserted after boot, at /sdcard
```

**Format & factory reset** (registered in `cli_cmd_sys.cpp`; the behavior is
state-store-level, so it is documented here — the command registration is
cross-referenced by the cli/init doc):

```
format flash           unmount, format, remount the on-flash `state` partition
format sd [KB]         reformat the SD card (FAT) in place, kept mounted; optional cluster size (default per Kconfig, 1-128 KB)
reset factory          format the flash `state` partition + reboot (next boot factory-repopulates)
```

`format flash` and `format sd` are synchronous — the command blocks on a
DRAM-stack worker until done, so scripted one-liners like
`format sd; mkdir /sdcard/state; reboot` run strictly in order. `reset factory`
is **refused when booted from SD** (it would wipe the inactive flash copy, not
the running SD store) and instead prints the SD-wipe recipe
(`format sd; mkdir /sdcard/state; reboot`).

For the worker model internals, the mount table, the state-partition self-grow,
and the FAT rename trap, see [fs-internals.md](fs-internals.md).
