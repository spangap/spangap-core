file(GLOB CORE_SRCS "src/*.cpp" "src/*.c")

# The host target has no power manager, no SPI bus, no USB device and no IDF
# heap-tracking to wrap. Each of those files is chip code end to end, and each
# has a host counterpart under src/host/ instead.
if(IDF_TARGET STREQUAL "linux")
    list(REMOVE_ITEM CORE_SRCS
        "${CMAKE_CURRENT_LIST_DIR}/src/pm.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/src/spi_helper.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/src/usb_ports.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/src/heap_track_stub.c")
    file(GLOB HOST_SRCS "src/host/*.cpp" "src/host/*.c")
    list(APPEND CORE_SRCS ${HOST_SRCS})
endif()

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
    spanfs
    esp_rom
    esp_app_format
    app_update
    spi_flash
    esp_partition
    lwip
    mbedtls
    bootloader_support)   # bootloader_random_enable() for random.cpp's seed window

# On the host only the components that actually register there. `driver` and
# `esp_timer` are the board straddle's own; the radio, power, USB, SD and
# LittleFS halves are not built at all.
if(IDF_TARGET STREQUAL "linux")
    set(_REQ
        driver
        esp_event
        nvs_flash
        json
        esp_timer
        esp_rom
        esp_app_format
        spi_flash
        esp_partition
        mbedtls
        esp_hw_support
        fatfs)
endif()

# The TinyUSB device stack is only on the link line when the `usb cdc` console
# transport is built (src/usb_ports.cpp compiles to the stub branch otherwise).
# The component manager still fetches it — idf_component.yml dependencies cannot
# be conditioned on a CONFIG_ symbol — but an unrequired archive contributes no
# code and no .bss to the image.
#
# Its .bss is internal DRAM held whether or not `usb cdc` is ever run, so it
# spends from CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL. That reserve is sized for
# the image's own boot peak rather than for this component, and does not move
# with this switch — see sdkconfig.defaults.spangap.
if(CONFIG_SPANGAP_USB_CDC)
    list(APPEND _REQ espressif__esp_tinyusb)
endif()

idf_component_register(
    SRCS ${CORE_SRCS}
    INCLUDE_DIRS "include"
    REQUIRES ${_REQ}
)

# The deflate/inflate the chip takes from ROM comes from the system zlib on the
# host (src/host/miniz_host.c is the translation). Named by soname because the
# container carries the runtime library and not its development symlink.
if(IDF_TARGET STREQUAL "linux")
    target_link_libraries(${COMPONENT_LIB} INTERFACE "-l:libz.so.1")
endif()

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
# otherwise. The host target's heap_caps_* wrap libc and track nothing,
# so there is no bookkeeping to bypass and no symbol to wrap.
if(NOT IDF_TARGET STREQUAL "linux")
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
endif()

# Keep the board straddle's detect_hw() on the link line.
#
# spangapInit() calls it through a WEAK declaration, so a build with no board
# straddle links a null and skips the check. But a weak UNDEFINED reference does
# not extract an archive member: the board's detect.cpp compiles into
# libhw-<board>.a, nothing else in the image references it, and the linker
# leaves it there. The symbol then resolves to null on a board that defines it
# perfectly well, and the check is inert while looking enforced — the worst
# possible failure mode for a safety check.
#
# `-u` makes the linker want the symbol, which is what extracts the member —
# and it is asked for ONLY when a board straddle is staged, because `-u` enters
# a STRONG undefined symbol that overrides the weak reference: on the generic
# image, which has nothing to define it, the link would fail outright rather
# than resolve to null. A staged board component is the exact condition under
# which a definition exists. Declared here rather than as WHOLE_ARCHIVE on each
# board, so the platform states the requirement once and a board author only has
# to define the function.
#
# Written as one comma-separated -Wl token on purpose: a LINK_OPTIONS entry
# reaches the compiler driver as a SINGLE argument, so "-u detect_hw" arrives as
# one token whose symbol name is " detect_hw" — no match, no extraction, and no
# error either. This form hands the linker `-u` and `detect_hw` separately.
idf_build_get_property(_all_comps BUILD_COMPONENTS)
foreach(_c IN LISTS _all_comps)
    if(_c MATCHES "(^|__)hw-")
        idf_build_set_property(LINK_OPTIONS "-Wl,-u,detect_hw" APPEND)
        break()
    endif()
endforeach()

# `idf.py reallyclean` is registered as an idf.py Python action via the
# consumer's idf_ext.py — NOT as a ninja custom target. ninja invocation
# deadlocks with the script's build/ deletion (ninja keeps writing logs
# into build/log/ after the script returns). The consumer's idf_ext.py
# imports spangap-core/scripts/reallyclean_action.py for the wiring.
