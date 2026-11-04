#!/bin/sh
# Build and run test_its_pickup.cpp against an its.cpp (default: this tree's)
# on the FreeRTOS Linux port. BUILD is a linux-target ESP-IDF build directory
# of an app including spangap-core; its compile flags and component archives
# are reused, its app (libmain) is not; it must match the host architecture.
#   BUILD=<linux build dir> sh test_its_pickup.sh [its.cpp]
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
SRC=${1:-$HERE/../../src/its.cpp}
OUT=${OUT:-/tmp/test_its_pickup}
: "${BUILD:?set BUILD to a linux-target build directory}"
mkdir -p "$OUT"

FLAGS=$(python3 -I - "$BUILD/compile_commands.json" <<'EOF'
import json, shlex, sys
for e in json.load(open(sys.argv[1])):
    if e['file'].endswith('/spangap-core/src/its.cpp'):
        a = shlex.split(e['command'])
        keep, skip = [], False
        for x in a:
            if skip: skip = False; continue
            if x in ('-o', '-c'): skip = True; continue
            keep.append(x)
        print(shlex.join(keep)); break
EOF
)
eval "set -- $FLAGS"
CXX=$1; shift
"$CXX" -I"$HERE/../../include" "$@" -fdiagnostics-color=never -c "$SRC" -o "$OUT/its.o"
"$CXX" -I"$HERE/../../include" "$@" -fdiagnostics-color=never -c "$HERE/test_its_pickup.cpp" -o "$OUT/test.o"

LIBS=$(find "$BUILD/esp-idf" -name '*.a' ! -path '*/main/*' | sort)
"$CXX" -o "$OUT/test_its_pickup" "$OUT/test.o" "$OUT/its.o" \
    -Wl,--start-group $LIBS -Wl,--end-group \
    -Wl,--gc-sections -lbsd -lpthread -ldl -l:libz.so.1
"$OUT/test_its_pickup"
