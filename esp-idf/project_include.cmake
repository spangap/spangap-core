# diptych-core helper functions, available to consumer CMakeLists.txt after
# project() returns. ESP-IDF includes this file automatically when this
# component is in the build.

# ─── Finalize the sdkconfig.defaults staleness check ───
# bootstrap.cmake (run pre-project by the consumer) staged the relevant paths
# in cache vars. Now that IDF has processed sdkconfig.defaults and (re)written
# sdkconfig, refresh the snapshot files used to detect future drift.
if(_DIPTYCH_SDKCONFIG AND EXISTS "${_DIPTYCH_SDKCONFIG}")
    if(_DIPTYCH_SDK_REGENERATED OR NOT EXISTS "${_DIPTYCH_SDKSNAPSHOT}")
        configure_file("${_DIPTYCH_SDKCONFIG}" "${_DIPTYCH_SDKSNAPSHOT}" COPYONLY)
        # Combined hash of every file currently in SDKCONFIG_DEFAULTS — must match
        # what bootstrap.cmake compares against on the next build.
        set(_combined "")
        foreach(_f IN LISTS _DIPTYCH_SDKCONFIG_DEFAULTS_LIST)
            if(EXISTS "${_f}")
                file(SHA256 "${_f}" _one)
                string(APPEND _combined "${_one}")
            endif()
        endforeach()
        string(SHA256 _combined_hash "${_combined}")
        file(WRITE "${_DIPTYCH_DEFHASH_FILE}" "${_combined_hash}\n")
    endif()
endif()

# diptych_create_factory_image(<partition_name> [DATA_DIR <dir>])
#
# Builds a LittleFS factory image for <partition_name> by merging:
#   1. diptych-core's static factory_state/ defaults — including the
#      platform-owned `factory_state/storage/external/s.time.zones.json`
#      (refresh with diptych-core's `make timezones`; see scripts/update-zones.py).
#   2. the consumer's own data/ (or DATA_DIR if specified) — wins on collisions.
#
# Calls `littlefs_create_partition_image(... FLASH_IN_PROJECT)` so the image
# is bundled into `idf.py flash`.
function(diptych_create_factory_image partition_name)
    cmake_parse_arguments(_DCFI "" "DATA_DIR" "" ${ARGN})
    set(_consumer_data "${_DCFI_DATA_DIR}")
    if(NOT _consumer_data)
        set(_consumer_data "${CMAKE_SOURCE_DIR}/data")
    endif()

    idf_component_get_property(_diptych_core_dir diptych-core COMPONENT_DIR)
    set(_diptych_data "${_diptych_core_dir}/data")
    set(_data_merged "${CMAKE_BINARY_DIR}/data_merged")

    add_custom_target(diptych_data_merge ALL
        COMMAND ${CMAKE_COMMAND} -E rm -rf "${_data_merged}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${_data_merged}"
        # 1. Diptych static defaults (factory_state/{boot,crontab,net_up,
        #    storage/external/s.time.zones.json, ...})
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${_diptych_data}" "${_data_merged}"
        # 2. Consumer overrides (whole-file replacement on collision)
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${_consumer_data}" "${_data_merged}"
        # macOS sprinkles .DS_Store everywhere; never want them in flash.
        COMMAND find "${_data_merged}" -name .DS_Store -delete
        COMMENT "Merging diptych defaults + ${PROJECT_NAME} data/ (consumer wins)"
        VERBATIM)

    littlefs_create_partition_image(${partition_name} "${_data_merged}" FLASH_IN_PROJECT)
    add_dependencies(littlefs_${partition_name}_bin diptych_data_merge)

    # Ballpark utilization report — runs after the partition image is built.
    # ESP-IDF already prints app utilization; this is the matching readout for
    # the fixed (factory data) side.
    add_custom_command(TARGET littlefs_${partition_name}_bin POST_BUILD
        COMMAND python3 "${_diptych_core_dir}/scripts/report-sizes.py"
            --partitions "${CMAKE_SOURCE_DIR}/partitions.csv"
            --data-dir "${_data_merged}"
            --partition-name ${partition_name}
        VERBATIM)

    # If the consumer also called diptych_browser_build(), wire its target as
    # a prerequisite so the SPA lands in data_merged before the merge runs.
    if(TARGET diptych_browser_build_target)
        add_dependencies(diptych_data_merge diptych_browser_build_target)
    endif()
endfunction()


# diptych_browser_build(<web_dir>)
#
# Adds a custom target that runs <web_dir>/deploy.sh on every build, so the
# consumer's SPA is rebuilt before the LittleFS factory image is assembled.
# Also tied to the `flash` target so `idf.py flash` (without a prior `build`)
# still picks up SPA changes.
function(diptych_browser_build web_dir)
    add_custom_target(diptych_browser_build_target ALL
        COMMAND bash ${web_dir}/deploy.sh
        WORKING_DIRECTORY ${web_dir}
        COMMENT "Building web interface in ${web_dir}"
        VERBATIM)
    add_dependencies(flash diptych_browser_build_target)

    # If the consumer already called diptych_create_factory_image(), retro-wire
    # the dependency now (works in either call order).
    if(TARGET diptych_data_merge)
        add_dependencies(diptych_data_merge diptych_browser_build_target)
    endif()
endfunction()


# diptych_lcd_icons([SRC_DIR <dir>] [SIZES "36x36"] [PARTITION fixed_a])
#
# Rasterizes launcher icon SOURCES (*.svg / *.png) into LVGL RGB565A8 .bin
# files — one per size bucket — written into the factory-image staging tree at
# data_merged/lcd/icons/<WxH>/<name>.bin, so they ship to
# /fixed/lcd/icons/<WxH>/<name>.bin. The lcd module loads them by basename at a
# fixed resolution (LAUNCHER_ICON_RES, 36x36, the launcher tile size), rendered
# native — so build at least that bucket. Sources live outside data/ so only the
# generated .bin ships.
#
# Two source dirs are merged at build time: diptych-core's own
# assets/lcd-icons/ (platform defaults — gear/log/cli) always, plus the
# consumer's SRC_DIR if given. On a basename collision the consumer wins — the
# build-time analogue of the data/ merge. SRC_DIR is therefore optional; a
# consumer with no icons of its own still inherits the platform defaults.
#
# Best-effort: the helper script warns and skips if the host lacks Pillow /
# cairosvg or LVGLImage.py — the build still succeeds (tiles show their label
# until icons are present). Requires diptych_create_factory_image() first and
# CONFIG_DIPTYCH_LCD=y (so the `lvgl` component, which ships LVGLImage.py, is
# in the build).
function(diptych_lcd_icons)
    cmake_parse_arguments(_DLI "" "SRC_DIR;PARTITION" "SIZES" ${ARGN})
    if(NOT _DLI_SIZES)
        set(_DLI_SIZES "36x36")
    endif()
    if(NOT _DLI_PARTITION)
        set(_DLI_PARTITION "fixed_a")
    endif()
    if(NOT TARGET diptych_data_merge)
        message(FATAL_ERROR "diptych_lcd_icons: call diptych_create_factory_image() first")
    endif()

    idf_component_get_property(_core_dir diptych-core COMPONENT_DIR)

    # LVGL ships LVGLImage.py. The component is "lvgl__lvgl" (managed) or
    # "lvgl" (vendored) — resolve whichever is in the build; if neither, leave
    # the path empty and the helper script skips with a warning.
    idf_build_get_property(_all_comps BUILD_COMPONENTS)
    set(_lvgl_image_py "")
    foreach(_lc IN ITEMS lvgl__lvgl lvgl)
        if(_lc IN_LIST _all_comps)
            idf_component_get_property(_lvgl_dir ${_lc} COMPONENT_DIR)
            set(_lvgl_image_py "${_lvgl_dir}/scripts/LVGLImage.py")
            break()
        endif()
    endforeach()

    set(_data_merged "${CMAKE_BINARY_DIR}/data_merged")
    string(REPLACE ";" "," _sizes_csv "${_DLI_SIZES}")

    # Platform defaults first (low precedence), consumer SRC_DIR second so it
    # overrides on basename. SRC_DIR is optional.
    set(_src_args --src "${_core_dir}/assets/lcd-icons")
    if(_DLI_SRC_DIR)
        list(APPEND _src_args --src "${_DLI_SRC_DIR}")
    endif()

    add_custom_target(diptych_lcd_icons ALL
        COMMAND ${CMAKE_COMMAND} -E make_directory "${_data_merged}/lcd/icons"
        COMMAND python3 "${_core_dir}/scripts/lcd-icons.py"
            ${_src_args}
            --out "${_data_merged}/lcd/icons"
            --sizes "${_sizes_csv}"
            --lvgl-image-py "${_lvgl_image_py}"
        COMMENT "Rasterizing lcd icons (${_sizes_csv}) -> /fixed/lcd/icons"
        VERBATIM)

    # Run after the data merge populates data_merged, before the LittleFS image.
    add_dependencies(diptych_lcd_icons diptych_data_merge)
    add_dependencies(littlefs_${_DLI_PARTITION}_bin diptych_lcd_icons)
endfunction()
