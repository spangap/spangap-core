/**
 * cron — minute-resolution task scheduler with deep sleep support.
 *
 * Entries in /state/crontab file (standard unix cron, no user field).
 * Commands sent to CLI via stream buffer for proper serialization.
 * Enabled/disabled via s.cron.enable config. Subscribes via storageSubscribeChanges.
 */
#include "cron.h"
#include "fs.h"
#include "log.h"
#include "storage.h"
#include "its.h"
#include "pm.h"
#include "cli.h"
#include "net.h"
#include "compat.h"
#include "esp_heap_caps.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
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
    int f = fs_open(fsStatePath("/crontab").c_str(), "r");
    if (f < 0) return false;
    char buf[512];
    size_t n = fs_read(buf, 1, sizeof(buf) - 1, f);
    fs_close(f);
    buf[n] = '\0';
    for (const char* p = buf; *p; ) {
        while (*p == ' ' || *p == '\t') p++;
        if (*p && *p != '#' && *p != '\n') return true;
        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;
    }
    return false;
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
        safeStrncpy(base, field, sizeof(base));
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

/* Cron flags: single-letter flags after the 5 time fields, or "-" for none.
 * A = awake only (skip on deep sleep wakeup),
 * N = upstream network required — STA connected to real network. AP-only does not count. Implies A. */
#define CRON_FLAG_AWAKE   0x01   /* A: skip when waking from deep sleep */
#define CRON_FLAG_NETWORK 0x02   /* N: only run when STA connected to upstream (not AP-only) */

static int parseFlags(const char* s) {
    if (s[0] == '-' && (s[1] == ' ' || s[1] == '\0')) return 0;
    int flags = 0;
    for (const char* p = s; *p && *p != ' '; p++) {
        if (*p == 'A' || *p == 'a') flags |= CRON_FLAG_AWAKE;
        else if (*p == 'N' || *p == 'n') flags |= CRON_FLAG_NETWORK | CRON_FLAG_AWAKE;
    }
    return flags;
}

/** Parse a cron line and check if it matches the given time.
 *  Format: min hour dom mon dow flags command
 *  Sets *outFlags to the parsed flags.
 *  Returns pointer to the command string, or nullptr if no match. */
static const char* cronMatch(const char* entry, const struct tm* tm, int* outFlags) {
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
        if (i == 4 && val == 7) val = 0;

        if (!cronMatchField(token, val, fields[i].min, fields[i].max))
            return nullptr;
    }

    /* Parse flags field */
    while (*p == ' ') p++;
    if (!*p) return nullptr;
    *outFlags = parseFlags(p);
    while (*p && *p != ' ') p++;

    /* Skip whitespace to command */
    while (*p == ' ') p++;
    return (*p) ? p : nullptr;
}

/* ---- cronPoll ---- */

static bool isDeepSleepWake = false;

bool cronPoll(bool execute) {
    if (!cronEnabled()) return false;

    time_t now = time(nullptr);
    uint32_t epochMinute = (uint32_t)(now / 60);

    if (epochMinute == cronLastMinute) return false;
    if (execute) cronLastMinute = epochMinute;

    struct tm tm;
    localtime_r(&now, &tm);

    bool netUp = netIsStaConnected();  /* N flag requires real upstream, not AP-only */
    bool hasWork = false;

    struct stat st;
    std::string cronFile = fsStatePath("/crontab");
    if (fs_stat(cronFile.c_str(), &st) != 0 || st.st_size <= 0) return false;
    char* buf = (char*)malloc(st.st_size + 1);
    if (!buf) return false;
    int fh = fs_open(cronFile.c_str(), "r");
    if (fh < 0) { free(buf); return false; }
    size_t nr = fs_read(buf, 1, st.st_size, fh);
    fs_close(fh);
    buf[nr] = '\0';

    /* Parse line by line */
    char* saveptr = nullptr;
    for (char* tok = strtok_r(buf, "\n", &saveptr); tok; tok = strtok_r(nullptr, "\n", &saveptr)) {
        /* Strip trailing CR */
        size_t len = strlen(tok);
        while (len > 0 && tok[len - 1] == '\r') tok[--len] = '\0';

        /* Skip empty lines and comments */
        const char* p = tok;
        while (*p == ' ' || *p == '\t') p++;
        if (!*p || *p == '#') continue;

        int flags = 0;
        const char* cmd = cronMatch(p, &tm, &flags);
        if (!cmd) continue;

        /* Apply flags */
        if ((flags & CRON_FLAG_AWAKE) && isDeepSleepWake) continue;
        if ((flags & CRON_FLAG_NETWORK) && !netUp) continue;

        hasWork = true;
        if (execute && cronStream) {
            size_t cmdLen = strlen(cmd);
            xStreamBufferSend(cronStream, cmd, cmdLen, pdMS_TO_TICKS(100));
            xStreamBufferSend(cronStream, "\n", 1, pdMS_TO_TICKS(100));
            info("%02d:%02d %s\n", tm.tm_hour, tm.tm_min, cmd);
        }
    }
    free(buf);

    return hasWork;
}

/* ---- Deep sleep ---- */

static void cronDeepSleep() {
    time_t now = time(nullptr);
    int sleepSec = 61 - (int)(now % 60);  /* +1s so we land inside the new minute */
    rtcRamSetValid();
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
    if (!rtcRamValid())
        return false;

    isDeepSleepWake = true;
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
    /* Stream buffer allocated in task context so heap tracking attributes it
       to cron, not the main task that spawned us. */
    cronStream = xStreamBufferCreate(CRON_STREAM_SIZE, 1);
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

/* Module config version. Bump when adding/changing defaults. See duckdns.cpp. */
#define CRON_VERSION 1

void cronInit() {
    int v = storageGetInt("s.cron.version", 0);
    if (v < CRON_VERSION) {
        storageDefault("s.cron.enable", 1);
        storageSet("s.cron.version", CRON_VERSION);
    }

    pmLockCreate(PM_NO_DEEP_SLEEP, "cron", &cronDeepLock);
    pmLockAcquire(cronDeepLock);
    spawnTask(cronTaskFn, "cron", 4096, nullptr, 1, 0);
}

/* ---- cronDefault ---- */

/** Extract the command portion of a cron line (after 5 time fields + flags),
 *  stripping leading '#' and whitespace. Returns true if a command was found. */
static bool extractCronCommand(const char* line, std::string& out) {
    const char* p = line;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '#') {
        p++;
        while (*p == ' ' || *p == '\t') p++;
    }
    if (!*p || *p == '\n' || *p == '\r') return false;
    /* Skip 6 whitespace-separated fields: 5 time fields + flags. */
    for (int i = 0; i < 6; i++) {
        while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') p++;
        if (!*p || *p == '\n' || *p == '\r') return false;
        while (*p == ' ' || *p == '\t') p++;
    }
    const char* end = p + strlen(p);
    while (end > p && (end[-1] == ' ' || end[-1] == '\t' ||
                       end[-1] == '\n' || end[-1] == '\r')) end--;
    out.assign(p, end - p);
    return true;
}

bool cronDefault(const char* schedule, const char* command) {
    std::string target = command;
    while (!target.empty() && (target.back() == ' ' || target.back() == '\t'))
        target.pop_back();

    /* Scan existing crontab for any line whose command matches. */
    bool fileExists = false;
    bool needsNewline = false;
    std::string cronFile = fsStatePath("/crontab");
    int fh = fs_open(cronFile.c_str(), "r");
    if (fh >= 0) {
        fileExists = true;
        struct stat st;
        if (fs_stat(cronFile.c_str(), &st) == 0 && st.st_size > 0) {
            char* buf = (char*)malloc(st.st_size + 1);
            if (!buf) { fs_close(fh); return false; }
            size_t nr = fs_read(buf, 1, st.st_size, fh);
            buf[nr] = '\0';
            needsNewline = (nr > 0 && buf[nr - 1] != '\n');
            char* saveptr = nullptr;
            for (char* tok = strtok_r(buf, "\n", &saveptr); tok;
                 tok = strtok_r(nullptr, "\n", &saveptr)) {
                std::string cmd;
                if (!extractCronCommand(tok, cmd)) continue;
                if (cmd == target) { free(buf); fs_close(fh); return false; }
            }
            free(buf);
        }
        fs_close(fh);
    }

    int wh = fs_open(cronFile.c_str(), fileExists ? "a" : "w");
    if (wh < 0) return false;
    if (needsNewline) fs_write("\n", 1, 1, wh);
    /* Format to match the column header in factory_state/crontab — all 4-wide. */
    char fields[6][16] = {};
    int fi = 0;
    const char* p = schedule;
    while (*p && fi < 6) {
        while (*p == ' ' || *p == '\t') p++;
        int j = 0;
        while (*p && *p != ' ' && *p != '\t' && j + 1 < (int)sizeof(fields[fi]))
            fields[fi][j++] = *p++;
        fields[fi][j] = '\0';
        fi++;
    }
    char line[256];
    int n = snprintf(line, sizeof(line), "%-4s %-4s %-4s %-4s %-4s %-4s %s\n",
                     fields[0], fields[1], fields[2], fields[3], fields[4], fields[5], command);
    if (n > 0) fs_write(line, 1, (size_t)n, wh);
    fs_close(wh);
    return true;
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
            if (pos > 0) {
                info("cli: %s\n", buf);
                cliProcess(buf);
            }
            pos = 0;
        } else if (pos < sizeof(buf) - 1) {
            buf[pos++] = c;
        }
    }
}
