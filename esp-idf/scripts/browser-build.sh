#!/bin/bash
# browser-build.sh <web_dir> <gen_dir> <out_dir> — build a buildable's SPA for
# one firmware build: lay the files `spangap build` generated for this build's
# staged set (<gen_dir>, in <web_dir>'s own layout — see write_browser_dispatch)
# into <web_dir>, then run its deploy.sh into <out_dir>.
#
# The SPA compiles in its source dir (npm's node_modules/, Quasar's .quasar/),
# so builds running side by side take turns on it: a lock per <web_dir>, held
# from laying the files in until the bundle is out. The lock lives in /tmp, the
# container's own file system, where every build of a workspace runs.
set -e
WEB="$(cd "$1" && pwd -P)"
GEN="$2"
OUT="$3"

exec 9>"/tmp/spangap-web-$(printf '%s' "$WEB" | md5sum | cut -c1-16).lock"
flock 9

if [ -d "$GEN" ]; then
    mkdir -p "$WEB/src/boot"
    cmp -s "$GEN/src/boot/straddles.gen.ts" "$WEB/src/boot/straddles.gen.ts" ||
        cp "$GEN/src/boot/straddles.gen.ts" "$WEB/src/boot/straddles.gen.ts"
    rm -rf "$WEB/src/app-icons"
    cp -R "$GEN/src/app-icons" "$WEB/src/app-icons"
    # Copied only when it differs: deploy.sh reinstalls whenever package.json
    # is newer than the last install.
    cmp -s "$GEN/package.json" "$WEB/package.json" ||
        cp "$GEN/package.json" "$WEB/package.json"
fi

# fd 9 stays with this shell: a process the SPA build leaves behind must not
# keep the lock.
bash "$WEB/deploy.sh" "$OUT" 9>&-
