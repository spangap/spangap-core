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

`state` starts **at the floor — the top of the on-flash partition table** — and
runs to the end of the real chip (`size = phys − floor`). The table is topped out
at the builder's declared **maximum firmware size** (`CONFIG_SPANGAP_MAX_FIRMWARE_KB`)
by a trailing empty `reserved` partition, so `statePartitionEnsure()` recovers the
floor by taking the highest partition end — no value is baked into the app. This
gives a deterministic start and a chip-scaled size (a bigger part yields a bigger
`state` with no rebuild). The floor is byte-granular and independent of
`CONFIG_ESPTOOLPY_FLASHSIZE` (the power-of-two flash *container* / bootloader-header
size, which only has to be a valid enum `≥` the floor). It is `≥` the shipped
firmware by construction — the build errors if `app + fixed` overflow it — so
there is no "underwater" case, and it never moves as `app`/`fixed` grow within it.

The floor is a **build-time policy number, not the chip size**: it must be set
below the smallest physical flash you ship on, or that device gets no `state`
(the start lands at/above `phys` and `statePartitionEnsure()` logs a warning and
skips). If the builder changes the floor between releases, the table top and thus
state's start move, its old LittleFS superblock is no longer found, and the first
mount reformats it (`fs.cpp` `format_if_mount_failed`) — a clean factory reset.
**Warn users before bumping the floor.** The same `format_if_mount_failed` means a
fresh or re-grown device self-heals to the factory defaults held in the read-only
`fixed` partition — usable even unattended.

If a board pins `state` in its own table, `statePartitionEnsure()` finds it
already present and does nothing.

## Shrink-wrap (single build + in-place finalize)

App size is only known after link, and data size only after the merge/SPA build,
so the build measures then shrinks:

1. **Build once** — configure with a generous provisional layout (so link and the
   LittleFS pack succeed) and build. The provisional `partitions.csv` fills the
   container: a container-scaled slice (1/5) for `fixed`, the rest for `app`, and
   no `reserved`/floor yet.
2. The post-build `measure_build_sizes()` reads the real `app.bin` size and the
   merged data size (each file rounded up to a 4 KB LittleFS block).
3. If the measured sizes differ from what was configured, `finalize_shrink_wrap()`
   (in `spangap-inside`) regenerates **only the four artifacts a resize changes,
   in place** — no second build:
   - `partitions.csv` (gen-partitions.py, `app` rounded to 64 K, `fixed` to 4 K,
     `reserved` carrying the table top up to the floor),
   - `partition-table.bin` (IDF's `gen_esp32part.py`),
   - `fixed.bin` (littlefs re-pack — deterministic, so byte-identical to a rebuild),
   - the `fixed` offset in `flasher_args.json`.

   `app.bin` and `bootloader.bin` are reused verbatim — `app.bin` is linked at a
   fixed offset and its content is independent of the partition *size*, so it is
   never relinked. This replaces the old touch-`partitions.csv`-and-rebuild (which
   forced a ~30 s CMake reconfigure); that full rebuild remains an automatic
   **fallback** if the in-place finalize errors, so a broken fixup can't ship a
   bad image. (`bootstrap.cmake` still reads the cached `.spangap-sizes` on the
   *next* build, keeping any later reconfigure single-pass.)

Steady-state rebuilds measure the same sizes and skip the finalize entirely. A
build whose `app + fixed` exceeds the flash container errors naming
`CONFIG_ESPTOOLPY_FLASHSIZE`; one that fits the container but exceeds the floor
errors naming `CONFIG_SPANGAP_MAX_FIRMWARE_KB` — both point at trimming
`app`/`fixed`.

The measure step adds small headroom margins — `app.bin` +128 K, data +64 K over
the per-file 4 KB rounding — so ordinary build-to-build growth fits the cached
partition without forcing an extra pass. They are not byte-exact.

## Kconfig

- `CONFIG_SPANGAP_MAX_FIRMWARE_KB` — the floor = the maximum firmware size and the
  offset where `state` begins (byte-granular; `0` defaults to the whole container).
  Must be `≤ CONFIG_ESPTOOLPY_FLASHSIZE` and below the physical flash of every
  device you ship on, or those devices get no `state`.
- `CONFIG_ESPTOOLPY_FLASHSIZE_*MB` — the flash *container* / bootloader-header
  size. A power-of-two enum `≥` the floor. Defaulted to 4 MB by
  `sdkconfig.defaults.spangap`; a board/buildable overrides it.

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
