/** CLI commands: reboot, reset, format, sleep, run, its, bat. */
#include "cli.h"
#include "spangap.h"
#include "storage.h"
#include "pm.h"
#include "log.h"
#include "its.h"
#include "fs.h"

#include "compat.h"
#include "esp_system.h"   /* esp_restart */
#include <cstring>
#include <cstdio>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

/* `format sd` is registered unconditionally (it just reports "no SD card" when
 * SD is compiled out), but this Kconfig only exists when SDCARD is enabled —
 * give it a harmless fallback so the command compiles on non-SD boards too. */
#ifndef CONFIG_SPANGAP_SDCARD_ALLOC_KB
#define CONFIG_SPANGAP_SDCARD_ALLOC_KB 8
#endif

static void cmdReboot(const char* a) {
    if (cliWantsHelp(a)) { cliPrintf("%-*s restart device\n", CLI_HELP_COL, "reboot"); return; }
    storageSave();  /* flush pending settings before reboot */
    cliPrintf("rebooting...\n");
    /* Hand the console back to the log before the chip goes: the session cannot
     * end itself across a restart, and everything the boot prints would come up
     * wearing the CLI's colour. */
    cliSerialResumeLogNow();
    fflush(stdout);
    delay(100);
    esp_restart();
}

/* Enter safe mode: persist the flag, then reboot into it.
 *
 * Done inline rather than by writing the key and leaving it to the flag watcher
 * (spangapWatchSafeModeFlags). The watcher exists for writers that cannot
 * reboot the device themselves — the browser, an rnsh `set`, a boot script — and
 * it rides a storage subscription delivered to the cron task, which is one more
 * moving part than a command that is already running on a task that can simply
 * do it. Here we are that task: set, flush, restart. */
[[noreturn]] static void enterSafeMode(const char* key, int value) {
    /* Same as `reboot`: the safe-mode boot's own output — the wipe's progress
     * among it — must not arrive in the colour of the session that asked for
     * it, and that session ends here or not at all. */
    cliSerialResumeLogNow();
    fflush(stdout);
    storageSet(key, value);
    storageSave();          /* the flag must be on disk before the restart */
    delay(200);
    esp_restart();
    for (;;) {}             /* unreachable */
}

static void cmdBackup(const char* a) {
    if (cliWantsHelp(a)) {
        cliPrintf("%-*s reboot into safe mode and stream the state store out\n",
                  CLI_HELP_COL, "backup");
        return;
    }
    cliPrintf("rebooting into safe mode to back up; fetch it from "
              "https://<device>/ when it comes back\n");
    enterSafeMode("s.sys.backup", 1);
}

static void cmdRestore(const char* a) {
    if (cliWantsHelp(a)) {
        cliPrintf("%-*s reboot into safe mode to take a backup archive back in\n",
                  CLI_HELP_COL, "restore");
        return;
    }
    cliPrintf("rebooting into safe mode to restore; upload the archive at "
              "https://<device>/ when it comes back. Everything on the state "
              "store is erased first.\n");
    enterSafeMode("s.sys.restore", 1);
}

/* A factory reset does not happen here. It sets the safe-mode flag and reboots:
 * the wipe overwrites the whole flash region above the firmware — including
 * whatever a lower-floored predecessor left there — with random bytes, and that
 * cannot be done under a live system with /state mounted and every straddle
 * writing to it. The next boot comes up in safe mode, wipes, and reboots again.
 *
 * This is also why the old "booted from SD, refusing" guard is gone: the target
 * is explicit now (`flash`, `sd`, or `both`) instead of implied by which store
 * happens to be active. */
static void cmdResetFactory(const char* a) {
    if (cliWantsHelp(a)) {
        cliPrintf("%-*s wipe user state and reboot; default target flash\n",
                  CLI_HELP_COL, "reset factory [flash|sd|both]");
        return;
    }
    while (*a == ' ') a++;
    int target = SAFE_WIPE_FLASH;
    if (*a) {
        if      (strncmp(a, "flash", 5) == 0) target = SAFE_WIPE_FLASH;
        else if (strncmp(a, "both",  4) == 0) target = SAFE_WIPE_FLASH | SAFE_WIPE_SD;
        else if (strncmp(a, "sd",    2) == 0) target = SAFE_WIPE_SD;
        else { cliPrintf("reset factory: target must be flash, sd, or both\n"); return; }
    }
    if ((target & SAFE_WIPE_SD) && !sdAvailable()) {
        cliPrintf("reset factory: no SD card mounted\n");
        return;
    }
    cliPrintf("factory reset (%s): rebooting to wipe, which takes about a "
              "minute per 12 MB. The device comes back on its own access "
              "point.\n",
              target == SAFE_WIPE_SD ? "sd"
                : target == SAFE_WIPE_FLASH ? "flash" : "flash + sd");
    enterSafeMode("s.sys.factory_reset", target);
}

/* format flash/sd run on a DRAM-stack worker (esp_littlefs_format disables
 * the PSRAM cache; the CLI task is PSRAM-stacked) but the CLI command must
 * BLOCK until the worker finishes — otherwise a scripted one-liner like
 * `format sd; mkdir /sdcard/state; reboot` races the format. The worker
 * signals `done`, stores `ok`, then self-terminates (a task must not
 * return); the CLI takes the semaphore before continuing. */
struct FmtCtx { SemaphoreHandle_t done; bool ok; int allocKb; };

static void formatFlashWorker(void* arg) {
    auto* c = (FmtCtx*)arg;
    fsFormatFlash();
    c->ok = true;
    xSemaphoreGive(c->done);
    killSelf();
}

static void formatSdWorker(void* arg) {
    auto* c = (FmtCtx*)arg;
    c->ok = fsFormatSd(c->allocKb);   /* 0 -> CONFIG_SPANGAP_SDCARD_ALLOC_KB */
    xSemaphoreGive(c->done);
    killSelf();
}

static bool runFmtWorker(TaskFunction_t fn, const char* name, int allocKb = 0) {
    FmtCtx c{ xSemaphoreCreateBinary(), false, allocKb };
    if (!c.done) { cliPrintf("(out of memory)\n"); return false; }
    spawnTask(fn, name, 3072, &c, 1, 0, STACK_DRAM);
    xSemaphoreTake(c.done, portMAX_DELAY);
    vSemaphoreDelete(c.done);
    return c.ok;
}

static void cmdFormatFlash(const char* a) {
    if (cliWantsHelp(a)) { cliPrintf("%-*s unmount, format, remount the flash state partition\n", CLI_HELP_COL, "format flash"); return; }
    cliPrintf("formatting flash state partition...\n");
    fflush(stdout);
    runFmtWorker(formatFlashWorker, "ffmt");
    cliPrintf("format flash: done\n");
}

static void cmdFormatSd(const char* a) {
    if (cliWantsHelp(a)) {
        cliPrintf("%-*s reformat the SD card (FAT), kept mounted at /sdcard;\n", CLI_HELP_COL, "format sd [KB]");
        cliPrintf("%-*s optional FAT cluster size in KB (default %d, range 1-128)\n",
                  CLI_HELP_COL, "", CONFIG_SPANGAP_SDCARD_ALLOC_KB);
        return;
    }
    if (!sdAvailable()) { cliPrintf("format sd: no SD card mounted\n"); return; }
    int kb = CONFIG_SPANGAP_SDCARD_ALLOC_KB;
    if (a && *a) {
        kb = atoi(a);
        if (kb < 1 || kb > 128) { cliPrintf("format sd: cluster size must be 1-128 KB (got '%s')\n", a); return; }
    }
    cliPrintf("formatting SD card (%d KB clusters)...\n", kb);
    fflush(stdout);
    bool ok = runFmtWorker(formatSdWorker, "sdfmt", kb);
    cliPrintf("format sd: %s\n", ok ? "done" : "failed");
}

static void cmdSleep(const char* a) {
    if (cliWantsHelp(a)) { cliPrintf("%-*s delay execution\n", CLI_HELP_COL, "sleep <seconds>"); return; }
    int secs = atoi(a);
    /* delay() drops this CLI task's auto boost for the wait — a raw vTaskDelay
     * would pin 240 MHz (and block light sleep) for the whole sleep. */
    if (secs > 0) delay((uint32_t)secs * 1000);
}

static void cmdRun(const char* a) {
    if (cliWantsHelp(a)) { cliPrintf("%-*s run CLI script file\n", CLI_HELP_COL, "run <file>"); return; }
    if (!*a) { cliPrintf("usage: run <file>\n"); return; }
    cliRunFile(a);
}

static void cmdIts(const char* a) {
    if (cliWantsHelp(a)) { cliPrintf("%-*s ITS connection + stream pool status\n", CLI_HELP_COL, "its"); return; }
    itsStatus(cliPrintf);
}

/* Reports the battery.* ephemerals a board's battery monitor publishes (e.g. the
 * T-Deck). Generic: boards without a battery sense never set the keys, so this
 * just says so rather than printing zeroes. */
static void cmdBat(const char* a) {
    if (cliWantsHelp(a)) { cliPrintf("%-*s battery voltage + percent\n", CLI_HELP_COL, "bat"); return; }
    if (!storageExists("battery.percent")) { cliPrintf("No battery information\n"); return; }
    int mv = storageGetInt("battery.millivolt", 0);
    cliPrintf("voltage:  %d.%03d V\n", mv / 1000, mv % 1000);
    cliPrintf("percent:  %d%%\n", storageGetInt("battery.percent", 0));
}

/* help is special — declared here but needs access to the cmd registry.
 * We call cliProcess("help") which handles it in the dispatcher. */

void cliCmdSysInit() {
    cliRegisterCmd("reboot", cmdReboot);
    cliRegisterCmd("backup", cmdBackup);
    cliRegisterCmd("restore", cmdRestore);
    cliRegisterCmd("reset factory", cmdResetFactory);
    cliRegisterCmd("format flash", cmdFormatFlash);
    cliRegisterCmd("format sd", cmdFormatSd);
    cliRegisterCmd("sleep", cmdSleep);
    cliRegisterCmd("run", cmdRun);
    cliRegisterCmd("its", cmdIts);
    cliRegisterCmd("bat", cmdBat);
}
