#!/usr/bin/env python3
"""Print a quick utilization summary for the `fixed` LittleFS partition.

ESP-IDF already prints app utilization (`seccam.bin binary size ... Smallest
app partition is ... X% free`); this complements it for the fixed (factory
data) side. The number is ballpark — `du` of the merged data dir, not actual
LittleFS internal usage. Good enough to flag "you're 90% full, plan ahead".
"""
import argparse
import os
import sys
from pathlib import Path


def parse_partitions(path: Path) -> dict[str, int]:
    """Return {partition_name: size_bytes} for every non-comment row."""
    out: dict[str, int] = {}
    for raw in path.read_text().splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        cols = [c.strip() for c in line.split(",")]
        if len(cols) < 5:
            continue
        name, _ty, _sub, _off, sz = cols[:5]
        try:
            out[name] = int(sz, 0)  # auto-detects 0x prefix
        except ValueError:
            continue
    return out


def du_bytes(root: Path) -> int:
    total = 0
    for dirpath, _dirs, files in os.walk(root):
        for f in files:
            try:
                total += (Path(dirpath) / f).stat().st_size
            except OSError:
                pass
    return total


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--partitions", required=True)
    ap.add_argument("--data-dir", required=True)
    ap.add_argument("--partition-name", default="fixed_a",
                    help="Partition to compare against (defaults to fixed_a, "
                         "falls back to 'fixed' if not present)")
    args = ap.parse_args()

    parts = parse_partitions(Path(args.partitions))
    target = args.partition_name
    if target not in parts and "fixed" in parts:
        target = "fixed"
    if target not in parts:
        # Nothing to report against — silently bail.
        return 0
    cap = parts[target]
    used = du_bytes(Path(args.data_dir))
    pct = (100 * used) // cap if cap else 0
    free_kb = max(0, cap - used) // 1024
    print(f"{args.data_dir.split('/')[-1] or args.data_dir} "
          f"({target}): {used // 1024} KB / {cap // 1024} KB used ({pct}%), "
          f"{free_kb} KB free")
    return 0


if __name__ == "__main__":
    sys.exit(main())
