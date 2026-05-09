#!/bin/bash
# reallyclean — remove every build/cache artifact in a diptych-consuming
# project, going beyond `idf.py fullclean` (which only nukes build/).
#
# Invoked from the consumer's idf_ext.py as a Python idf.py action, NOT
# as a ninja custom-target. ninja-target invocation deadlocks with build/
# deletion because ninja keeps writing logs into build/log/ after the
# script returns.
#
# Usage: bash reallyclean.sh <project_root>
set -e
proj="${1:?project root required}"
proj="$(cd "$proj" && pwd)"
cd "$proj"

# rm -rf races with macOS background services (Spotlight, mds_stores) that
# drop .DS_Store into directories mid-walk → ENOTEMPTY on the final rmdir.
# rrm: try once, on failure clear .DS_Store inside and retry.
rrm() {
    rm -rf "$@" 2>/dev/null && return 0
    for path in "$@"; do
        [ -e "$path" ] || continue
        find "$path" -name .DS_Store -delete 2>/dev/null || true
    done
    rm -rf "$@"
}

# IDF artifacts (build/ comes last, separately)
rrm managed_components
rm -f  sdkconfig sdkconfig.old dependencies.lock partitions.csv

# LittleFS image inputs that are generated, not source-of-truth
rm -f  data/build_times
rrm data/webroot

# Browser side
rrm web-interface/node_modules \
    web-interface/dist \
    web-interface/.quasar
rm -f web-interface/package-lock.json

# macOS noise
find . -name .DS_Store -delete 2>/dev/null || true

# build/ last
rrm build

echo "reallyclean: done — repo back to source-only state."
