/** CLI commands: reboot, reset, format, sleep, run, its. */
#include "cli.h"
#include "storage.h"
#include "pm.h"
#include "log.h"
#include "its.h"
#include "fs.h"

#include "compat.h"
#include <cstring>
#include <cstdio>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static void cmdReboot(const char* a) {
    if (cliWantsHelp(a)) { cliPrintf("%-*s restart device\n", CLI_HELP_COL, "reboot"); return; }
    storageSave();  /* flush pending settings before reboot */
    cliPrintf("rebooting...\n");
    fflush(stdout);
    delay(100);
    esp_restart();
}

/* All three tasks run on a DRAM stack — esp_littlefs_format disables the
 * PSRAM cache during SPI-flash writes; SD format serializes DMA likewise. */

static void resetFactoryTask(void*) {
    fsFormatFlash();           /* format flash; on reboot it repopulates */
    delay(100);
    esp_restart();
}

static void cmdResetFactory(const char* a) {
    if (cliWantsHelp(a)) { cliPrintf("%-*s factory reset: format flash state, reboot\n", CLI_HELP_COL, "reset factory"); return; }
    if (fsStateOnSd()) {
        cliPrintf("You've booted with an SDcard present: 'reset factory' is "
                  "only for wiping all user state in device flash. Boot "
                  "without SDcard and try again if that is what you want. To "
                  "delete all user information on an sdcard, type:\n\n");
        cliPrintf("format sd; mkdir /sdcard/state; reboot\n");
        return;
    }
    cliPrintf("factory reset: formatting flash state, rebooting...\n");
    fflush(stdout);
    spawnTask(resetFactoryTask, "rfact", 3072, nullptr, 1, 0, STACK_DRAM);
}

/* format flash/sd run on a DRAM-stack worker (esp_littlefs_format disables
 * the PSRAM cache; the CLI task is PSRAM-stacked) but the CLI command must
 * BLOCK until the worker finishes — otherwise a scripted one-liner like
 * `format sd; mkdir /sdcard/state; reboot` races the format. The worker
 * signals `done`, stores `ok`, then self-terminates (a task must not
 * return); the CLI takes the semaphore before continuing. */
struct FmtCtx { SemaphoreHandle_t done; bool ok; };

static void formatFlashWorker(void* arg) {
    auto* c = (FmtCtx*)arg;
    fsFormatFlash();
    c->ok = true;
    xSemaphoreGive(c->done);
    killSelf();
}

static void formatSdWorker(void* arg) {
    auto* c = (FmtCtx*)arg;
    c->ok = fsFormatSd();
    xSemaphoreGive(c->done);
    killSelf();
}

static bool runFmtWorker(TaskFunction_t fn, const char* name) {
    FmtCtx c{ xSemaphoreCreateBinary(), false };
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
    if (cliWantsHelp(a)) { cliPrintf("%-*s reformat the SD card (FAT), kept mounted at /sdcard\n", CLI_HELP_COL, "format sd"); return; }
    if (!sdAvailable()) { cliPrintf("format sd: no SD card mounted\n"); return; }
    cliPrintf("formatting SD card...\n");
    fflush(stdout);
    bool ok = runFmtWorker(formatSdWorker, "sdfmt");
    cliPrintf("format sd: %s\n", ok ? "done" : "failed");
}

static void cmdSleep(const char* a) {
    if (cliWantsHelp(a)) { cliPrintf("%-*s delay execution\n", CLI_HELP_COL, "sleep <seconds>"); return; }
    int secs = atoi(a);
    if (secs > 0) vTaskDelay(pdMS_TO_TICKS(secs * 1000));
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

/* help is special — declared here but needs access to the cmd registry.
 * We call cliProcess("help") which handles it in the dispatcher. */

void cliCmdSysInit() {
    cliRegisterCmd("reboot", cmdReboot);
    cliRegisterCmd("reset factory", cmdResetFactory);
    cliRegisterCmd("format flash", cmdFormatFlash);
    cliRegisterCmd("format sd", cmdFormatSd);
    cliRegisterCmd("sleep", cmdSleep);
    cliRegisterCmd("run", cmdRun);
    cliRegisterCmd("its", cmdIts);
}
