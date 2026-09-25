/**
 * Log — logging macros, log task API, log levels.
 */
#ifndef SPANGAP_LOG_H
#define SPANGAP_LOG_H

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

/** True iff this tag's effective log level is at verbose. Same resolution as
 *  logIsDebug, one level finer — for a trace that would otherwise drown the
 *  decisions a component logs at debug. */
bool logIsVerbose(const char* tag);

/** Apply all log levels from cfg store (global + per-tag). Call on boot and level changes. */
void logApplyLevels();

/** Set global log level (updates cfg + applies). */
void logSetGlobal(const char* level);

/** Set per-tag log level (updates cfg + applies). level="-" means inherit from global. */
void logSetTag(const char* tag, const char* level);

/** A component's own default level for a tag it knows to be chatty — below the
 *  global level, above nothing an operator sets: `log <tag> <level>` still wins,
 *  and `log <tag> -` comes back here rather than to the global level. Callable
 *  from a constructor, before logging is up; up to 16 tags across the image. */
void logTagDefault(const char* tag, const char* level);

/** Rewrite one noisy library line, matched by what it starts with. A `prefix`
 *  without a colon is compared against the start of the message body (the text
 *  after the tag), so a few characters settle it and nothing else that tag
 *  emits is touched. A prefix WITH a colon is read as "tag: body-prefix" — the
 *  tag must match exactly and the (possibly empty) remainder matches the body,
 *  so "NimBLE: " covers every tagged line of a chatty library whose body
 *  prefixes differ per line. `level`
 *  is the level letter to re-emit it at — 'E'/'W'/'I'/'D'/'V', or 'N' to drop the
 *  line entirely. A demotion is re-filtered against that tag's own threshold, so
 *  demoting below the level in force drops it too.
 *
 *  For a library line whose severity is wrong for this device, or that
 *  duplicates what the owning module reports itself. Prefer it to a per-tag
 *  level: it takes out the one line and leaves everything else that tag says at
 *  whatever the settings ask for. `prefix` must outlive the process (use a
 *  literal). Bounded set; a full one warns and no-ops. */
void logRule(const char* prefix, char level);

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
