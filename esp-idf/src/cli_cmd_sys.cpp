/** CLI commands: reboot, reset, date, sleep, run, help. */
#include "cli.h"
#include "nvs_config.h"
#include "esp_littlefs.h"
#include "compat.h"
#include <cstring>
#include <cstdio>
#include <time.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static void cmdReboot(const char* a) {
    if (strcmp(a, "help") == 0) { cliPrintf("  %-*s restart device\n", CLI_HELP_COL, "reboot"); return; }
    cliPrintf("rebooting...\n");
    fflush(stdout);
    delay(100);
    esp_restart();
}

static void cmdResetFactory(const char* a) {
    if (strcmp(a, "help") == 0) { cliPrintf("  %-*s factory          erase NVS and reboot\n", CLI_HELP_COL, "reset"); return; }
    cliPrintf("erasing NVS + state and rebooting...\n");
    fflush(stdout);
    nvsErase();
    esp_littlefs_format("state");
    delay(100);
    esp_restart();
}

static void cmdDate(const char* a) {
    if (strcmp(a, "help") == 0) { cliPrintf("  %-*s [yyyymmddhhmmss]  show or set date/time\n", CLI_HELP_COL, "date"); return; }
    if (!*a) {
        struct timeval tv;
        gettimeofday(&tv, NULL);
        struct tm tm;
        localtime_r(&tv.tv_sec, &tm);
        cliPrintf("%04d-%02d-%02d %02d:%02d:%02d\n",
            tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
            tm.tm_hour, tm.tm_min, tm.tm_sec);
    } else {
        struct tm tm = {};
        if (sscanf(a, "%4d%2d%2d%2d%2d%2d",
            &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
            &tm.tm_hour, &tm.tm_min, &tm.tm_sec) == 6) {
            tm.tm_year -= 1900; tm.tm_mon -= 1;
            time_t t = mktime(&tm);
            struct timeval tv = { .tv_sec = t, .tv_usec = 0 };
            settimeofday(&tv, NULL);
            cliPrintf("date set\n");
        } else {
            cliPrintf("usage: date [yyyymmddhhmmss]\n");
        }
    }
}

static void cmdSleep(const char* a) {
    if (strcmp(a, "help") == 0) { cliPrintf("  %-*s <seconds>        delay execution\n", CLI_HELP_COL, "sleep"); return; }
    int secs = atoi(a);
    if (secs > 0) vTaskDelay(pdMS_TO_TICKS(secs * 1000));
}

static void cmdRun(const char* a) {
    if (strcmp(a, "help") == 0) { cliPrintf("  %-*s <file>             run CLI script file\n", CLI_HELP_COL, "run"); return; }
    if (!*a) { cliPrintf("usage: run <file>\n"); return; }
    cliRunFile(a);
}

/* help is special — declared here but needs access to the cmd registry.
 * We call cliProcess("help") which handles it in the dispatcher. */

void cliCmdSysInit() {
    cliRegisterCmd("reboot", cmdReboot);
    cliRegisterCmd("reset factory", cmdResetFactory);
    cliRegisterCmd("date", cmdDate);
    cliRegisterCmd("sleep", cmdSleep);
    cliRegisterCmd("run", cmdRun);
}
