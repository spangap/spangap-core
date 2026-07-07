/* Host-side reference driver for the spanfs reader. Not built on target.
 *
 *   host_reader <image>                 -> print "<len>\t<path>" per entry
 *   host_reader <image> cat <path>      -> write that file's bytes to stdout
 *   host_reader <image> check           -> exit 0 iff the image opens (CRC ok)
 */
#include "spanfs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned char *slurp(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *buf = malloc(sz > 0 ? (size_t)sz : 1);
    *n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    return buf;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: host_reader <image> [cat <path>|check]\n"); return 2; }
    size_t n = 0;
    unsigned char *buf = slurp(argv[1], &n);
    if (!buf) { fprintf(stderr, "cannot read %s\n", argv[1]); return 2; }

    spanfs_t *fs = NULL;
    esp_err_t e = spanfs_open_mem(buf, n, &fs);
    if (e != ESP_OK) { fprintf(stderr, "open failed: %d\n", e); return 1; }

    if (argc >= 3 && strcmp(argv[2], "check") == 0) {
        spanfs_close(fs); free(buf); return 0;
    }
    if (argc >= 4 && strcmp(argv[2], "cat") == 0) {
        const void *d = NULL; size_t len = 0;
        if (spanfs_lookup(fs, argv[3], &d, &len) != ESP_OK) {
            fprintf(stderr, "not found: %s\n", argv[3]); return 1;
        }
        fwrite(d, 1, len, stdout);
        spanfs_close(fs); free(buf); return 0;
    }

    uint32_t c = spanfs_count(fs);
    for (uint32_t i = 0; i < c; i++) {
        const char *p = NULL; const void *d = NULL; size_t len = 0;
        spanfs_entry_at(fs, i, &p, &d, &len);
        printf("%zu\t%s\n", len, p);
    }
    spanfs_close(fs); free(buf); return 0;
}
