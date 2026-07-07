#!/usr/bin/env python3
"""mkspanfs — pack a directory tree into a spanfs v1 image.

Read-only, mmap-native, no compression, no directory entries (dirs are implicit
in paths). Deterministic: a sorted walk with no timestamps/owners/modes and no
environment leakage, so packing the same tree twice is byte-identical.

Format v1 (little-endian, offsets from image start):

    [0]   header, 20 bytes
          u32 magic 0x53504653, u8 version 1, u8 rsvd, u16 rsvd,
          u32 entry_count, u32 total_size, u32 crc32 (over index + path blob)
    [20]  index: entry_count x 12 bytes (u32 path_off, u32 data_off, u32 data_len),
          sorted bytewise by path
    [..]  path blob (NUL-terminated UTF-8 paths, no leading '/')
    [..]  file data (each data_off aligned to 4)

Only the Python standard library is used.
"""
import argparse
import binascii
import os
import struct
import sys
from pathlib import Path

MAGIC = 0x53504653
VERSION = 1
HEADER_SZ = 20
ENTRY_SZ = 12
DATA_ALIGN = 4


def align_up(x: int, a: int) -> int:
    return (x + a - 1) // a * a


def collect(root: Path) -> list[tuple[str, bytes]]:
    """(relative_path, data) for every regular file under root, sorted bytewise
    by the UTF-8 path. Paths use '/' separators and carry no leading '/'."""
    entries: list[tuple[str, bytes]] = []
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames.sort()
        for name in sorted(filenames):
            full = Path(dirpath) / name
            if full.is_symlink() or not full.is_file():
                continue
            rel = full.relative_to(root).as_posix()
            entries.append((rel, full.read_bytes()))
    # Bytewise sort on the encoded path (matches the C reader's strcmp on
    # unsigned char and readdir's prefix range walk).
    entries.sort(key=lambda e: e[0].encode("utf-8"))
    return entries


def pack(root: Path) -> bytes:
    entries = collect(root)
    count = len(entries)

    # Path blob layout (path_off values), relative to image start.
    path_blob = bytearray()
    path_offs: list[int] = []
    blob_start = HEADER_SZ + count * ENTRY_SZ
    for rel, _ in entries:
        path_offs.append(blob_start + len(path_blob))
        path_blob += rel.encode("utf-8") + b"\x00"

    # Data region: each file 4-byte aligned, in the same sorted order (so
    # data_offs increase monotonically and entry 0 marks the region start).
    data_start = align_up(blob_start + len(path_blob), DATA_ALIGN)
    data_offs: list[int] = []
    data_blob = bytearray()
    cur = data_start
    for _, payload in entries:
        pad = align_up(cur, DATA_ALIGN) - cur
        data_blob += b"\x00" * pad
        cur += pad
        data_offs.append(cur)
        data_blob += payload
        cur += len(payload)

    total_size = cur

    index = bytearray()
    for i, (_, payload) in enumerate(entries):
        index += struct.pack("<III", path_offs[i], data_offs[i], len(payload))

    align_pad = b"\x00" * (data_start - (blob_start + len(path_blob)))

    # CRC over the metadata region [20, data_start): index + path blob + the
    # padding up to the data region. Data itself is excluded (it's the
    # flasher's/updater's responsibility). The reader recomputes this exact
    # range — its upper bound is entry 0's data_off == data_start.
    meta = bytes(index) + bytes(path_blob) + align_pad
    crc = binascii.crc32(meta) & 0xFFFFFFFF

    header = struct.pack("<IBBHIII", MAGIC, VERSION, 0, 0, count, total_size, crc)

    img = bytearray()
    img += header
    img += meta          # index + path blob + alignment padding to data_start
    img += data_blob     # already internally padded to keep each data_off aligned
    assert len(img) == total_size, (len(img), total_size)
    return bytes(img)


def main() -> int:
    ap = argparse.ArgumentParser(description="Pack a directory into a spanfs image.")
    ap.add_argument("srcdir", help="directory tree to pack")
    ap.add_argument("image", help="output image path")
    ap.add_argument("--max-size", type=lambda s: int(s, 0), default=0,
                    help="error if the (byte-exact) image exceeds this partition size")
    ap.add_argument("--pad-to", type=lambda s: int(s, 0), default=0,
                    help="pad the image up to this size with 0xff (0 = byte-exact)")
    args = ap.parse_args()

    root = Path(args.srcdir)
    if not root.is_dir():
        sys.exit(f"mkspanfs: {root} is not a directory")
    img = pack(root)
    limit = args.max_size or args.pad_to
    if limit and len(img) > limit:
        sys.exit(f"mkspanfs: image {len(img)} bytes exceeds partition/limit {limit} "
                 "— trim /fixed data or raise the partition size")
    if args.pad_to and len(img) < args.pad_to:
        img = img + b"\xff" * (args.pad_to - len(img))
    Path(args.image).write_bytes(img)
    return 0


if __name__ == "__main__":
    sys.exit(main())
