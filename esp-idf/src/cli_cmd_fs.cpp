/** CLI commands: ls, cd, mkdir, rm, cat, df — filesystem operations (per-session cwd). */
#include "cli.h"
#include "fs.h"
#include "compat.h"
#include <cstring>
#include <cstdio>
#include <cerrno>
#include <dirent.h>
#include <esp_heap_caps.h>
#include <sys/stat.h>
#include <algorithm>
#include <unistd.h>
#include "ff.h" /* FATFS */

static void cmdLs(const char* a) {
  if (cliWantsHelp(a)) {
    cliPrintf("%-*s list files (default: cwd)\n", CLI_HELP_COL, "ls [-la] [path]");
    return;
  }
  const char* arg = a;
  bool showAll = false, longFmt = false;
  while (*arg == '-') {
    for (const char* f = arg + 1; *f && *f != ' '; f++) {
      if (*f == 'a') showAll = true;
      else if (*f == 'l') longFmt = true;
    }
    while (*arg && *arg != ' ') arg++;
    while (*arg == ' ') arg++;
  }
  char path[256];
  if (*arg) {
    if (!cliResolveFsPath(arg, path, sizeof(path))) {
      cliPrintf("ls: path too long\n");
      return;
    }
  } else {
    cliGetCwd(path, sizeof(path));
  }
  constexpr int LS_MAX = 1024;
  struct ls_entry {
    char name[80];
    time_t mtime;
    uint32_t size;
    bool isDir;
  };
  auto* entries = (ls_entry*)heap_caps_malloc(LS_MAX * sizeof(ls_entry), MALLOC_CAP_SPIRAM);
  if (!entries) { cliPrintf("ls: out of memory\n"); return; }
  int count = 0;
  /* A plain file argument: stat it and emit the single entry, so
   * `ls [-l] /path/to/file` works like coreutils ls. */
  struct stat pst;
  if (fs_stat(path, &pst) == 0 && !S_ISDIR(pst.st_mode)) {
    const char* slash = strrchr(path, '/');
    safeStrncpy(entries[0].name, slash ? slash + 1 : path, sizeof(entries[0].name));
    entries[0].mtime = pst.st_mtime;
    entries[0].size  = (uint32_t)pst.st_size;
    entries[0].isDir = false;
    count = 1;
  } else {
    auto* listing = (fs_listing_t*)heap_caps_malloc(LS_MAX * sizeof(fs_listing_t), MALLOC_CAP_SPIRAM);
    if (!listing) { heap_caps_free(entries); cliPrintf("ls: out of memory\n"); return; }
    int n = fs_listdir(path, listing, LS_MAX);
    if (n < 0) { heap_caps_free(listing); heap_caps_free(entries); cliPrintf("ls: cannot open %s\n", path); return; }
    for (int i = 0; i < n && count < LS_MAX; i++) {
      if (!showAll && listing[i].name[0] == '.') continue;
      safeStrncpy(entries[count].name, listing[i].name, sizeof(entries[0].name));
      entries[count].mtime = listing[i].mtime;
      entries[count].size  = listing[i].size;
      entries[count].isDir = listing[i].isDir;
      count++;
    }
    heap_caps_free(listing);
  }
  std::sort(entries, entries + count, [](const ls_entry& a, const ls_entry& b) { return a.mtime < b.mtime; });
  if (count == 0) cliPrintf("(empty)\n");
  const bool color = cliWantsColor();
  for (int i = 0; i < count; i++) {
    /* Directories print bold-blue when the client wants color. The escapes sit
     * outside the %-40s field so column alignment (computed on the name only) is
     * preserved. */
    const bool hl = color && entries[i].isDir;
    if (hl) cliWrite(CLI_C_DIR, sizeof(CLI_C_DIR) - 1);
    if (longFmt) {
      struct tm tm;
      localtime_r(&entries[i].mtime, &tm);
      char sz[16];
      if (entries[i].isDir)
        snprintf(sz, sizeof(sz), "<dir>");
      else
        fmtSize(entries[i].size, sz, sizeof(sz));
      cliPrintf("%-40s", entries[i].name);
      if (hl) cliWrite(CLI_C_RESET, sizeof(CLI_C_RESET) - 1);
      cliPrintf("  %04d-%02d-%02d %02d:%02d  %7s\n", tm.tm_year + 1900,
                tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, sz);
    } else {
      cliPrintf("%s", entries[i].name);
      if (hl) cliWrite(CLI_C_RESET, sizeof(CLI_C_RESET) - 1);
      cliPrintf("\n");
    }
  }
  heap_caps_free(entries);
}

static void cmdCd(const char* a) {
  if (cliWantsHelp(a)) {
    cliPrintf("%-*s change cwd (bare cd → s.cli.start_dir)\n", CLI_HELP_COL, "cd [path]");
    return;
  }
  if (!*a) {
    cliCdToStartDir();
    return;
  }
  char target[256];
  if (!cliResolveFsPath(a, target, sizeof(target))) {
    cliPrintf("cd: path too long\n");
    return;
  }
  if (!cliSetCwd(target)) cliPrintf("cd: not a directory: %s\n", target);
}

static void cmdPwd(const char* a) {
  if (cliWantsHelp(a)) {
    cliPrintf("%-*s print current directory\n", CLI_HELP_COL, "pwd");
    return;
  }
  char cwd[256];
  cliGetCwd(cwd, sizeof(cwd));
  cliPrintf("%s\n", cwd);
}

/** Create path and parents; path is absolute normalized. */
static bool mkdirParents(const char* path) {
  char tmp[256];
  safeStrncpy(tmp, path, sizeof(tmp));
  size_t len = strlen(tmp);
  for (size_t i = 1; i < len; i++) {
    if (tmp[i] != '/') continue;
    tmp[i] = '\0';
    struct stat st;
    if (fs_stat(tmp, &st) == 0) {
      if (!S_ISDIR(st.st_mode)) {
        tmp[i] = '/';
        return false;
      }
    } else {
      if (fs_mkdir(tmp) != 0 && errno != EEXIST) {
        tmp[i] = '/';
        return false;
      }
    }
    tmp[i] = '/';
  }
  if (fs_mkdir(tmp) == 0) return true;
  if (errno != EEXIST) return false;
  struct stat st;
  return fs_stat(tmp, &st) == 0 && S_ISDIR(st.st_mode);
}

static void cmdMkdir(const char* a) {
  if (cliWantsHelp(a)) {
    cliPrintf("%-*s create directory\n", CLI_HELP_COL, "mkdir [-p] <path>");
    return;
  }
  const char* p = a;
  bool parents = false;
  while (*p == ' ') p++;
  while (*p == '-') {
    for (p++; *p && *p != ' '; p++) {
      if (*p == 'p') parents = true;
    }
    while (*p == ' ') p++;
  }
  if (!*p) {
    cliPrintf("usage: mkdir [-p] <path>\n");
    return;
  }
  char full[256];
  if (!cliResolveFsPath(p, full, sizeof(full))) {
    cliPrintf("mkdir: path too long\n");
    return;
  }
  bool ok = parents ? mkdirParents(full) : (fs_mkdir(full) == 0);
  if (!ok) cliPrintf("mkdir: %s: %s\n", full, strerror(errno));
}

static bool rmDotSkip(const char* name) {
  return name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0'));
}

static void rmRecursive(const char* filepath, bool opt_f) {
  constexpr int RM_PATH = 256, RM_DEPTH = 16;
  struct rm_frame {
    int dir;
    char path[RM_PATH];
  };
  auto* stack = (rm_frame*)malloc(RM_DEPTH * sizeof(rm_frame));
  if (!stack) {
    cliPrintf("rm: out of memory\n");
    return;
  }
  int depth = 0;
  safeStrncpy(stack[0].path, filepath, RM_PATH);
  stack[0].dir = fs_opendir(filepath);
  if (stack[0].dir < 0) {
    free(stack);
    if (!opt_f) cliPrintf("rm: cannot open %s\n", filepath);
    return;
  }
  while (depth >= 0) {
    fs_dirent_t ent;
    if (!fs_readdir(stack[depth].dir, &ent)) {
      fs_closedir(stack[depth].dir);
      if (fs_remove(stack[depth].path) != 0 && !opt_f)
        cliPrintf("rm: cannot remove %s: %s\n", stack[depth].path, strerror(errno));
      depth--;
      continue;
    }
    if (rmDotSkip(ent.name)) continue;
    char child[RM_PATH];
    snprintf(child, RM_PATH, "%.150s/%.80s", stack[depth].path, ent.name);
    if (fs_remove(child) == 0) continue;
    struct stat cst;
    if (fs_stat(child, &cst) == 0 && S_ISDIR(cst.st_mode) && depth < RM_DEPTH - 1) {
      depth++;
      safeStrncpy(stack[depth].path, child, RM_PATH);
      stack[depth].dir = fs_opendir(child);
      if (stack[depth].dir < 0) {
        if (!opt_f) cliPrintf("rm: cannot open %s\n", child);
        depth--;
      }
    } else if (!opt_f) {
      cliPrintf("rm: cannot remove %s\n", child);
    }
  }
  free(stack);
}

static void cmdRm(const char* a) {
  if (cliWantsHelp(a)) {
    cliPrintf("%-*s remove files or dirs\n", CLI_HELP_COL, "rm [-rf] <path>...");
    return;
  }
  bool opt_f = false, opt_r = false;
  char paths[8][256];
  int np = 0;
  const char* p = a;
  while (*p && np < 8) {
    while (*p == ' ') p++;
    if (!*p) break;
    if (*p == '-') {
      for (p++; *p && *p != ' '; p++) {
        if (*p == 'f') opt_f = true;
        else if (*p == 'r') opt_r = true;
      }
      continue;
    }
    const char* start = p;
    while (*p && *p != ' ') p++;
    size_t wl = (size_t)(p - start);
    if (wl >= sizeof(paths[0])) {
      cliPrintf("rm: path too long\n");
      return;
    }
    memcpy(paths[np], start, wl);
    paths[np][wl] = '\0';
    np++;
  }
  if (np == 0) {
    cliPrintf("usage: rm [-rf] <path>...\n");
    return;
  }
  for (int i = 0; i < np; i++) {
    char full[256];
    if (!cliResolveFsPath(paths[i], full, sizeof(full))) {
      cliPrintf("rm: path too long\n");
      continue;
    }
    struct stat st;
    if (fs_stat(full, &st) != 0) {
      if (!opt_f) cliPrintf("rm: cannot stat %s\n", full);
      continue;
    }
    if (S_ISDIR(st.st_mode)) {
      if (!opt_r) {
        cliPrintf("rm: %s: is a directory\n", full);
        continue;
      }
      rmRecursive(full, opt_f);
    } else {
      if (fs_remove(full) != 0) {
        if (!(opt_f && errno == ENOENT)) cliPrintf("rm: cannot remove %s: %s\n", full, strerror(errno));
      }
    }
  }
}

static void cmdCat(const char* a) {
  if (cliWantsHelp(a)) {
    cliPrintf("%-*s print file(s) to CLI\n", CLI_HELP_COL, "cat <file>...");
    return;
  }
  if (!*a) {
    cliPrintf("usage: cat <file>...\n");
    return;
  }
  char paths[8][256];
  int np = 0;
  const char* p = a;
  while (*p && np < 8) {
    while (*p == ' ') p++;
    if (!*p) break;
    const char* start = p;
    while (*p && *p != ' ') p++;
    size_t wl = (size_t)(p - start);
    if (wl >= sizeof(paths[0])) {
      cliPrintf("cat: path too long\n");
      return;
    }
    memcpy(paths[np], start, wl);
    paths[np][wl] = '\0';
    np++;
  }
  for (int i = 0; i < np; i++) {
    char full[256];
    if (!cliResolveFsPath(paths[i], full, sizeof(full))) {
      cliPrintf("cat: path too long\n");
      continue;
    }
    struct stat st;
    if (fs_stat(full, &st) != 0 || S_ISDIR(st.st_mode)) {
      cliPrintf("cat: %s: not a regular file\n", full);
      continue;
    }
    int f = fs_open(full, "rb");
    if (f < 0) {
      cliPrintf("cat: cannot open %s\n", full);
      continue;
    }
    char buf[512];
    size_t n;
    while ((n = fs_read(buf, 1, sizeof(buf), f)) > 0) cliWrite(buf, n);
    fs_close(f);
  }
}

static void cmdDf(const char* a) {
  if (cliWantsHelp(a)) {
    cliPrintf("%-*s show disk usage\n", CLI_HELP_COL, "df [/path]");
    return;
  }
  char path[256];
  if (*a) {
    if (!cliResolveFsPath(a, path, sizeof(path))) {
      cliPrintf("df: path too long\n");
      return;
    }
  } else
    cliGetCwd(path, sizeof(path));
  uint64_t total = 0, used = 0;
  bool ok = false;
  const char* lfsLabel = nullptr;
  if (strcmp(path, "/webroot") == 0 || strncmp(path, "/fixed", 6) == 0)
    lfsLabel = "webroot";
  else if (strncmp(path, "/state", 6) == 0)
    lfsLabel = "state";
  if (lfsLabel) {
    size_t t = 0, u = 0;
    if (fsLittlefsInfo(lfsLabel, &t, &u) == ESP_OK) {
      total = t;
      used = u;
      ok = true;
    }
  } else {
    FATFS* fs;
    DWORD fre_clust;
    if (f_getfree("0:", &fre_clust, &fs) == FR_OK) {
      /* FATFS only carries a runtime `ssize` field in variable-sector mode
       * (FF_MAX_SS != FF_MIN_SS); with a single configured sector size the
       * size is the compile-time constant FF_MAX_SS. */
#if FF_MAX_SS != FF_MIN_SS
      uint32_t ssize = fs->ssize;
#else
      uint32_t ssize = FF_MAX_SS;
#endif
      uint64_t cs = (uint64_t)fs->csize * ssize;
      total = (uint64_t)(fs->n_fatent - 2) * cs;
      used = total - (uint64_t)fre_clust * cs;
      ok = true;
    }
  }
  if (!ok) {
    cliPrintf("df: cannot stat %s\n", path);
    return;
  }
  char tBuf[16], uBuf[16];
  auto fmt64 = [](uint64_t b, char* buf, size_t len) {
    if (b <= UINT32_MAX) {
      fmtSize((uint32_t)b, buf, len);
      return;
    }
    snprintf(buf, len, "%.2fGB", b / (1024.0 * 1024 * 1024));
  };
  fmt64(total, tBuf, sizeof(tBuf));
  fmt64(used, uBuf, sizeof(uBuf));
  int pctFree = total > 0 ? (int)((total - used) * 100 / total) : 0;
  cliPrintf("total  %s\nused   %s\nfree   %d%%\n", tBuf, uBuf, pctFree);
}

/* ---- cp / mv ----
 * Stream copy via the fs worker, so these work on any path (LittleFS
 * /state, /fixed, FAT /sdcard) and across filesystems. mv tries an
 * in-place rename first (cheap, same-fs) and falls back to copy+delete
 * when the rename crosses a mount (fs_rename → POSIX rename → EXDEV). */

/** Last path component of an absolute, normalised path. */
static const char* baseName(const char* path) {
  const char* slash = strrchr(path, '/');
  return slash ? slash + 1 : path;
}

/** If dst is an existing directory, append basename(src): cp/mv-into-dir
 *  semantics. Otherwise dst is used verbatim. */
static bool resolveDest(const char* src, const char* dst, char* out, size_t outLen) {
  struct stat st;
  if (fs_stat(dst, &st) == 0 && S_ISDIR(st.st_mode)) {
    if ((size_t)snprintf(out, outLen, "%s/%s", dst, baseName(src)) >= outLen) return false;
  } else {
    if (strlen(dst) >= outLen) return false;
    safeStrncpy(out, dst, outLen);
  }
  return true;
}

/** Byte-copy one regular file. Returns 0 on success, -1 on error. */
static int copyFile(const char* src, const char* dst) {
  int in = fs_open(src, "rb");
  if (in < 0) return -1;
  int out = fs_open(dst, "wb");
  if (out < 0) { fs_close(in); return -1; }
  constexpr size_t BUFSZ = 8192;
  char* buf = (char*)heap_caps_malloc(BUFSZ, MALLOC_CAP_SPIRAM);
  if (!buf) { fs_close(in); fs_close(out); return -1; }
  int rc = 0;
  size_t n;
  while ((n = fs_read(buf, 1, BUFSZ, in)) > 0) {
    if (fs_write(buf, 1, n, out) != n) { rc = -1; break; }
  }
  heap_caps_free(buf);
  fs_sync(out);
  fs_close(out);
  fs_close(in);
  if (rc != 0) fs_remove(dst);
  return rc;
}

/** Recursively copy src (file or directory tree) to dst. dst is the final
 *  target path (already resolved). Returns 0 on success, -1 on error. */
static int copyTree(const char* src, const char* dst) {
  struct stat st;
  if (fs_stat(src, &st) != 0) return -1;
  if (!S_ISDIR(st.st_mode)) return copyFile(src, dst);

  /* DFS via open dir handles — like rmRecursive, the stack grows with
   * tree *depth*, not directory width. One open dir handle per level. */
  constexpr int MAX_DEPTH = 16, PATHSZ = 256;
  struct frame {
    int  dir;
    char s[PATHSZ];
    char d[PATHSZ];
  };
  auto* stack = (frame*)heap_caps_malloc(MAX_DEPTH * sizeof(frame), MALLOC_CAP_SPIRAM);
  if (!stack) return -1;

  if (fs_mkdir(dst) != 0 && errno != EEXIST) { heap_caps_free(stack); return -1; }
  safeStrncpy(stack[0].s, src, PATHSZ);
  safeStrncpy(stack[0].d, dst, PATHSZ);
  stack[0].dir = fs_opendir(src);
  if (stack[0].dir < 0) { heap_caps_free(stack); return -1; }

  int rc = 0, depth = 0;
  while (depth >= 0) {
    fs_dirent_t ent;
    if (!fs_readdir(stack[depth].dir, &ent)) {
      fs_closedir(stack[depth].dir);
      depth--;
      continue;
    }
    if (rmDotSkip(ent.name)) continue;
    char cs[PATHSZ], cd[PATHSZ];
    if ((size_t)snprintf(cs, PATHSZ, "%.150s/%.80s", stack[depth].s, ent.name) >= PATHSZ ||
        (size_t)snprintf(cd, PATHSZ, "%.150s/%.80s", stack[depth].d, ent.name) >= PATHSZ) {
      rc = -1;
      break;
    }
    struct stat cst;
    if (fs_stat(cs, &cst) != 0) { rc = -1; break; }
    if (S_ISDIR(cst.st_mode)) {
      if (fs_mkdir(cd) != 0 && errno != EEXIST) { rc = -1; break; }
      if (depth + 1 >= MAX_DEPTH) { rc = -1; break; }  /* tree too deep */
      int nd = fs_opendir(cs);
      if (nd < 0) { rc = -1; break; }
      depth++;
      stack[depth].dir = nd;
      safeStrncpy(stack[depth].s, cs, PATHSZ);
      safeStrncpy(stack[depth].d, cd, PATHSZ);
    } else if (copyFile(cs, cd) != 0) {
      rc = -1;
      break;
    }
  }
  while (depth >= 0) fs_closedir(stack[depth--].dir);  /* unwind on error */
  heap_caps_free(stack);
  return rc;
}

/** Remove src entirely — file or directory tree (reuses rm's recursion). */
static void removeTree(const char* path) {
  struct stat st;
  if (fs_stat(path, &st) != 0) return;
  if (S_ISDIR(st.st_mode)) rmRecursive(path, true);
  else fs_remove(path);
}

static void cmdCp(const char* a) {
  if (cliWantsHelp(a)) {
    cliPrintf("%-*s copy file or tree (cross-fs ok)\n", CLI_HELP_COL, "cp [-r] <src> <dst>");
    return;
  }
  bool opt_r = false;
  const char* p = a;
  char words[2][256];
  int nw = 0;
  while (*p && nw < 2) {
    while (*p == ' ') p++;
    if (!*p) break;
    if (*p == '-') {
      for (p++; *p && *p != ' '; p++)
        if (*p == 'r' || *p == 'R') opt_r = true;
      continue;
    }
    const char* start = p;
    while (*p && *p != ' ') p++;
    size_t wl = (size_t)(p - start);
    if (wl >= sizeof(words[0])) { cliPrintf("cp: path too long\n"); return; }
    memcpy(words[nw], start, wl);
    words[nw][wl] = '\0';
    nw++;
  }
  if (nw != 2) { cliPrintf("usage: cp [-r] <src> <dst>\n"); return; }
  char src[256], dstArg[256], dst[256];
  if (!cliResolveFsPath(words[0], src, sizeof(src)) ||
      !cliResolveFsPath(words[1], dstArg, sizeof(dstArg))) {
    cliPrintf("cp: path too long\n");
    return;
  }
  struct stat st;
  if (fs_stat(src, &st) != 0) { cliPrintf("cp: %s: %s\n", src, strerror(errno)); return; }
  if (S_ISDIR(st.st_mode) && !opt_r) { cliPrintf("cp: %s: is a directory (use -r)\n", src); return; }
  if (!resolveDest(src, dstArg, dst, sizeof(dst))) { cliPrintf("cp: path too long\n"); return; }
  if (copyTree(src, dst) != 0) cliPrintf("cp: failed copying %s -> %s\n", src, dst);
}

static void cmdMv(const char* a) {
  if (cliWantsHelp(a)) {
    cliPrintf("%-*s move/rename (cross-fs ok)\n", CLI_HELP_COL, "mv <src> <dst>");
    return;
  }
  char words[2][256];
  int nw = 0;
  const char* p = a;
  while (*p && nw < 2) {
    while (*p == ' ') p++;
    if (!*p) break;
    const char* start = p;
    while (*p && *p != ' ') p++;
    size_t wl = (size_t)(p - start);
    if (wl >= sizeof(words[0])) { cliPrintf("mv: path too long\n"); return; }
    memcpy(words[nw], start, wl);
    words[nw][wl] = '\0';
    nw++;
  }
  if (nw != 2) { cliPrintf("usage: mv <src> <dst>\n"); return; }
  char src[256], dstArg[256], dst[256];
  if (!cliResolveFsPath(words[0], src, sizeof(src)) ||
      !cliResolveFsPath(words[1], dstArg, sizeof(dstArg))) {
    cliPrintf("mv: path too long\n");
    return;
  }
  struct stat st;
  if (fs_stat(src, &st) != 0) { cliPrintf("mv: %s: %s\n", src, strerror(errno)); return; }
  if (!resolveDest(src, dstArg, dst, sizeof(dst))) { cliPrintf("mv: path too long\n"); return; }
  /* Same-fs fast path: a single rename. fs_rename → POSIX rename, which
   * fails with EXDEV across mounts (LittleFS ↔ FAT, /state ↔ /sdcard). */
  if (fs_rename(src, dst) == 0) return;
  /* Cross-fs (or rename-unsupported): copy the tree, then delete src. */
  if (copyTree(src, dst) != 0) {
    cliPrintf("mv: failed copying %s -> %s\n", src, dst);
    return;
  }
  removeTree(src);
}

void cliCmdFsInit() {
  cliRegisterCmd("ls", cmdLs);
  cliRegisterCmd("cd", cmdCd);
  cliRegisterCmd("mkdir", cmdMkdir);
  cliRegisterCmd("pwd", cmdPwd);
  cliRegisterCmd("rm", cmdRm);
  cliRegisterCmd("cat", cmdCat);
  cliRegisterCmd("cp", cmdCp);
  cliRegisterCmd("mv", cmdMv);
  cliRegisterCmd("df", cmdDf);
}
