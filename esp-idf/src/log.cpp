/**
 * Log — log task, ESP-IDF vprintf hook, log reformatting, log levels.
 * Split from ipc.cpp. CLI command: log.
 */
#include "log.h"
#include "pm.h"
#include "cli.h"
#include "its.h"
#include "web.h"
#include "net.h"
#include "storage.h"
#include "compat.h"
#include "freertos/stream_buffer.h"
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <cstdio>
#include <cstring>

/* ---- Log input stream ---- */

static bool logInited = false;

#define LOG_INPUT_SIZE 2048
static StreamBufferHandle_t logInputStream = NULL;  /* vprintf callback writes here */

/* ---- Log ITS server — consumers connect as clients ---- */

#define LOG_MAX_CONSUMERS 4
static log_ansi_t logConsumerAnsi[LOG_MAX_CONSUMERS];  /* per-handle ansi pref */
static bool logConsumerWs[LOG_MAX_CONSUMERS];           /* per-handle WS flag */

/* Strip ANSI escape codes from a buffer in-place, return new length */
static size_t stripAnsi(char* buf, size_t len) {
    size_t out = 0;
    for (size_t i = 0; i < len; ) {
        if (buf[i] == '\033' && i + 1 < len && buf[i + 1] == '[') {
            i += 2;
            while (i < len && buf[i] != 'm') i++;
            if (i < len) i++;  /* skip 'm' */
        } else {
            buf[out++] = buf[i++];
        }
    }
    return out;
}

/* Reformat ESP-IDF log line: "I (12345) tag: msg" → "[taskname] I tag: msg"
 * Input may have ANSI color prefix/suffix. We strip those and add our own. */
static int logReformat(const char* src, char* dst, size_t dstSize, bool ansi) {
    const char* p = src;
    /* Strip all leading ANSI escape sequences (CSI: ESC [ params m) */
    while (*p == '\033' && *(p + 1) == '[') {
        p += 2;  /* skip ESC [ */
        while ((*p >= '0' && *p <= '9') || *p == ';') p++;  /* skip params */
        if (*p == 'm') p++;  /* skip terminator */
    }

    /* Validate ESP log format: must be "L (timestamp) tag: msg" */
    char level = *p;
    const char* afterL = p + 1;
    while (*afterL == ' ') afterL++;
    bool validLevel = (level == 'E' || level == 'W' || level == 'I' || level == 'D' || level == 'V')
                      && *afterL == '(';  /* must have timestamp */

    const char* taskName = pcTaskGetName(NULL);
    if (!taskName) taskName = "?";

    static char lastLevel = 'I';  /* remember level for continuation lines */

    if (!validLevel) {
        /* Continuation line (ESP wifi splits messages) — use last seen level */
        size_t msgLen = strlen(p);
        while (msgLen > 0 && (p[msgLen - 1] == '\n' || p[msgLen - 1] == '\r')) msgLen--;
        if (msgLen > 4 && p[msgLen - 1] == 'm' && p[msgLen - 4] == '\033') msgLen -= 4;
        /* Skip empty/whitespace-only continuation lines */
        { const char* ck = p; size_t cl = msgLen;
          while (cl > 0 && (*ck == ' ' || *ck == '\t')) { ck++; cl--; }
          if (cl == 0) return 0;
        }
        const char* color = "";
        const char* reset = "";
        if (ansi) {
            switch (lastLevel) {
                case 'E': color = "\033[0;31m"; break;
                case 'W': color = "\033[0;33m"; break;
                case 'I': color = "\033[0;32m"; break;
                default: break;
            }
            if (*color) reset = "\033[0m";
        }
        return snprintf(dst, dstSize, "%s%c [%s] %.*s%s\n",
            color, lastLevel, taskName, (int)msgLen, p, reset);
    }

    lastLevel = level;

    /* Timestamp: keep or skip "(12345) " based on s.log.timestamp */
    const char* afterLevel = p + 1;
    while (*afterLevel == ' ') afterLevel++;
    const char* tsStart = nullptr;
    int tsLen = 0;
    if (*afterLevel == '(') {
        tsStart = afterLevel;
        const char* tsEnd = strchr(afterLevel, ')');
        if (tsEnd) {
            tsLen = (int)(tsEnd - tsStart + 1);
            afterLevel = tsEnd + 1;
        } else {
            afterLevel = p + 1;
        }
        while (*afterLevel == ' ') afterLevel++;
    }
    bool showTimestamp = storageGetInt("s.log.timestamp", 0) != 0;

    /* Extract TAG (everything up to ": " or ":\033" or ":\n") */
    const char* tagStart = afterLevel;
    const char* colon = strchr(afterLevel, ':');
    const char* msgStart = afterLevel;
    char tag[32] = {};
    if (colon && (colon - tagStart) < (int)sizeof(tag) && (colon - tagStart) > 0) {
        memcpy(tag, tagStart, colon - tagStart);
        tag[colon - tagStart] = '\0';
        msgStart = colon + 1;
        if (*msgStart == ' ') msgStart++;  /* skip optional space after colon */
    }

    /* Strip trailing ANSI reset + newlines */
    size_t msgLen = strlen(msgStart);
    while (msgLen > 0 && (msgStart[msgLen - 1] == '\n' || msgStart[msgLen - 1] == '\r'))
        msgLen--;
    /* Strip trailing ANSI reset (\033[0m or \033[0;xxm etc) */
    while (msgLen > 0) {
        if (msgStart[msgLen - 1] != 'm') break;
        /* Scan backwards for ESC */
        size_t i = msgLen - 2;
        while (i > 0 && ((msgStart[i] >= '0' && msgStart[i] <= '9') || msgStart[i] == ';' || msgStart[i] == '[')) i--;
        if (msgStart[i] == '\033') { msgLen = i; } else break;
    }
    /* Skip empty/whitespace-only messages (ESP wifi emits "I (ts) wifi: \n" then content on next line) */
    while (msgLen > 0 && (msgStart[0] == ' ' || msgStart[0] == '\t')) { msgStart++; msgLen--; }
    if (msgLen == 0) return 0;

    /* ANSI color by level */
    const char* color = "";
    const char* reset = "";
    if (ansi) {
        switch (level) {
            case 'E': color = "\033[0;31m"; break;  /* red */
            case 'W': color = "\033[0;33m"; break;  /* yellow */
            case 'I': color = "\033[0;32m"; break;  /* green */
            default: break;
        }
        if (*color) reset = "\033[0m";
    }

    /* Format: "L (ts) [task] msg" or "L [task] msg" */
    char tsBuf[24] = "";
    if (showTimestamp && tsStart && tsLen > 0)
        snprintf(tsBuf, sizeof(tsBuf), " %.*s", tsLen, tsStart);

    /* Suppress TAG if it matches the task name */
    if (tag[0] && strcmp(tag, taskName) == 0) {
        return snprintf(dst, dstSize, "%s%c%s [%s] %.*s%s\n",
            color, level, tsBuf, taskName, (int)msgLen, msgStart, reset);
    } else if (tag[0]) {
        return snprintf(dst, dstSize, "%s%c%s [%s] %s: %.*s%s\n",
            color, level, tsBuf, taskName, tag, (int)msgLen, msgStart, reset);
    } else {
        return snprintf(dst, dstSize, "%s%c%s [%s] %.*s%s\n",
            color, level, tsBuf, taskName, (int)msgLen, msgStart, reset);
    }
}

static TaskHandle_t logTaskHandle = NULL;

/* vprintf callback — called by ESP-IDF log system from any task */
static int logVprintf(const char* fmt, va_list args) {
    if (!logInputStream) return 0;

    char buf[256];
    int rawLen = vsnprintf(buf, sizeof(buf), fmt, args);
    if (rawLen <= 0) return 0;

    /* Reformat with task name (ANSI version for the input stream — consumers strip if needed) */
    char formatted[288];
    int fmtLen = logReformat(buf, formatted, sizeof(formatted), true);
    if (fmtLen <= 0) return rawLen;

    /* Write to input stream (non-blocking — drop if full) */
    xStreamBufferSend(logInputStream, formatted, fmtLen, 0);

    /* Wake log task */
    if (logTaskHandle) xTaskNotifyGive(logTaskHandle);

    return rawLen;
}

/* ---- Log ITS server callbacks ---- */

static bool logOnConnect(int handle, int itsPort, const void* data, size_t len) {
  logConsumerWs[handle] = false;
  if (len >= sizeof(net_connect_t) && ((const net_connect_t*)data)->ws) {
    if (!wsUpgrade(handle)) return false;
    logConsumerWs[handle] = true;
    logConsumerAnsi[handle] = LOG_NO_ANSI;
  } else if (len >= sizeof(log_connect_t)) {
    auto* req = (const log_connect_t*)data;
    logConsumerAnsi[handle] = req->ansi;
  } else {
    logConsumerAnsi[handle] = LOG_NO_ANSI;
  }
  return true;
}

static void logOnDisconnect(int handle) {
  (void)handle;
}

/* ---- Log task: drains input stream → fan out to ITS consumers ---- */

static void logTaskFn(void* arg) {
  itsServerInit(LOG_MAX_CONSUMERS, 0, 2048);
  itsServerOnConnect(logOnConnect);
  itsServerOnDisconnect(logOnDisconnect);

  /* Register TCP port with network */
  { net_port_msg_t reg = {};
    reg.itsPort = 8080;  /* convention for log */
    strncpy(reg.nvsKey, "log_port", sizeof(reg.nvsKey));
    while (!itsSendAux("net", &reg, sizeof(reg), pdMS_TO_TICKS(500)))
      vTaskDelay(pdMS_TO_TICKS(100));
  }
  /* Register WS path with web */
  { web_path_msg_t reg = {};
    reg.itsPort = 8080;
    strncpy(reg.path, "log", sizeof(reg.path));
    while (!itsSendAux("web", &reg, sizeof(reg), pdMS_TO_TICKS(500)))
      vTaskDelay(pdMS_TO_TICKS(100));
  }

  char buf[512];
  for (;;) {
    pmPollUsb();
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(200));
    itsPoll();
    /* Drain input stream → fan out to all ITS consumers */
    for (;;) {
      size_t n = xStreamBufferReceive(logInputStream, buf, sizeof(buf) - 1, 0);
      if (n == 0) break;
      buf[n] = '\0';
      if (itsServerActive() == 0) continue;
      char plain[512];
      size_t pn = 0;
      bool plainDone = false;
      for (int i = 0; i < LOG_MAX_CONSUMERS; i++) {
        if (!itsConnected(i)) continue;
        const char* out = buf;
        size_t outLen = n;
        if (logConsumerAnsi[i] != LOG_ANSI) {
          if (!plainDone) { memcpy(plain, buf, n); pn = stripAnsi(plain, n); plainDone = true; }
          out = plain; outLen = pn;
        }
        if (logConsumerWs[i])
          wsSendText(i, out, outLen);
        else
          itsSend(i, out, outLen, 0);
      }
    }
  }
}

/* ---- Init ---- */

void logInit() {
  if (logInited) return;
  logInited = true;

  /* Log input stream: vprintf callback writes here */
  logInputStream = xStreamBufferCreateWithCaps(LOG_INPUT_SIZE, 1, MALLOC_CAP_SPIRAM);

  /* Set INFO as default until nvsRunBoot sets the real level via logApplyLevels() */
  esp_log_level_set("*", ESP_LOG_INFO);

  /* Install ESP-IDF vprintf hook — all logging now flows through our callback */
  esp_log_set_vprintf(logVprintf);

  xTaskCreatePinnedToCoreWithCaps(logTaskFn, "log", 4096, NULL, 1, &logTaskHandle, 1, MALLOC_CAP_SPIRAM);
}

/* ---- Log levels ---- */

static log_level_t currentLogLevel = LOG_DEBUG;

/* Parse level string (first char, case-insensitive): n/e/w/i/d and - for inherit */
static esp_log_level_t parseEspLevel(const char* val) {
  char c = val[0];
  if (c >= 'A' && c <= 'Z') c += 32;  /* tolower */
  switch (c) {
    case 'n': return ESP_LOG_NONE;
    case 'e': return ESP_LOG_ERROR;
    case 'w': return ESP_LOG_WARN;
    case 'i': return ESP_LOG_INFO;
    case 'd': return ESP_LOG_DEBUG;
    default:  return ESP_LOG_INFO;
  }
}

void logApplyLevels() {
  /* Global level from "s.log.level" key */
  char val[16];
  storageGetStr("s.log.level", val, sizeof(val), "info");
  esp_log_level_t global = parseEspLevel(val);
  esp_log_level_set("*", global);

  /* Map to our internal level for logIsDebug() etc */
  if (global >= ESP_LOG_DEBUG) currentLogLevel = LOG_DEBUG;
  else if (global >= ESP_LOG_INFO) currentLogLevel = LOG_INFO;
  else currentLogLevel = LOG_ERROR;

  /* Per-tag overrides from "s.log.tag.*" keys */
  storageForEach("s.log.tag.", [](const char* key, const char* val) {
    const char* tag = key + 10;  /* skip "s.log.tag." */
    if (val[0] == '-') {
      /* Inherit: re-apply global */
      char gval[16];
      storageGetStr("s.log.level", gval, sizeof(gval), "info");
      esp_log_level_set(tag, parseEspLevel(gval));
    } else {
      esp_log_level_set(tag, parseEspLevel(val));
    }
  });
}

void logSetGlobal(const char* level) {
  storageSet("s.log.level", level);
  logApplyLevels();
}

void logSetTag(const char* tag, const char* level) {
  char key[32];
  snprintf(key, sizeof(key), "s.log.tag.%.19s", tag);
  storageSet(key, level);
  logApplyLevels();
}

bool logIsDebug() { return currentLogLevel >= LOG_DEBUG; }

const char* cfd(int fd) {
  static char buf[12];
  if (!logIsDebug()) return "";
  snprintf(buf, sizeof(buf), "{%d} ", fd);
  return buf;
}

/* ---- CLI command: log ---- */

void logRegisterCmds() {
    cliRegisterCmd("log", [](const char* a) {
        if (strcmp(a, "help") == 0) { cliPrintf("  %-*s show/set log level\n", CLI_HELP_COL, "log [tag] [level]"); return; }
        if (!*a) {
            char val[16]; storageGetStr("s.log.level", val, sizeof(val), "info");
            cliPrintf("  %s\n", val);
            return;
        }
        char first[24] = {};
        int i = 0;
        while (a[i] && a[i] != ' ' && i < 23) { first[i] = a[i]; i++; }
        first[i] = '\0';
        const char* rest = a + i;
        while (*rest == ' ') rest++;
        bool isLevel = (first[1] == '\0' && strchr("newidNEWID", first[0])) ||
                       strcasecmp(first, "none") == 0 || strcasecmp(first, "error") == 0 ||
                       strcasecmp(first, "warn") == 0 || strcasecmp(first, "info") == 0 ||
                       strcasecmp(first, "debug") == 0;
        if (isLevel && !*rest) logSetGlobal(first);
        else if (!*rest) {
            char key[32]; snprintf(key, sizeof(key), "s.log.tag.%.19s", first);
            char val[16]; storageGetStr(key, val, sizeof(val), "-");
            cliPrintf("  %s: %s\n", first, val[0] == '-' ? "(default)" : val);
        } else logSetTag(first, rest);
    });
}
