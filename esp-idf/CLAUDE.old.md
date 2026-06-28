# spangap-core (component scope)

This is the IDF managed component published as `spangap/spangap-core`. It contains only platform code — no camera, audio, or app-specific logic.

For the full spangap platform context (architecture, conventions, recipes, module map, gotchas, ESP-IDF specifics, partition layout), see [`../CLAUDE.md`](../CLAUDE.md). For the consumer-facing install/use guide, see [`README.md`](README.md).

## Layout

```
core/
├── idf_component.yml     declares: name spangap-core, namespace spangap, dep on spangap/esp_wireguard
├── CMakeLists.txt        component-level: globs src/*.{cpp,c}, INCLUDE_DIRS include, REQUIRES platform deps
├── include/              public headers (consumed by app + transitive consumers)
└── src/                  implementations + private headers
```

## Working in core/

- All files in `src/` compile as part of the component. Files reference each other via `#include "foo.h"` — both `include/` and `src/` are on the include path within the component.
- Public headers (the API surface) live in `include/`. The CMake REQUIRES list must cover everything that appears as a type or symbol in those headers.
- Camera, audio, detect, recording, RTSP, and AVI playback are app concerns — they belong in the consumer (e.g. seccam), not here.
- The OTA component takes a public key at init: `otaInit(pubkey_pem, len)`. The consumer supplies `OTA_PUBKEY_PEM` from a header that core never includes.

## Heap-tracking guard

`src/heap_track_stub.c` and the `--wrap` linker options in `CMakeLists.txt` work around an IDF 5.5 regression — see [`../docs/idf-tweaks.md`](../docs/idf-tweaks.md). The CMake guard fails the build loud if Espressif renames the wrapped functions.

## Recent platform changes

- **`ITS_MAX_MSG_DATA`** is now `320` (was 96). Connect / aux / forward payloads can be up to 320 bytes before truncation. Consumers that previously sized payload structs to exactly 96 should now use `<= ITS_MAX_MSG_DATA` in static_asserts. See [`include/its.h`](include/its.h).
- **ITS inbox queues are PSRAM-backed** via `xQueueCreateWithCaps(MALLOC_CAP_SPIRAM)`. Reclaims ~40 KB DRAM platform-wide (storage task alone was 22 KB because of its depth-64 inbox). Trade-off: **ITS is no longer ISR-safe** — `itsSend` / `itsSendAux` / `itsConnect` / `itsPoll` may only be called from task context. ISRs should set a heap flag + `vTaskNotifyGiveFromISR`, picked up by the target task's `itsPoll`. See [`docs/its.md`](../docs/its.md) "ISR safety".
- **Change notifications are variable-length heap messages** (`{cb, key\0, val\0}`; the fixed `storage_change_msg_t` is gone). Keys travel whole — hierarchical keys like `lxmf.directory.<32hex>.<field>` never truncate — but values cap at `STORAGE_NOTIFY_VAL_MAX` (512 B): notifies are change signals, not value transport; handlers re-read storage by key for the full value. Same-task subscribers (invoked directly) see the full value.
- **`storageUnsubscribe(scope)`** added — pairs with `storageSubscribeChanges`; removes the calling task's subscription matching `scope` exactly. Used by lifecycle code that creates/destroys subscriptions (e.g. lxmf's per-identity cmd subs).
- **`logIsDebug(const char* tag)`** added — per-tag overload. Resolves the per-tag level (`s.log.tag.<tag>`) first, falls back to global (`s.log.level`). Useful for short-circuiting expensive work that only feeds a specific tag's `dbg()` output.
- **State store can live on SD.** `/state` is the on-flash partition (always mounted); if `/sdcard/state` exists at boot it becomes the *active* store. **Build all state paths from `fsStateDir()` / `fsStatePath()` ([`include/fs.h`](include/fs.h)) — never hard-code `FS_STATE`.** The config file is `<stateDir>/storage/root.json` (was `/state/settings.json`); externals stay under `storage/external/`. SD selection + first-boot seeding moved out of `fs_init()` into `fsSelectStateStore()` (run by `spangapInit` between `fs_mount_sd()` and `storageLoad()`). `fs_rename` is now overwrite-correct on FAT (required for SD persistence). New CLI: `format flash`, `format sd`; `reset factory` is refused when booted from SD. See [`docs/storage.md`](../docs/storage.md) + [`docs/unified-fs-api.md`](../docs/unified-fs-api.md).
- **`netRegister()` level-replays UP edges.** A handler registered for `NET_EV_UP` / `NET_EV_UPSTREAM_UP` *after* the link is already up is now invoked immediately (was edge-only, so late subscribers silently missed the one-shot — this is what made mDNS flaky on fast AP-only boots). UP handlers must therefore be idempotent and must tolerate running on the registering task, not just the net task. DOWN/CFG/POLL remain edge-only.
- **Per-device AP SSID.** `netInit()`'s first-boot defaults now compute `s.net.wifi.ap.ssid = "<s.net.hostname>_<last 2 MAC bytes hex>"` (e.g. `reticulous_dcbc`) so a fleet doesn't present identical APs; `s.net.hostname` still defaults to `CONFIG_SPANGAP_PROJECT_NAME`.
