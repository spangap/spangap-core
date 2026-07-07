/* spanfs — ESP-only glue: esp_partition_mmap + a read-only esp_vfs face.
 *
 * The portable reader (spanfs.c) does all format work; this file maps a flash
 * partition in place and exposes POSIX open/read/lseek/fstat/stat + opendir/
 * readdir/closedir over the mapping. Every read is a memcpy from mmap'd flash,
 * so it is safe from any task (PSRAM stack included) with no cache-disable
 * round-trip through the SPI flash driver.
 */
#include "spanfs.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_partition.h"
#include "esp_vfs.h"
#include "esp_log.h"

static const char *TAG = "spanfs";

/* ---- partition open via mmap ---- */

static void munmap_release(void *ctx) {
    esp_partition_munmap((esp_partition_mmap_handle_t)(uintptr_t)ctx);
}

esp_err_t spanfs_open_partition(const char *label, spanfs_t **out) {
    if (!label || !out) return ESP_ERR_INVALID_ARG;
    const esp_partition_t *p = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, label);
    if (!p) {
        ESP_LOGE(TAG, "partition '%s' not found", label);
        return ESP_ERR_NOT_FOUND;
    }
    const void *map = NULL;
    esp_partition_mmap_handle_t h = 0;
    esp_err_t e = esp_partition_mmap(p, 0, p->size, ESP_PARTITION_MMAP_DATA, &map, &h);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "mmap '%s' failed: %s", label, esp_err_to_name(e));
        return e;
    }
    spanfs_t *fs = NULL;
    e = spanfs_open_mem(map, p->size, &fs);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "bad image on '%s': %s", label, esp_err_to_name(e));
        esp_partition_munmap(h);
        return e;
    }
    spanfs_set_release(fs, munmap_release, (void *)(uintptr_t)h);
    *out = fs;
    return ESP_OK;
}

/* ---- VFS state ---- */

#define SPANFS_MAX_MOUNTS 2
#define SPANFS_MAX_FD     8
#define SPANFS_MAX_DIR    4
#define SPANFS_NAME_MAX   128

typedef struct {
    bool           used;
    const uint8_t *data;
    size_t         len;
    off_t          pos;
} sfd_t;

typedef struct {
    bool          used;
    char          prefix[SPANFS_NAME_MAX];  /* relative dir, trailing '/', "" = root */
    size_t        plen;
    uint32_t      cursor;                    /* next index to examine */
    char          last[SPANFS_NAME_MAX];     /* last child name emitted (subdir dedup) */
    long          loc;                       /* telldir position */
    struct dirent de;
} sdir_t;

typedef struct {
    bool      used;
    spanfs_t *fs;
    char      base[ESP_VFS_PATH_MAX + 1];
    sfd_t     fds[SPANFS_MAX_FD];
    sdir_t    dirs[SPANFS_MAX_DIR];
} mount_t;

static mount_t s_mounts[SPANFS_MAX_MOUNTS];

/* ---- path helpers ---- */

/* VFS strips the mountpoint, so paths arrive as "/webroot/x" or "/". Return the
 * spanfs-relative path (no leading '/'). */
static const char *rel(const char *path) {
    while (*path == '/') path++;
    return path;
}

/* Is `dir` (relative, no leading '/') a directory — i.e. does any entry start
 * with dir + '/'? "" (root) is always a directory. */
static bool is_dir(spanfs_t *fs, const char *dir) {
    if (dir[0] == '\0') return true;
    size_t n = strlen(dir);
    uint32_t count = spanfs_count(fs);
    for (uint32_t i = 0; i < count; i++) {
        const char *p = NULL;
        spanfs_entry_at(fs, i, &p, NULL, NULL);
        if (strncmp(p, dir, n) == 0 && p[n] == '/') return true;
    }
    return false;
}

/* ---- file ops ---- */

static int sfd_alloc(mount_t *m) {
    for (int i = 0; i < SPANFS_MAX_FD; i++)
        if (!m->fds[i].used) return i;
    return -1;
}

static int spanfs_vfs_open(void *ctx, const char *path, int flags, int mode) {
    (void)mode;
    mount_t *m = (mount_t *)ctx;
    if ((flags & O_ACCMODE) != O_RDONLY || (flags & (O_CREAT | O_TRUNC))) {
        errno = EROFS;
        return -1;
    }
    const void *data = NULL;
    size_t len = 0;
    if (spanfs_lookup(m->fs, rel(path), &data, &len) != ESP_OK) {
        errno = ENOENT;
        return -1;
    }
    int fd = sfd_alloc(m);
    if (fd < 0) { errno = ENFILE; return -1; }
    m->fds[fd].used = true;
    m->fds[fd].data = (const uint8_t *)data;
    m->fds[fd].len  = len;
    m->fds[fd].pos  = 0;
    return fd;
}

static ssize_t spanfs_vfs_read(void *ctx, int fd, void *dst, size_t size) {
    mount_t *m = (mount_t *)ctx;
    if (fd < 0 || fd >= SPANFS_MAX_FD || !m->fds[fd].used) { errno = EBADF; return -1; }
    sfd_t *f = &m->fds[fd];
    if ((size_t)f->pos >= f->len) return 0;
    size_t avail = f->len - (size_t)f->pos;
    size_t n = size < avail ? size : avail;
    memcpy(dst, f->data + f->pos, n);   /* memcpy from mmap — task-agnostic */
    f->pos += n;
    return (ssize_t)n;
}

static off_t spanfs_vfs_lseek(void *ctx, int fd, off_t off, int whence) {
    mount_t *m = (mount_t *)ctx;
    if (fd < 0 || fd >= SPANFS_MAX_FD || !m->fds[fd].used) { errno = EBADF; return -1; }
    sfd_t *f = &m->fds[fd];
    off_t base = (whence == SEEK_SET) ? 0 :
                 (whence == SEEK_CUR) ? f->pos :
                 (whence == SEEK_END) ? (off_t)f->len : -1;
    if (base < 0) { errno = EINVAL; return -1; }
    off_t np = base + off;
    if (np < 0) { errno = EINVAL; return -1; }
    f->pos = np;
    return np;
}

static int spanfs_vfs_close(void *ctx, int fd) {
    mount_t *m = (mount_t *)ctx;
    if (fd < 0 || fd >= SPANFS_MAX_FD || !m->fds[fd].used) { errno = EBADF; return -1; }
    m->fds[fd].used = false;
    return 0;
}

static void fill_file_stat(struct stat *st, size_t len) {
    memset(st, 0, sizeof(*st));
    st->st_mode = S_IFREG | 0444;
    st->st_size = (off_t)len;
    st->st_nlink = 1;
}

static void fill_dir_stat(struct stat *st) {
    memset(st, 0, sizeof(*st));
    st->st_mode = S_IFDIR | 0555;
    st->st_nlink = 1;
}

static int spanfs_vfs_fstat(void *ctx, int fd, struct stat *st) {
    mount_t *m = (mount_t *)ctx;
    if (fd < 0 || fd >= SPANFS_MAX_FD || !m->fds[fd].used) { errno = EBADF; return -1; }
    fill_file_stat(st, m->fds[fd].len);
    return 0;
}

static int spanfs_vfs_stat(void *ctx, const char *path, struct stat *st) {
    mount_t *m = (mount_t *)ctx;
    const char *r = rel(path);
    const void *data = NULL;
    size_t len = 0;
    if (spanfs_lookup(m->fs, r, &data, &len) == ESP_OK) {
        fill_file_stat(st, len);
        return 0;
    }
    if (is_dir(m->fs, r)) {
        fill_dir_stat(st);
        return 0;
    }
    errno = ENOENT;
    return -1;
}

static int spanfs_vfs_access(void *ctx, const char *path, int amode) {
    mount_t *m = (mount_t *)ctx;
    if (amode & W_OK) { errno = EROFS; return -1; }
    const char *r = rel(path);
    if (spanfs_lookup(m->fs, r, NULL, NULL) == ESP_OK || is_dir(m->fs, r)) return 0;
    errno = ENOENT;
    return -1;
}

/* ---- directory ops ---- */

static DIR *spanfs_vfs_opendir(void *ctx, const char *name) {
    mount_t *m = (mount_t *)ctx;
    const char *r = rel(name);
    if (!is_dir(m->fs, r)) { errno = ENOENT; return NULL; }
    sdir_t *d = NULL;
    for (int i = 0; i < SPANFS_MAX_DIR; i++)
        if (!m->dirs[i].used) { d = &m->dirs[i]; break; }
    if (!d) { errno = ENFILE; return NULL; }
    memset(d, 0, sizeof(*d));
    d->used = true;
    if (r[0] == '\0') {
        d->prefix[0] = '\0';
        d->plen = 0;
    } else {
        snprintf(d->prefix, sizeof(d->prefix), "%s/", r);
        d->plen = strlen(d->prefix);
    }
    d->cursor = 0;
    d->loc = 0;
    d->last[0] = '\0';
    return (DIR *)d;
}

/* Emit the next immediate child (file or subdir) under the open dir. */
static struct dirent *spanfs_vfs_readdir(void *ctx, DIR *pdir) {
    mount_t *m = (mount_t *)ctx;
    sdir_t *d = (sdir_t *)pdir;
    uint32_t count = spanfs_count(m->fs);

    for (; d->cursor < count; d->cursor++) {
        const char *p = NULL;
        spanfs_entry_at(m->fs, d->cursor, &p, NULL, NULL);
        if (d->plen) {
            if (strncmp(p, d->prefix, d->plen) != 0) {
                /* Sorted: once past the prefix range there are no more children. */
                if (strcmp(p, d->prefix) > 0) break;
                continue;
            }
        }
        const char *rem = p + d->plen;
        if (rem[0] == '\0') continue;           /* the dir path itself (shouldn't occur) */
        const char *slash = strchr(rem, '/');
        size_t nlen = slash ? (size_t)(slash - rem) : strlen(rem);
        if (nlen >= sizeof(d->de.d_name)) nlen = sizeof(d->de.d_name) - 1;

        char name[SPANFS_NAME_MAX];
        if (nlen >= sizeof(name)) nlen = sizeof(name) - 1;
        memcpy(name, rem, nlen);
        name[nlen] = '\0';

        if (slash) {
            /* subdirectory — collapse the run of entries sharing this name */
            if (strcmp(name, d->last) == 0) continue;
            strncpy(d->last, name, sizeof(d->last) - 1);
            d->last[sizeof(d->last) - 1] = '\0';
        } else {
            d->last[0] = '\0';
        }

        memset(&d->de, 0, sizeof(d->de));
        d->de.d_ino = 0;
        d->de.d_type = slash ? DT_DIR : DT_REG;
        strncpy(d->de.d_name, name, sizeof(d->de.d_name) - 1);
        d->cursor++;
        d->loc++;
        return &d->de;
    }
    return NULL;
}

static long spanfs_vfs_telldir(void *ctx, DIR *pdir) {
    (void)ctx;
    return ((sdir_t *)pdir)->loc;
}

static void spanfs_vfs_seekdir(void *ctx, DIR *pdir, long offset) {
    mount_t *m = (mount_t *)ctx;
    sdir_t *d = (sdir_t *)pdir;
    /* Rewind and replay to the requested logical position (readdir is cheap). */
    d->cursor = 0;
    d->loc = 0;
    d->last[0] = '\0';
    while (d->loc < offset && spanfs_vfs_readdir(ctx, pdir))
        ;
    (void)m;
}

static int spanfs_vfs_closedir(void *ctx, DIR *pdir) {
    (void)ctx;
    sdir_t *d = (sdir_t *)pdir;
    d->used = false;
    return 0;
}

/* ---- registration ---- */

esp_err_t spanfs_vfs_register(spanfs_t *fs, const char *mountpoint) {
    if (!fs || !mountpoint) return ESP_ERR_INVALID_ARG;
    mount_t *m = NULL;
    for (int i = 0; i < SPANFS_MAX_MOUNTS; i++)
        if (!s_mounts[i].used) { m = &s_mounts[i]; break; }
    if (!m) return ESP_ERR_NO_MEM;
    memset(m, 0, sizeof(*m));
    m->used = true;
    m->fs = fs;
    strncpy(m->base, mountpoint, sizeof(m->base) - 1);

    esp_vfs_t vfs = {0};
    vfs.flags = ESP_VFS_FLAG_CONTEXT_PTR | ESP_VFS_FLAG_READONLY_FS;
    vfs.open_p     = spanfs_vfs_open;
    vfs.read_p     = spanfs_vfs_read;
    vfs.lseek_p    = spanfs_vfs_lseek;
    vfs.close_p    = spanfs_vfs_close;
    vfs.fstat_p    = spanfs_vfs_fstat;
    vfs.stat_p     = spanfs_vfs_stat;
    vfs.access_p   = spanfs_vfs_access;
    vfs.opendir_p  = spanfs_vfs_opendir;
    vfs.readdir_p  = spanfs_vfs_readdir;
    vfs.telldir_p  = spanfs_vfs_telldir;
    vfs.seekdir_p  = spanfs_vfs_seekdir;
    vfs.closedir_p = spanfs_vfs_closedir;

    esp_err_t e = esp_vfs_register(mountpoint, &vfs, m);
    if (e != ESP_OK) {
        m->used = false;
        ESP_LOGE(TAG, "vfs register '%s' failed: %s", mountpoint, esp_err_to_name(e));
    }
    return e;
}

esp_err_t spanfs_vfs_unregister(const char *mountpoint) {
    if (!mountpoint) return ESP_ERR_INVALID_ARG;
    for (int i = 0; i < SPANFS_MAX_MOUNTS; i++) {
        if (s_mounts[i].used && strcmp(s_mounts[i].base, mountpoint) == 0) {
            esp_err_t e = esp_vfs_unregister(mountpoint);
            s_mounts[i].used = false;
            return e;
        }
    }
    return ESP_ERR_NOT_FOUND;
}
