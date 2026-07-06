# spangap-core/esp-idf — internals

Component-scope developer notes. Per-subsystem maintainer references live in
[../docs/](../docs/) (each `<func>-internals.md`).

## Layout

```
esp-idf/
├── idf_component.yml     name spangap-core, namespace spangap, dep on spangap/esp_wireguard
├── CMakeLists.txt        component-level: globs src/*.{cpp,c}, INCLUDE_DIRS include, REQUIRES platform deps
├── Kconfig               CONFIG_SPANGAP_* knobs
├── project_include.cmake CMake helpers visible to consumers' top-level CMakeLists
├── cmake/                CMake helpers (e.g. spangap_create_factory_image)
├── data/factory_state/   image flashed to /fixed at build time
├── include/              public headers
├── src/                  implementations + private headers
└── scripts/              partitions, ota-keygen/release, icon raster, timezones, size, build-epoch
```

## Heap-tracking guard

`src/heap_track_stub.c` plus the `--wrap` linker options in
`CMakeLists.txt` work around an IDF-5.5 regression: the
`HEAP_TASK_TRACKING` feature took a global mutex on every alloc/free,
which deadlocked cJSON-heavy workloads under load.

The CMake guard fails the build loud if Espressif renames the wrapped
functions — without that guard a silent rename would leave the wraps
unbound and the deadlock would silently come back. Cross-reference:
[../docs/idf-tweaks.md](../docs/idf-tweaks.md).

## Build-time integration

`project_include.cmake` exposes helpers visible at the *project* scope
(top-level `CMakeLists.txt` of the consumer), in particular
`spangap_create_factory_image` which folds the consumer's `data/`
overlay together with this straddle's `data/factory_state/` into the
LittleFS image flashed to `/fixed`.

`sdkconfig.defaults.spangap` is the platform's recommended sdkconfig
defaults — PSRAM placement, lwIP TCP buffers, watchdog policy, console
on USB-CDC, ChaCha20-Poly1305 over AES-GCM, etc. Layer it first in the
consumer's `sdkconfig.defaults` chain.

## Scripts

Operator-facing scripts are not run on the device. They live here
because they ship as part of the component:

- `partitions.py` — partition layout generator (the platform default;
  apps may override).
- `ota-keygen.py` — generate the OTA signing keypair (run once per app).
- `ota-release.py` — sign a release manifest from a built firmware.
- `lcd-icons.py` — rasterize SVG launcher icons (depends on cairo +
  Pillow + cairosvg + pypng + lz4; without all four it silently ships
  label-only tiles).
- `update-zones.py` — refresh `factory_state/timezones.json` (IANA →
  POSIX map; a plain user-state file, not a config blob). The result is
  committed here; every consumer inherits it via the factory-image merge.
- `size.py` — section-size report against the most recent build.
- `build-epoch.py` — embed the build's UTC epoch into the firmware.

## Conventions

These are load-bearing for keeping this component a reusable foundation.

**Expose tunables via Kconfig, not magic constants.** When a value is a
genuine deployment tunable — SD `max_files`, the FAT format cluster size —
give it a `CONFIG_SPANGAP_*` option in `Kconfig` (with range, default, and
help text), reference `CONFIG_...` at the use site, and document it in
`../README.md`. Operators need to override platform defaults without patching
source, and the README is the discovery surface. Existing examples:
`SPANGAP_SDCARD_MAX_FILES` and `SPANGAP_SDCARD_ALLOC_KB`. Board-agnostic IDF
defaults that are not per-app tunables (e.g. `WL_SECTOR_SIZE_512`,
`FATFS_ALLOC_PREFER_EXTRAM=n`) go in `sdkconfig.defaults.spangap`, not a
per-board `straddle.yaml`.

**Don't over-Kconfig, and keep straddle-specific config in the owning
straddle.** A knob is only worth a config symbol if it is a real deployment
tunable — otherwise prefer a hardcoded constant with an explaining comment,
or ask, rather than inventing a switch. And core stays generic: an RNS/rnsd
policy value (e.g. the boot-settle floor) belongs in the rns straddle, a LoRa
value in `iface-lora`, and so on. Sibling straddles use bare prefixes
(`CONFIG_LORA_*`, `CONFIG_LCD_*`), *not* `CONFIG_SPANGAP_*` — that prefix is
reserved for this foundation component. Generic *primitives* are fine to add
here (e.g. `waitForFlag(key, timeout)` in `src/spangap_init.cpp`); it is
straddle-specific values and policy that must not leak in. This is the
config-placement counterpart of [What does NOT belong here](#what-does-not-belong-here).

**Keep shared platform primitives simple; let consumers do the gymnastics.**
When the choice is "smarten the shared component" versus "each consumer does a
little boilerplate", keep the primitive single-responsibility and push the
boilerplate to the call site. Canonical case (in spangap-lcd): `lcdInstall`
stays an on-task primitive that returns the new app id; it does *not*
self-detect task context and internally hop via `lcdRun`. Consumers keep the
explicit `lcdRun([](void*){ lcdInstall(new FooApp()); })` wrapper at their
`*LcdRegister` call sites. Folding the hop in would smuggle a context-dependent
return contract (valid id on-task, `-1` off-task) into a primitive nearly every
straddle consumes — a mode-dependent API is worse to live with than visible
boilerplate, and the explicit `lcdRun` also makes the task hop honest. Don't
push context-detection or dual-path behavior into shared `-core`/`-lcd`/`-web`
primitives just to save a line at the call site.

## Temporary (revert before ship): heap-corruption debug kit

A heap-corruption debug kit was added 2026-06-13 to hunt a use-after-free that
poisoning caught as `CORRUPT HEAP`. **Marked temporary because the
comprehensive poisoning has real CPU / internal-DRAM cost and must not ship
enabled.**

Current state (verify before relying on this): the aggressive part has already
been reverted. The DEBUG block in `sdkconfig.defaults.spangap`
(`CONFIG_HEAP_POISONING_COMPREHENSIVE`, `CONFIG_FREERTOS_CHECK_STACKOVERFLOW_PTRVAL`,
`CONFIG_SPANGAP_HEAP_INTEGRITY_POLL=y`, `CONFIG_SPANGAP_WATCH_ADDR=0x0`) is
**gone** — reverting it restored the implicit `HEAP_POISONING_DISABLED` /
`CHECK_STACKOVERFLOW_CANARY`. The root cause it was chasing (the `main_task`
TCB UAF — an orphaned cleanup hook) is fixed by wiring `its.cpp`'s
`vTaskPreDeletionHook` via `CONFIG_FREERTOS_TASK_PRE_DELETION_HOOK`.

What remains (keepable, off by default) are the Kconfig knobs and their guarded
code, so the kit can be re-armed from menuconfig without re-patching source:

- `SPANGAP_HEAP_INTEGRITY_POLL` (+ `_MS`) — the log task (`logTaskFn` in
  `src/log.cpp`) runs `heap_caps_check_integrity(MALLOC_CAP_INTERNAL, true)` at
  the configured interval, aborting at the first corrupted block. For the full
  hunt this also needs comprehensive poisoning + the strict PTRVAL stack check
  enabled by hand in menuconfig (kconfiglib can't `select` a choice option).
- `SPANGAP_WATCH_ADDR` — when non-zero, arms a hardware STORE watchpoint over a
  16-byte window (both cores via `esp_ipc`, slot 1) at that address to catch a
  UAF writer's backtrace.

Gotcha worth keeping: a fixed-address watchpoint is useless on memory that is
live-then-freed (e.g. a TCB) — it fires on the legitimate write during the live
phase and boot-loops. It is only useful for addresses that stay free for the
whole watch window.

## What does NOT belong here

Camera, audio, detect, recording, RTSP, AVI playback are *app*
concerns. They belong in the consuming straddle (e.g. seccam, reticulous-
tdeck), not in this component.

Likewise: networking, TLS, NTP, mDNS now live in
[spangap-net](../../spangap-net); HTTPS/auth/WebRTC in
[spangap-web](../../spangap-web); LCD launcher in
[spangap-lcd](../../spangap-lcd).
