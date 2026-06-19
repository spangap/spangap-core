# spangap-core pre-project bootstrap. Consumers `include()` this file BEFORE
# `project()`, like:
#
#   include("${CMAKE_SOURCE_DIR}/managed_components/spangap__spangap-core/cmake/bootstrap.cmake")
#
# The build tooling stages spangap-core into managed_components/ before CMake
# runs — committed consumer files carry no workspace-relative paths.
#
# What it does:
#   1. Prepends spangap's sdkconfig.defaults.spangap to SDKCONFIG_DEFAULTS,
#      and appends the spangap-inside-generated staging/sdkconfig.spangap-overrides
#      (which carries CLI-driven values like --flash-size). Order: platform
#      defaults → consumer's sdkconfig.defaults → CLI overrides. Later wins.
#   2. Runs the sdkconfig.defaults staleness check: if any SDKCONFIG_DEFAULTS
#      file changed since sdkconfig was last seeded from it, delete sdkconfig so
#      IDF reseeds from the new defaults. This works around IDF's one-shot
#      defaults behavior (without it, edited defaults are silently ignored until
#      you `rm sdkconfig`). The check covers the override file too, so changing
#      --flash-size between builds takes effect. Suppressed by a
#      `.spangap-manual-kconfig` marker in the workspace (`spangap menuconfig`
#      sets it, `spangap autoconfig` clears it) for deliberate hand-tuning.
#   3. Generates `${CMAKE_SOURCE_DIR}/partitions.csv` from CONFIG_SPANGAP_APP_PERCENT,
#      CONFIG_ESPTOOLPY_FLASHSIZE_*MB, and whether `staging/components/ota/`
#      exists (the post-Kconfig-cleanup signal for "OTA is in this build").
#      With OTA on, partitions are paired A/B; with OTA off they're single
#      (and twice the size).

# This file lives in spangap-core/cmake/bootstrap.cmake; one level up is the
# component root, where sdkconfig.defaults.spangap and partitions.csv live.
get_filename_component(_SPANGAP_CORE_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

# ─── 1. Layer SDKCONFIG_DEFAULTS: platform → straddle kconfig: → consumer → overrides ───
# _SPANGAP_FRAGMENTS is the spangap-inside-collected `kconfig:` blocks from every
# staged straddle (board pins etc.) — see write_sdkconfig_fragments. It sits
# above the bare platform defaults but below the buildable's own sdkconfig.defaults
# and the CLI overrides, so the building straddle and explicit CLI flags still win.
set(_SPANGAP_DEFAULTS "${_SPANGAP_CORE_DIR}/sdkconfig.defaults.spangap")
set(_SPANGAP_FRAGMENTS "${CMAKE_SOURCE_DIR}/staging/sdkconfig.spangap-fragments")
set(_CONSUMER_DEFAULTS "${CMAKE_SOURCE_DIR}/sdkconfig.defaults")
set(_SPANGAP_OVERRIDES "${CMAKE_SOURCE_DIR}/staging/sdkconfig.spangap-overrides")
set(SDKCONFIG_DEFAULTS "")
foreach(_f "${_SPANGAP_DEFAULTS}" "${_SPANGAP_FRAGMENTS}" "${_CONSUMER_DEFAULTS}" "${_SPANGAP_OVERRIDES}")
    if(EXISTS "${_f}")
        list(APPEND SDKCONFIG_DEFAULTS "${_f}")
    endif()
endforeach()

# ─── 2. sdkconfig.defaults staleness check ───
# IDF's default behavior: sdkconfig.defaults is one-shot — its values seed
# sdkconfig once, then sdkconfig is authoritative. Edits to defaults after
# that point are silently ignored until you `rm sdkconfig`.
#
# We undo that: whenever any file in SDKCONFIG_DEFAULTS changes (tracked by a
# combined content hash in build/sdkconfig.defaults.hash — hashes, not mtimes,
# so `git checkout` doesn't fool us), we delete sdkconfig so IDF reseeds it from
# the new defaults. No snapshots, no guessing whether the diff is a real
# menuconfig edit or just IDF's own churn (new Kconfig symbols, re-evaluated
# dependencies) — that guess produced false "manual edits" warnings on ordinary
# builds, because IDF rewrites sdkconfig as a normal part of every build.
#
# Opt-out for deliberate hand-tuning: a `.spangap-manual-kconfig` marker in the
# workspace (dropped by `spangap menuconfig`, removed by `spangap autoconfig`)
# means "I'm managing sdkconfig by hand — don't clobber it." With the marker
# present we keep the current sdkconfig and just note that the new defaults
# aren't being applied. SPANGAP_WORKSPACE is exported by spangap-outside on
# every docker exec; a raw `idf.py` build (no marker visible) just gets the
# default auto-regen behavior.
set(_SDKCONFIG "${CMAKE_SOURCE_DIR}/sdkconfig")
set(_DEFHASH_FILE "${CMAKE_BINARY_DIR}/sdkconfig.defaults.hash")
set(_SDK_REGENERATED FALSE)

set(_MANUAL_MARKER "")
if(DEFINED ENV{SPANGAP_WORKSPACE})
    set(_MANUAL_MARKER "$ENV{SPANGAP_WORKSPACE}/.spangap-manual-kconfig")
endif()

if(EXISTS "${_SDKCONFIG}" AND EXISTS "${_DEFHASH_FILE}" AND SDKCONFIG_DEFAULTS)
    # Combined hash of all defaults files in order
    set(_DEF_NOW_HASH "")
    foreach(_f IN LISTS SDKCONFIG_DEFAULTS)
        if(EXISTS "${_f}")
            file(SHA256 "${_f}" _one_hash)
            string(APPEND _DEF_NOW_HASH "${_one_hash}")
        endif()
    endforeach()
    string(SHA256 _DEF_NOW_HASH "${_DEF_NOW_HASH}")

    file(READ "${_DEFHASH_FILE}" _DEF_OLD_HASH)
    string(STRIP "${_DEF_OLD_HASH}" _DEF_OLD_HASH)

    if(NOT "${_DEF_NOW_HASH}" STREQUAL "${_DEF_OLD_HASH}")
        # Defaults changed since the sdkconfig was last seeded from them.
        if(_MANUAL_MARKER AND EXISTS "${_MANUAL_MARKER}")
            message(NOTICE
                "─ sdkconfig.defaults changed, but manual kconfig mode is on — "
                "keeping your sdkconfig.\n"
                "  Run `spangap autoconfig` to drop the marker and reseed from defaults.")
        else()
            message(NOTICE "─ sdkconfig.defaults changed — reseeding sdkconfig from defaults")
            file(REMOVE "${_SDKCONFIG}")
            set(_SDK_REGENERATED TRUE)
        endif()
    endif()
endif()

# Stash the values into the cache so project_include.cmake can record the
# defaults hash post-project (after IDF has read sdkconfig.defaults and possibly
# rewritten sdkconfig).
set(_SPANGAP_SDK_REGENERATED ${_SDK_REGENERATED} CACHE INTERNAL "")
set(_SPANGAP_SDKCONFIG "${_SDKCONFIG}" CACHE INTERNAL "")
set(_SPANGAP_DEFHASH_FILE "${_DEFHASH_FILE}" CACHE INTERNAL "")
set(_SPANGAP_SDKCONFIG_DEFAULTS_LIST "${SDKCONFIG_DEFAULTS}" CACHE INTERNAL "")

# ─── 3. Generate partitions.csv (provisional / configure-time) ───
# This is a size-agnostic FLOOR image: the table is sized to the configured
# CONFIG_ESPTOOLPY_FLASHSIZE (default 4 MB via sdkconfig.defaults.spangap), and
# `state` is NOT in it — the firmware grows flash to the real chip size at first
# boot and registers `state` itself (statePartitionEnsure() in fs.cpp). So a
# single image boots on any chip >= the floor. The updater on/off signal comes
# from the staged set — spangap-inside drops a `staging/components/updater/`
# symlink iff the updater straddle is in the build (mirrors the old ota signal).
# This provisional pass writes generous app/fixed so the first configure can link
# + pack littlefs; project_include.cmake re-runs it shrink-wrapped post-build.
set(_FLASH_MB 4)        # floor; overwritten by the configured flash size below

set(_PARTGEN_FILES ${SDKCONFIG_DEFAULTS} "${_SDKCONFIG}")
foreach(_f IN LISTS _PARTGEN_FILES)
    if(NOT EXISTS "${_f}")
        continue()
    endif()
    file(STRINGS "${_f}" _lines)
    foreach(_line IN LISTS _lines)
        if(_line MATCHES "^CONFIG_ESPTOOLPY_FLASHSIZE_([0-9]+)MB=y$")
            set(_FLASH_MB "${CMAKE_MATCH_1}")
        endif()
    endforeach()
endforeach()

if(EXISTS "${CMAKE_SOURCE_DIR}/staging/components/updater")
    set(_UPDATER y)
else()
    set(_UPDATER n)
endif()
# Single fixed partition now (no A/B) — exposed so the factory-image / lcd-icons
# helpers default to the right name without the consumer knowing the layout.
set(SPANGAP_FIXED_PARTITION "fixed" CACHE INTERNAL "")

# Shrink-wrap inputs: spangap-inside measures the real app .bin + fixed data
# after a build and writes build/.spangap-sizes; reading it here sizes app/fixed
# to actual (no cap on fixed). Absent (first build) → 0 → generous provisional.
set(_APP_BYTES 0)
set(_FIXED_BYTES 0)
set(_SIZES_FILE "${CMAKE_BINARY_DIR}/.spangap-sizes")
# Reconfigure when the measured sizes change, so spangap-inside's second pass
# (which writes this file) re-runs gen-partitions with the real sizes.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_SIZES_FILE}")
if(EXISTS "${_SIZES_FILE}")
    file(STRINGS "${_SIZES_FILE}" _szlines)
    foreach(_l IN LISTS _szlines)
        if(_l MATCHES "^app=([0-9]+)$")
            set(_APP_BYTES "${CMAKE_MATCH_1}")
        elseif(_l MATCHES "^fixed=([0-9]+)$")
            set(_FIXED_BYTES "${CMAKE_MATCH_1}")
        endif()
    endforeach()
endif()

execute_process(
    COMMAND python3 "${_SPANGAP_CORE_DIR}/scripts/gen-partitions.py"
        --flash-mb ${_FLASH_MB}
        --updater ${_UPDATER}
        --app-bytes ${_APP_BYTES}
        --fixed-bytes ${_FIXED_BYTES}
        --out "${CMAKE_SOURCE_DIR}/partitions.csv"
    COMMAND_ERROR_IS_FATAL ANY)

# Print the layout so `spangap build` shows how flash is carved up.
execute_process(
    COMMAND python3 "${_SPANGAP_CORE_DIR}/scripts/report-partitions.py"
        --partitions "${CMAKE_SOURCE_DIR}/partitions.csv")

# Stash for post-project (shrink-wrap re-gen + `fixed` utilization report).
set(_SPANGAP_PART_FLASH_MB ${_FLASH_MB} CACHE INTERNAL "")
set(_SPANGAP_PART_UPDATER ${_UPDATER} CACHE INTERNAL "")
