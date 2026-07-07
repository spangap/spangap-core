#!/usr/bin/env bash
# Host tests for spanfs: pack a fixture, walk it with the C reference reader,
# compare bytes against the source tree, assert determinism and CRC failure.
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
comp="$(dirname "$here")"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

cc="${CC:-cc}"
reader="$work/host_reader"
"$cc" -std=c11 -Wall -Wextra -Werror -I"$comp/include" \
    "$comp/src/spanfs.c" "$here/host_reader.c" -o "$reader"

# ---- fixture tree ----
src="$work/src"
mkdir -p "$src/webroot/assets" "$src/factory_state/storage" "$src/empty_ok/sub"
printf 'hello world\n'            > "$src/webroot/index.html"
printf '<gz-bytes\x00\x01\x02>'   > "$src/webroot/app.js.gz"
head -c 5000 /dev/urandom         > "$src/webroot/assets/font.ttf"
printf '{}'                       > "$src/factory_state/storage/s.json"
: > "$src/factory_state/empty"          # zero-length file
printf 'x'                        > "$src/a.txt"      # sorts before webroot/
printf 'z'                        > "$src/z.txt"

img="$work/fixed.bin"
python3 "$comp/tools/mkspanfs.py" "$src" "$img"

echo "== enumerate =="
"$reader" "$img"

# ---- byte-for-byte compare every file ----
echo "== compare bytes =="
fail=0
while IFS= read -r rel; do
    "$reader" "$img" cat "$rel" > "$work/got" || { echo "MISSING: $rel"; fail=1; continue; }
    if ! cmp -s "$work/got" "$src/$rel"; then echo "DIFF: $rel"; fail=1; fi
done < <(cd "$src" && find . -type f | sed 's#^\./##' | sort)
[ "$fail" = 0 ] && echo "all files match"

# count parity
n_src=$(cd "$src" && find . -type f | wc -l | tr -d ' ')
n_img=$("$reader" "$img" | wc -l | tr -d ' ')
[ "$n_src" = "$n_img" ] || { echo "COUNT MISMATCH src=$n_src img=$n_img"; fail=1; }

# ---- determinism ----
echo "== determinism =="
img2="$work/fixed2.bin"
python3 "$comp/tools/mkspanfs.py" "$src" "$img2"
cmp -s "$img" "$img2" && echo "byte-identical repack" || { echo "NON-DETERMINISTIC"; fail=1; }

# ---- corruption rejected ----
echo "== corruption =="
cp "$img" "$work/bad.bin"
# flip a byte inside the index (offset 24) -> CRC mismatch
python3 - "$work/bad.bin" <<'PY'
import sys
p=sys.argv[1]
b=bytearray(open(p,'rb').read())
b[24]^=0xFF
open(p,'wb').write(b)
PY
if "$reader" "$work/bad.bin" check 2>/dev/null; then echo "CRC NOT ENFORCED"; fail=1; else echo "corrupt image rejected"; fi
# truncated image
head -c 10 "$img" > "$work/trunc.bin"
if "$reader" "$work/trunc.bin" check 2>/dev/null; then echo "TRUNCATION NOT REJECTED"; fail=1; else echo "truncated image rejected"; fi

echo
[ "$fail" = 0 ] && echo "SPANFS HOST TESTS: PASS" || { echo "SPANFS HOST TESTS: FAIL"; exit 1; }
