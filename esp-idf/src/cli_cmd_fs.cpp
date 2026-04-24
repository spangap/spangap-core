/** CLI commands: ls, cd, mkdir, rm, cat, df — filesystem operations (per-session cwd). */
#include "cli.h"
#include "fs.h"
#include "compat.h"
#include "esp_littlefs.h"
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
  if (strcmp(a, "help") == 0) {
    cliPrintf("  %-*s list files (default: cwd)\n", CLI_HELP_COL, "ls [-la] [path]");
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
  auto* listing = (fs_listing_t*)heap_caps_malloc(LS_MAX * sizeof(fs_listing_t), MALLOC_CAP_SPIRAM);
  if (!listing) { heap_caps_free(entries); cliPrintf("ls: out of memory\n"); return; }
  int n = fs_listdir(path, listing, LS_MAX);
  if (n < 0) { heap_caps_free(listing); heap_caps_free(entries); cliPrintf("ls: cannot open %s\n", path); return; }
  int count = 0;
  for (int i = 0; i < n && count < LS_MAX; i++) {
    if (!showAll && listing[i].name[0] == '.') continue;
    safeStrncpy(entries[count].name, listing[i].name, sizeof(entries[0].name));
    entries[count].mtime = listing[i].mtime;
    entries[count].size  = listing[i].size;
    entries[count].isDir = listing[i].isDir;
    count++;
  }
  heap_caps_free(listing);
  std::sort(entries, entries + count, [](const ls_entry& a, const ls_entry& b) { return a.mtime < b.mtime; });
  if (count == 0) cliPrintf("  (empty)\n");
  for (int i = 0; i < count; i++) {
    if (longFmt) {
      struct tm tm;
      localtime_r(&entries[i].mtime, &tm);
      char sz[16];
      if (entries[i].isDir)
        snprintf(sz, sizeof(sz), "<dir>");
      else
        fmtSize(entries[i].size, sz, sizeof(sz));
      cliPrintf("  %-40s  %04d-%02d-%02d %02d:%02d  %7s\n", entries[i].name, tm.tm_year + 1900,
                tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, sz);
    } else {
      cliPrintf("  %s\n", entries[i].name);
    }
  }
  heap_caps_free(entries);
}

static void cmdCd(const char* a) {
  if (strcmp(a, "help") == 0) {
    cliPrintf("  %-*s change cwd (bare cd → s.cli.start_dir)\n", CLI_HELP_COL, "cd [path]");
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
  if (strcmp(a, "help") == 0) {
    cliPrintf("  %-*s print current directory\n", CLI_HELP_COL, "pwd");
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
  if (strcmp(a, "help") == 0) {
    cliPrintf("  %-*s create directory\n", CLI_HELP_COL, "mkdir [-p] <path>");
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
  if (strcmp(a, "help") == 0) {
    cliPrintf("  %-*s remove files or dirs\n", CLI_HELP_COL, "rm [-rf] <path>...");
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
  if (strcmp(a, "help") == 0) {
    cliPrintf("  %-*s print file(s) to CLI\n", CLI_HELP_COL, "cat <file>...");
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
  if (strcmp(a, "help") == 0) {
    cliPrintf("  %-*s show disk usage\n", CLI_HELP_COL, "df [/path]");
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
    if (esp_littlefs_info(lfsLabel, &t, &u) == ESP_OK) {
      total = t;
      used = u;
      ok = true;
    }
  } else {
    FATFS* fs;
    DWORD fre_clust;
    if (f_getfree("0:", &fre_clust, &fs) == FR_OK) {
      uint64_t cs = (uint64_t)fs->csize * fs->ssize;
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
  cliPrintf("  total  %s\n  used   %s\n  free   %d%%\n", tBuf, uBuf, pctFree);
}

void cliCmdFsInit() {
  cliRegisterCmd("ls", cmdLs);
  cliRegisterCmd("cd", cmdCd);
  cliRegisterCmd("mkdir", cmdMkdir);
  cliRegisterCmd("pwd", cmdPwd);
  cliRegisterCmd("rm", cmdRm);
  cliRegisterCmd("cat", cmdCat);
  cliRegisterCmd("df", cmdDf);
}
