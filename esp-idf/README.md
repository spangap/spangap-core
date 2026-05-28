# spangap-core (esp-idf)

The firmware half of the [spangap-core](..) straddle — an ESP-IDF
managed component published as `spangap/spangap-core`.

## What this subdir owns

Standard IDF component layout:

```
esp-idf/
├── idf_component.yml           name spangap-core, namespace spangap, dep on spangap/esp_wireguard
├── CMakeLists.txt              globs src/*.{cpp,c}, INCLUDE_DIRS include, REQUIRES platform deps
├── Kconfig                     component config (e.g. CONFIG_SPANGAP_LCD)
├── project_include.cmake       project-scope cmake helpers picked up by consumers
├── sdkconfig.defaults.spangap  the sdkconfig consumers should layer first
├── cmake/                      cmake helpers (e.g. spangap_create_factory_image)
├── data/                       factory_state/ image flashed to /fixed
├── include/                    public headers (consumed by app + transitive consumers)
├── src/                        implementations + private headers
└── scripts/                    operator scripts (partitions, OTA keygen/release, icon raster,
                                timezones, size report, build epoch)
```

## How a consumer pulls it in

Registry shape (the committed `idf_component.yml` of the consumer):

```yaml
dependencies:
  spangap/spangap-core: "^0.1.0"
```

Sibling-checkout development:

```yaml
dependencies:
  spangap/spangap-core:
    version: "^0.1.0"
    path: "../../path/to/spangap-core/esp-idf"
```

The consumer's top-level `CMakeLists.txt` typically locates the sibling
checkout via the path injected by `--spangap`, layers
`sdkconfig.defaults.spangap` first, then its own.

## Working in this subdir

- All files in `src/` compile as part of the component. Files reference
  each other via `#include "foo.h"` — both `include/` and `src/` are on
  the include path within the component.
- Public headers (the API surface) live in `include/`. The CMake
  `REQUIRES` list must cover everything that appears as a type or
  symbol in those headers.
- Camera, audio, detect, recording, RTSP, AVI playback — none of these
  belong here. They are app concerns in the consuming straddle.
- OTA takes a public key at init (`otaInit(pubkey_pem, len)`). The
  consumer supplies `OTA_PUBKEY_PEM` from a header that core never
  includes.

## Read next

- [INTERNALS.md](INTERNALS.md) for the component-scope developer notes
  (heap-tracking guard, `--wrap` workaround, CMake details).
- [../README.md](../README.md) for the straddle-level overview.
- [../INTERNALS.md](../INTERNALS.md) for the module-by-module deep dive.

## See also

- [README-old.md](README-old.md) — pre-split consumer guide (still
  accurate as a quickstart).
- [CLAUDE.md](CLAUDE.md) — pre-split component-scope notes.
