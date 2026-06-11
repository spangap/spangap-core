/** CLI commands: mount status + `mount sd`.
 *
 * `mount` (no args) reports whether /sdcard, /fixed and /state are mounted,
 * with backing fs and usage. `mount sd` mounts an SD card inserted after boot.
 *
 * /fixed and /state are flash LittleFS partitions mounted at fs_init() and are
 * not separately mountable/formattable here yet (use `format flash` to wipe
 * /state). `mount fixed` / `mount state` say so rather than silently no-op.
 */
#include "cli.h"
#include "fs.h"

#include "compat.h"
#include <cstring>
#include <cstdio>
#include <cstdint>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

/* "used/total UNIT" with the unit chosen from the total size. */
static void fmtUsage(uint64_t used, uint64_t total, char* out, size_t n) {
    const char* unit = "B";
    double div = 1.0;
    if      (total >= 1024ULL * 1024 * 1024) { unit = "GB"; div = 1024.0 * 1024 * 1024; }
    else if (total >= 1024ULL * 1024)        { unit = "MB"; div = 1024.0 * 1024; }
    else if (total >= 1024ULL)               { unit = "KB"; div = 1024.0; }
    snprintf(out, n, "%.1f/%.1f %s", (double)used / div, (double)total / div, unit);
}

static void row(const char* path, bool mounted, const char* detail) {
    cliPrintf("  %-8s %-11s %s\n", path, mounted ? "mounted" : "not mounted", detail);
}

/* One row for a flash LittleFS partition. fsLittlefsInfo() returning ESP_OK
 * doubles as the "is mounted" probe (it errors if the partition isn't
 * registered). Proxied through the fs worker, so PSRAM-stack safe. */
static void rowLittlefs(const char* path, const char* label, const char* mode,
                        const char* suffix) {
    size_t total = 0, used = 0;
    bool mounted = fsLittlefsInfo(label, &total, &used) == ESP_OK;
    char detail[64] = "";
    if (mounted) {
        char usage[32];
        fmtUsage(used, total, usage, sizeof(usage));
        snprintf(detail, sizeof(detail), "LittleFS %s %s%s", mode, usage, suffix);
    }
    row(path, mounted, detail);
}

static void cmdMountStatus(void) {
    /* /sdcard — FAT on SD, optional. */
    {
        char detail[48] = "insert a card and run `mount sd`";
        bool mounted = sdAvailable();
        if (mounted) {
            uint64_t total = 0, used = 0;
            char usage[32] = "";
            if (fsSdInfo(&total, &used)) fmtUsage(used, total, usage, sizeof(usage));
            snprintf(detail, sizeof(detail), "FAT %s", usage);
        }
        row(FS_SDCARD, mounted, detail);
    }

    /* /fixed — read-only LittleFS, active OTA slot. */
    {
        char suffix[24];
        snprintf(suffix, sizeof(suffix), "  [%s]", fs_active_fixed_label());
        rowLittlefs(FS_FIXED, fs_active_fixed_label(), "ro", suffix);
    }

    /* /state — writable LittleFS, always mounted. */
    rowLittlefs(FS_STATE, "state", "rw", "");

    cliPrintf("  active state store: %s\n", fsStateDir());
}

static void cmdMount(const char* a) {
    if (cliWantsHelp(a)) {
        cliPrintf("%-*s show filesystem mount status\n", CLI_HELP_COL, "mount");
        return;
    }
    if (!*a) { cmdMountStatus(); return; }
    /* `mount sd` is its own registered command (longest-prefix match), so
     * anything reaching here is some other target. */
    if (strcmp(a, "fixed") == 0 || strcmp(a, "state") == 0) {
        cliPrintf("mount %s: %s is a flash partition mounted at boot; explicit "
                  "mount/format isn't wired up yet (use `format flash` to wipe "
                  "/state)\n",
                  a, strcmp(a, "fixed") == 0 ? FS_FIXED : FS_STATE);
        return;
    }
    cliPrintf("mount: unknown target '%s' (try `mount` for status, `mount sd` "
              "to mount the SD card)\n", a);
}

/* fs_mount_sd() runs on a DRAM stack (the CLI task is PSRAM-stacked) and the
 * CLI must block until it finishes so the result line is accurate. Mirrors the
 * format-worker pattern in cli_cmd_sys.cpp. */
struct MountCtx { SemaphoreHandle_t done; bool ok; };

static void mountSdWorker(void* arg) {
    auto* c = (MountCtx*)arg;
    c->ok = fs_mount_sd();
    xSemaphoreGive(c->done);
    killSelf();
}

static void cmdMountSd(const char* a) {
    if (cliWantsHelp(a)) {
        cliPrintf("%-*s mount an SD card inserted after boot, at /sdcard\n",
                  CLI_HELP_COL, "mount sd");
        return;
    }
    if (sdAvailable()) { cliPrintf("mount sd: already mounted at " FS_SDCARD "\n"); return; }
    cliPrintf("mounting SD card...\n");
    fflush(stdout);
    MountCtx c{ xSemaphoreCreateBinary(), false };
    if (!c.done) { cliPrintf("(out of memory)\n"); return; }
    spawnTask(mountSdWorker, "sdmnt", 4096, &c, 1, 0, STACK_DRAM);
    xSemaphoreTake(c.done, portMAX_DELAY);
    vSemaphoreDelete(c.done);
    cliPrintf("mount sd: %s\n", c.ok ? "mounted at " FS_SDCARD : "no SD card found");
}

void cliCmdMountInit() {
    cliRegisterCmd("mount", cmdMount);
    cliRegisterCmd("mount sd", cmdMountSd);
}
