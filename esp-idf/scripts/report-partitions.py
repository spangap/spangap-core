#!/usr/bin/env python3
"""Print the partition layout (name / offset / size) for `spangap build`.

Reads the generated partitions.csv and prints a compact table so the build log
shows exactly how flash is carved up. Note `state` is NOT here — it's created at
runtime in the upper half of flash (see fs.cpp), so this reports only what ships
in the floor image.
"""
import argparse
import sys
from pathlib import Path


def kb(n: int) -> str:
    return f"{n/1024:.0f}K" if n < 1024 * 1024 else f"{n/1024/1024:.2f}M"


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--partitions", required=True)
    args = ap.parse_args()

    p = Path(args.partitions)
    if not p.exists():
        return 0

    rows = []
    for raw in p.read_text().splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        cols = [c.strip() for c in line.split(",")]
        if len(cols) < 5:
            continue
        name, ty, sub, off, sz = cols[:5]
        try:
            off_i, sz_i = int(off, 0), int(sz, 0)
        except ValueError:
            continue
        rows.append((name, sub, off_i, sz_i))

    if not rows:
        return 0

    end = max(o + s for _, _, o, s in rows)
    print("── spangap partition layout (floor image; `state` added at runtime) ──")
    print(f"   {'name':<10} {'subtype':<8} {'offset':>10} {'size':>10} {'':>6}")
    for name, sub, off, sz in rows:
        print(f"   {name:<10} {sub:<8} {off:#010x} {sz:#010x} {kb(sz):>6}")
    print(f"   {'─'*46}")
    print(f"   image end {end:#010x} ({kb(end)} used of the floor)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
