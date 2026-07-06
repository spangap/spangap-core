#!/usr/bin/env python3
"""Generate spangap's partitions.csv — size-agnostic, state-less layout.

The flashed image is a **floor-sized** image (default 4 MB) that boots on any
chip >= that size; the firmware grows flash to the real size at first boot and
registers the `state` LittleFS partition itself (esp_partition_register_external,
no on-flash table entry). So the partition table here contains ONLY the things
that ship in the image:

    nvs        0x09000   0x05000     IDF internal (WiFi PHY cal)
    otadata    0x0E000   0x02000     (only with updater — selects app vs updater)
    updater    0x10000   0x80000     (only with updater) tiny serial-updated flasher (ota_1)
    app        ...        <remainder> firmware (factory, or ota_0 when updater present)
    fixed      <high>     <wrapped>   read-only LittleFS (SPA + factory defaults)
    reserved   ...        <filler>    inert; carries the table top up to the state floor

`state` is deliberately absent — it lives above the table (from the state floor
to the top of real flash) and is created at runtime (statePartitionEnsure() in
fs.cpp). Flash order keeps the updater BELOW app so its slot never moves.

## Why `app` is the remainder (and nothing is measured-then-pinned)

The firmware region is everything between `app`'s start and the state floor
(= CONFIG_SPANGAP_MAX_FIRMWARE_KB). We place `fixed` at the TOP of that region
(just under the floor) and give `app` all the space below it:

    app_size = fixed_start - app_start          (fixed_start = floor - fixed_size)

So `app`'s partition size is a pure function of `fixed`'s size and the Kconfig
floor — it does NOT depend on app.bin. app.bin just has to fit the (large)
remainder, which `check_sizes` verifies during the build, and the real ceiling
(app.bin + fixed <= firmware region) is asserted in the final pass via the
optional --app-bytes (a fresh post-build measurement, never a cached size). This
is what lets the build be single-pass with nothing carried between runs: the only
measured quantity that sizes the table is `fixed`, and it is packed from static
assets, so it is known before app.bin is even linked.

Two modes:
  * provisional (--fixed-bytes 0): `fixed` gets a slice of the firmware region so
    the first configure can pack the littlefs image; `app` gets the rest.
  * final (real --fixed-bytes from the post-build measure): `fixed` shrink-wrapped
    to 4 K, placed high; `app` grows into the freed space. Everything must fit the
    firmware region or it errors.

App partitions are 64 K-aligned; LittleFS partitions 4 K-aligned.
"""
import argparse
import sys
from pathlib import Path

APP_START   = 0x10000   # first app/updater partition (64K-aligned by spec)
APP_ALIGN   = 0x10000   # app slots must be 64K-aligned
DATA_ALIGN  = 0x1000    # LittleFS partitions: 4K alignment
NVS_START   = 0x9000
NVS_SIZE    = 0x5000
OTADATA_START = 0xE000
OTADATA_SIZE  = 0x2000
# Fixed updater slot when present (never shrink-wrapped — keeping it fixed means
# `app`'s start offset is stable, so app.bin is never relinked).
UPDATER_SIZE = 0x80000   # 512 KB
# Provisional fixed reserve = this fraction of the FIRMWARE REGION (the space
# below the state floor, not the whole flash), so the first littlefs pack has
# room while `app` still gets the majority; the post-build pass shrinks it.
FIXED_PROVISIONAL_DIV = 5        # 1/5 = 20% of the firmware region


def align_up(x: int, a: int) -> int:
    return ((x + a - 1) // a) * a


def align_down(x: int, a: int) -> int:
    return (x // a) * a


def render(flash_mb: int, updater: bool,
           fixed_bytes: int, app_bytes: int = 0,
           state_floor: int = 0) -> str:
    # `container` = the FLASH size (CONFIG_ESPTOOLPY_FLASHSIZE): the bootloader
    # header size and the ceiling every partition must fit under. `floor` = the
    # builder's MAX FIRMWARE SIZE (CONFIG_SPANGAP_MAX_FIRMWARE_KB): the offset
    # where the runtime `state` partition begins. `app_bytes`, when given, is a
    # fresh post-build measurement used ONLY to assert the ceiling — it never
    # sizes a partition, so it can't stale-pin anything.
    container = flash_mb * 1024 * 1024
    provisional = (fixed_bytes == 0)
    floor = align_down(state_floor if state_floor else container, DATA_ALIGN)
    if floor > container:
        sys.exit(f"gen-partitions: state floor ({floor:#x}) exceeds the {flash_mb}MB "
                 f"flash container ({container:#x}) — raise CONFIG_ESPTOOLPY_FLASHSIZE")

    rows: list[tuple[str, str, str, int, int, str]] = []
    rows.append(("nvs", "data", "nvs", NVS_START, NVS_SIZE, ""))

    offset = APP_START
    if updater:
        rows.append(("otadata", "data", "ota", OTADATA_START, OTADATA_SIZE, ""))
        rows.append(("updater", "app", "ota_1", offset, UPDATER_SIZE, ""))
        offset += UPDATER_SIZE
        app_sub = "ota_0"
    else:
        app_sub = "factory"
    app_start = offset

    if provisional:
        # A slice of the firmware region for `fixed`; `app` gets the rest. Scales
        # with the floor so the first littlefs pack has room without starving app.
        fixed_sz = align_up((floor - app_start) // FIXED_PROVISIONAL_DIV, DATA_ALIGN)
    else:
        fixed_sz = align_up(fixed_bytes, DATA_ALIGN)

    # Place `fixed` as high as possible under the floor; `app` fills below it.
    fixed_start = align_down(floor - fixed_sz, DATA_ALIGN)
    if fixed_start <= app_start:
        sys.exit(f"gen-partitions: fixed data ({fixed_sz:#x}) leaves no room for app "
                 f"below the state floor ({floor:#x}) — trim fixed data or raise "
                 "CONFIG_SPANGAP_MAX_FIRMWARE_KB / CONFIG_ESPTOOLPY_FLASHSIZE")
    # `app` gets everything from app_start up to fixed_start (64K-aligned; any
    # sub-64K remainder is an unflashed gap just below `fixed`).
    app_sz = align_down(fixed_start - app_start, APP_ALIGN)
    if app_sz < APP_ALIGN:
        sys.exit(f"gen-partitions: firmware region below the state floor ({floor:#x}) "
                 "is too small for an app partition")
    # Ceiling assertion (final pass only): the real app.bin must fit the remainder.
    # This is where the build ENFORCES the max firmware size — a genuine overflow
    # of app + fixed past the floor lands here, not as a silent bad image.
    if not provisional and app_bytes and app_bytes > app_sz:
        sys.exit(f"gen-partitions: firmware (app.bin {app_bytes:#x} + fixed {fixed_sz:#x}) "
                 f"exceeds the firmware region below the state floor ({floor:#x}) — app "
                 f"partition is only {app_sz:#x}. Trim app/fixed or raise "
                 "CONFIG_SPANGAP_MAX_FIRMWARE_KB.")
    rows.append(("app", "app", app_sub, app_start, app_sz, ""))
    rows.append(("fixed", "data", "spiffs", fixed_start, fixed_sz, "readonly"))

    # `reserved` carries the table top up to the floor so `state` (runtime, above
    # the table) always begins at exactly the floor, independent of `fixed`'s
    # size. Data subtype 0x40: an inert, unmounted reserve fs.cpp never opens.
    fixed_end = fixed_start + fixed_sz
    if floor > fixed_end:
        rows.append(("reserved", "data", "0x40", fixed_end, floor - fixed_end, ""))

    floorpart = f", state floor {floor/1024/1024:.2f}MB" if (state_floor and floor != container) else ""
    out = [
        "# Generated by spangap — do not edit. Size-agnostic floor image;",
        "# `state` is created at runtime (fs.cpp) above the table top, not listed here.",
        f"# Container {flash_mb}MB, updater={'on' if updater else 'off'}, "
        f"{'provisional' if provisional else 'shrink-wrapped'}{floorpart}",
        "#",
        "# Name,    Type, SubType, Offset,    Size,      Flags",
    ]
    for name, ty, sub, off, sz, flags in rows:
        out.append(f"{name+',':10s} {ty+',':5s} {sub+',':8s} {off:#010x}, {sz:#010x},"
                   + (f" {flags}" if flags else ""))
    return "\n".join(out) + "\n"


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--flash-mb", type=int, required=True, choices=[4, 8, 16, 32, 64, 128],
                    help="Floor size = configured CONFIG_ESPTOOLPY_FLASHSIZE (default 4)")
    ap.add_argument("--updater", choices=["y", "n"], required=True)
    ap.add_argument("--fixed-bytes", type=int, default=0, help="Actual fixed data size (0 = provisional)")
    ap.add_argument("--app-bytes", type=int, default=0,
                    help="Actual app .bin size — final-pass ceiling assertion only, "
                         "never sizes a partition (0 = skip the check)")
    ap.add_argument("--state-floor", type=int, default=0,
                    help="Max firmware size in bytes = where /state begins "
                         "(0 = default to the flash container)")
    ap.add_argument("--out", required=True)
    args = ap.parse_args()

    content = render(args.flash_mb, args.updater == "y",
                     args.fixed_bytes, args.app_bytes, args.state_floor)

    out = Path(args.out)
    # Skip the write when unchanged — keeps CMake from a needless reconfigure cascade.
    if out.exists() and out.read_text() == content:
        return 0
    out.write_text(content)
    return 0


if __name__ == "__main__":
    sys.exit(main())
