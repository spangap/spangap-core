/**
 * Log — log task, ESP-IDF vprintf hook, log reformatting, log levels.
 * CLI command: log.
 */
#include "log.h"
#include "pm.h"
#include "cli.h"
#include "its.h"
#include "web.h"
#include "net.h"
#include "storage.h"
#include "compat.h"
#include "fs.h"
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <cstdio>
#include <cstring>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>

/* ---- Log input ring buffer ---- */

static bool logInited = false;

#define LOG_RING_SIZE 16384
static uint8_t  logRing[LOG_RING_SIZE];   /* static DRAM — no heap alloc to corrupt */
static volatile uint32_t logRingHead = 0; /* write position (writers) */
static volatile uint32_t logRingTail = 0; /* read position (log task only) */
static portMUX_TYPE logSpinlock = portMUX_INITIALIZER_UNLOCKED;

static size_t logRingWrite(const char* data, size_t len) {
  taskENTER_CRITICAL(&logSpinlock);
  uint32_t h = logRingHead, t = logRingTail;
  uint32_t free = LOG_RING_SIZE - (h - t);
  if (len > free) len = free;
  for (size_t i = 0; i < len; i++)
    logRing[(h + i) % LOG_RING_SIZE] = data[i];
  logRingHead = h + len;
  taskEXIT_CRITICAL(&logSpinlock);
  return len;
}

static size_t logRingRead(char* buf, size_t maxLen) {
  uint32_t h = logRingHead, t = logRingTail;
  uint32_t avail = h - t;
  if (avail == 0) return 0;
  if (avail > maxLen) avail = maxLen;
  for (size_t i = 0; i < avail; i++)
    buf[i] = logRing[(t + i) % LOG_RING_SIZE];
  logRingTail = t + avail;
  return avail;
}

/* ---- Log file output ---- */

static int logFile = -1;
static char logFilePath[128] = {};
static int64_t lastFlushUs = 0;
static int logFileLevelMax = 4;  /* 0=E 1=W 2=I 3=D 4=V — default: pass all */

static int levelCharToNum(char c) {
    switch (c) {
        case 'E': return 0; case 'W': return 1; case 'I': return 2;
        case 'D': return 3; case 'V': return 4; default: return 2;
    }
}

static int parseLevelNum(const char* s) {
    char c = s[0];
    if (c >= 'a' && c <= 'z') c -= 32;
    switch (c) {
        case 'E': case 'N': return 0;  /* error/none */
        case 'W': return 1; case 'I': return 2;
        case 'D': return 3; case 'V': return 4;
        default: return 4;
    }
}

static bool isLevelArg(const char* s) {
    if (!*s) return false;
    if (s[1] == '\0' && strchr("newidvNEWIDV", s[0])) return true;
    return strcasecmp(s, "none") == 0 || strcasecmp(s, "error") == 0 ||
           strcasecmp(s, "warn") == 0 || strcasecmp(s, "info") == 0 ||
           strcasecmp(s, "debug") == 0 || strcasecmp(s, "verbose") == 0;
}

/* Find log level character in a plain-text line: the char before " [" */
static char lineLevel(const char* line, size_t len) {
    for (size_t i = 1; i + 1 < len && line[i] != '\n'; i++) {
        if (line[i] == ' ' && line[i + 1] == '[') {
            char c = line[i - 1];
            if (c == 'E' || c == 'W' || c == 'I' || c == 'D' || c == 'V') return c;
        }
    }
    return 'I';
}

/* ---- Configurable ANSI colors per level + timestamp ---- */

static char logColors[6][16];  /* E, W, I, D, V, timestamp */

static void logLoadColors() {
    auto load = [](int idx, const char* key, const char* def) {
        storageGetStr(key, logColors[idx], sizeof(logColors[idx]), def);
    };
    load(0, "s.log.colors.error",   "0;31");  /* red */
    load(1, "s.log.colors.warn",    "0;33");  /* yellow */
    load(2, "s.log.colors.info",    "0;32");  /* green */
    load(3, "s.log.colors.debug",   "0;37");  /* light grey */
    load(4, "s.log.colors.verbose", "0;90");  /* dark grey */
    load(5, "s.log.colors.timestamp", "0;90"); /* dark grey */
}

static const char* logColor(char level) {
    switch (level) {
        case 'E': return logColors[0];
        case 'W': return logColors[1];
        case 'I': return logColors[2];
        case 'D': return logColors[3];
        case 'V': return logColors[4];
        default:  return logColors[2];
    }
}

static void logFileMsg(const char* msg) {
    if (logFile < 0) return;
    char ts[24]; fmtWallClock(ts, sizeof(ts));
    char line[256];
    int n = snprintf(line, sizeof(line), "%s I [log] %s\n", ts, msg);
    if (n > 0) fs_write(line, 1, n, logFile);
}

static void logFileClose() {
    if (logFile < 0) return;
    logFileMsg("log file closed");
    fs_sync(logFile);
    fs_close(logFile);
    logFile = -1;
    logFilePath[0] = '\0';
}

static void logFileOpen(const char* path) {
    logFileClose();
    if (!path || !*path) return;
    int f = fs_open(path, "a");
    if (f < 0) return;
    logFile = f;
    safeStrncpy(logFilePath, path, sizeof(logFilePath));
    lastFlushUs = esp_timer_get_time();
    logFileMsg("log file opened");
}

/* Write plain text to log file, filtering lines by level */
static void logFileWrite(const char* data, size_t len) {
    if (logFile < 0) return;
    const char* p = data;
    const char* end = data + len;
    while (p < end) {
        const char* nl = (const char*)memchr(p, '\n', end - p);
        size_t lineLen = nl ? (size_t)(nl - p + 1) : (size_t)(end - p);
        if (levelCharToNum(lineLevel(p, lineLen)) <= logFileLevelMax)
            fs_write(p, 1, lineLen, logFile);
        p += lineLen;
    }
}

static void logFileFlush() {
    if (logFile < 0) return;
    int intervalS = storageGetInt("s.log.file.interval", 5);
    int64_t now = esp_timer_get_time();
    if (now - lastFlushUs >= (int64_t)intervalS * 1000000) {
        fs_sync(logFile);
        lastFlushUs = now;
    }
}

/* ---- Log ITS server — consumers connect as clients ---- */

#define LOG_MAX_CONSUMERS 4
static struct {
  int itsHandle;
  log_ansi_t ansi;
  bool ws;
} logSlots[LOG_MAX_CONSUMERS];

static int logAllocSlot(int h) {
  for (int i = 0; i < LOG_MAX_CONSUMERS; i++)
    if (logSlots[i].itsHandle < 0) { logSlots[i].itsHandle = h; return i; }
  return -1;
}

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
 * Input may have ANSI color prefix/suffix. We strip those and add our own.
 * When ESP tag equals the FreeRTOS task name, we omit the tag from the output line.
 * If the message body then still starts with "taskname:" (common printf habit), strip
 * that duplicate too — keeps subsystem prefixes like "camera:" when tag != task. */
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
        char color[24] = "";
        const char* reset = "";
        if (ansi) {
            snprintf(color, sizeof(color), "\033[%sm", logColor(lastLevel));
            reset = "\033[0m";
        }
        bool showTs = storageGetInt("s.log.timestamp", 0) != 0;
        char tsBuf[48] = "";
        if (showTs) {
            char ts[24]; fmtWallClock(ts, sizeof(ts));
            if (ansi) snprintf(tsBuf, sizeof(tsBuf), "\033[%sm%s\033[0m ", logColors[5], ts);
            else snprintf(tsBuf, sizeof(tsBuf), "%s ", ts);
        }
        return snprintf(dst, dstSize, "%s%s%c [%s] %.*s%s\n",
            tsBuf, color, lastLevel, taskName, (int)msgLen, p, reset);
    }

    lastLevel = level;

    /* Timestamp: keep or skip "(12345) " based on s.log.timestamp */
    const char* afterLevel = p + 1;
    while (*afterLevel == ' ') afterLevel++;
    if (*afterLevel == '(') {
        const char* tsEnd = strchr(afterLevel, ')');
        if (tsEnd) {
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
    /* Drop whitespace-only messages (ESP wifi emits "I (ts) wifi: \n" then content on next line)
     * but preserve leading whitespace on real content — itsStatus() and similar use it for indentation. */
    { bool allWhite = true;
      for (size_t i = 0; i < msgLen; i++)
        if (msgStart[i] != ' ' && msgStart[i] != '\t') { allWhite = false; break; }
      if (allWhite) return 0; }

    /* ESP_LOGD(task, "task: …") → line is "… task: task: …" — drop the redundant body prefix */
    if (tag[0] && strcmp(tag, taskName) == 0 && taskName[0] && strcmp(taskName, "?") != 0) {
        size_t tn = strlen(taskName);
        while (msgLen > tn + 1 && strncmp(msgStart, taskName, tn) == 0 && msgStart[tn] == ':') {
            msgStart += tn + 1;
            msgLen -= tn + 1;
            while (msgLen > 0 && (msgStart[0] == ' ' || msgStart[0] == '\t')) { msgStart++; msgLen--; }
        }
        if (msgLen == 0) return 0;
    }

    /* ANSI color by level */
    char color[24] = "";
    const char* reset = "";
    if (ansi) {
        snprintf(color, sizeof(color), "\033[%sm", logColor(level));
        reset = "\033[0m";
    }

    /* Format: "Mar 27 16:23:15.342 L [task] msg" or "L [task] msg" */
    char tsBuf[48] = "";
    if (showTimestamp) {
        char ts[24];
        fmtWallClock(ts, sizeof(ts));
        if (ansi) snprintf(tsBuf, sizeof(tsBuf), "\033[%sm%s\033[0m ", logColors[5], ts);
        else snprintf(tsBuf, sizeof(tsBuf), "%s ", ts);
    }

    /* Suppress TAG if it matches the task name */
    if (tag[0] && strcmp(tag, taskName) == 0) {
        return snprintf(dst, dstSize, "%s%s%c [%s] %.*s%s\n",
            tsBuf, color, level, taskName, (int)msgLen, msgStart, reset);
    } else if (tag[0]) {
        return snprintf(dst, dstSize, "%s%s%c [%s] %s: %.*s%s\n",
            tsBuf, color, level, taskName, tag, (int)msgLen, msgStart, reset);
    } else {
        return snprintf(dst, dstSize, "%s%s%c [%s] %.*s%s\n",
            tsBuf, color, level, taskName, (int)msgLen, msgStart, reset);
    }
}

static TaskHandle_t logTaskHandle = NULL;

/* vprintf callback — called by ESP-IDF log system from any task */
/* True while the serial task is in CLI mode — suppresses the direct-stdout
 * log echo so command output isn't tangled with background log lines.
 * Defined in cli.cpp. */
extern "C" volatile bool serialInCli;

static int logVprintf(const char* fmt, va_list args) {
    if (!logInited) return 0;

    char buf[256];
    int rawLen = vsnprintf(buf, sizeof(buf), fmt, args);
    if (rawLen <= 0) return 0;

    /* Reformat with task name (ANSI version for the input stream — consumers strip if needed) */
    char formatted[288];
    int fmtLen = logReformat(buf, formatted, sizeof(formatted), true);
    if (fmtLen <= 0) return rawLen;

    /* Write to ring buffer — spinlock serializes concurrent writers.
     * Safe from any task context (spinlock disables interrupts briefly). */
    logRingWrite(formatted, fmtLen);

    /* Wake log task */
    if (logTaskHandle) xTaskNotifyGive(logTaskHandle);

    /* Always echo to stdout (USB Serial JTAG) unless serial is in CLI mode.
     * This bypasses the ITS log→serial consumer path entirely so logs reach
     * the wire even if the serial task is wedged or not yet connected. */
    if (!serialInCli) {
        fwrite(formatted, 1, fmtLen, stdout);
    }

    return rawLen;
}

/* ---- Log paste-back: send tail of log file to new WS client ---- */

static void logPasteBack(int handle) {
  int pasteKB = storageGetInt("s.log.file.paste", 48);
  if (pasteKB <= 0 || !logFilePath[0]) return;

  /* Flush current file so all recent data is on disk */
  if (logFile >= 0) fs_sync(logFile);

  int f = fs_open(logFilePath, "r");
  if (f < 0) return;

  fs_seek(f, 0, SEEK_END);
  long fileSize = fs_tell(f);
  long readSize = (long)pasteKB * 1024;
  long offset = fileSize > readSize ? fileSize - readSize : 0;
  long want = fileSize - offset;
  fs_seek(f, offset, SEEK_SET);

  /* Read the whole tail chunk in one go — one ITS roundtrip instead of ~50K.
   * PSRAM buffer so we don't eat DRAM. */
  char* in = (char*)heap_caps_malloc(want + 1, MALLOC_CAP_SPIRAM);
  if (!in) { fs_close(f); return; }
  size_t got = fs_read(in, 1, want, f);
  fs_close(f);
  if (got == 0) { heap_caps_free(in); return; }
  in[got] = '\0';

  /* Worst case output: every line gets ~40 bytes of ANSI codes. Overallocate. */
  char* out = (char*)heap_caps_malloc(got * 2 + 64, MALLOC_CAP_SPIRAM);
  if (!out) { heap_caps_free(in); return; }

  char tsColor[24];
  snprintf(tsColor, sizeof(tsColor), "\033[%sm", logColors[5]);
  size_t tsLen = strlen(tsColor);

  /* If starting mid-file, skip to first newline */
  size_t p = 0;
  if (offset > 0) {
    while (p < got && in[p] != '\n') p++;
    if (p < got) p++;
  }

  size_t op = 0;
  while (p < got) {
    size_t lineStart = p;
    while (p < got && in[p] != '\n') p++;
    if (p < got) p++;  /* include newline */
    size_t len = p - lineStart;
    const char* line = in + lineStart;

    /* Locate level char X in "... X [task] ..." (colorize same as live) */
    int lvlPos = -1;
    for (size_t i = 1; i + 1 < len && line[i] != '\n'; i++) {
      if (line[i] == ' ' && line[i + 1] == '[') {
        char ch = line[i - 1];
        if (ch == 'E' || ch == 'W' || ch == 'I' || ch == 'D' || ch == 'V') { lvlPos = i - 1; break; }
      }
    }

    if (lvlPos > 0) {
      memcpy(out + op, tsColor, tsLen); op += tsLen;
      memcpy(out + op, line, lvlPos); op += lvlPos;
      char lvlColor[24];
      int lvlLen = snprintf(lvlColor, sizeof(lvlColor), "\033[%sm", logColor(line[lvlPos]));
      memcpy(out + op, lvlColor, lvlLen); op += lvlLen;
      memcpy(out + op, line + lvlPos, len - lvlPos); op += len - lvlPos;
      memcpy(out + op, "\033[0m", 4); op += 4;
    } else {
      memcpy(out + op, line, len); op += len;
    }
  }

  wsSendText(handle, out, op);
  heap_caps_free(out);
  heap_caps_free(in);
}

/* ---- Log ITS server callbacks ---- */

static int logOnConnect(int handle, const void* data, size_t len) {
  int s = logAllocSlot(handle);
  if (s < 0) return -1;
  logSlots[s].ws = false;
  if (len >= sizeof(net_connect_t) && ((const net_connect_t*)data)->ws) {
    if (!wsUpgrade(handle)) { logSlots[s].itsHandle = -1; return -1; }
    logSlots[s].ws = true;
    logSlots[s].ansi = LOG_ANSI;
    logPasteBack(handle);
  } else if (len >= sizeof(log_connect_t)) {
    logSlots[s].ansi = ((const log_connect_t*)data)->ansi;
  } else {
    logSlots[s].ansi = LOG_NO_ANSI;
  }
  return s;
}

static void logOnDisconnect(int ref) {
  if (ref >= 0 && ref < LOG_MAX_CONSUMERS) logSlots[ref].itsHandle = -1;
}

/* ---- Log task: drains input stream → fan out to ITS consumers ---- */

static SemaphoreHandle_t logReadySem = nullptr;

static void logTaskFn(void* arg) {
  for (int i = 0; i < LOG_MAX_CONSUMERS; i++) logSlots[i].itsHandle = -1;
  itsServerInit();
  itsServerPortOpen(LOG_PORT, LOG_MAX_CONSUMERS, 0, 2048);
  itsServerOnConnect(LOG_PORT, logOnConnect);
  itsServerOnDisconnect(LOG_PORT, logOnDisconnect);
  /* Unblock logInit() — server is open for clients (e.g. serial task). */
  if (logReadySem) xSemaphoreGive(logReadySem);

  logLoadColors();
  storageSubscribeChanges("s.log.colors.", ON_CHANGE { logLoadColors(); });

  /* Open log file if configured */
  { char lvl[16];
    storageGetStr("s.log.file.level", lvl, sizeof(lvl), "verbose");
    logFileLevelMax = parseLevelNum(lvl);
    char name[64];
    storageGetStr("s.log.file.name", name, sizeof(name));
    if (name[0]) {
      char dir[64]; storageGetStr("s.log.dir", dir, sizeof(dir), "/sdcard/log");
      fs_mkdirp(dir);
      char path[128]; snprintf(path, sizeof(path), "%s/%s", dir, name);
      logFileOpen(path);
    }
  }

  storageSubscribeChanges("s.log.file.name", ON_CHANGE {
    logFileClose();
    if (val[0]) {
      char dir[64]; storageGetStr("s.log.dir", dir, sizeof(dir), "/sdcard/log");
      fs_mkdirp(dir);
      char path[128]; snprintf(path, sizeof(path), "%s/%s", dir, val);
      logFileOpen(path);
    }
  });
  storageSubscribeChanges("s.log.file.level", ON_CHANGE {
    logFileLevelMax = parseLevelNum(val);
  });

  /* Register TCP port + WS path (non-blocking: retry from main loop so serial
   * log connection is accepted immediately during boot). */
  bool netRegistered = false, webRegistered = false;

  char buf[512];
  for (;;) {
    pmPollUsb();
    while (itsPoll(pdMS_TO_TICKS(200))) {}

    if (!netRegistered) {
      net_port_msg_t reg = {};
      reg.itsPort = LOG_PORT;
      safeStrncpy(reg.nvsKey, "log_port", sizeof(reg.nvsKey));
      netRegistered = itsSendAux("net", NET_PORT_REG_PORT, &reg, sizeof(reg), 0);
    }
    if (!webRegistered) {
      web_path_msg_t reg = {};
      reg.itsPort = LOG_PORT;
      safeStrncpy(reg.path, "log", sizeof(reg.path));
      webRegistered = itsSendAux("web", WEB_PATH_REG_PORT, &reg, sizeof(reg), 0);
    }

    /* Skip the drain entirely while there are no consumers and no log file.
     * The ring keeps its contents so they fan out the moment a consumer
     * (typically the serial task) connects. Without this, the boot script's
     * logs would silently disappear because the drain happens between hook
     * install and the first consumer's connect being processed. */
    bool hasConsumer = (itsServerActive() > 0);
    bool hasFile = (logFile >= 0);
    if (!hasConsumer && !hasFile) {
      logFileFlush();
      continue;
    }

    /* Drain input stream → fan out to ITS consumers + log file */
    for (;;) {
      size_t n = logRingRead(buf, sizeof(buf) - 1);
      if (n == 0) break;
      buf[n] = '\0';
      char plain[512];
      size_t pn = 0;
      bool plainDone = false;

      /* Strip ANSI once if needed by file or any non-ANSI consumer */
      auto ensurePlain = [&]() {
        if (plainDone) return;
        memcpy(plain, buf, n); pn = stripAnsi(plain, n); plainDone = true;
      };

      /* Write plain text to log file */
      if (hasFile) { ensurePlain(); logFileWrite(plain, pn); }

      if (!hasConsumer) continue;
      for (int i = 0; i < LOG_MAX_CONSUMERS; i++) {
        int h = logSlots[i].itsHandle;
        if (h < 0 || !itsConnected(h)) continue;
        const char* out = buf;
        size_t outLen = n;
        if (logSlots[i].ansi != LOG_ANSI) {
          ensurePlain();
          out = plain; outLen = pn;
        }
        if (logSlots[i].ws) {
          /* WS frame header + payload, non-blocking — drop if buffer full */
          uint8_t hdr[4];
          int hdrLen;
          hdr[0] = 0x81; /* FIN + text */
          if (outLen < 126) { hdr[1] = outLen; hdrLen = 2; }
          else { hdr[1] = 126; hdr[2] = outLen >> 8; hdr[3] = outLen & 0xff; hdrLen = 4; }
          if (itsSend(h, hdr, hdrLen, 0) == (size_t)hdrLen)
            itsSend(h, out, outLen, 0);
        } else {
          itsSend(h, out, outLen, 0);
        }
      }
    }
    logFileFlush();
  }
}

/* ---- Init ---- */

void logInit() {
  if (logInited) return;
  logInited = true;

  /* Ring buffer is static DRAM — no heap allocation needed */

  /* Set INFO as default until boot script sets the real level via logApplyLevels() */
  esp_log_level_set("*", ESP_LOG_INFO);

  /* Install ESP-IDF vprintf hook now so all messages from this point land
   * in the ring. The log task does NOT drain the ring while there are no
   * consumers (see logTaskFn), so boot logs are buffered until the serial
   * task connects, then fan out. */
  esp_log_set_vprintf(logVprintf);

  /* PSRAM stack — log task no longer hits flash directly (file I/O via fs worker,
   * directory iteration via fs_opendir/readdir/closedir). The DRAM ring buffer is
   * accessed via spinlock from any caller; cache-disable windows freeze this task. */
  logReadySem = xSemaphoreCreateBinary();
  xTaskCreatePinnedToCoreWithCaps(logTaskFn, "log", 4608, NULL, 1, &logTaskHandle, 1, MALLOC_CAP_SPIRAM);
  /* Block until log task has its ITS server open — otherwise the serial task
   * (created right after by cliInit) races and its initial connectLog fails. */
  xSemaphoreTake(logReadySem, portMAX_DELAY);
  vSemaphoreDelete(logReadySem);
  logReadySem = nullptr;
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
    case 'v': return ESP_LOG_VERBOSE;
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

/* ---- CLI commands: log, logfile, logrotate ---- */

static void cmdLogfile(const char* a) {
    if (strcmp(a, "help") == 0) {
        cliPrintf("  %-*s start/stop logging to file\n", CLI_HELP_COL, "logfile [level] [name|off]");
        return;
    }

    /* logfile off — stop logging */
    if (strcmp(a, "off") == 0) { storageSet("s.log.file.name", ""); return; }

    /* Parse optional level, then optional name */
    char first[24] = {};
    int i = 0;
    while (a[i] && a[i] != ' ' && i < 23) { first[i] = a[i]; i++; }
    first[i] = '\0';
    const char* rest = a + i;
    while (*rest == ' ') rest++;

    const char* level = "verbose";
    const char* nameArg = nullptr;

    if (first[0] && isLevelArg(first)) {
        level = first;
        if (*rest) nameArg = rest;
    } else if (first[0]) {
        nameArg = a;  /* no level, entire arg is the name */
    }

    storageSet("s.log.file.level", level);

    char name[64];
    if (!nameArg) {
        /* Default: YYYYMMDD.log */
        time_t now = time(nullptr);
        struct tm tm;
        localtime_r(&now, &tm);
        snprintf(name, sizeof(name), "%04d%02d%02d.log",
            tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    } else {
        /* Extract basename if full path given */
        const char* base = strrchr(nameArg, '/');
        safeStrncpy(name, base ? base + 1 : nameArg, sizeof(name));
    }

    storageSet("s.log.file.name", name);
    char dir[64]; storageGetStr("s.log.dir", dir, sizeof(dir), "/sdcard/log");
    cliPrintf("  %s/%s (%s)\n", dir, name, level);
}

/* ---- CLI command: logrotate ---- */

static bool isLogDateFile(const char* name) {
    /* Match YYYYMMDD.log — 8 digits + ".log" */
    if (strlen(name) != 12) return false;
    for (int i = 0; i < 8; i++)
        if (name[i] < '0' || name[i] > '9') return false;
    return strcmp(name + 8, ".log") == 0;
}

static time_t logDateToTime(const char* name) {
    /* Parse YYYYMMDD from filename */
    struct tm tm = {};
    char buf[9];
    memcpy(buf, name, 8); buf[8] = '\0';
    if (sscanf(buf, "%4d%2d%2d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday) != 3) return 0;
    tm.tm_year -= 1900; tm.tm_mon -= 1;
    return mktime(&tm);
}

static void cmdLogrotate(const char* a) {
    if (strcmp(a, "help") == 0) {
        cliPrintf("  %-*s rotate log, delete old files\n", CLI_HELP_COL, "logrotate [days]");
        return;
    }

    /* Check current log file is in date format */
    char dir[64];
    storageGetStr("s.log.dir", dir, sizeof(dir), "/sdcard/log");
    char curName[64];
    storageGetStr("s.log.file.name", curName, sizeof(curName));
    if (curName[0] && !isLogDateFile(curName)) {
        cliPrintf("logrotate: current log file is not a date-format log\n");
        return;
    }

    /* Set log name to today's date */
    time_t now = time(nullptr);
    struct tm tm;
    localtime_r(&now, &tm);
    char name[64];
    snprintf(name, sizeof(name), "%04d%02d%02d.log",
        tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    storageSet("s.log.file.name", name);

    /* Delete expired files if days specified */
    int days = *a ? atoi(a) : 0;
    if (days <= 0) return;

    time_t cutoff = now - (time_t)days * 86400;
    constexpr int MAX = 256;
    auto* listing = (fs_listing_t*)heap_caps_malloc(MAX * sizeof(fs_listing_t), MALLOC_CAP_SPIRAM);
    if (!listing) return;
    int n = fs_listdir(dir, listing, MAX);
    int deleted = 0;
    for (int i = 0; i < n; i++) {
        if (!isLogDateFile(listing[i].name)) continue;
        time_t ft = logDateToTime(listing[i].name);
        if (ft > 0 && ft < cutoff) {
            char fp[80];  /* dir(64) + '/' + "YYYYMMDD.log"(12) + NUL */
            snprintf(fp, sizeof(fp), "%.63s/%.12s", dir, listing[i].name);
            if (fs_remove(fp) == 0) deleted++;
        }
    }
    heap_caps_free(listing);
    if (deleted) cliPrintf("  deleted %d old log file%s\n", deleted, deleted > 1 ? "s" : "");
}

void logRegisterCmds() {
    cliRegisterCmd("logfile", cmdLogfile);
    cliRegisterCmd("logrotate", cmdLogrotate);
    cliRegisterCmd("log", [](const char* a) {
        if (strcmp(a, "help") == 0) {
            cliPrintf("  %-*s show/set log level\n", CLI_HELP_COL, "log [tag] [level]");
            cliPrintf("  %-*s toggle timestamps\n", CLI_HELP_COL, "log [no]timestamp");
            return;
        }
        if (strcmp(a, "timestamp") == 0) { storageSet("s.log.timestamp", 1); return; }
        if (strcmp(a, "notimestamp") == 0) { storageSet("s.log.timestamp", 0); return; }
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
        bool isLevel = (first[1] == '\0' && strchr("newidvNEWIDV", first[0])) ||
                       strcasecmp(first, "none") == 0 || strcasecmp(first, "error") == 0 ||
                       strcasecmp(first, "warn") == 0 || strcasecmp(first, "info") == 0 ||
                       strcasecmp(first, "debug") == 0 || strcasecmp(first, "verbose") == 0;
        if (isLevel && !*rest) logSetGlobal(first);
        else if (!*rest) {
            char key[32]; snprintf(key, sizeof(key), "s.log.tag.%.19s", first);
            char val[16]; storageGetStr(key, val, sizeof(val), "-");
            cliPrintf("  %s: %s\n", first, val[0] == '-' ? "(default)" : val);
        } else logSetTag(first, rest);
    });
}
