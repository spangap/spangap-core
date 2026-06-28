# fs — internals

Maintainer reference for `fs.cpp` / `fs.h` (plus the CLI fronts
`cli_cmd_fs.cpp`, `cli_cmd_mount.cpp`, and the format/reset verbs in
`cli_cmd_sys.cpp`). The [operator guide](fs.md) covers usage.

## 1. Inventory

**Worker tasks** (both DRAM-stack — `STACK_DRAM` — because flash ops disable the
PSRAM cache and SDMMC DMA must serialize):

- **`fs`** — 5120-byte DRAM stack, prio 1, core 1. Serves `FS_OP_PORT` (1) one
  `fs_op_t*` per message via `onFsOp`/`handleOp`. Callers reach it through
  `proxyOp`, which sends with `ITS_WAIT_PICKUP` and blocks for the whole op.
  Its loop also emits diagnostic pulse logs (ops/s, in-op slot).
- **`fs_strm`** — 5120-byte DRAM stack, prio 1, core 1. Serves the streaming
  write port (`FS_STREAM_PORT` 2), the streaming read port (`FS_READ_PORT` 3),
  and the stream-sync aux (`FS_STREAM_SYNC_PORT` 4). It pumps active read slots
  (`fsReadPumpOnce`) inline from its loop before each `itsPoll`.

**Handle tables** (fixed, statically sized):

| Table | Size | Notes |
|---|---|---|
| `fileSlots` | `MAX_FILE_SLOTS` 12 | `{FILE* fp, active, flash, sd}` per open file. |
| `dirSlots` | `MAX_DIR_SLOTS` 4 | open `DIR*` per directory iterator. |
| `fsStreamSlots` | `FS_STREAM_MAX_HANDLES` 2 | streaming-write connections, 16 KB buffer each. |
| `fsReadSlots` | `FS_READ_MAX_HANDLES` 2 | streaming-read connections, up to 512 KB buffer each. |

**Mount table** — `FS_MOUNTS[]` in `fs.cpp`:

| Path | Partition label | RO | DRAM-stack | Format-on-fail |
|---|---|---|---|---|
| `/fixed` | `fixedLabel` (resolved to `"fixed"` at `fs_init`) | yes | yes | no |
| `/state` | `"state"` | no | yes | yes |

`/sdcard` is **not** in the table — it is mounted separately and optionally by
`fs_mount_sd()`.

**State-store selection** — `s_stateDir` (`"/state"` | `"/sdcard/state"`), set
once in `fsSelectStateStore()` and exposed via `fsStateDir`/`fsStatePath`/
`fsStateOnSd`. There is no path rewriting: both locations are real and always
available; this string only records which one holds durable state.

**SD write-staging** (SD files only — LittleFS never touches SDMMC):
`fsTuneSdFile` pins an SD `FILE` to a one-sector (512 B) fully-buffered stdio
buffer, and `fsSdFwrite` writes in sub-sector chunks. Both exist to keep
FatFs's `disk_write` from ever receiving an unaligned multi-sector PSRAM
pointer, which falls back to a per-write `MALLOC_CAP_DMA` bounce buffer and
fails with `ESP_ERR_NO_MEM` (0x101) when the internal DMA pool is tight.

## 2. Proxy routing

`needsProxy`/`needsProxyHandle` currently return true whenever the worker
exists, so **every** `fs_*` call routes through the `fs` worker once it is up —
this serializes all SDMMC access and is PSRAM-stack safe unconditionally. The
`isFlashPath()` / `callerOnPsram()` plumbing is kept but unused; flipping back to
`isFlashPath(path) && callerOnPsram()` restores selective proxying. The only
direct, un-proxied `fopen` is the pre-boot `settings.json`-era read that happens
before the worker exists.

The worker handler `handleOp` does **no path rewriting** — `/state` and
`/sdcard/state` are both real, and callers already pass whichever is active.
`STAT`/`LISTDIR` special-case `"/"` (not a real VFS mount) by synthesizing a
directory and the three mount-point entries (`fixed`, `state`, and `sdcard` only
when `sdReady`).

## 3. Streaming model

- **Write** (`fs_open_stream`): the client connects to `FS_STREAM_PORT` and
  `itsSend`s bytes; the worker drains the connection's client-side buffer when
  fill crosses `triggerLevel` and bursts to disk with one `fwrite`, eliminating
  per-line SD round-trips. `fs_stream_sync` (aux on port 4) forces a drain +
  `fflush` + `fsync`. Disconnect drains, syncs, and closes.
- **Read** (`fs_open_stream_read`): the worker `fread`s into the server→client
  buffer ahead of the client's `itsRecv`. Pumped inline from the `fs_strm` loop
  (FAT/SDMMC serializes anyway, so a separate pump task buys nothing). At EOF the
  pump stops topping up; queued bytes keep delivering until `itsRecv` returns 0
  and `itsConnected` is false, at which point the client disconnects.
  `FS_READ_LOW_WATER` (4096) gates re-arming the free-notify wake.

## 4. Boot order

`fs_init()` (called early, before `storageLoad`):

1. `nvs_flash_init()` (ESP-IDF internals only — WiFi cal / PHY; erase+retry on
   version/page errors).
2. Resolve `fixedLabel` to `"fixed"`, then `statePartitionEnsure()` (see §5).
3. Mount every LittleFS partition from `FS_MOUNTS`, **including `/state`** — it
   is always mounted regardless of where the active store ends up.
4. Spawn the `fs` and `fs_strm` workers (each gated on a ready semaphore).

`fs_init` deliberately does **not** probe SD or do the first-boot copy —
mounting SD that early raced the bus/power bring-up. Those run later from
`spangapInit`, in this order: `fs_mount_sd()` → `fsSelectStateStore()` →
`storageLoad()` (see the init doc; don't duplicate that sequence here).

`fsSelectStateStore()`: if SD mounted and `/sdcard/state` is a directory, that
becomes the active store; otherwise `/state` stays active. If the chosen store
has **no non-dot entries** it is treated as first boot — `firstBoot` is set,
`fs_factory_reset()` copies `/fixed/factory_state/*` in, and
`applyAdditionalState()` overlays `/fixed/additional_state/` (both recursive;
top-level `settings.json` is skipped — `storageLoad` deep-merges it instead).

## 5. Runtime state-partition self-grow

The flashed image is a size-agnostic floor image whose on-flash partition table
omits `state`. `statePartitionEnsure()` reads the real physical flash size
(SFDP), raises the driver's idea of the chip to it (`g_rom_flashchip` +
`esp_flash_default_chip`), and registers `state` in the upper flash via
`esp_partition_register_external` — purely in-memory, recreated identically
every boot, so LittleFS data persists while the on-flash table is never
rewritten. `state` starts at `CONFIG_SPANGAP_STATE_PERCENT` (default 50) of real
flash, with fallbacks if the shipped firmware (fixed partition end) would
overlap. If a board pins `state` in its own table, this no-ops. This is the
flash-partition architecture's concern — see the flash-partitions doc; don't
duplicate the layout reasoning here.

## 6. Format / reset

- `fsFormatFlash()` unregisters, `esp_littlefs_format("state")`, and remounts —
  always the on-flash partition (label-based), even when the active store is SD.
  Must run on a DRAM stack.
- `fsFormatSd()` reformats the card in place (FAT) with the chosen cluster size
  and keeps it mounted; returns false if no card or SD is compiled out.
- The CLI verbs (`format flash`, `format sd`, `reset factory`) run their format
  on a **DRAM-stack worker** and block the (PSRAM-stacked) CLI task on a
  semaphore until done — so a scripted `format sd; mkdir …; reboot` can't race
  the format. `reset factory` refuses when `fsStateOnSd()` and reboots after the
  flash format.

## 7. Pitfalls

- **Never hard-code `"/state"` for durable paths.** Use `fsStateDir()` /
  `fsStatePath()`. A literal `/state` writes to the on-flash partition even when
  the active store is the SD one, silently splitting state across two stores.
  (`/state` *as a literal* is still correct only when you specifically mean the
  always-mounted flash partition, e.g. `format flash`.)
- **PSRAM-stack tasks must not do direct file I/O.** The whole point of the
  workers is that flash ops disable the PSRAM cache. Any new code path that
  `fopen`s a flash path directly from a PSRAM stack faults in
  `esp_task_stack_is_sane_cache_disabled()`. Route through `fs_*`.
- **The FAT rename trap.** `f_rename` returns `FR_EXIST` rather than overwriting;
  `handleOp`'s `RENAME` case removes the dest and retries to make rename
  overwrite-correct everywhere. There is a brief window where the dest is absent
  (unavoidable on FAT) but the source still exists, so no data is lost. Any
  atomic `<file>.new` + rename persistence relies on this — don't "simplify" the
  retry away.
- **SD writes need aligned, sub-sector staging.** Keep `fsTuneSdFile` +
  `fsSdFwrite` on every SD-file open/write path; bypassing them re-introduces the
  unaligned-PSRAM-pointer `ESP_ERR_NO_MEM 0x101` failures under DMA pressure.
- **`fs_init` runs before the workers exist for exactly one read.** Don't add
  more pre-worker direct I/O; the only sanctioned one is the early settings read,
  and everything after worker spawn must proxy.
