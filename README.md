# spangap-core

The base firmware runtime of the [spangap](../spangap) platform — the primitives
every spangap device needs no matter what application sits on top. It is one
ESP-IDF managed component, published as `spangap/spangap-core`, and its straddle
`prefix` is the **empty string**: its symbols (`storageGet`, `cliRegister`,
`info`, `itsPoll`, `gp_alloc`, …) are meant to read as language primitives, so
every other firmware straddle depends on it and assumes it is there.

It also builds for ESP-IDF's Linux host target, where host-only code lives in
[`esp-idf/src/host/`](esp-idf/src/host/) and chip-only code drops out. That is
there for the simulated testbed, not for running spangap on a Linux machine —
see `reticulous/sim/`.

This is a multi-function straddle: each function has its own operator guide under
[`docs/`](docs/), with a companion `-internals.md` maintainer reference. Start
with the function you need.

## Functions

| Function | Guide | What it is |
|---|---|---|
| **init** | [docs/init.md](docs/init.md) | Platform bring-up: the four-call boot sequence, the generated init dispatcher, boot barriers (`waitForTime`/`waitForFlag`), the board check that halts a wrong-board image, firmware identity. |
| **storage** | [docs/storage.md](docs/storage.md) | The in-memory cJSON config tree and its prefixes (`s.*`, `secrets.*`, bare, read-only `fw.*`, telemetry `sys.*`), browser sync, persistence, defaults. |
| **fs** | [docs/fs.md](docs/fs.md) | DRAM-stack file-I/O workers for LittleFS + FAT/SD, the POSIX API, streaming, and the flash-or-SD state store. |
| **its** | [docs/its.md](docs/its.md) | Inter-Task Streaming — the platform's only inter-task IPC: socket-style connections, ports, and aux messages between FreeRTOS tasks. |
| **logging** | [docs/logging.md](docs/logging.md) | The log task, the `info()`/`warn()`/`err()`/`dbg()`/`verb()` macros, levels, log files, and the serial console. |
| **cli** | [docs/cli.md](docs/cli.md) | The on-device command line — registry, line editor, boot scripts, and the full core command manual. |
| **auth** | [docs/auth.md](docs/auth.md) | The credential primitive: realm passwords and session cookies (`secrets.auth.*`). HTTP enforcement lives in spangap-web. |
| **random** | [docs/random.md](docs/random.md) | The device's one CSPRNG: `randomBytes()`/`randomU32()`, a CTR-DRBG seeded at boot inside an entropy-source window, and why `esp_fill_random` alone is not enough. |
| **cron** | [docs/cron.md](docs/cron.md) | Minute-resolution, deep-sleep-aware scheduler driven by `s.cron.tab.*` entries. |
| **power-management** | [docs/power-management.md](docs/power-management.md) | DFS + light/deep sleep, PM locks, notify-driven CPU boost, USB pullup, GPIO wake. |
| **usb-console** | [docs/usb-console.md](docs/usb-console.md) | Which USB controller drives the console — the built-in USB-Serial-JTAG port or a two-port TinyUSB CDC device (`usb cdc`, off by default: `CONFIG_SPANGAP_USB_CDC`) — and who owns each serial port. |
| **memory** | [docs/memory.md](docs/memory.md) | PSRAM-vs-internal-DRAM placement policy and the `gp_alloc`/`dram_alloc`/`dma_alloc` allocators. |
| **idf-tweaks** | [docs/idf-tweaks.md](docs/idf-tweaks.md) | Guarded ESP-IDF/toolchain workarounds (heap-tracking `--wrap`, shared-SPI-bus helpers, FATFS/SD defaults). |

## Cross-cutting

These describe platform-wide architecture that spans several straddles and live
in core by design (no operator/internals split):

- [docs/flash-partitions.md](docs/flash-partitions.md) — the size-agnostic floor
  image, runtime-grown `state` partition, and the two-pass shrink-wrap build.
- [docs/safe-mode.md](docs/safe-mode.md) — the recovery boot that backs the state
  store up, restores one, or factory-resets the device: a normal boot that stops
  after the web band so nothing else is touching the store while it works.
- [docs/framed-rpc.md](docs/framed-rpc.md) — the framed side-channel on the
  console port that lets a host tool run a command and read its output, without
  disturbing the log or an interactive CLI session.
- [docs/onboarding-output.md](docs/onboarding-output.md) — `-O`, the `key=value`
  contract those queries read, and the rules that let it evolve.
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
| Streaming tar.gz | `targz.h` | `targz.cpp` |
| FS workers | `fs.h` | `fs.cpp` |
| ITS | `its.h` | `its.cpp` |
| Log | `log.h` | `log.cpp` |
| CLI | `cli.h` | `cli.cpp`, `cli_cmd_fs.cpp`, `cli_cmd_sys.cpp`, `cli_cmd_mount.cpp` |
| Auth | `auth.h` | `auth.cpp` |
| Cron | `cron.h` | `cron.cpp` |
| Power management | `pm.h` | `pm.cpp` |
| USB console transport | `cli.h` (serial ports), `pm.h` | `usb_ports.cpp` |
| Memory | `mem.h` | `mem_new.cpp` (global `operator new`/`delete`) |
| Compat / RTC RAM | `compat.h` | header-only (`millis`, `safeStrncpy`, `spawnTask`, fmt helpers) |
| Shared SPI bus | `spi_helper.h` | `spi_helper.cpp` |
| Board probe vocabulary | `detect_probe.h` | header-only (the board straddle writes its own `detect_hw()` against it) |
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
