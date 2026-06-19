# spangap-core helper functions, available to consumer CMakeLists.txt after
# project() returns. ESP-IDF includes this file automatically when this
# component is in the build.

# ─── Finalize the sdkconfig.defaults staleness check ───
# bootstrap.cmake (run pre-project by the consumer) staged the relevant paths
# in cache vars. Now that IDF has processed sdkconfig.defaults and (re)written
# sdkconfig, record the combined hash of the defaults files so the next build
# only reacts to a *further* change. Written unconditionally — first build,
# auto-reseed, or manual-mode keep alike — so the "defaults changed" signal is
# edge-triggered: it fires once per change, not on every build. (`spangap
# autoconfig` reseeds independently by removing sdkconfig, so a hash recorded
# during manual mode never strands stale defaults.)
if(_SPANGAP_SDKCONFIG AND EXISTS "${_SPANGAP_SDKCONFIG}")
    set(_combined "")
    foreach(_f IN LISTS _SPANGAP_SDKCONFIG_DEFAULTS_LIST)
        if(EXISTS "${_f}")
            file(SHA256 "${_f}" _one)
            string(APPEND _combined "${_one}")
        endif()
    endforeach()
    string(SHA256 _combined_hash "${_combined}")
    file(WRITE "${_SPANGAP_DEFHASH_FILE}" "${_combined_hash}\n")
endif()

# spangap_create_factory_image([PARTITION <name>] [DATA_DIR <dir>])
#
# Builds a LittleFS factory image for the read-only data partition by merging:
#   1. spangap-core's static factory_state/ defaults — including the
#      platform-owned `factory_state/storage/external/s.time.zones.json`
#      (refresh with spangap-core's `make timezones`; see scripts/update-zones.py).
#   2. the consumer's own data/ (or DATA_DIR if specified) — wins on collisions.
#
# PARTITION defaults to `${SPANGAP_FIXED_PARTITION}` — set by bootstrap.cmake
# to "fixed_a" when OTA is staged (paired A/B layout) or "fixed" when OTA is
# off (single partition). Override only if you have a non-standard layout.
#
# Calls `littlefs_create_partition_image(... FLASH_IN_PROJECT)` so the image
# is bundled into `idf.py flash`.
function(spangap_create_factory_image)
    cmake_parse_arguments(_DCFI "" "PARTITION;DATA_DIR" "" ${ARGN})
    set(partition_name "${_DCFI_PARTITION}")
    if(NOT partition_name)
        set(partition_name "${SPANGAP_FIXED_PARTITION}")
    endif()
    if(NOT partition_name)
        message(FATAL_ERROR
            "spangap_create_factory_image: no PARTITION specified and "
            "SPANGAP_FIXED_PARTITION isn't set (bootstrap.cmake didn't run?)")
    endif()
    set(_consumer_data "${_DCFI_DATA_DIR}")
    if(NOT _consumer_data)
        set(_consumer_data "${CMAKE_SOURCE_DIR}/data")
    endif()

    idf_component_get_property(_spangap_core_dir spangap-core COMPONENT_DIR)
    set(_spangap_data "${_spangap_core_dir}/data")
    set(_data_merged "${CMAKE_BINARY_DIR}/data_merged")

    add_custom_target(spangap_data_merge ALL
        COMMAND ${CMAKE_COMMAND} -E rm -rf "${_data_merged}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${_data_merged}"
        # 1. Spangap static defaults (factory_state/{boot,crontab,net_up,
        #    storage/external/s.time.zones.json, ...})
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${_spangap_data}" "${_data_merged}"
        # 2. Consumer overrides (whole-file replacement on collision)
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${_consumer_data}" "${_data_merged}"
        # macOS sprinkles .DS_Store everywhere; never want them in flash.
        COMMAND find "${_data_merged}" -name .DS_Store -delete
        COMMENT "Merging spangap defaults + ${PROJECT_NAME} data/ (consumer wins)"
        VERBATIM)

    littlefs_create_partition_image(${partition_name} "${_data_merged}" FLASH_IN_PROJECT)
    add_dependencies(littlefs_${partition_name}_bin spangap_data_merge)

    # Ballpark utilization report — runs after the partition image is built.
    # ESP-IDF already prints app utilization; this is the matching readout for
    # the fixed (factory data) side.
    add_custom_command(TARGET littlefs_${partition_name}_bin POST_BUILD
        COMMAND python3 "${_spangap_core_dir}/scripts/report-sizes.py"
            --partitions "${CMAKE_SOURCE_DIR}/partitions.csv"
            --data-dir "${_data_merged}"
            --partition-name ${partition_name}
        VERBATIM)

    # Full partition layout (name/offset/size) on every build — not just the
    # configure-time print in bootstrap.cmake (which caches away on rebuilds).
    add_custom_command(TARGET littlefs_${partition_name}_bin POST_BUILD
        COMMAND python3 "${_spangap_core_dir}/scripts/report-partitions.py"
            --partitions "${CMAKE_SOURCE_DIR}/partitions.csv"
        VERBATIM)

    # If the consumer also called spangap_browser_build(), wire its target as
    # a prerequisite so the SPA lands in data_merged before the merge runs.
    if(TARGET spangap_browser_build_target)
        add_dependencies(spangap_data_merge spangap_browser_build_target)
    endif()
endfunction()


# spangap_browser_build(<web_dir>)
#
# Adds a custom target that runs <web_dir>/deploy.sh on every build, so the
# consumer's SPA is rebuilt before the LittleFS factory image is assembled.
# Also tied to the `flash` target so `idf.py flash` (without a prior `build`)
# still picks up SPA changes.
#
# Kept in spangap-core (not spangap-web) because the SPA's compiled output
# becomes part of the factory image — the merge logic in
# spangap_create_factory_image() lives here, so the build helper that
# precedes it does too.
function(spangap_browser_build web_dir)
    add_custom_target(spangap_browser_build_target ALL
        COMMAND bash ${web_dir}/deploy.sh
        WORKING_DIRECTORY ${web_dir}
        COMMENT "Building web interface in ${web_dir}"
        VERBATIM)
    add_dependencies(flash spangap_browser_build_target)

    # If the consumer already called spangap_create_factory_image(), retro-wire
    # the dependency now (works in either call order).
    if(TARGET spangap_data_merge)
        add_dependencies(spangap_data_merge spangap_browser_build_target)
    endif()
endfunction()
