# An image consumed in place from a flash mapping, and a VFS driver to reach it
# through. The host target has neither: its read-only tree is a directory, so
# the component registers with nothing in it.
if(IDF_TARGET STREQUAL "linux")
    idf_component_register()
    return()
endif()

idf_component_register(
    SRCS "src/spanfs.c" "src/spanfs_esp.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_partition vfs
)
