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

## Configuration

Most behaviour is driven by the board's pins and the app; a few Kconfig
knobs are worth knowing (`idf.py menuconfig` → *spangap: spangap-core*):

- **`CONFIG_SPANGAP_SDCARD`** + the bus choice (`…_BUS_SDMMC` /
  `…_BUS_SPI`) and pins — enable and wire up the SD card.
- **`CONFIG_SPANGAP_SDCARD_MAX_FILES`** (default **6**) — the cap on
  files open at once on the SD volume. FATFS pre-allocates one sector
  cache per slot at mount, so each slot costs `FF_MAX_SS` bytes
  (512 B by default) of RAM up front. The default covers the platform's
  own users (log stream, fs read/write streams, cron logfile) with
  headroom — **raise it if your app opens many SD files concurrently**,
  lower it to reclaim RAM.
- **`CONFIG_SPANGAP_SDCARD_ALLOC_KB`** (default **8**) — FAT cluster size
  used when the firmware *formats* the SD (`fsFormatSd`, format-on-mount-
  fail). Only affects device-side formats; a card formatted on a computer
  keeps its own cluster size. Each file rounds up to a whole cluster, so
  big clusters waste space with many small files (a tree of ~7 KB map tiles
  on a 64 KB-cluster card uses ~9× its real size). 8 KB ≈ one cluster per
  tile while keeping the FAT small enough to mount large cards quickly.

ITS mailbox knobs (`idf.py menuconfig` → *spangap: spangap-core*):

- **`CONFIG_SPANGAP_ITS_INBOX_DEPTH`** (default **32**) — default depth, in
  messages, of each ITS task's mailbox. Slots are single pointers (~4 B) to
  heap-allocated messages, so depth is nearly free. Tasks override with a
  non-zero `inboxDepth` to `itsServerInit`/`itsClientInit`.
- **`CONFIG_SPANGAP_ITS_INBOX_MSG_MAX`** (default **4096**) — default per-message
  payload guard for mailboxes (connect handshake / aux / forward). Oversized
  sends are rejected, not truncated; the guard is floored at `ITS_MAX_MSG_DATA`
  (320). Per-task override: `itsServerInit`'s `inboxMaxMsgLen` (0 = this
  default). Payload memory is borrowed per-message and freed on read, so a
  generous guard costs nothing at idle.
- **`CONFIG_SPANGAP_ITS_PKT_DEPTH`** (default **16**) — default in-flight
  messages a packet-link direction queues (descriptor-ring depth). Slots are
  8-byte descriptors; payloads are borrowed per message. Per-port override:
  `itsServerPortOpen`'s `depth`.
- **`CONFIG_SPANGAP_ITS_MSG_MAX`** (default **65536**) — default packet-link
  per-message size guard (NOT the backpressure window). `itsRecvBufSize`/
  `itsSendBufSize` report this. Per-port override: `itsServerPortOpen`'s `maxMsg`.

Storage-actor knobs (`idf.py menuconfig` → *spangap: spangap-core*):

- **`CONFIG_SPANGAP_STORAGE_OP_TIMEOUT_MS`** (default **5000**) — how long a
  public storage write waits for the storage actor to apply its op message
  before warning and giving up.
- **`CONFIG_SPANGAP_STORAGE_NOTIFY_TIMEOUT_MS`** (default **10**) — how long the
  actor blocks delivering one CHANGED message to a remote subscriber before
  dropping it (bounded so a subscriber stuck in a sync write can't deadlock the
  actor).
- **`CONFIG_SPANGAP_STORAGE_NOTIFY_INBOX`** (default **256**) — depth of the
  storage task's inbox (PSRAM-backed pointer-slot queue carrying op messages
  and change self-sends); sizes the actor's op backlog under multi-producer
  write bursts.
- **`CONFIG_SPANGAP_STORAGE_OP_MSG_MAX`** (default **192 KB**) — per-message
  payload guard on the storage task's inbox: the largest single op a foreign
  task can send (a big `storageSet` value, or a `storageSetTree`/`storageCopy`
  subtree serialized as one message). Must comfortably exceed the largest
  single value any producer publishes — today a Nomad page body
  (`NOMAD_MAX_PAGE_PUBLISH`, 128 KB) plus op overhead. Owned ops hand over a
  heap pointer, so a generous guard costs nothing at idle.

Cross-task change notifications truncate the carried value at
`STORAGE_NOTIFY_VAL_MAX` (**512 B**): notifies are change *signals*, not
value transport — a subscriber that needs the full value re-reads storage by
key. Same-task subscribers (invoked directly) still see the full value.

spangap-core's `sdkconfig.defaults.spangap` also pins
`CONFIG_WL_SECTOR_SIZE_512=y` for all consumers: 512-byte FATFS sectors,
since we use LittleFS (not FAT) on flash, so the 4 KB wear-levelling
default would only waste RAM.

On **SD-over-SPI** boards, spangap-core's CMakeLists defines
`SOC_SDMMC_PSRAM_DMA_CAPABLE=1` for the `sdmmc` component so `sdmmc_cmd`
skips its per-write PSRAM bounce buffer (a `MALLOC_CAP_DMA` alloc that
fails with `0x101` when the internal DMA pool is tight) — the SDSPI host
already bounces PSRAM through a buffer it allocates once. This is gated
on `CONFIG_SPANGAP_SDCARD_BUS_SPI`; SDMMC-peripheral boards keep the
bounce because their dedicated DMA genuinely cannot reach PSRAM.

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
