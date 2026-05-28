# spangap-core pre-project bootstrap. Consumers `include()` this file BEFORE
# `project()`, like:
#
#   include("${CMAKE_SOURCE_DIR}/managed_components/spangap__spangap-core/cmake/bootstrap.cmake")
#
# The build tooling stages spangap-core into managed_components/ before CMake
# runs — committed consumer files carry no workspace-relative paths.
#
# What it does:
#   1. Prepends spangap's sdkconfig.defaults.spangap to SDKCONFIG_DEFAULTS so
#      the consumer's sdkconfig.defaults can stay small (consumer values still
#      override on collision — IDF processes the list in order).
#   2. Runs the sdkconfig.defaults staleness check: if defaults were edited
#      after the last `idf.py reconfigure` and the user has no manual menuconfig
#      edits, regenerate sdkconfig automatically. This works around IDF's
#      one-shot defaults behavior (without it, edited defaults are silently
#      ignored until you `rm sdkconfig`).
#   3. Generates `${CMAKE_SOURCE_DIR}/partitions.csv` from CONFIG_SPANGAP_OTA,
#      CONFIG_SPANGAP_APP_PERCENT, and CONFIG_ESPTOOLPY_FLASHSIZE_*MB. With
#      OTA on, partitions are paired A/B; with OTA off they're single (and
#      twice the size). Tune via `idf.py menuconfig`.

# This file lives in spangap-core/cmake/bootstrap.cmake; one level up is the
# component root, where sdkconfig.defaults.spangap and partitions.csv live.
get_filename_component(_SPANGAP_CORE_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

# ─── 1. Layer spangap's sdkconfig.defaults in front of the consumer's ───
set(_SPANGAP_DEFAULTS "${_SPANGAP_CORE_DIR}/sdkconfig.defaults.spangap")
set(_CONSUMER_DEFAULTS "${CMAKE_SOURCE_DIR}/sdkconfig.defaults")
if(EXISTS "${_SPANGAP_DEFAULTS}")
    if(EXISTS "${_CONSUMER_DEFAULTS}")
        set(SDKCONFIG_DEFAULTS "${_SPANGAP_DEFAULTS};${_CONSUMER_DEFAULTS}")
    else()
        set(SDKCONFIG_DEFAULTS "${_SPANGAP_DEFAULTS}")
    endif()
endif()

# ─── 2. sdkconfig.defaults staleness check ───
# IDF's default behavior: sdkconfig.defaults is one-shot — its values seed
# sdkconfig once, then sdkconfig is authoritative. Edits to defaults after
# that point are silently ignored until you `rm sdkconfig`.
#
# We compare hashes of every file in SDKCONFIG_DEFAULTS (not mtimes, so
# `git checkout` doesn't fool us). Two snapshots tracked in build/:
#   sdkconfig.from_defaults   — sdkconfig right after last regen from defaults
#   sdkconfig.defaults.hash   — combined hash of all defaults files at last regen
# If defaults hash changed AND sdkconfig matches its snapshot (no manual
# menuconfig edits), regenerate. If defaults changed AND user has manual
# edits, warn and keep their sdkconfig.
set(_SDKCONFIG "${CMAKE_SOURCE_DIR}/sdkconfig")
set(_SDKSNAPSHOT "${CMAKE_BINARY_DIR}/sdkconfig.from_defaults")
set(_DEFHASH_FILE "${CMAKE_BINARY_DIR}/sdkconfig.defaults.hash")
set(_SDK_REGENERATED FALSE)

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
        # Defaults changed since last regen.
        set(_HAS_MANUAL_EDITS FALSE)
        if(EXISTS "${_SDKSNAPSHOT}")
            file(SHA256 "${_SDKCONFIG}" _SDK_NOW_HASH)
            file(SHA256 "${_SDKSNAPSHOT}" _SDK_SNAP_HASH)
            if(NOT "${_SDK_NOW_HASH}" STREQUAL "${_SDK_SNAP_HASH}")
                set(_HAS_MANUAL_EDITS TRUE)
            endif()
        else()
            set(_HAS_MANUAL_EDITS TRUE)
        endif()
        if(_HAS_MANUAL_EDITS)
            message(WARNING
                "  sdkconfig.defaults has changed since this sdkconfig was generated,\n"
                "  AND sdkconfig has manual menuconfig edits — keeping the current\n"
                "  sdkconfig and ignoring the new defaults. To accept defaults:\n"
                "      rm sdkconfig && idf.py reconfigure")
        else()
            message(NOTICE "─ sdkconfig.defaults changed, no manual edits — regenerating sdkconfig")
            file(REMOVE "${_SDKCONFIG}")
            set(_SDK_REGENERATED TRUE)
        endif()
    endif()
endif()

# Stash the values into the cache so project_include.cmake can finalize the
# snapshot post-project (after IDF has read sdkconfig.defaults and possibly
# rewritten sdkconfig).
set(_SPANGAP_SDK_REGENERATED ${_SDK_REGENERATED} CACHE INTERNAL "")
set(_SPANGAP_SDKCONFIG "${_SDKCONFIG}" CACHE INTERNAL "")
set(_SPANGAP_SDKSNAPSHOT "${_SDKSNAPSHOT}" CACHE INTERNAL "")
set(_SPANGAP_DEFHASH_FILE "${_DEFHASH_FILE}" CACHE INTERNAL "")
set(_SPANGAP_SDKCONFIG_DEFAULTS_LIST "${SDKCONFIG_DEFAULTS}" CACHE INTERNAL "")

# ─── 3. Generate partitions.csv from Kconfig values ───
# Walk SDKCONFIG_DEFAULTS in order (so later files override earlier), then
# sdkconfig last (highest precedence, reflects any menuconfig edits). On the
# first build `sdkconfig` doesn't exist yet — we generate from defaults, IDF
# processes the same defaults, the resulting sdkconfig matches.
set(_FLASH_MB 8)        # matches spangap default in sdkconfig.defaults.spangap
set(_OTA y)             # matches Kconfig default
set(_APP_PCT 75)        # matches Kconfig default

set(_PARTGEN_FILES ${SDKCONFIG_DEFAULTS} "${_SDKCONFIG}")
foreach(_f IN LISTS _PARTGEN_FILES)
    if(NOT EXISTS "${_f}")
        continue()
    endif()
    file(STRINGS "${_f}" _lines)
    foreach(_line IN LISTS _lines)
        if(_line MATCHES "^CONFIG_ESPTOOLPY_FLASHSIZE_([0-9]+)MB=y$")
            set(_FLASH_MB "${CMAKE_MATCH_1}")
        elseif(_line STREQUAL "CONFIG_SPANGAP_OTA=y")
            set(_OTA y)
        elseif(_line STREQUAL "# CONFIG_SPANGAP_OTA is not set")
            set(_OTA n)
        elseif(_line MATCHES "^CONFIG_SPANGAP_APP_PERCENT=([0-9]+)$")
            set(_APP_PCT "${CMAKE_MATCH_1}")
        endif()
    endforeach()
endforeach()

execute_process(
    COMMAND python3 "${_SPANGAP_CORE_DIR}/scripts/gen-partitions.py"
        --flash-mb ${_FLASH_MB}
        --ota ${_OTA}
        --app-percent ${_APP_PCT}
        --out "${CMAKE_SOURCE_DIR}/partitions.csv"
    COMMAND_ERROR_IS_FATAL ANY)

# Stash for post-project so we can report `fixed` utilization against the
# partition we just generated.
set(_SPANGAP_PART_FLASH_MB ${_FLASH_MB} CACHE INTERNAL "")
set(_SPANGAP_PART_OTA ${_OTA} CACHE INTERNAL "")
set(_SPANGAP_PART_APP_PCT ${_APP_PCT} CACHE INTERNAL "")
