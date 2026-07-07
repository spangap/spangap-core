# spanfs

A read-only, mmap-native filesystem for ESP-IDF (and host tools). The image is
packed at build time and consumed **in place**: it lives in an `esp_partition_mmap`'d
flash partition (or embedded rodata) and every file is readable as a raw `const`
pointer. Zero RAM, zero SPI-flash-driver traffic, no cache-disable hazard — so any
task, PSRAM stack included, reads it freely.

Deliberately tiny (KISS): **no writes, no compression, no directory entries, no
hash table, no timestamps/owners/modes**. Sorted full paths + binary search;
`readdir` is a prefix range walk over the sorted index. Pre-compressed files
(`.gz`) are stored as-is; fonts stay raw for FreeType memory-face use.

## Format v1

Little-endian; offsets from image start.

```
[0]   header, 20 bytes
      u32  magic        0x53504653
      u8   version      1
      u8   reserved     0
      u16  reserved     0
      u32  entry_count
      u32  total_size   whole image in bytes
      u32  crc32        IEEE/zlib, over index + path blob (not data)
[20]  index: entry_count × 12 bytes, sorted bytewise by path
      u32  path_off     NUL-terminated UTF-8, no leading '/'
      u32  data_off     4-byte aligned
      u32  data_len
[..]  path blob
[..]  file data
```

The CRC covers only the metadata (index + path blob + alignment padding up to the
data region, i.e. `[20, data_off_of_entry_0)`): a mount fails fast on a truncated
or garbage image, while data integrity stays the flasher's/updater's job. The
image is byte-exact and placement-independent — no block rounding, no partition
padding.

## API

```c
esp_err_t spanfs_open_partition(const char *label, spanfs_t **out);      // esp_partition_mmap
esp_err_t spanfs_open_mem(const void *base, size_t size, spanfs_t **out); // embedded / host
void      spanfs_close(spanfs_t *fs);
esp_err_t spanfs_lookup(spanfs_t *fs, const char *path, const void **data, size_t *len);
esp_err_t spanfs_vfs_register(spanfs_t *fs, const char *mountpoint);      // POSIX face
```

The portable format/lookup/readdir core (`src/spanfs.c`) has no IDF includes, so
it builds host-side for tests and doubles as a reference reader; the partition
mmap + VFS glue live in `src/spanfs_esp.c`.

## Build integration

`project_include.cmake` provides, mirroring littlefs:

```cmake
spanfs_create_partition_image(<partition> <dir> [FLASH_IN_PROJECT])
```

Determinism is a contract: a sorted walk, no timestamps, no environment leakage,
so packing the same tree twice is byte-identical (asserted in `test/`).

## Partition subtype

spangap tags the `fixed` partition with data subtype **`0x8a`** (an arbitrary,
spanfs-owned value distinct from `spiffs`/`littlefs`). Runtime lookup is by
label, so the subtype is cosmetic but honest — it says "this is not littlefs".

## Tests

`test/run_host_tests.sh` builds the reference reader, packs a fixture, walks it,
compares bytes against the source tree, and asserts determinism + CRC/truncation
rejection.

Apache-2.0 licensed.
