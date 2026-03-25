/**
 * cron — minute-resolution task scheduler with deep sleep support.
 *
 * Entries in /state/crontab file (standard unix cron, no user field).
 * Commands sent to CLI via stream buffer for proper serialization.
 * Enabled/disabled via s.cron.enable config. Subscribes via storageSubscribeChanges.
 */
#include "cron.h"
#include "storage.h"
#include "its.h"
#include "pm.h"
#include "cli.h"
#include "nvs_config.h"
#include "compat.h"
#include "esp_heap_caps.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"
#include "esp_sleep.h"

/* ---- RTC state (survives deep sleep) ---- */

RTC_DATA_ATTR static uint32_t cronLastMinute = 0;

/* ---- Command stream → CLI task ---- */

#define CRON_STREAM_SIZE 256
static StreamBufferHandle_t cronStream = nullptr;
static pm_lock_handle_t cronDeepLock = nullptr;

static bool cronEnabled() {
    return storageGetInt("s.cron.enable") != 0;
}

/** Check if /state/crontab has any non-comment entries. */
static bool crontabHasEntries() {
    FILE* f = fopen("/state/crontab", "r");
    if (!f) return false;
    char line[192];
    bool found = false;
    while (fgets(line, sizeof(line), f)) {
        const char* p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p && *p != '#' && *p != '\n') { found = true; break; }
    }
    fclose(f);
    return found;
}

/** Update deep sleep lock: held unless cron is enabled with entries. */
static void cronUpdateLock() {
    bool allow = cronEnabled() && crontabHasEntries();
    static bool released = false;
    if (allow && !released) {
        pmLockRelease(cronDeepLock);
        released = true;
    } else if (!allow && released) {
        pmLockAcquire(cronDeepLock);
        released = false;
    }
}

/* ---- Cron field matching ---- */

/** Check if a single value matches a cron field expression.
 *  Supports: *, N, N-M, N/S, N-M/S, and comma-separated lists of any. */
static bool cronMatchField(const char* field, int value, int minVal, int maxVal) {
    /* Comma-separated: check each part */
    const char* comma = strchr(field, ',');
    if (comma) {
        /* Check parts iteratively to avoid deep recursion */
        char buf[32];
        const char* p = field;
        while (p) {
            const char* next = strchr(p, ',');
            size_t len = next ? (size_t)(next - p) : strlen(p);
            if (len >= sizeof(buf)) return false;
            memcpy(buf, p, len);
            buf[len] = '\0';
            if (cronMatchField(buf, value, minVal, maxVal)) return true;
            p = next ? next + 1 : nullptr;
        }
        return false;
    }

    /* Parse step: field/step */
    int step = 0;
    char base[16];
    const char* slash = strchr(field, '/');
    if (slash) {
        size_t blen = slash - field;
        if (blen >= sizeof(base)) return false;
        memcpy(base, field, blen);
        base[blen] = '\0';
        step = atoi(slash + 1);
        if (step <= 0) return false;
    } else {
        strncpy(base, field, sizeof(base) - 1);
        base[sizeof(base) - 1] = '\0';
    }

    /* Star: match any (with optional step) */
    if (strcmp(base, "*") == 0) {
        if (step == 0) return true;
        return (value - minVal) % step == 0;
    }

    /* Range: N-M (with optional step) */
    const char* dash = strchr(base, '-');
    if (dash) {
        int lo = atoi(base);
        int hi = atoi(dash + 1);
        if (value < lo || value > hi) return false;
        if (step == 0) return true;
        return (value - lo) % step == 0;
    }

    /* Plain number (with optional step — step on a single number is odd but handle it) */
    int num = atoi(base);
    if (step == 0) return value == num;
    if (value < num) return false;
    return (value - num) % step == 0;
}

/** Parse a cron line and check if it matches the given time.
 *  Returns pointer to the command string (after the 5 time fields),
 *  or nullptr if no match. */
static const char* cronMatch(const char* entry, const struct tm* tm) {
    const char* p = entry;
    while (*p == ' ') p++;

    /* Parse 5 fields: minute hour dom month dow */
    struct { int value; int min; int max; } fields[] = {
        { tm->tm_min,  0, 59 },
        { tm->tm_hour, 0, 23 },
        { tm->tm_mday, 1, 31 },
        { tm->tm_mon + 1, 1, 12 },
        { tm->tm_wday, 0, 7 },  /* 0 and 7 are both Sunday */
    };

    for (int i = 0; i < 5; i++) {
        /* Extract field token */
        while (*p == ' ') p++;
        if (*p == '\0') return nullptr;
        const char* start = p;
        while (*p && *p != ' ') p++;
        char token[32];
        size_t tlen = p - start;
        if (tlen >= sizeof(token)) return nullptr;
        memcpy(token, start, tlen);
        token[tlen] = '\0';

        int val = fields[i].value;
        /* Day of week: treat 7 as 0 (both Sunday) */
        if (i == 4 && val == 7) val = 0;

        if (!cronMatchField(token, val, fields[i].min, fields[i].max))
            return nullptr;
    }

    /* Skip whitespace to command */
    while (*p == ' ') p++;
    return (*p) ? p : nullptr;
}

/* ---- cronPoll ---- */

bool cronPoll(bool execute) {
    if (!cronEnabled()) return false;

    time_t now = time(nullptr);
    uint32_t epochMinute = (uint32_t)(now / 60);

    if (epochMinute == cronLastMinute) return false;
    if (execute) cronLastMinute = epochMinute;

    struct tm tm;
    localtime_r(&now, &tm);

    bool hasWork = false;

    FILE* f = fopen("/state/crontab", "r");
    if (!f) return false;

    char line[192];
    while (fgets(line, sizeof(line), f)) {
        /* Strip trailing newline */
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';

        /* Skip empty lines and comments */
        const char* p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (!*p || *p == '#') continue;

        const char* cmd = cronMatch(p, &tm);
        if (cmd) {
            hasWork = true;
            if (execute && cronStream) {
                size_t cmdLen = strlen(cmd);
                xStreamBufferSend(cronStream, cmd, cmdLen, pdMS_TO_TICKS(100));
                xStreamBufferSend(cronStream, "\n", 1, pdMS_TO_TICKS(100));
                printf("[cron] %02d:%02d %s\n", tm.tm_hour, tm.tm_min, cmd);
            }
        }
    }
    fclose(f);

    return hasWork;
}

/* ---- Deep sleep ---- */

static void cronDeepSleep() {
    time_t now = time(nullptr);
    int sleepSec = 61 - (int)(now % 60);  /* +1s so we land inside the new minute */
    rtcSetValid();
    int64_t sleepUs = (int64_t)sleepSec * 1000000;
    pmRecordDeepSleep(sleepUs);
    printf("cron: deep sleep %ds\n", sleepSec);
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(50));
    esp_sleep_enable_timer_wakeup((uint64_t)sleepUs);
    esp_deep_sleep_start();
}

bool cronWakeupHandler() {
    if (esp_sleep_get_wakeup_cause() != ESP_SLEEP_WAKEUP_TIMER)
        return false;
    if (!rtcValid())
        return false;

    printf("cron: deep sleep wakeup\n");

    if (!cronPoll(false)) {
        /* No work this minute — go back to sleep */
        cronDeepSleep();
        return false;  /* unreachable */
    }

    /* Work to do — stay awake, cron task will handle it */
    return true;
}

/* ---- Cron task ---- */

static void cronTaskFn(void*) {
    cronUpdateLock();

    storageSubscribeChanges("s.cron", ON_CHANGE { cronUpdateLock(); });
    storageSubscribeChanges("sys.going_down", ON_CHANGE {
        if (atoi(val)) cronDeepSleep();  /* never returns */
    });

    for (;;) {
        while (itsPoll(pdMS_TO_TICKS(30000))) {}
        cronPoll(true);
    }
}

void cronInit() {
    cronStream = xStreamBufferCreate(CRON_STREAM_SIZE, 1);
    pmLockCreate(PM_NO_DEEP_SLEEP, "cron", &cronDeepLock);
    pmLockAcquire(cronDeepLock);
    xTaskCreatePinnedToCoreWithCaps(cronTaskFn, "cron", 4096, nullptr, 1, nullptr, 0, MALLOC_CAP_SPIRAM);
}

/* ---- CLI drain ---- */

void cronDrainCommands() {
    if (!cronStream) return;
    char buf[128];
    size_t pos = 0;
    for (;;) {
        char c;
        size_t n = xStreamBufferReceive(cronStream, &c, 1, 0);
        if (n == 0) break;
        if (c == '\n') {
            buf[pos] = '\0';
            if (pos > 0) cliProcess(buf);
            pos = 0;
        } else if (pos < sizeof(buf) - 1) {
            buf[pos++] = c;
        }
    }
}
