/* spanfs — a read-only, mmap-native filesystem.
 *
 * The image is packed at build time and consumed in place: it lives in an
 * mmap'd flash partition (or embedded rodata) and every file is readable as a
 * raw const pointer. Zero RAM, zero SPI-flash-driver traffic, no cache-disable
 * hazard — any task, PSRAM stack included, reads it freely.
 *
 * Format v1 (little-endian, offsets from image start):
 *
 *   [0]   header, 20 bytes
 *         u32  magic        0x53504653
 *         u8   version      1
 *         u8   reserved     0
 *         u16  reserved     0
 *         u32  entry_count
 *         u32  total_size   whole image in bytes
 *         u32  crc32        IEEE, over index + path blob (not data)
 *   [20]  index: entry_count x 12 bytes, sorted bytewise by path
 *         u32  path_off     NUL-terminated UTF-8, no leading '/'
 *         u32  data_off     4-byte aligned
 *         u32  data_len
 *   [..]  path blob
 *   [..]  file data
 *
 * The core parser/lookup has no IDF includes so it builds host-side for tests
 * and doubles as a reference reader. esp_partition_mmap + VFS glue live in a
 * separate ESP-only translation unit (spanfs_esp.c).
 */
#ifndef SPANFS_H
#define SPANFS_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef ESP_PLATFORM
#include "esp_err.h"
#else
/* Host build: provide the slice of esp_err_t the reader needs, with the same
 * numeric values ESP-IDF uses so error codes match across host and target. */
typedef int esp_err_t;
#define ESP_OK                0
#define ESP_FAIL             -1
#define ESP_ERR_NO_MEM        0x101
#define ESP_ERR_INVALID_ARG   0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_INVALID_SIZE  0x104
#define ESP_ERR_NOT_FOUND     0x105
#define ESP_ERR_INVALID_CRC   0x10c
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define SPANFS_MAGIC     0x53504653u
#define SPANFS_VERSION   1
#define SPANFS_HEADER_SZ 20
#define SPANFS_ENTRY_SZ  12
#define SPANFS_DATA_ALIGN 4

typedef struct spanfs spanfs_t;

/* Open an image already mapped into memory (embedded rodata, or a host buffer).
 * `base` must stay valid for the lifetime of the handle. Does NOT copy or own
 * the image; only the small handle struct is heap-allocated. */
esp_err_t spanfs_open_mem(const void *base, size_t size, spanfs_t **out);

/* Open a flash partition by label via esp_partition_mmap (ESP-only). */
esp_err_t spanfs_open_partition(const char *label, spanfs_t **out);

void spanfs_close(spanfs_t *fs);

/* Zero-copy lookup. On success *data points into the mapping (valid for the
 * lifetime of the handle) and *len is the file's byte length. `path` has no
 * leading '/'. Returns ESP_ERR_NOT_FOUND if absent. */
esp_err_t spanfs_lookup(spanfs_t *fs, const char *path,
                        const void **data, size_t *len);

/* Enumeration helpers (used by tests and the VFS readdir synthesis). */
uint32_t  spanfs_count(const spanfs_t *fs);
esp_err_t spanfs_entry_at(const spanfs_t *fs, uint32_t i,
                          const char **path, const void **data, size_t *len);

/* Total image size from the header. */
size_t    spanfs_image_size(const spanfs_t *fs);

/* Internal: attach an owner hook run by spanfs_close (e.g. the ESP layer's
 * esp_partition_munmap). Not needed by plain spanfs_open_mem users. */
void      spanfs_set_release(spanfs_t *fs, void (*release)(void *), void *ctx);

/* POSIX VFS face: open/read/lseek/fstat/stat/opendir/readdir/closedir.
 * read = memcpy from the mapping — safe from any task. ESP-only. */
esp_err_t spanfs_vfs_register(spanfs_t *fs, const char *mountpoint);
esp_err_t spanfs_vfs_unregister(const char *mountpoint);

#ifdef __cplusplus
}
#endif

#endif /* SPANFS_H */
