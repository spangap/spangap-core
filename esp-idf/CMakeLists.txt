file(GLOB CORE_SRCS "src/*.cpp" "src/*.c")

# Build epoch — `app_build_unix` (UTC seconds at link time). Generated each
# build by scripts/write-build-epoch.py so OTA can refuse images older than
# the running firmware. Lives in the build dir to avoid dirtying the source
# tree; added to CORE_SRCS so the symbol lands in libspangap-core.a.
set(_BUILD_EPOCH_C "${CMAKE_CURRENT_BINARY_DIR}/spangap_app_build_epoch.c")
if(NOT EXISTS "${_BUILD_EPOCH_C}")
    execute_process(
        COMMAND python3 "${CMAKE_CURRENT_LIST_DIR}/scripts/write-build-epoch.py" "${_BUILD_EPOCH_C}"
        COMMAND_ERROR_IS_FATAL ANY)
endif()
list(APPEND CORE_SRCS "${_BUILD_EPOCH_C}")

# Build invocation identity — `app_build_straddle` / `app_build_version` /
# `app_build_args`, populated from the SPANGAP_BUILD_* env that `spangap build`
# exports around idf.py. Same lives-in-the-build-dir, regenerate-every-build
# treatment as the epoch above so the strings track the producing invocation.
set(_BUILD_INFO_C "${CMAKE_CURRENT_BINARY_DIR}/spangap_app_build_info.c")
if(NOT EXISTS "${_BUILD_INFO_C}")
    execute_process(
        COMMAND python3 "${CMAKE_CURRENT_LIST_DIR}/scripts/write-build-info.py" "${_BUILD_INFO_C}"
        COMMAND_ERROR_IS_FATAL ANY)
endif()
list(APPEND CORE_SRCS "${_BUILD_INFO_C}")

# Narrowed REQUIRES — net/tls/mdns/http_client/web/auth/webrtc/lcd/wg/etc.
# moved to sibling straddles in Phase 2 of the straddles plan. esp_wifi stays
# because pm.cpp queries it; lwip stays because cli/log/cron register ITS
# ports with spangap-net using net.h structs that embed lwIP types.
set(_REQ
    driver
    esp_driver_gpio
    esp_driver_spi
    esp_pm
    esp_driver_usb_serial_jtag
    esp_wifi
    esp_netif
    esp_event
    nvs_flash
    json
    esp_timer
    esp_psram
    fatfs
    sdmmc
    esp_driver_sdmmc
    esp_driver_sdspi
    joltwallet__littlefs
    esp_rom
    esp_app_format
    app_update
    lwip
    mbedtls)

idf_component_register(
    SRCS ${CORE_SRCS}
    INCLUDE_DIRS "include"
    REQUIRES ${_REQ}
)

# Cross-straddle header injection for the one genuinely-cyclic case:
# cli/log/cron in this component call into spangap-net's ITS-port-registration
# protocol (net_port_msg_t, NET_PORT_REG_PORT, netIsStaConnected), but
# spangap-net REQUIRES spangap-core — adding spangap-net to REQUIRES here
# would form a cycle. Pull net's include/ in privately; symbols resolve at
# executable link time (main REQUIRES spangap-net so the lib is on the link
# line). All OTHER cross-straddle calls (wg/web/lcd/upnp/duckdns/acme/ota
# inits, mdns/ntp/tls inits) moved out of spangapInit() into the consumer's
# main.cpp as part of the layering rework — see spangap.h docstring — so
# this is the last sibling spangap-core needs to know exists.
idf_build_get_property(_all_comps BUILD_COMPONENTS)
foreach(_n IN ITEMS spangap-net spangap__spangap-net)
    if(_n IN_LIST _all_comps)
        idf_component_get_property(_dn_dir ${_n} COMPONENT_DIR)
        target_include_directories(${COMPONENT_LIB} PRIVATE "${_dn_dir}/include")
        break()
    endif()
endforeach()

# SD-on-SPI: let sdmmc_cmd.c skip its per-write PSRAM bounce buffer.
# sdmmc_cmd's sector read/write bounce every PSRAM-sourced transfer through a
# fresh heap_caps_malloc(512, MALLOC_CAP_DMA) — guarded by the SDMMC
# *peripheral's* SOC_SDMMC_PSRAM_DMA_CAPABLE (unset on the S3), which is
# irrelevant to SDSPI. FatFs hands full sectors straight from the caller's
# (PSRAM) buffer, so that per-write alloc runs constantly and fails with
# ESP_ERR_NO_MEM (0x101) whenever the internal DMA pool is tight (WiFi + LCD).
# The SDSPI host already bounces PSRAM sources through its own per-slot buffer,
# allocated once and reused (esp_driver_sdspi get_block_buf), so sdmmc_cmd's
# bounce is pure overhead here. Force the cap on for the sdmmc component ONLY
# when the SD is on SPI — a real SDMMC-peripheral board (BUS_SDMMC) must keep
# the bounce, as its dedicated DMA genuinely cannot reach PSRAM.
if(CONFIG_SPANGAP_SDCARD_BUS_SPI)
    idf_build_get_property(_all_comps BUILD_COMPONENTS)
    if("sdmmc" IN_LIST _all_comps)
        idf_component_get_property(_sdmmc_lib sdmmc COMPONENT_LIB)
        target_compile_definitions(${_sdmmc_lib} PRIVATE SOC_SDMMC_PSRAM_DMA_CAPABLE=1)
    endif()
endif()

# Refresh the build-epoch .c every ninja invocation. ALL+BYPRODUCTS forces
# the script to run each build; the resulting .c is in SRCS so it relinks
# into libspangap-core.a. Without this, app_build_unix would freeze at the
# value from the first build of the day even when other code changed.
add_custom_target(spangap_build_epoch ALL
    BYPRODUCTS "${_BUILD_EPOCH_C}"
    COMMAND python3 "${CMAKE_CURRENT_LIST_DIR}/scripts/write-build-epoch.py" "${_BUILD_EPOCH_C}"
    COMMENT "Generate spangap build epoch (always)")
add_dependencies(${COMPONENT_LIB} spangap_build_epoch)

# Same always-rerun treatment for the build invocation identity — re-reads the
# SPANGAP_BUILD_* env each ninja invocation so a rebuild after `spangap build`
# with different flags relinks the new strings in.
add_custom_target(spangap_build_info ALL
    BYPRODUCTS "${_BUILD_INFO_C}"
    COMMAND python3 "${CMAKE_CURRENT_LIST_DIR}/scripts/write-build-info.py" "${_BUILD_INFO_C}"
    COMMENT "Generate spangap build invocation info (always)")
add_dependencies(${COMPONENT_LIB} spangap_build_info)

# IDF 5.5 heap-task-tracking eager-bookkeeping bypass — see
# docs/idf-tweaks.md and src/heap_track_stub.c. We --wrap three IDF
# heap-tracking entry points to no-ops; if Espressif renames or splits
# them, the --wrap silently reverts to the broken behavior, so verify
# their names still appear at the expected call sites and fail loud
# otherwise.
set(_HEAP_TRACK_FNS
    heap_caps_update_per_task_info_alloc
    heap_caps_update_per_task_info_free
    heap_caps_update_per_task_info_realloc)
set(_HEAP_TRACK_FILES
    "${IDF_PATH}/components/heap/heap_caps_base.c"
    "${IDF_PATH}/components/heap/heap_caps_init.c")
set(_HEAP_TRACK_OK TRUE)
foreach(_f IN LISTS _HEAP_TRACK_FILES)
    if(NOT EXISTS "${_f}")
        set(_HEAP_TRACK_OK FALSE)
        set(_HEAP_TRACK_REASON "missing IDF source ${_f}")
        break()
    endif()
endforeach()
if(_HEAP_TRACK_OK)
    foreach(_fn IN LISTS _HEAP_TRACK_FNS)
        set(_found FALSE)
        foreach(_f IN LISTS _HEAP_TRACK_FILES)
            file(READ "${_f}" _src)
            string(FIND "${_src}" "${_fn}(" _idx)
            if(NOT _idx EQUAL -1)
                set(_found TRUE)
                break()
            endif()
        endforeach()
        if(NOT _found)
            set(_HEAP_TRACK_OK FALSE)
            set(_HEAP_TRACK_REASON "expected function '${_fn}' not found in IDF heap component")
            break()
        endif()
    endforeach()
endif()
if(NOT _HEAP_TRACK_OK)
    message(FATAL_ERROR
        "spangap-core: heap-task-tracking --wrap guard failed — ${_HEAP_TRACK_REASON}.\n"
        "ESP-IDF appears to have renamed or restructured the heap tracking call sites.\n"
        "The --wrap intercept in CMakeLists.txt + src/heap_track_stub.c will silently\n"
        "no longer take effect, restoring the IWDT crash documented in docs/idf-tweaks.md.\n"
        "Either:\n"
        "  - update _HEAP_TRACK_FNS / src/heap_track_stub.c to match the new IDF symbols, or\n"
        "  - set CONFIG_HEAP_TASK_TRACKING=n in the consumer's sdkconfig.defaults to drop\n"
        "    the feature entirely (loses per-task DRAM/PSRAM columns in the 'top' CLI command\n"
        "    and makes heapDump() in pm.cpp report no per-task attribution).")
endif()
target_link_options(${COMPONENT_LIB} INTERFACE
    "-Wl,--wrap=heap_caps_update_per_task_info_alloc"
    "-Wl,--wrap=heap_caps_update_per_task_info_free"
    "-Wl,--wrap=heap_caps_update_per_task_info_realloc")

# `idf.py reallyclean` is registered as an idf.py Python action via the
# consumer's idf_ext.py — NOT as a ninja custom target. ninja invocation
# deadlocks with the script's build/ deletion (ninja keeps writing logs
# into build/log/ after the script returns). The consumer's idf_ext.py
# imports spangap-core/scripts/reallyclean_action.py for the wiring.
