idf_component_register(
    SRCS "src/spanfs.c" "src/spanfs_esp.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_partition vfs
)
