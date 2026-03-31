/** CLI commands: reboot, reset, sleep, run. */
#include "cli.h"
#include "storage.h"
#include "pm.h"
#include "log.h"
#include "esp_littlefs.h"

extern void factoryReset();
#include "compat.h"
#include <cstring>
#include <cstdio>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static void cmdReboot(const char* a) {
    if (strcmp(a, "help") == 0) { cliPrintf("  %-*s restart device\n", CLI_HELP_COL, "reboot"); return; }
    storageSave();  /* flush pending settings before reboot */
    cliPrintf("rebooting...\n");
    fflush(stdout);
    delay(100);
    esp_restart();
}

static void cmdResetFactory(const char* a) {
    if (strcmp(a, "help") == 0) { cliPrintf("  %-*s factory reset and reboot\n", CLI_HELP_COL, "reset factory"); return; }
    cliPrintf("factory reset: formatting /state and rebooting...\n");
    fflush(stdout);
    esp_littlefs_format("state");
    factoryReset();
    delay(100);
    esp_restart();
}

static void cmdSleep(const char* a) {
    if (strcmp(a, "help") == 0) { cliPrintf("  %-*s delay execution\n", CLI_HELP_COL, "sleep <seconds>"); return; }
    int secs = atoi(a);
    if (secs > 0) vTaskDelay(pdMS_TO_TICKS(secs * 1000));
}

static void cmdRun(const char* a) {
    if (strcmp(a, "help") == 0) { cliPrintf("  %-*s run CLI script file\n", CLI_HELP_COL, "run <file>"); return; }
    if (!*a) { cliPrintf("usage: run <file>\n"); return; }
    cliRunFile(a);
}

/* help is special — declared here but needs access to the cmd registry.
 * We call cliProcess("help") which handles it in the dispatcher. */

void cliCmdSysInit() {
    cliRegisterCmd("reboot", cmdReboot);
    cliRegisterCmd("reset factory", cmdResetFactory);
    cliRegisterCmd("sleep", cmdSleep);
    cliRegisterCmd("run", cmdRun);
}
