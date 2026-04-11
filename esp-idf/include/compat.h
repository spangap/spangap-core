#pragma once
#include "esp_timer.h"
#include "esp_sleep.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
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

/** Create directory and all parent components (like mkdir -p). */
static inline void mkdirp(const char* path) {
    char tmp[128];
    safeStrncpy(tmp, path, sizeof(tmp));
    for (char* p = tmp + 1; *p; p++) {
        if (*p == '/') { *p = '\0'; mkdir(tmp, 0755); *p = '/'; }
    }
    mkdir(tmp, 0755);
}

static inline uint32_t millis() {
    return static_cast<uint32_t>(esp_timer_get_time() / 1000);
}

static inline void delay(uint32_t ms) {
    vTaskDelay(pdMS_TO_TICKS(ms));
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
