#!/bin/bash
# reallyclean — remove every build/cache artifact in a diptych-consuming
# project, going beyond `idf.py fullclean` (which only nukes build/).
#
# Wired into consumers as `idf.py reallyclean` via a custom target in
# diptych-core/CMakeLists.txt.
#
# Order: everything except build/ first, then build/ last. When invoked
# via ninja's custom-target machinery, deleting build/ kills ninja's
# stdout/stderr capture files; doing it last means all other cleanup is
# already complete by the time that happens.
#
# Usage: bash reallyclean.sh <project_root>
set -e
proj="${1:?project root required}"
proj="$(cd "$proj" && pwd)"
cd "$proj"

# IDF artifacts (build/ comes last, separately)
rm -rf managed_components
rm -f  sdkconfig sdkconfig.old dependencies.lock

# Generated source files (regenerated each build by tools/*.py)
rm -f  main/app_build_epoch.c main/build_id.h

# LittleFS image inputs that are generated, not source-of-truth
rm -f  data/build_times
rm -rf data/webroot

# Browser side
rm -rf web-interface/node_modules \
       web-interface/dist \
       web-interface/.quasar \
       web-interface/package-lock.json

# macOS noise
find . -name .DS_Store -delete 2>/dev/null || true

# build/ last — destroying ninja's working tree after everything else is done.
rm -rf build

echo "reallyclean: done — repo back to source-only state."
