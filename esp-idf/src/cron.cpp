/**
 * cron — minute-resolution task scheduler with deep sleep support.
 *
 * Entries are storage keys: s.cron.tab.<name>, one standard unix cron line
 * each (no user field). Commands sent to CLI via stream buffer for proper
 * serialization. Enabled/disabled via s.cron.enable config.
 *
 * Next-minute scheduling: cronReschedule() computes the first minute any
 * entry must run and stores it in RTC RAM (cronNextMinute); cronPoll() is a
 * cheap compare until that minute arrives, then runs due entries and
 * recomputes. On hardware with a working RTC chip this collapses into
 * "program RTC alarm for cronWakeMinute, wake on the int pin, poll" — the
 * 30s poll and the deep-sleep timer iteration are both stand-ins for it.
 */
#include "cron.h"
#include "spangap.h"
#include "fs.h"
#include "log.h"
#include "storage.h"
#include "its.h"
#include "pm.h"
#include "cli.h"
#include "compat.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "esp_sleep.h"

/* ---- RTC state (survives deep sleep and soft resets; zeroed on power-on) ---- */

RTC_DATA_ATTR static uint32_t cronLastMinute = 0;  /* last epoch minute serviced */
RTC_DATA_ATTR static uint32_t cronNextMinute = 0;  /* first minute any entry runs; 0 = stale, recompute */
RTC_DATA_ATTR static uint32_t cronWakeMinute = 0;  /* first minute a non-A entry runs (deep-sleep wake target) */

/* ---- Command stream → CLI task ---- */

#define CRON_STREAM_SIZE 256
static StreamBufferHandle_t cronStream = nullptr;
static pm_lock_handle_t cronDeepLock = nullptr;

/* Serializes entry collection and the RTC minute bookkeeping: cronPoll runs on
 * the cron task every ~30s and once on app_main at the end of boot. */
static SemaphoreHandle_t cronMutex = nullptr;

static bool cronEnabled() {
    return storageGetInt("s.cron.enable") != 0;
}

/* ---- Entry collection (s.cron.tab.*) ---- */

#define CRON_TAB_PREFIX "s.cron.tab."

struct cron_tab_t { std::string name, line; };
static std::vector<cron_tab_t> s_entries;   /* scratch, guarded by cronMutex */

static void cronCollectCb(const char* key, const char* val) {
    const char* name = key;
    if (strncmp(key, CRON_TAB_PREFIX, sizeof(CRON_TAB_PREFIX) - 1) == 0)
        name = key + sizeof(CRON_TAB_PREFIX) - 1;
    s_entries.push_back({ name, val ? val : "" });
}

static void cronCollect() {
    s_entries.clear();
    storageForEach("s.cron.tab", cronCollectCb);
}

/** Update deep sleep lock: held unless cron is enabled with entries. */
static void cronUpdateLock() {
    bool allow = cronEnabled() && !s_entries.empty();
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
 * A = awake only (skip on deep-sleep wakeup),
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

/* One tokenized entry: min hour dom mon dow flags command. */
struct cron_parsed_t {
    char field[5][32];
    int flags;
    const char* cmd;   /* points into the source line */
};

static bool cronParse(const char* entry, cron_parsed_t* out) {
    const char* p = entry;
    for (int i = 0; i < 5; i++) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) return false;
        const char* start = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        size_t len = p - start;
        if (len >= sizeof(out->field[i])) return false;
        memcpy(out->field[i], start, len);
        out->field[i][len] = '\0';
    }
    while (*p == ' ' || *p == '\t') p++;
    if (!*p) return false;
    out->flags = parseFlags(p);
    while (*p && *p != ' ' && *p != '\t') p++;
    while (*p == ' ' || *p == '\t') p++;
    if (!*p) return false;
    out->cmd = p;
    return true;
}

/** Earliest epoch minute >= fromMinute at which the entry's five time fields
 *  match (local time), or UINT32_MAX if none within ~400 days (malformed or
 *  impossible entry). Steps day-first, so a sparse schedule costs hundreds of
 *  field checks, not hundreds of thousands. All five fields must match, dow
 *  included (same AND semantics as the due check in cronPoll). */
static uint32_t cronNextRun(const cron_parsed_t& e, uint32_t fromMinute) {
    time_t t = (time_t)fromMinute * 60;
    struct tm tm;
    localtime_r(&t, &tm);
    tm.tm_sec = 0;
    uint32_t horizon = fromMinute + 400u * 24 * 60;
    for (int guard = 0; guard < 1000; guard++) {
        if ((uint32_t)(t / 60) > horizon) return UINT32_MAX;
        /* tm_wday is 0-6; a Sunday also matches a "7" in the dow field. */
        bool dowOk = cronMatchField(e.field[4], tm.tm_wday, 0, 7) ||
                     (tm.tm_wday == 0 && cronMatchField(e.field[4], 7, 0, 7));
        if (!cronMatchField(e.field[3], tm.tm_mon + 1, 1, 12) ||
            !cronMatchField(e.field[2], tm.tm_mday, 1, 31) ||
            !dowOk) {
            tm.tm_mday++; tm.tm_hour = 0; tm.tm_min = 0;
        } else if (!cronMatchField(e.field[1], tm.tm_hour, 0, 23)) {
            tm.tm_hour++; tm.tm_min = 0;
        } else if (!cronMatchField(e.field[0], tm.tm_min, 0, 59)) {
            tm.tm_min++;
        } else {
            return (uint32_t)(t / 60);
        }
        tm.tm_isdst = -1;      /* let mktime decide across DST edges */
        t = mktime(&tm);       /* normalizes tm in place */
        if (t == (time_t)-1) return UINT32_MAX;
    }
    return UINT32_MAX;
}

/** Recompute the stored minutes from the already-collected entries.
 *  cronNextMinute drives the awake poll; cronWakeMinute is the deep-sleep wake
 *  target and skips A-flagged entries (they are suppressed on a deep-sleep
 *  wake anyway, so waking for one is pure waste). Caller holds cronMutex. */
static void cronComputeNextLocked(uint32_t fromMinute) {
    uint32_t next = UINT32_MAX, wake = UINT32_MAX;
    for (auto& e : s_entries) {
        cron_parsed_t p;
        if (!cronParse(e.line.c_str(), &p)) {
            warn("cron: bad entry %s: %s\n", e.name.c_str(), e.line.c_str());
            continue;
        }
        uint32_t n = cronNextRun(p, fromMinute);
        if (n < next) next = n;
        if (!(p.flags & CRON_FLAG_AWAKE) && n < wake) wake = n;
    }
    cronNextMinute = next;
    cronWakeMinute = wake;
}

/* Sane means "within a day of now": cold boot zeroes RTC RAM, NTP's first sync
 * jumps the clock out of 1970, and a manual clock set can go either way. In
 * all of those the due window (cronLastMinute, now] is meaningless — re-anchor
 * and schedule forward instead of catching up. */
static bool cronAnchorSane(uint32_t nowMin) {
    return cronLastMinute != 0 && nowMin >= cronLastMinute &&
           nowMin - cronLastMinute <= 24 * 60;
}

void cronReschedule() {
    if (!cronMutex) return;
    xSemaphoreTake(cronMutex, portMAX_DELAY);
    uint32_t nowMin = (uint32_t)(time(nullptr) / 60);
    if (!cronAnchorSane(nowMin)) cronLastMinute = nowMin;
    cronCollect();
    cronUpdateLock();
    cronComputeNextLocked(cronLastMinute + 1);
    xSemaphoreGive(cronMutex);
}

/* ---- cronPoll ---- */

static bool isDeepSleepWake = false;

void cronPoll() {
    if (!cronMutex || !cronEnabled()) return;
    xSemaphoreTake(cronMutex, portMAX_DELAY);
    time_t now = time(nullptr);
    uint32_t nowMin = (uint32_t)(now / 60);

    if (!cronAnchorSane(nowMin)) {
        cronLastMinute = nowMin;
        cronCollect();
        cronComputeNextLocked(nowMin + 1);
        xSemaphoreGive(cronMutex);
        return;
    }

    if (nowMin < cronNextMinute) {
        xSemaphoreGive(cronMutex);
        return;
    }

    /* N flag requires real upstream, not AP-only. Read it off the storage
     * state bus (net publishes "wifi.sta.state") rather than calling into net
     * — core has no net dependency. No net staged → key absent → never up, so
     * N-flagged jobs simply don't run. */
    char netState[16] = {};
    storageGetStr("wifi.sta.state", netState, sizeof(netState), "");
    bool netUp = strcmp(netState, "connected") == 0;

    struct tm tm;
    localtime_r(&now, &tm);

    cronCollect();
    for (auto& e : s_entries) {
        cron_parsed_t p;
        if (!cronParse(e.line.c_str(), &p)) continue;   /* warned in reschedule */

        /* Due = would have fired in (cronLastMinute, now]. Checking the window
         * rather than "matches this exact minute" means a poll that lands late
         * — sleep-timer skew, a busy CLI — still fires the slot once. */
        if (cronNextRun(p, cronLastMinute + 1) > nowMin) continue;

        if ((p.flags & CRON_FLAG_AWAKE) && isDeepSleepWake) continue;
        if ((p.flags & CRON_FLAG_NETWORK) && !netUp) continue;

        if (cronStream) {
            xStreamBufferSend(cronStream, p.cmd, strlen(p.cmd), pdMS_TO_TICKS(100));
            xStreamBufferSend(cronStream, "\n", 1, pdMS_TO_TICKS(100));
            cliWake();   /* the stream buffer carries no notify; wake the CLI task to drain it */
            info("%02d:%02d %s\n", tm.tm_hour, tm.tm_min, p.cmd);
        }
    }
    cronLastMinute = nowMin;
    cronComputeNextLocked(nowMin + 1);
    xSemaphoreGive(cronMutex);
}

/* ---- Deep sleep ---- */

/* Deep sleep is not supported at the moment — every entry path is commented
 * out: pmLockRelease() no longer raises sys.going_down (pm.cpp), the
 * subscription below is disabled, and cronWakeupHandler() no longer re-sleeps
 * on an early wake. */
#if 0
/* Sleep toward cronWakeMinute with skew margin. Sleep timers run off an RC
 * oscillator that can be off by single-digit percent, so a single full-length
 * sleep could overshoot the minute. Each hop sleeps 85% of the remaining time
 * — we can only ever land short — and the wake handler hops again until the
 * minute arrives; once ≤5s remain the full remainder is slept (worst-case
 * skew there is tens of ms, still inside the minute thanks to the +1s).
 * Converges in a handful of hops (remaining shrinks ~7x per hop). */
static void cronDeepSleep() {
    if (cronWakeMinute == UINT32_MAX) return;  /* no wakeable entry — pm lock should be held */
    time_t target = (time_t)cronWakeMinute * 60 + 1;  /* +1s lands inside the minute */
    int64_t remain = (int64_t)(target - time(nullptr));
    if (remain < 1) remain = 1;
    int64_t sleepSec = remain > 5 ? (remain * 85) / 100 : remain;
    rtcRamSetValid();
    int64_t sleepUs = sleepSec * 1000000;
    pmRecordDeepSleep(sleepUs);
    printf("cron: deep sleep %llds (%llds to minute)\n",
           (long long)sleepSec, (long long)remain);
    fflush(stdout);
    delay(50);
    esp_sleep_enable_timer_wakeup((uint64_t)sleepUs);
    esp_deep_sleep_start();
}
#endif

bool cronWakeupHandler() {
    if (esp_sleep_get_wakeup_cause() != ESP_SLEEP_WAKEUP_TIMER)
        return false;
    if (!rtcRamValid())
        return false;

    isDeepSleepWake = true;
    printf("cron: deep sleep wakeup\n");

    /* Deep sleep is not supported at the moment — never re-enter it on an
     * early (skew-margin) wake; finish booting instead. */
    // if ((uint32_t)(time(nullptr) / 60) < cronWakeMinute) {
    //     cronDeepSleep();   /* hop again toward the minute; never returns */
    // }

    /* The minute has arrived — stay awake, cron task will service it */
    return true;
}

/* ---- Cron task ---- */

static void cronTaskFn(void*) {
    /* Stream buffer allocated in task context so heap tracking attributes it
       to cron, not the main task that spawned us. */
    cronStream = xStreamBufferCreate(CRON_STREAM_SIZE, 1);
    cronReschedule();

    storageSubscribeChanges("s.cron", ON_CHANGE { cronReschedule(); });
    /* Safe-mode flag watcher. It has to live on a task that outlives boot (a
     * storage subscription is delivered to its registering task), and this one
     * is core's long-lived housekeeping actor — the same reason the deep-sleep
     * watcher below was written here. */
    spangapWatchSafeModeFlags();
    /* Deep sleep is not supported at the moment (see cronDeepSleep above) —
     * and going_down must not sleep the device even if set by hand. */
    // storageSubscribeChanges("sys.going_down", ON_CHANGE {
    //     if (atoi(val)) cronDeepSleep();  /* never returns */
    // });

    for (;;) {
        while (itsPoll(pdMS_TO_TICKS(30000))) {}
        cronPoll();
    }
}

/* Module config version. Bump when adding/changing defaults that must install
 * on existing devices (storageDefault alone is set-if-absent, so brand-new
 * keys need no bump — the gate is for re-installing something a user may have
 * changed or removed). */
#define CRON_VERSION 1

void cronInit() {
    /* cron registers in the safe band (it is core's), but a recovery boot must
     * not fire scheduled commands: they assume straddles that aren't running,
     * and one of them may be the very thing being restored away. Nothing else
     * gates on cron, so simply not coming up is the whole skip. */
    if (spangapSafeMode() != SAFE_MODE_NONE) {
        info("cron: not started in safe mode\n");
        return;
    }

    int v = storageGetInt("s.cron.version", 0);
    if (v < CRON_VERSION) {
        storageBegin();
        storageDefault("s.cron.enable", 1);
        storageSet("s.cron.version", CRON_VERSION);
        storageEnd();
    }

    /* Entries live in s.cron.tab.* — remove a crontab file left behind by a
     * firmware that stored them on disk (owners recreate their entries). */
    fs_remove(fsStatePath("/crontab").c_str());

    cronMutex = xSemaphoreCreateMutex();
    pmLockCreate(PM_NO_DEEP_SLEEP, "cron", &cronDeepLock);
    pmLockAcquire(cronDeepLock);
    spawnTask(cronTaskFn, "cron", 4096, nullptr, 1, 0);
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
