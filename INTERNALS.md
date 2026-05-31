# spangap-core — internals

Module-by-module developer reference for the spangap base runtime.
Cross-cutting conventions (ITS, storage, gotchas, ESP-IDF specifics,
partition layout) live in [spangap/INTERNALS.md](../spangap/INTERNALS.md).

## Module map

### `spangap_init.cpp` / `spangap.h`

Drives the canonical platform startup sequence. `spangapInit()` brings
up pm/log/fs/storage/its/cli/cron in order; `spangapPostAppInit()` runs
after the app's per-task inits to finish second-phase work that needs
the app graph in place. Consumers that need finer control can skip the
wrapper and call each module's `*Init()` directly.

State store selection runs here, between `fs_mount_sd()` and
`storageLoad()`: if `/sdcard/state` exists at boot it becomes the
active store; `/state` (the on-flash partition) stays mounted but
unused.

### `its.cpp` / `its.h`

Inter-task streaming — the single communication primitive for the
platform. See [spangap/INTERNALS.md → ITS](../spangap/INTERNALS.md) for
the API and rules.

Inbox queues are PSRAM-backed via
`xQueueCreateWithCaps(MALLOC_CAP_SPIRAM)`, which reclaims ~40 KB DRAM
platform-wide. The cost: **ITS is no longer ISR-safe.** `itsSend` /
`itsSendAux` / `itsConnect` / `itsPoll` may only be called from task
context.

### `fs.cpp` / `fs.h`

All file I/O is routed through dedicated DRAM-stack worker tasks. The
worker pattern serializes SDMMC DMA (the recorder, log, and CLI can't
collide on the SD card) and lets PSRAM-stack callers do file I/O safely
(a direct LittleFS or FAT read from a PSRAM-stack task crashes — any
SPI-flash op disables the PSRAM cache).

Key entry points: `fs_open` / `fs_read` / `fs_write` / `fs_close` /
`fs_rename` / `fs_stat` / `fs_remove`. State-store-aware helpers:
`fsStateDir()` / `fsStatePath()`. Mount helpers: `fs_init()`,
`fs_mount_sd()`, `fsSelectStateStore()`.

`fs_rename` is overwrite-correct on FAT (required for SD persistence —
LittleFS already had it).

### `storage.cpp` / `storage.h`

The in-memory cJSON config tree. Synced device↔browser through a single
packet-mode `storage:1` channel (registered with `web` once
spangap-web is present). Prefixes: `s.*` (persisted + synced),
`secrets.*` (persisted, never sent to browser), no prefix (ephemeral).

`storageDefault*` writes are silent — they don't fire change
subscriptions. Use `storageSet` when subscribers need to react.

`storage_change_msg_t` is `cb(4) + key[128] + val[128]`. Hierarchical
keys like `lxmf.directory.<32hex>.<field>` fit without subscriber-
notify truncation. `ITS_MAX_MSG_DATA` is `320` (lifted from 96) to
carry the widened struct.

`storageSubscribeChanges(scope, cb)` plus the matching
`storageUnsubscribe(scope)` — pair them in lifecycle code that creates
and destroys subscriptions (e.g. lxmf's per-identity command subs).

### `log.cpp` / `log.h`

Log task hooks ESP-IDF's `vprintf`, tags every line with its
originating task (`[taskname]`), and keeps a DRAM ring buffer. Fans out
to serial, the browser's log channel (when spangap-web is present), and
optionally a log file under `/state` or `/sdcard`.

Macros: `info()` / `warn()` / `err()` / `dbg()` / `verb()`. Code on
unregistered tasks must prefix the task name manually.
`logIsDebug(const char* tag)` — per-tag overload that resolves the
per-tag level (`s.log.tag.<tag>`) first, falling back to global
(`s.log.level`). Useful for short-circuiting expensive work that only
feeds a specific tag's `dbg()` output.

### `cli.cpp`, `cli_cmd_fs.cpp`, `cli_cmd_sys.cpp` / `cli.h`

CLI registry, line editor, history, `cliRunFile` for boot scripts.
Commands self-register via `cliRegisterCmd(name, fn)`. Conventions:

- **Help is uniform.** `args == "help"` prints exactly **one** short line
  (the `help` listing collects these; the "try `<cmd> -h`" banner is
  printed once by `help`, never per-command). `args == "-h"`/`"--help"`
  prints fuller per-command help. `cliWantsHelp(args)` matches all three
  spellings for commands whose brief and detailed help coincide.
- **No `status` verb.** A command with no args prints its status.
- **Output is flush-left** — no leading indent on emitted lines.
- **Silent on success** — `set`, `unset`, `save`, `detect` produce no
  output on success.

Built-ins: file/dir verbs (`ls`, `cat`, `cp`, `mv`, `rm`, `mkdir`,
`stat`, `df`, `format`) in `cli_cmd_fs.cpp`; system verbs (`reset`,
`top`, `pm`, `usb`, `date`, `log`, …) in `cli_cmd_sys.cpp`.

### `cron.cpp` / `cron.h`

Minute-resolution scheduler with deep-sleep support. Flags on each
entry: `-` always, `A` awake only, `N` STA-upstream only. Defaults
registered with `cronDefault(spec, cmd)`; only inserted on first boot
(version-gated).

### `pm.cpp` / `pm.h`

Power-management locks (`pmAcquire`/`pmRelease`), USB pullup control
(`usb` CLI), deep-sleep statistics. CLI: `pm`, `top`, `usb`.

### `spi_helper.cpp` / `spi_helper.h`

Shared-bus arbitration helpers for the FSPI bus (display + SD + LoRa
all share SCK/MOSI/MISO on the T-Deck Plus). Used by the board HAL in
the consuming buildable straddle.

### `compat.h`

Header-only utilities: `millis()` / `delay()` shims, `safeStrncpy`,
`fpsToIntervalMs`, `utcOffsetMinutes`, format helpers `fmtElapsed` /
`fmtSize` / `fmtBps` / `fmtWallClock`. Anything that wants to read as
"part of the language" lives here.

### `heap_track_stub.c`

`--wrap` linker stubs that no-op the IDF-5.5 `HEAP_TASK_TRACKING` global
mutex. Required for stable cJSON-heavy workloads. The CMake guard fails
the build loud if Espressif renames the wrapped functions. See
`docs/idf-tweaks.md`.

## Recent platform-wide changes

These are the deltas that consumers might still trip over — most have
landed but documentation may lag.

- **`ITS_MAX_MSG_DATA`** is `320` (was 96). Static_asserts on payload
  structs should use `<= ITS_MAX_MSG_DATA`, not `== 96`.
- **ITS inbox queues are PSRAM-backed.** ITS is no longer ISR-safe;
  ISRs use a heap flag + `vTaskNotifyGiveFromISR`, picked up by the
  target task's `itsPoll`.
- **`storage_change_msg_t`** widened to `cb(4) + key[128] + val[128]`
  (was `48`/`44`). Required the `ITS_MAX_MSG_DATA` bump.
- **`storageUnsubscribe(scope)`** added — pairs with
  `storageSubscribeChanges`. Removes the calling task's subscription
  matching `scope` exactly.
- **`logIsDebug(const char* tag)`** added — per-tag overload that
  resolves the per-tag level (`s.log.tag.<tag>`) first.
- **State store can live on SD.** Code never hard-codes `/state` — use
  `fsStateDir()` / `fsStatePath()`. Config file is
  `<stateDir>/storage/root.json` (was `/state/settings.json`).
  Externals stay under `storage/external/`. `reset factory` is refused
  when booted from SD. New CLI: `format flash`, `format sd`.

## Conventions you must follow

- **Logging**: `info()` / `warn()` / `err()` / `dbg()` / `verb()`. Never
  raw `ESP_LOGx`. Never include the task name yourself —
  `[taskname]` is prepended.
- **Strings**: `safeStrncpy(dst, src, n)`, never `strncpy`.
- **Style**: modern C++ — `std::string`, `std::string_view`. Avoid
  `char[]` / `strstr` parsing in new code.
- **Config keys**: `s.*` persisted+synced; `secrets.*` persisted, never
  to browser; bare = ephemeral. `storageDefault` is silent, `storageSet`
  fires subscriptions.
- **The `itsPoll` loop** (canonical): `for (;;) { while (itsPoll(0)) {}; /* work */; itsPoll(block); }`.
- **No PlatformIO** — IDF only.

## Docs that still live here

The `docs/` tree under this straddle is the historic home for platform-
wide deep dives. Until those move into their owning straddles, the
authoritative texts remain:

- [docs/its.md](docs/its.md) — ITS architecture
- [docs/storage.md](docs/storage.md) — config tree
- [docs/unified-fs-api.md](docs/unified-fs-api.md) — fs worker model
- [docs/cron.md](docs/cron.md)
- [docs/power-management.md](docs/power-management.md)
- [docs/logging.md](docs/logging.md)
- [docs/key-fixes.md](docs/key-fixes.md)
- [docs/idf-tweaks.md](docs/idf-tweaks.md)
- [docs/getting-started.md](docs/getting-started.md)
- [docs/development.md](docs/development.md)

(Web, auth, tls, ntp, ota, webrtc, lcd, and remote-access docs should
move to the straddle that owns the code; until then `docs/` here still
holds them.)
