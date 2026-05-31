/**
 * Log — logging macros, log task API, log levels.
 */
#ifndef SECCAM_LOG_H
#define SECCAM_LOG_H

#include <stdint.h>
#include <esp_log.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/** Log task's ITS server ports.
 *    LOG_PORT_TCP: stream-mode, plain bytes for raw TCP `nc` access.
 *    LOG_PORT_DC:  packet-mode, one log line per DataChannel message,
 *                  addressed from the browser as `log:1`. */
static constexpr uint16_t LOG_PORT_TCP = 8080;
static constexpr uint16_t LOG_PORT_DC  = 1;

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

/** True iff the global log level is at debug or finer. */
bool logIsDebug();

/** True iff this tag's effective log level is at debug or finer.
 *  Resolves the per-tag override from `s.log.tag.<tag>` if set;
 *  otherwise falls back to the global level from `s.log.level`.
 *  Useful for short-circuiting expensive work that only feeds dbg()
 *  output for a specific component. */
bool logIsDebug(const char* tag);

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
#define warn(fmt, ...) ESP_LOGW(pcTaskGetName(NULL), fmt, ##__VA_ARGS__)
#define info(fmt, ...) ESP_LOGI(pcTaskGetName(NULL), fmt, ##__VA_ARGS__)
#define dbg(fmt, ...)  ESP_LOGD(pcTaskGetName(NULL), fmt, ##__VA_ARGS__)
#define verb(fmt, ...) ESP_LOGV(pcTaskGetName(NULL), fmt, ##__VA_ARGS__)

#endif
