# spangap-core

The base firmware runtime of the [spangap](../spangap) platform — the primitives
every spangap device needs no matter what application sits on top. It is one
ESP-IDF managed component, published as `spangap/spangap-core`, and its straddle
`prefix` is the **empty string**: its symbols (`storageGet`, `cliRegister`,
`info`, `itsPoll`, `gp_alloc`, …) are meant to read as language primitives, so
every other firmware straddle depends on it and assumes it is there.

This is a multi-function straddle: each function has its own operator guide under
[`docs/`](docs/), with a companion `-internals.md` maintainer reference. Start
with the function you need.

## Functions

| Function | Guide | What it is |
|---|---|---|
| **init** | [docs/init.md](docs/init.md) | Platform bring-up: the four-call boot sequence, the generated init dispatcher, boot barriers (`waitForTime`/`waitForFlag`), firmware identity. |
| **storage** | [docs/storage.md](docs/storage.md) | The in-memory cJSON config tree and its prefixes (`s.*`, `secrets.*`, bare, read-only `fw.*`, telemetry `sys.*`), browser sync, persistence, defaults. |
| **fs** | [docs/fs.md](docs/fs.md) | DRAM-stack file-I/O workers for LittleFS + FAT/SD, the POSIX API, streaming, and the flash-or-SD state store. |
| **its** | [docs/its.md](docs/its.md) | Inter-Task Streaming — the platform's only inter-task IPC: socket-style connections, ports, and aux messages between FreeRTOS tasks. |
| **logging** | [docs/logging.md](docs/logging.md) | The log task, the `info()`/`warn()`/`err()`/`dbg()`/`verb()` macros, levels, log files, and the serial console. |
| **cli** | [docs/cli.md](docs/cli.md) | The on-device command line — registry, line editor, boot scripts, and the full core command manual. |
| **auth** | [docs/auth.md](docs/auth.md) | The credential primitive: realm passwords and session cookies (`secrets.auth.*`). HTTP enforcement lives in spangap-web. |
| **cron** | [docs/cron.md](docs/cron.md) | Minute-resolution, deep-sleep-aware scheduler driven by the `crontab` file. |
| **power-management** | [docs/power-management.md](docs/power-management.md) | DFS + light/deep sleep, PM locks, notify-driven CPU boost, USB pullup, GPIO wake. |
| **memory** | [docs/memory.md](docs/memory.md) | PSRAM-vs-internal-DRAM placement policy and the `gp_alloc`/`dram_alloc`/`dma_alloc` allocators. |
| **idf-tweaks** | [docs/idf-tweaks.md](docs/idf-tweaks.md) | Guarded ESP-IDF/toolchain workarounds (heap-tracking `--wrap`, shared-SPI-bus helpers, FATFS/SD defaults). |

## Cross-cutting

These describe platform-wide architecture that spans several straddles and live
in core by design (no operator/internals split):

- [docs/flash-partitions.md](docs/flash-partitions.md) — the size-agnostic floor
  image, runtime-grown `state` partition, and the two-pass shrink-wrap build.
- [docs/remote-access.md](docs/remote-access.md) — how the optional
  [upnp](../upnp) / [duckdns](../duckdns) / [acme](../acme) trio combine to reach
  a device from the public internet, and the `dns.txtrecord` seam between them.

## Source layout

Public headers in `esp-idf/include/`, implementations in `esp-idf/src/` — one
header/source pair per module.

| Module | Header | Source(s) |
|---|---|---|
| Init | `spangap.h` | `spangap_init.cpp` |
| Storage | `storage.h` | `storage.cpp` |
| FS workers | `fs.h` | `fs.cpp` |
| ITS | `its.h` | `its.cpp` |
| Log | `log.h` | `log.cpp` |
| CLI | `cli.h` | `cli.cpp`, `cli_cmd_fs.cpp`, `cli_cmd_sys.cpp`, `cli_cmd_mount.cpp` |
| Auth | `auth.h` | `auth.cpp` |
| Cron | `cron.h` | `cron.cpp` |
| Power management | `pm.h` | `pm.cpp` |
| Memory | `mem.h` | `mem_new.cpp` (global `operator new`/`delete`) |
| Compat / RTC RAM | `compat.h` | header-only (`millis`, `safeStrncpy`, `spawnTask`, fmt helpers) |
| Shared SPI bus | `spi_helper.h` | `spi_helper.cpp` |
| IDF heap-tracking bypass | — | `heap_track_stub.c` (`--wrap` no-ops) |

## What it does NOT own

- WiFi / TCP / UDP / TLS / NTP / mDNS / `wget` — [spangap-net](../spangap-net).
- HTTPS serving, auth *enforcement*, WebRTC, the browser app shell —
  [spangap-web](../spangap-web).
- The on-device LVGL shell and apps — [spangap-lcd](../spangap-lcd).
- Camera, audio, and any app-specific logic — the consuming application straddle.

## Dependency

```yaml
# main/idf_component.yml
dependencies:
  spangap/spangap-core: "^0.1.0"
```

For sibling-checkout development, point `path:` at this straddle's `esp-idf/`.
spangap-core is in the build for every spangap firmware, so its tasks start
automatically — consumers compose around it via `storageSubscribeChanges`, cron
entries, `cliRegisterCmd`, and `/state/boot` scripts.
