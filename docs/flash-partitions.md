# Flash partitions — floor image, runtime-grown state, shrink-wrap

spangap ships a **size-agnostic floor image** (default 4 MB) that boots on any
chip ≥ the floor. The firmware grows flash to the real chip size at first boot
and creates the writable `state` partition itself. The shipped partitions
(`app`/`fixed`, and `updater` when present) are **shrink-wrapped** to their
actual sizes by the build. There is no flash-size build parameter and no A/B
layout in the default table.

## Layout

The on-flash partition table is generated into `partitions.csv` by
`esp-idf/scripts/gen-partitions.py` — **never hand-edit it**. The table holds
only what ships in the floor image; `state` is deliberately absent because it is
created at runtime.

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
it. App slots are 64 K-aligned, LittleFS partitions 4 K-aligned. Flash above
`fixed`, up to the real chip size, is where `state` lives.

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
50 % → starts at the half-flash mark). If the shipped firmware (the `fixed`
partition end) would extend past that mark, the start falls back to half that
share (state starts higher up); if it still won't fit, state is clamped to
whatever is above `fixed` and a warning is logged. Because state's start offset
is stable for a given chip size, firmware/data upgrades never move it. `state`
is formatted on first mount (`fs.cpp` `format_if_mount_failed`), so a fresh or
re-grown device self-heals to factory defaults held in the read-only `fixed`
partition — usable even unattended.

If a board pins `state` in its own table, `statePartitionEnsure()` finds it
already present and does nothing.

To pin a fixed size on a board (disabling self-grow's reach), set
`CONFIG_ESPTOOLPY_FLASHSIZE_*MB` in the board's `kconfig:` — `state` then fills
the upper `CONFIG_SPANGAP_STATE_PERCENT` of that pinned size.

## Shrink-wrap (two-pass build)

App size is only known after link, and data size only after the merge/SPA build,
so the build measures then shrinks:

1. **Pass 1** — configure with a generous provisional layout (so link and the
   LittleFS pack succeed) and build. The provisional `partitions.csv` fills the
   floor: a floor-scaled slice (1/5 of the floor) for `fixed`, the rest for
   `app`.
2. The post-build `measure_build_sizes()` reads the real `app.bin` size and the
   merged data size (each file rounded up to a 4 KB LittleFS block).
3. If the measured sizes differ from what was configured, the build touches
   `partitions.csv` (a CMake configure-dependency) and **rebuilds once**.
   `bootstrap.cmake` regenerates `partitions.csv` sized to actual — `app`
   rounded to 64 K, `fixed` to 4 K (bounded only by the floor). IDF regenerates
   the table, flash offsets, and LittleFS image consistently.

Steady-state rebuilds measure the same sizes and stay **single-pass**. A build
whose `app + fixed` exceeds the floor errors clearly, naming
`CONFIG_ESPTOOLPY_FLASHSIZE` and pointing at trimming `app`/`fixed`.

The measure step adds small headroom margins — `app.bin` +128 K, data +64 K over
the per-file 4 KB rounding — so ordinary build-to-build growth fits the cached
partition without forcing an extra pass. They are not byte-exact.

## Kconfig

- `CONFIG_SPANGAP_STATE_PERCENT` (range 10–75, default 50) — state's share of
  real flash.
- `CONFIG_ESPTOOLPY_FLASHSIZE_*MB` — the floor. Defaulted to 4 MB by
  `sdkconfig.defaults.spangap`; a board overrides it to pin a size.

## Updating in the field

Field updates use the single-slot **`updater`** model. A tiny permanent updater
app (the `ota_1` slot, kept low and stable) reflashes `app`/`fixed` from a staged
image; the main app stages the image and flips the boot selection. See the
[`updater` straddle](../../updater/) for the full flow.

Only the groundwork exists today: stage-check, the boot-flip into the updater,
and the runtime `state` / on-flash `updater` partitions. **Signed download and
signature verification are not implemented yet** — there is no trust check on a
staged image.

The legacy A/B `ota` path is not used. The default table ships no second app
slot; `ota` appears only as the superseded A/B model the floor layout replaces.

## Gotchas

- High-region `state` access depends on the runtime flash-size bump (step 2
  under self-grow) reaching the upper flash through the cache/MMU — validate on
  hardware for a new board or flash chip.
