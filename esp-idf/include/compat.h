#pragma once
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static inline uint32_t millis() {
    return static_cast<uint32_t>(esp_timer_get_time() / 1000);
}

static inline void delay(uint32_t ms) {
    vTaskDelay(pdMS_TO_TICKS(ms));
}

/** Convert fps NVS setting to interval in ms.
 *  x > 0: x fps → 1000/x ms.  x < 0: 1/(-x) fps → -x*1000 ms.  0 → 0 (disabled). */
static inline uint32_t fpsToIntervalMs(int fps) {
    if (fps == 0) return 0;
    return fps > 0 ? (uint32_t)(1000 / fps) : (uint32_t)(-fps) * 1000;
}
