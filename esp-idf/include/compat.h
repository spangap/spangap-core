#pragma once
#include "esp_timer.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/** Central RTC RAM validity — one magic word for all application RTC state.
 *  Set before deep sleep, checked on wakeup. If invalid, all RTC vars are stale. */
bool rtcValid();
void rtcSetValid();

static inline uint32_t millis() {
    return static_cast<uint32_t>(esp_timer_get_time() / 1000);
}

static inline void delay(uint32_t ms) {
    vTaskDelay(pdMS_TO_TICKS(ms));
}

/** Convert fps config setting to interval in ms.
 *  x > 0: x fps → 1000/x ms.  x < 0: 1/(-x) fps → -x*1000 ms.  0 → 0 (disabled). */
static inline uint32_t fpsToIntervalMs(int fps) {
    if (fps == 0) return 0;
    return fps > 0 ? (uint32_t)(1000 / fps) : (uint32_t)(-fps) * 1000;
}

/** Format elapsed seconds as "3s", "1m22s", "2h3m7s", "3d5h12m7s". */
static inline const char* fmtElapsed(uint32_t secs, char* buf, size_t len) {
    unsigned d = secs / 86400, h = (secs % 86400) / 3600, m = (secs % 3600) / 60, s = secs % 60;
    if (d > 0)      snprintf(buf, len, "%ud%uh%um%us", d, h, m, s);
    else if (h > 0) snprintf(buf, len, "%uh%um%us", h, m, s);
    else if (m > 0) snprintf(buf, len, "%um%us", m, s);
    else            snprintf(buf, len, "%us", s);
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
