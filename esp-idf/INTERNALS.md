# spangap-core/esp-idf — internals

Component-scope developer notes. Module-by-module content lives in
[../INTERNALS.md](../INTERNALS.md).

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
- `update-zones.py` — refresh `s.time.zones.json` (IANA → POSIX cache).
  The result is committed here; every consumer inherits it via the
  factory-image merge.
- `size.py` — section-size report against the most recent build.
- `build-epoch.py` — embed the build's UTC epoch into the firmware.

## What does NOT belong here

Camera, audio, detect, recording, RTSP, AVI playback are *app*
concerns. They belong in the consuming straddle (e.g. seccam, reticulous-
tdeck), not in this component.

Likewise: networking, TLS, NTP, mDNS now live in
[spangap-net](../../spangap-net); HTTPS/auth/WebRTC in
[spangap-web](../../spangap-web); LCD launcher in
[spangap-lcd](../../spangap-lcd).
