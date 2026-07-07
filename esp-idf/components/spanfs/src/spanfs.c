/* spanfs — portable format parser, lookup and enumeration.
 *
 * No IDF includes: this file builds host-side for tests and is the reference
 * reader. The ESP-only partition mmap + VFS glue lives in spanfs_esp.c.
 */
#include "spanfs.h"

#include <stdlib.h>
#include <string.h>

struct spanfs {
    const uint8_t *base;
    size_t         size;
    uint32_t       entry_count;
    const uint8_t *index;    /* base + SPANFS_HEADER_SZ */
    uint32_t       total_size;
    /* Optional owner hook: the ESP layer sets this to munmap the partition on
     * close. Portable code never sets it (spanfs_open_mem does not own base). */
    void          (*release)(void *);
    void           *release_ctx;
};

void spanfs_set_release(spanfs_t *fs, void (*release)(void *), void *ctx) {
    if (!fs) return;
    fs->release = release;
    fs->release_ctx = ctx;
}

/* ---- little-endian scalar reads (alignment- and endian-safe) ---- */

static uint32_t rd_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* ---- CRC32 (IEEE / zlib, reflected, poly 0xEDB88320) ---- */

static uint32_t spanfs_crc32(const uint8_t *p, size_t n) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        crc ^= p[i];
        for (int b = 0; b < 8; b++) {
            uint32_t mask = -(crc & 1u);
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

/* ---- index accessors ---- */

static uint32_t ent_path_off(const spanfs_t *fs, uint32_t i) {
    return rd_u32(fs->index + (size_t)i * SPANFS_ENTRY_SZ + 0);
}
static uint32_t ent_data_off(const spanfs_t *fs, uint32_t i) {
    return rd_u32(fs->index + (size_t)i * SPANFS_ENTRY_SZ + 4);
}
static uint32_t ent_data_len(const spanfs_t *fs, uint32_t i) {
    return rd_u32(fs->index + (size_t)i * SPANFS_ENTRY_SZ + 8);
}
static const char *ent_path(const spanfs_t *fs, uint32_t i) {
    return (const char *)(fs->base + ent_path_off(fs, i));
}

/* ---- open / validate ---- */

esp_err_t spanfs_open_mem(const void *base, size_t size, spanfs_t **out) {
    if (!base || !out) return ESP_ERR_INVALID_ARG;
    if (size < SPANFS_HEADER_SZ) return ESP_ERR_INVALID_SIZE;

    const uint8_t *b = (const uint8_t *)base;
    if (rd_u32(b) != SPANFS_MAGIC) return ESP_ERR_INVALID_STATE;
    if (b[4] != SPANFS_VERSION) return ESP_ERR_INVALID_STATE;

    uint32_t count = rd_u32(b + 8);
    uint32_t total = rd_u32(b + 12);
    uint32_t crc   = rd_u32(b + 16);

    if (total < SPANFS_HEADER_SZ || total > size) return ESP_ERR_INVALID_SIZE;

    size_t index_end = (size_t)SPANFS_HEADER_SZ + (size_t)count * SPANFS_ENTRY_SZ;
    if (index_end > total) return ESP_ERR_INVALID_SIZE;

    /* CRC covers index + path blob: [20, first_data_off). Data offsets are
     * assigned in path-sorted order and increase monotonically, so entry 0
     * holds the smallest — the start of the data region. With no entries the
     * metadata runs to total_size. */
    const uint8_t *index = b + SPANFS_HEADER_SZ;
    uint32_t crc_end = total;
    if (count > 0) {
        crc_end = rd_u32(index + 4);   /* entry 0 data_off */
        if (crc_end < index_end || crc_end > total) return ESP_ERR_INVALID_SIZE;
    }
    if (spanfs_crc32(b + SPANFS_HEADER_SZ, crc_end - SPANFS_HEADER_SZ) != crc)
        return ESP_ERR_INVALID_CRC;

    /* Validate each entry now, so lookups/reads never range-check. */
    for (uint32_t i = 0; i < count; i++) {
        uint32_t po = rd_u32(index + (size_t)i * SPANFS_ENTRY_SZ + 0);
        uint32_t doff = rd_u32(index + (size_t)i * SPANFS_ENTRY_SZ + 4);
        uint32_t dlen = rd_u32(index + (size_t)i * SPANFS_ENTRY_SZ + 8);
        if (po < index_end || po >= total) return ESP_ERR_INVALID_SIZE;
        if ((uint64_t)doff + dlen > total)  return ESP_ERR_INVALID_SIZE;
        /* path must be NUL-terminated within the image */
        const uint8_t *pe = (const uint8_t *)memchr(b + po, '\0', total - po);
        if (!pe) return ESP_ERR_INVALID_SIZE;
    }

    spanfs_t *fs = (spanfs_t *)calloc(1, sizeof(*fs));
    if (!fs) return ESP_ERR_NO_MEM;
    fs->base = b;
    fs->size = size;
    fs->entry_count = count;
    fs->index = index;
    fs->total_size = total;
    *out = fs;
    return ESP_OK;
}

void spanfs_close(spanfs_t *fs) {
    if (!fs) return;
    if (fs->release) fs->release(fs->release_ctx);  /* e.g. esp_partition_munmap */
    free(fs);
}

/* ---- lookup (binary search, bytewise/unsigned path compare) ---- */

esp_err_t spanfs_lookup(spanfs_t *fs, const char *path,
                        const void **data, size_t *len) {
    if (!fs || !path) return ESP_ERR_INVALID_ARG;
    while (*path == '/') path++;   /* tolerate a leading slash */

    uint32_t lo = 0, hi = fs->entry_count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        int c = strcmp(path, ent_path(fs, mid));
        if (c == 0) {
            if (data) *data = fs->base + ent_data_off(fs, mid);
            if (len)  *len  = ent_data_len(fs, mid);
            return ESP_OK;
        }
        if (c < 0) hi = mid;
        else       lo = mid + 1;
    }
    return ESP_ERR_NOT_FOUND;
}

/* ---- enumeration ---- */

uint32_t spanfs_count(const spanfs_t *fs) {
    return fs ? fs->entry_count : 0;
}

esp_err_t spanfs_entry_at(const spanfs_t *fs, uint32_t i,
                          const char **path, const void **data, size_t *len) {
    if (!fs || i >= fs->entry_count) return ESP_ERR_INVALID_ARG;
    if (path) *path = ent_path(fs, i);
    if (data) *data = fs->base + ent_data_off(fs, i);
    if (len)  *len  = ent_data_len(fs, i);
    return ESP_OK;
}

size_t spanfs_image_size(const spanfs_t *fs) {
    return fs ? fs->total_size : 0;
}
