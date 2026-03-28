/** CLI commands: ls, rm, df — filesystem operations. */
#include "cli.h"
#include "compat.h"
#include "esp_littlefs.h"
#include <cstring>
#include <cstdio>
#include <dirent.h>
#include <sys/stat.h>
#include <algorithm>
#include <unistd.h>  /* rmdir */
#include "ff.h"  /* FATFS */

static void cmdLs(const char* a) {
    if (strcmp(a, "help") == 0) { cliPrintf("  %-*s list files\n", CLI_HELP_COL, "ls [-la] [/path]"); return; }
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
    const char* path = *arg ? arg : "/sdcard";
    DIR* dir = opendir(path);
    if (!dir) { cliPrintf("ls: cannot open %s\n", path); return; }
    constexpr int LS_MAX = 128;
    struct ls_entry { char name[80]; time_t mtime; uint32_t size; bool isDir; };
    auto* entries = (ls_entry*)malloc(LS_MAX * sizeof(ls_entry));
    if (!entries) { closedir(dir); cliPrintf("ls: out of memory\n"); return; }
    int count = 0;
    struct dirent* ent;
    while ((ent = readdir(dir)) != nullptr && count < LS_MAX) {
        if (!showAll && ent->d_name[0] == '.') continue;
        char fullpath[192];
        snprintf(fullpath, sizeof(fullpath), "%.80s/%.80s", path, ent->d_name);
        struct stat st;
        if (stat(fullpath, &st) == 0) {
            safeStrncpy(entries[count].name, ent->d_name, sizeof(entries[0].name));
            entries[count].mtime = st.st_mtime;
            entries[count].size = (uint32_t)st.st_size;
            entries[count].isDir = S_ISDIR(st.st_mode);
            count++;
        }
    }
    closedir(dir);
    std::sort(entries, entries + count, [](const ls_entry& a, const ls_entry& b) {
        return a.mtime < b.mtime;
    });
    for (int i = 0; i < count; i++) {
        if (longFmt) {
            struct tm tm;
            localtime_r(&entries[i].mtime, &tm);
            char sz[16];
            if (entries[i].isDir) snprintf(sz, sizeof(sz), "<dir>");
            else fmtSize(entries[i].size, sz, sizeof(sz));
            cliPrintf("  %-40s  %04d-%02d-%02d %02d:%02d  %7s\n",
                entries[i].name, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                tm.tm_hour, tm.tm_min, sz);
        } else {
            cliPrintf("  %s\n", entries[i].name);
        }
    }
    free(entries);
}

static void cmdRm(const char* a) {
    if (strcmp(a, "help") == 0) { cliPrintf("  %-*s delete file or directory\n", CLI_HELP_COL, "rm <file|dir>"); return; }
    if (!*a) { cliPrintf("usage: rm <file>\n"); return; }
    char filepath[192];
    if (a[0] == '/') snprintf(filepath, sizeof(filepath), "%s", a);
    else snprintf(filepath, sizeof(filepath), "/sdcard/%s", a);
    if (remove(filepath) == 0) return;
    struct stat st;
    if (stat(filepath, &st) != 0 || !S_ISDIR(st.st_mode)) {
        cliPrintf("rm: cannot remove %s\n", filepath);
        return;
    }
    constexpr int RM_PATH = 192, RM_DEPTH = 8;
    struct rm_frame { DIR* dir; char path[RM_PATH]; };
    auto* stack = (rm_frame*)malloc(RM_DEPTH * sizeof(rm_frame));
    if (!stack) { cliPrintf("rm: out of memory\n"); return; }
    int depth = 0;
    snprintf(stack[0].path, RM_PATH, "%s", filepath);
    stack[0].dir = opendir(filepath);
    if (!stack[0].dir) { free(stack); cliPrintf("rm: cannot open %s\n", filepath); return; }
    while (depth >= 0) {
        struct dirent* ent = readdir(stack[depth].dir);
        if (!ent) { closedir(stack[depth].dir); rmdir(stack[depth].path); depth--; continue; }
        char child[RM_PATH];
        snprintf(child, RM_PATH, "%.80s/%.80s", stack[depth].path, ent->d_name);
        if (remove(child) == 0) continue;
        struct stat cst;
        if (stat(child, &cst) == 0 && S_ISDIR(cst.st_mode) && depth < RM_DEPTH - 1) {
            depth++;
            memcpy(stack[depth].path, child, RM_PATH);
            stack[depth].dir = opendir(child);
            if (!stack[depth].dir) { depth--; continue; }
        }
    }
    free(stack);
}

static void cmdDf(const char* a) {
    if (strcmp(a, "help") == 0) { cliPrintf("  %-*s show disk usage\n", CLI_HELP_COL, "df [/path]"); return; }
    const char* path = *a ? a : "/sdcard";
    uint64_t total = 0, used = 0;
    bool ok = false;
    const char* lfsLabel = nullptr;
    if (strcmp(path, "/webroot") == 0) lfsLabel = "webroot";
    else if (strcmp(path, "/state") == 0) lfsLabel = "state";
    if (lfsLabel) {
        size_t t = 0, u = 0;
        if (esp_littlefs_info(lfsLabel, &t, &u) == ESP_OK) { total = t; used = u; ok = true; }
    } else {
        FATFS* fs; DWORD fre_clust;
        if (f_getfree("0:", &fre_clust, &fs) == FR_OK) {
            uint64_t cs = (uint64_t)fs->csize * fs->ssize;
            total = (uint64_t)(fs->n_fatent - 2) * cs;
            used = total - (uint64_t)fre_clust * cs;
            ok = true;
        }
    }
    if (!ok) { cliPrintf("df: cannot stat %s\n", path); return; }
    char tBuf[16], uBuf[16];
    auto fmt64 = [](uint64_t b, char* buf, size_t len) {
        if (b <= UINT32_MAX) { fmtSize((uint32_t)b, buf, len); return; }
        snprintf(buf, len, "%.2fGB", b / (1024.0 * 1024 * 1024));
    };
    fmt64(total, tBuf, sizeof(tBuf));
    fmt64(used, uBuf, sizeof(uBuf));
    int pctFree = total > 0 ? (int)((total - used) * 100 / total) : 0;
    cliPrintf("  total  %s\n  used   %s\n  free   %d%%\n", tBuf, uBuf, pctFree);
}

void cliCmdFsInit() {
    cliRegisterCmd("ls", cmdLs);
    cliRegisterCmd("rm", cmdRm);
    cliRegisterCmd("df", cmdDf);
}
