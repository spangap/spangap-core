/** CLI commands: reboot, reset, date, sleep, run, help. */
#include "cli.h"
#include "storage.h"
#include "pm.h"
#include "log.h"
#include "esp_littlefs.h"

extern void factoryReset();
#include "compat.h"
#include <cstring>
#include <cstdio>
#include <time.h>
#include <sys/time.h>
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

static const time_t VALID_EPOCH = 1735689600;  /* 2025-01-01 00:00:00 UTC */

static void cmdDateWait(const char* a) {
    if (strcmp(a, "help") == 0) { cliPrintf("  %-*s wait for valid date/time\n", CLI_HELP_COL, "date wait [timeout_secs]"); return; }
    if (time(nullptr) >= VALID_EPOCH) return;  /* already valid */
    int timeout = *a ? atoi(a) : 60;
    pm_lock_handle_t lock = nullptr;
    pmLockCreate(PM_NO_DEEP_SLEEP, "datewait", &lock);
    pmLockAcquire(lock);
    uint32_t start = millis();
    while (time(nullptr) < VALID_EPOCH) {
        if ((int)(millis() - start) >= timeout * 1000) {
            info("date wait: timed out after %ds\n", timeout);
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    if (time(nullptr) >= VALID_EPOCH)
        info("valid date received\n");
    pmLockRelease(lock);
}

static void cmdDate(const char* a) {
    if (strcmp(a, "help") == 0) { cliPrintf("  %-*s show or set date/time\n", CLI_HELP_COL, "date [wait] [yyyymmddhhmmss]"); return; }
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
    cliRegisterCmd("date wait", cmdDateWait);
    cliRegisterCmd("date", cmdDate);
    cliRegisterCmd("sleep", cmdSleep);
    cliRegisterCmd("run", cmdRun);
}
