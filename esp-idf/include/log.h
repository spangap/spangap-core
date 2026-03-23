/**
 * Log — logging macros, log task API, log levels.
 */
#ifndef SECCAM_LOG_H
#define SECCAM_LOG_H

#include <stdint.h>
#include <esp_log.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* ---- Connect payload for log ITS server ---- */

enum log_ansi_t : uint8_t {
    LOG_ANSI,
    LOG_NO_ANSI,
};

typedef struct {
    log_ansi_t ansi;
} log_connect_t;

/* ---- Log level enum ---- */

typedef enum {
  LOG_ERROR,
  LOG_WARN,
  LOG_INFO,
  LOG_DEBUG,
  LOG_ESPDEBUG,   // also enables ESP-IDF internal logging
} log_level_t;

/* ---- Log API ---- */

void logInit();
void logRegisterCmds();
bool logIsDebug();

/** Apply all log levels from cfg store (global + per-tag). Call on boot and level changes. */
void logApplyLevels();

/** Set global log level (updates cfg + applies). */
void logSetGlobal(const char* level);

/** Set per-tag log level (updates cfg + applies). level="-" means inherit from global. */
void logSetTag(const char* tag, const char* level);

/** Returns "{fd} " when log level is debug, "" otherwise.
 *  Use to prefix per-connection log messages. */
const char* cfd(int fd);

/* ---- Log macros — route through ESP-IDF logging with task name as TAG ---- */

#define err(fmt, ...)  ESP_LOGE(pcTaskGetName(NULL), fmt, ##__VA_ARGS__)
#define info(fmt, ...) ESP_LOGI(pcTaskGetName(NULL), fmt, ##__VA_ARGS__)
#define dbg(fmt, ...)  ESP_LOGD(pcTaskGetName(NULL), fmt, ##__VA_ARGS__)

#endif
