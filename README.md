# spangap-core

## What is this?

**spangap-core** is the base firmware runtime of the [spangap](../spangap)
platform: the things every spangap device needs no matter what
application sits on top — inter-task streaming (ITS), a unified
filesystem worker, the config tree (storage), the log task, the CLI, a
cron scheduler, and the power-management plumbing. It is one ESP-IDF
managed component published as `spangap/spangap-core`.

## What this straddle owns

Public API headers in `esp-idf/include/`, implementations in
`esp-idf/src/`. One header/source pair per module.

| Module       | Header              | Source(s)                                                |
| ------------ | ------------------- | -------------------------------------------------------- |
| Init plumbing | `spangap.h`        | `spangap_init.cpp` — drives the platform startup sequence |
| ITS          | `its.h`             | `its.cpp` — inter-task streaming (server/client/forward) |
| FS workers   | `fs.h`              | `fs.cpp` — DRAM-stack file I/O for LittleFS + FAT/SD    |
| Storage      | `storage.h`         | `storage.cpp` — cJSON config tree, browser sync, secrets |
| Log          | `log.h`             | `log.cpp` — vprintf hook, DRAM ring, task tagging        |
| CLI          | `cli.h`             | `cli.cpp`, `cli_cmd_fs.cpp`, `cli_cmd_sys.cpp`           |
| Cron         | `cron.h`            | `cron.cpp` — minute-resolution scheduler, sleep-aware    |
| Power mgmt   | `pm.h`              | `pm.cpp` — power locks, USB pullup, deep-sleep stats     |
| SPI helper   | `spi_helper.h`      | `spi_helper.cpp` — shared-bus arbitration helpers        |
| Compat       | `compat.h`          | header-only — `millis()`, `safeStrncpy`, fmt helpers     |
| IDF workaround | —                | `heap_track_stub.c` — `--wrap` no-ops for IDF 5.5 mutex   |

## How others use it

Every other firmware straddle (spangap-net, spangap-web, …) depends on
spangap-core and assumes its symbols read as language primitives —
`storageGet`, `cliRegister`, `info`, `itsPoll`, … (the straddle's
`prefix` is the empty string for exactly this reason).

Install from `components.espressif.com`:

```yaml
# main/idf_component.yml
dependencies:
  spangap/spangap-core: "^0.1.0"
```

For sibling-checkout development:

```yaml
dependencies:
  spangap/spangap-core:
    version: "^0.1.0"
    path: "../../path/to/spangap-core/esp-idf"
```

In `app_main()`:

```cpp
pmInit();
logInit();
fs_init();
storageInit();
itsInit();
cliInit();
// ... then the other straddles' init() functions in dependency order
```

`spangapInit()` / `spangapPostAppInit()` (in `spangap.h`) wrap this
sequence for app code that wants the canonical bring-up.

## What it does NOT own

- WiFi / TCP / UDP / TLS / NTP / mDNS — in [spangap-net](../spangap-net).
- HTTPS / auth / WebRTC / browser UI shell — in
  [spangap-web](../spangap-web).
- LCD launcher — in [spangap-lcd](../spangap-lcd).
- Camera, audio, app-specific logic — those belong in the consuming
  application straddle.

## State of the split (Phase 1)

This is the **Phase-1 ur-straddle** for the firmware base. Networking,
auth, web, and WebRTC were split out into `spangap-net` and
`spangap-web`. The on-device LVGL launcher moved into `spangap-lcd`.
What remains here is the bedrock every other firmware straddle assumes.

## Operator tools

Operator-facing tools that work against any spangap consumer (this
straddle, reticulous, anything else) live in `scripts/` at the
platform-meta-repo root, not here:

- `flasher` — esptool-direct reflash + always-live monitor daemon.
  `-d` polls `build/flashme`, holds the monitor passively (`--no-reset`)
  while idle, wipes `build/flasher.log` per flash.
- `reallyclean.sh` — deep-clean a tree to source-only state; wired into
  `idf.py reallyclean` via `idf_ext.py` in consumer projects.

## Read next

- [INTERNALS.md](INTERNALS.md) — module-by-module deep dive and the
  conventions you need to honour when touching this code.
- The platform-wide [spangap/INTERNALS.md](../spangap/INTERNALS.md) for
  ITS, storage, gotchas, ESP-IDF specifics, partition layout.
- Subsystem docs under [docs/](docs/) — these are still the platform-
  wide deep-dive home and remain canonical for ITS, storage, cron,
  power management, logging.

## See also

- [README-old.md](README-old.md) — the pre-split README (platform-wide
  pitch); content moved to [spangap/README.md](../spangap/README.md).
- [CLAUDE.md](CLAUDE.md) — still in place; content moved to
  [spangap/INTERNALS.md](../spangap/INTERNALS.md) and this straddle's
  [INTERNALS.md](INTERNALS.md).
