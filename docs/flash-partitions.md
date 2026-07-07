# Flash partitions — floor image, runtime-grown state, shrink-wrap

spangap ships a **size-agnostic floor image** (default 4 MB) that boots on any
chip ≥ the floor. The firmware grows flash to the real chip size at first boot
and creates the writable `state` partition itself. `fixed` (the read-only data
image) is placed high, just under the firmware ceiling, and **`app` takes all the
space below it** — so `app`'s partition size is the remainder, never a measured,
pinned number. Only `fixed` is shrink-wrapped. There is no flash-size build
parameter and no A/B layout in the default table.

## Layout

The on-flash partition table is generated into `partitions.csv` by
`esp-idf/scripts/gen-partitions.py` — **never hand-edit it**. The table holds
only what ships in the floor image; `state` is deliberately absent because it is
created at runtime.

Without the updater (default):

```
nvs       data, nvs,     0x9000,   0x5000     IDF internal (WiFi PHY cal)
app       app,  factory, 0x10000,  <remainder> firmware — everything up to `fixed`
fixed     data, 0x8a,    <high>     <wrapped>  read-only spanfs (SPA + factory defaults)
reserved  data, 0x40,    ...        <filler>   inert; carries the table top to the floor
```

With `spangap/updater` staged:

```
nvs       data, nvs,     0x9000,   0x5000
otadata   data, ota,     0xe000,   0x2000     selects app vs updater
updater   app,  ota_1,   0x10000,  0x80000    tiny serial-updated flasher (low/stable)
app       app,  ota_0,   0x90000,  <remainder> firmware — everything up to `fixed`
fixed     data, 0x8a,    <high>     <wrapped>  read-only spanfs
reserved  data, 0x40,    ...        <filler>   inert; carries the table top to the floor
```

The updater sits **below** `app` at a fixed 512 K slot, so `app`'s start offset
is stable and `app.bin` is never relinked. `fixed` is placed as high as possible
(just under the firmware ceiling); `app` fills the gap between the updater/nvs and
`fixed`, so it is sized purely by `fixed`'s size and the ceiling — the build never
measures or pins `app`. App slots are 64 K-aligned, data partitions
4 K-aligned. The trailing `reserved` partition carries the table top up to the
floor, so `state` (above the table, up to the real chip size) always starts at
exactly the floor regardless of `fixed`'s size.

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

## Sizing: zero state between runs

Nothing about the layout is carried from one build to the next — no cached sizes,
no hint file. Both partitions derive from measurements taken *within* the build:

**`app` is never measured or pinned.** It is the space **below** a high-placed
`fixed`, so its partition size is a pure function of `fixed`'s size and the
firmware ceiling. The old two-pass "measure app.bin → feed its size back into the
next build's table" loop — and its cross-build staleness, where a binary that grew
past the cached size overflowed a too-small `app` partition — is gone. `check_sizes`
validates `app.bin` against the (large) remainder during the build; the real
ceiling (`app.bin + fixed ≤` the firmware region below the floor) is asserted in
the finalize pass and errors naming `CONFIG_SPANGAP_MAX_FIRMWARE_KB`.

**`fixed` is measured, but only within the same build.** The data image is only
assembled *by* the build graph (straddle data dirs are discovered during
configure; `spangap-lcd` rasterizes icons into it with build-time tooling) — so
`fixed`'s real size isn't knowable when `partitions.csv` is generated at
configure. Rather than cache it, the build sizes `fixed` **generously** and
shrink-wraps it afterward:

1. **Configure** — `bootstrap.cmake` always generates a *provisional* `partitions.csv`:
   `fixed` gets a generous slice of the firmware region, `app` the rest. The size is
   only ever a safe upper bound, never exact — so it needs no prior-run input.
2. **Build** — link `app.bin` and pack the spanfs image at the generous size. The
   pack can never `NOSPC` (it always has room), and `app`, the remainder, is still
   huge so `check_sizes` passes with margin.
3. **Post-build** — `measure_build_sizes()` takes `fixed`'s real size as the packed
   `fixed.bin`'s byte size (a spanfs image is byte-exact — no filesystem block
   rounding or free-space cushion). If it differs from the generous size in
   `partitions.csv` (this build's own table — not a cross-run cache),
   `finalize_shrink_wrap()` regenerates, **in place, with no second build**, the
   artifacts a `fixed` resize touches: `partitions.csv` (gen-partitions.py),
   `partition-table.bin` (IDF's `gen_esp32part.py`), and the `fixed` offset in
   `flasher_args.json`. `fixed.bin` itself is reused verbatim — a spanfs image is
   placement- and partition-size-independent — as are `app.bin` and `bootloader.bin`
   (`app.bin` is linked at a fixed offset, independent of any partition *size*, so
   it is never relinked). On any failure it falls back to a full rebuild so a
   broken fixup can't ship a bad image.

Shrinking `fixed` only ever *grows* `app` (a smaller `fixed` moves higher, freeing
the space below it), so the generous-then-shrink dance can never overflow `app` and
the two passes never disagree about app.bin's validity. A rebuild that doesn't
reconfigure keeps the shrink-wrapped `partitions.csv` and re-measures the same size,
so the finalize is a no-op.

`fixed` is byte-exact (gen-partitions.py rounds it up only to the 4 K data
alignment); the `app.bin` ceiling assertion keeps ~128 K of app slack so
ordinary build-to-build growth doesn't trip the ceiling.

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

The 512 K updater slot is a fixed size, not shrink-wrapped, and carries no
growth padding — padding only helps if the binary can grow into it without a
reflash, but the updater can only change via a serial flash, which rewrites the
whole layout anyway, so pre-padding would just steal flash from `state`. The
size itself comes from what a no-network, no-crypto flasher needs: IDF baseline
plus littlefs, `esp_ota`/partition write, and optional inflate — roughly
340–580 KB (signature verification is app-side, in the stage path, so no
mbedTLS). Note the updater is the flasher and structurally skips the slot it
runs from — it can never write a new copy of *itself* (XIP self-overwrite), so
updating the updater binary is serial-only by construction.

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
