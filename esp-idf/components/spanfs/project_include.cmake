# spanfs_create_partition_image(<partition> <base_dir> [FLASH_IN_PROJECT] [DEPENDS ...])
#
# Pack <base_dir> into a spanfs image sized byte-exact (placement- and
# partition-size-independent) and, with FLASH_IN_PROJECT, bundle it into
# `idf.py flash`. Mirrors littlefs_create_partition_image() so the one-liner is
# familiar, but the image is not padded to the partition: it is exactly as big
# as its contents, which is what the shrink-wrap pass measures.

set(SPANFS_MKIMAGE "${CMAKE_CURRENT_LIST_DIR}/tools/mkspanfs.py")

function(spanfs_create_partition_image partition base_dir)
    set(options FLASH_IN_PROJECT)
    set(multi DEPENDS)
    cmake_parse_arguments(arg "${options}" "" "${multi}" ${ARGN})

    get_filename_component(base_dir_full_path "${base_dir}" ABSOLUTE)

    partition_table_get_partition_info(size "--partition-name ${partition}" "size")
    partition_table_get_partition_info(offset "--partition-name ${partition}" "offset")

    if("${size}" AND "${offset}")
        set(image_file "${CMAKE_BINARY_DIR}/${partition}.bin")

        # No CMake dependency can watch base_dir's contents, so always repack
        # (deterministic — byte-identical when nothing changed). The image is
        # byte-exact, never padded to the partition: its size is what the
        # shrink-wrap pass measures. It must simply fit — assert that here.
        add_custom_target(spanfs_${partition}_bin ALL
            COMMAND ${PYTHON} "${SPANFS_MKIMAGE}"
                    "${base_dir_full_path}" "${image_file}" --max-size ${size}
            DEPENDS ${arg_DEPENDS}
            BYPRODUCTS "${image_file}"
            VERBATIM)

        set_property(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" APPEND PROPERTY
            ADDITIONAL_MAKE_CLEAN_FILES "${image_file}")

        idf_component_get_property(main_args esptool_py FLASH_ARGS)
        idf_component_get_property(sub_args esptool_py FLASH_SUB_ARGS)
        esptool_py_flash_target(${partition}-flash "${main_args}" "${sub_args}")
        esptool_py_flash_target_image(${partition}-flash "${partition}" "${offset}" "${image_file}")
        add_dependencies(${partition}-flash spanfs_${partition}_bin)

        if(arg_FLASH_IN_PROJECT)
            esptool_py_flash_target_image(flash "${partition}" "${offset}" "${image_file}")
            add_dependencies(flash spanfs_${partition}_bin)
        endif()
    else()
        fail_at_build_time(spanfs_${partition}_bin
            "Failed to create spanfs image for partition '${partition}'. "
            "Check the partition table has a '${partition}' entry.")
    endif()
endfunction()
