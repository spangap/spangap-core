# Flash partitions — floor image, runtime-grown state, shrink-wrap

spangap ships a **size-agnostic floor image** (default 4 MB) that boots on any
chip ≥ the floor; the firmware grows flash to the real chip size at first boot
and creates the writable `state` partition itself. The shipped partitions
(`app`/`fixed`) are **shrink-wrapped** to their actual sizes by the build. There
is no flash-size build parameter and no A/B layout.

This supersedes the old paired-A/B OTA layout (see [ota.md](ota.md), now stale).

## Layout

The on-flash partition table (generated into `partitions.csv` by
`esp-idf/scripts/gen-partitions.py`, **never hand-edit**) holds only what ships
in the floor image. `state` is deliberately absent — it's created at runtime.

Without the updater (default):

```
nvs       data, nvs,     0x9000,   0x5000     IDF internal (WiFi PHY cal)
app       app,  factory, 0x10000,  <wrapped>  firmware
fixed     data, spiffs,  ...        <wrapped>  read-only LittleFS (SPA + factory defaults)
```

With `spangap/updater` staged:

```
nvs       data, nvs,     0x9000,   0x5000
otadata   data, ota,     0xe000,   0x2000     selects app vs updater
updater   app,  ota_1,   0x10000,  <wrapped>  tiny serial-updated flasher (low/stable)
app       app,  ota_0,   ...        <wrapped>  firmware
fixed     data, spiffs,  ...        <wrapped>  read-only LittleFS
```

The updater sits **below** `app`/`fixed` so their shrink-wrap growth never moves
it. Flash above `fixed` (up to the real chip size) is where `state` lives.

`spangap build` prints this layout after every build.

## Runtime self-grow + `state`

`statePartitionEnsure()` in `esp-idf/src/fs.cpp` runs at the top of `fs_init()`,
before the LittleFS mounts:

1. Reads the **real** chip size via `esp_flash_get_physical_size` (the driver
   otherwise clamps to the floor/header size).
2. Raises `esp_flash_default_chip->size` and `g_rom_flashchip.chip_size` to it,
   unlocking the upper region for the partition/flash layer.
3. Registers `state` with **`esp_partition_register_external`** — purely
   in-memory, recreated identically every boot. The LittleFS data persists on
   flash; the on-flash partition table is never rewritten.

`state` occupies the upper `CONFIG_SPANGAP_STATE_PERCENT` of real flash (default
50 % → starts at the half-flash mark). If the shipped firmware (`fixed` end)
would extend past that mark, it falls back to half that share (state starts
higher up); if it still won't fit, state is clamped above `fixed` with a warning.
Because state's start offset is stable for a given chip size, firmware/data
upgrades never move it. `state` is formatted on first mount (`fs.cpp`
`format_if_mount_failed`), so a fresh or re-grown device self-heals to factory
defaults (read-only `fixed` holds them) — usable even unattended.

To **disable** self-grow on a board (pin a fixed size), set
`CONFIG_ESPTOOLPY_FLASHSIZE_*MB` in the board's `kconfig:` — then `state` fills
the upper half of that pinned size.

## Shrink-wrap (two-pass build)

App size is only known after link, and data size only after the merge/SPA build,
so the build measures then shrinks:

1. **Pass 1** — configure with a generous provisional layout (so link + the
   LittleFS pack succeed), build.
2. `spangap-inside` `measure_build_sizes()` reads the real `app.bin` size and the
   merged data (each file rounded up to a 4 KB LittleFS block), with small
   headroom margins, and writes `build/.spangap-sizes`.
3. If those differ from what was configured, it touches `partitions.csv` (a CMake
   configure-dependency) and **rebuilds once**. `bootstrap.cmake` reads
   `.spangap-sizes` and regenerates `partitions.csv` sized to actual — `app`
   rounded to 64 K, `fixed` to 4 K, **no cap on `fixed`** (bounded only by the
   floor). IDF regenerates the table, flash offsets, and LittleFS image
   consistently.

Steady-state rebuilds measure the same sizes and stay **single-pass**. A build
whose `app + fixed` exceeds the floor errors clearly — raise
`CONFIG_ESPTOOLPY_FLASHSIZE` (or trim).

The headroom margins (app `.bin` +128 K, data +64 K over per-file rounding) let
ordinary build-to-build growth fit the cached partition without forcing an extra
pass; they are not byte-exact.

## Kconfig

- `CONFIG_SPANGAP_STATE_PERCENT` (10–75, default 50) — state's share of real
  flash. Replaces the old `CONFIG_SPANGAP_APP_PERCENT`.
- `CONFIG_ESPTOOLPY_FLASHSIZE_*MB` — the floor. Defaulted to 4 MB by
  `sdkconfig.defaults.spangap`; a board overrides to pin a size.

## Updating in the field

Updates use the single-slot **`updater`** model, not A/B: a tiny permanent
updater app reflashes `app`/`fixed` from a staged image, while signature
verification and download stay in the main app. See the
[`updater` straddle](../../updater/) and
[`/straddles/plans/updater-straddle-staging-trigger.md`].

## Gotchas

- Editing `spangap-inside` (the two-pass logic) needs a host image rebuild to
  take effect through `spangap build`; test in-container by running the source
  with **system** python (`. $IDF_PATH/export.sh && /usr/bin/python3
  .../spangap-inside build …` — the IDF venv python lacks `jsonschema`).
- High-region `state` access depends on the runtime flash-size bump (step 2
  above) reaching the upper flash through the cache/MMU — validate on hardware
  for a new board/chip.
