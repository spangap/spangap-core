#pragma once
#include "esp_timer.h"
#include "esp_sleep.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "pm.h"
#include <sys/time.h>
#include <sys/stat.h>
#include <time.h>
#include <cstring>
#include <cerrno>

/** Central RTC RAM validity — one magic word for all application RTC state.
 *  Set before deep sleep, checked on wakeup. If invalid, all RTC vars are stale. */
bool rtcRamValid();
void rtcRamSetValid();


/** strncpy with truncation warning. Always NUL-terminates. */
static inline char* safeStrncpy(char* dst, const char* src, size_t n) {
    if (strlen(src) >= n)
        ESP_LOGE("strncpy", "truncated: '%s' to %u chars", src, (unsigned)(n - 1));
    strncpy(dst, src, n - 1);
    dst[n - 1] = '\0';
    return dst;
}


static inline uint32_t millis() {
    return static_cast<uint32_t>(esp_timer_get_time() / 1000);
}

static inline void delay(uint32_t ms) {
    if (!ms) { vTaskDelay(0); return; }   /* bare yield: nothing to do */
    /* A delay is a timeout, not an event — but it shouldn't change whether we're
     * handling one. Drop the auto boost to the floor while we sleep (save power),
     * then restore it if we held it, so mid-event work resumes at speed. Manual
     * pmBoost() holds are untouched and stay up across the delay. */
    bool was = pmBoostHeld();
    pmBoostAuto(false);
    vTaskDelay(pdMS_TO_TICKS(ms));
    if (was) pmBoostAuto(true);
}

/** Task stack location. Default PSRAM — use DRAM only when the task runs
 *  SPI-flash code paths (direct fopen/fread on LittleFS), which disable
 *  the PSRAM cache for the duration of the flash op. */
enum stack_caps_t { STACK_PSRAM, STACK_DRAM };

/** Spawn a task with heap-capability-backed TCB + stack. Wraps
 *  xTaskCreatePinnedToCoreWithCaps; must be paired with killSelf()
 *  (or vTaskDeleteWithCaps) at teardown. Returns NULL on failure.
 *
 *  No boost flag: CPU boost is automatic and notify-driven (itsPoll boosts the
 *  handling of event-wakes, stays at the floor for timeout ticks). A task that
 *  needs sustained 240 MHz calls pmBoost()/pmBoostEnd() itself. See pm-task-boost.md. */
static inline TaskHandle_t spawnTask(TaskFunction_t fn, const char* name,
                                      uint32_t stackBytes, void* arg,
                                      UBaseType_t prio, BaseType_t core,
                                      stack_caps_t stackMem = STACK_PSRAM) {
    TaskHandle_t h = nullptr;
    uint32_t caps = (stackMem == STACK_DRAM) ? MALLOC_CAP_INTERNAL : MALLOC_CAP_SPIRAM;
    BaseType_t r = xTaskCreatePinnedToCoreWithCaps(fn, name, stackBytes, arg,
                                                   prio, &h, core, caps);
    return (r == pdPASS) ? h : nullptr;
}

/** End the current task and free its TCB + stack. Project convention:
 *  all tasks are created via spawnTask() → xTaskCreatePinnedToCoreWithCaps,
 *  so self-deletes always need the -WithCaps cleanup path (idle task
 *  won't free the heap-allocated buffers on its own). */
[[noreturn]] static inline void killSelf() {
    pmBoostAuto(false);            /* release any auto boost count this task holds before it vanishes */
    vTaskDeleteWithCaps(nullptr);
    /* unreachable: vTaskDeleteWithCaps(nullptr) deletes the current task */
    while (1) { vTaskDelay(portMAX_DELAY); }
}

/** UTC offset in minutes from epoch seconds (compares localtime vs gmtime). */
static inline int16_t utcOffsetMinutes(time_t t) {
    struct tm lt, gt;
    localtime_r(&t, &lt);
    gmtime_r(&t, &gt);
    int diff = (lt.tm_hour - gt.tm_hour) * 60 + (lt.tm_min - gt.tm_min);
    int dday = lt.tm_mday - gt.tm_mday;
    if (dday > 1) dday = -1;   /* month wrap */
    if (dday < -1) dday = 1;
    diff += dday * 24 * 60;
    return (int16_t)diff;
}

/** Convert fps config setting to interval in ms.
 *  x > 0: x fps → 1000/x ms.  x < 0: 1/(-x) fps → -x*1000 ms.  0 → 0 (disabled). */
static inline uint32_t fpsToIntervalMs(int fps) {
    if (fps == 0) return 0;
    return fps > 0 ? (uint32_t)(1000 / fps) : (uint32_t)(-fps) * 1000;
}

/** Format wall clock: "Mar 27 16:23:15.342". Returns buf. */
static inline const char* fmtWallClock(char* buf, size_t len) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm tm;
    localtime_r(&tv.tv_sec, &tm);
    int n = strftime(buf, len, "%b %d %H:%M:%S", &tm);
    snprintf(buf + n, len - n, ".%03d", (int)(tv.tv_usec / 1000));
    return buf;
}

/** Format elapsed seconds as "3s", "1m22s", "2h3m", "3d5h". Zero components omitted. */
static inline const char* fmtElapsed(uint32_t secs, char* buf, size_t len) {
    unsigned d = secs / 86400, h = (secs % 86400) / 3600, m = (secs % 3600) / 60, s = secs % 60;
    char* p = buf;
    char* end = buf + len;
    if (d) p += snprintf(p, end - p, "%ud", d);
    if (h) p += snprintf(p, end - p, "%uh", h);
    if (m) p += snprintf(p, end - p, "%um", m);
    if (s || p == buf) snprintf(p, end - p, "%us", s);
    return buf;
}

/** Format byte count with 3 significant digits: "123B", "1.23kB", "45.6MB", "1.23GB". */
static inline const char* fmtSize(uint32_t bytes, char* buf, size_t len) {
    if (bytes < 1000)
        snprintf(buf, len, "%uB", (unsigned)bytes);
    else if (bytes < 10000)   snprintf(buf, len, "%.2fkB", bytes / 1024.0);
    else if (bytes < 100000)  snprintf(buf, len, "%.1fkB", bytes / 1024.0);
    else if (bytes < 1000000) snprintf(buf, len, "%.0fkB", bytes / 1024.0);
    else if (bytes < 10000000)  snprintf(buf, len, "%.2fMB", bytes / (1024.0 * 1024));
    else if (bytes < 100000000) snprintf(buf, len, "%.1fMB", bytes / (1024.0 * 1024));
    else if (bytes < 1000000000) snprintf(buf, len, "%.0fMB", bytes / (1024.0 * 1024));
    else snprintf(buf, len, "%.2fGB", bytes / (1024.0 * 1024 * 1024));
    return buf;
}

/** Format bandwidth with 3 significant digits: "123bps", "1.23kbps", "45.6Mbps". */
static inline const char* fmtBps(uint32_t bps, char* buf, size_t len) {
    if (bps < 1000)
        snprintf(buf, len, "%ubps", (unsigned)bps);
    else if (bps < 10000)   snprintf(buf, len, "%.2fkbps", bps / 1000.0);
    else if (bps < 100000)  snprintf(buf, len, "%.1fkbps", bps / 1000.0);
    else if (bps < 1000000) snprintf(buf, len, "%.0fkbps", bps / 1000.0);
    else if (bps < 10000000)  snprintf(buf, len, "%.2fMbps", bps / 1000000.0);
    else if (bps < 100000000) snprintf(buf, len, "%.1fMbps", bps / 1000000.0);
    else snprintf(buf, len, "%.0fMbps", bps / 1000000.0);
    return buf;
}
