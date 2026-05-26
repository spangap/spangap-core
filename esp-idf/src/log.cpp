/**
 * Log — log task, ESP-IDF vprintf hook, log reformatting, log levels.
 * CLI command: log.
 */
#include "log.h"
#include "pm.h"
#include "cli.h"
#include "cron.h"
#include "its.h"
#include "net.h"
#include "storage.h"
#include "compat.h"
#include "fs.h"
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <string>
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

#define LOG_RING_SIZE 8192
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
static int logFileIntervalMs = 5000;  /* tail-flush cadence; 0 = size-batch only */
static bool logFileDirty = false;     /* lines written since the last fsync */

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

/* Find log level character in a plain-text line: a single E/W/I/D/V
 * surrounded by spaces (or at start of line). Matches both the device's
 * native "<ts> L [task] msg" and free-form "<ts> L Browser: msg". */
static char lineLevel(const char* line, size_t len) {
    for (size_t i = 0; i + 1 < len && line[i] != '\n'; i++) {
        char c = line[i];
        if (c != 'E' && c != 'W' && c != 'I' && c != 'D' && c != 'V') continue;
        bool leftOk  = (i == 0) || line[i - 1] == ' ';
        bool rightOk = line[i + 1] == ' ';
        if (leftOk && rightOk) return c;
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
    if (n > 0) { itsSend(logFile, line, n, 0); logFileDirty = true; }
}

static void logFileClose() {
    if (logFile < 0) return;
    logFileMsg("log file closed");
    itsDisconnect(logFile);
    logFile = -1;
    logFilePath[0] = '\0';
}

static void logFileOpen(const char* path) {
    logFileClose();
    if (!path || !*path) return;
    /* If the file lives on the SD card and there is no card, refuse the open
     * with one warn. Without this, fs_open_stream would block on a doomed
     * mount path or spam the log with retry errors. */
    if (strncmp(path, "/sdcard/", 8) == 0 && !sdAvailable()) {
        static bool warned = false;
        if (!warned) { warn("logfile: no SD card, log file disabled\n"); warned = true; }
        return;
    }
    /* Stream buffer 16KB, fs worker drains once 4KB have accumulated —
     * batches ~4KB per fs_write instead of per-line. */
    int h = fs_open_stream(path, "a", 16384, 4096);
    if (h < 0) return;
    logFile = h;
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
        if (levelCharToNum(lineLevel(p, lineLen)) <= logFileLevelMax) {
            itsSend(logFile, p, lineLen, 0);
            logFileDirty = true;
        }
        p += lineLen;
    }
}

static void logFileFlush() {
    /* The fs worker auto-drains the stream buffer once 4KB accumulate. This adds
     * a time-based tail flush so the last partial (<4KB) batch reaches the card
     * within s.log.file.interval seconds — otherwise a crash/power-loss loses
     * it. interval=0 disables, leaving pure size-batching. Called every task
     * loop; the time gate sets the real cadence. */
    if (logFile < 0 || logFileIntervalMs <= 0 || !logFileDirty) return;
    int64_t now = esp_timer_get_time();
    if (now - lastFlushUs < (int64_t)logFileIntervalMs * 1000) return;
    lastFlushUs = now;
    logFileDirty = false;
    fs_stream_sync(logFile);
}

/* ---- Log ITS server — consumers connect as clients ---- */

#define LOG_MAX_CONSUMERS 5      /* up to 3 TCP + 2 DC (browser + on-device viewer) */
#define LOG_INBOUND_BUF   512    /* per-slot accumulator for partial inbound lines */
static struct {
  int itsHandle;
  log_ansi_t ansi;
  char  lineBuf[LOG_INBOUND_BUF];
  size_t lineLen;
  bool  pastePending;   /* DC: paste-back deferred out of onConnect (below) */
  long  pasteBacklog;   /* kB requested (0 = default) */
} logSlots[LOG_MAX_CONSUMERS];

static int logAllocSlot(int h) {
  for (int i = 0; i < LOG_MAX_CONSUMERS; i++)
    if (logSlots[i].itsHandle < 0) {
      logSlots[i].itsHandle = h;
      logSlots[i].lineLen = 0;
      return i;
    }
  return -1;
}

/* True if the buffer contains any ANSI escape sequence */
static bool containsAnsi(const char* buf, size_t len) {
    for (size_t i = 0; i + 1 < len; i++)
        if (buf[i] == '\033' && buf[i + 1] == '[') return true;
    return false;
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

    /* PSRAM-backed via the default allocator (CONFIG_SPIRAM_MALLOC_ALWAYS-
     * INTERNAL=0 prefers SPIRAM for std::string). RAII frees on every
     * return path — no leaks possible. */
    std::string buf(1024, '\0');
    int rawLen = vsnprintf(buf.data(), buf.size(), fmt, args);
    if (rawLen <= 0) return 0;

    std::string formatted(1056, '\0');
    int fmtLen = logReformat(buf.c_str(), formatted.data(), formatted.size(), true);
    if (fmtLen <= 0) return rawLen;

    /* Write to ring buffer — spinlock serializes concurrent writers.
     * Safe from any task context (spinlock disables interrupts briefly). */
    logRingWrite(formatted.data(), fmtLen);

    /* Wake log task */
    if (logTaskHandle) xTaskNotifyGive(logTaskHandle);

    /* Always echo to stdout (USB Serial JTAG) unless serial is in CLI mode.
     * This bypasses the ITS log→serial consumer path entirely so logs reach
     * the wire even if the serial task is wedged or not yet connected. */
    if (!serialInCli) {
        fwrite(formatted.data(), 1, fmtLen, stdout);
    }

    return rawLen;
}

/* ---- Log paste-back: send tail of log file to new WS client ---- */

/** Send tail of log file to a newly connected DC client.
 *  backlogBytes: if > 0, tail exactly that many bytes (from DCEP protocol
 *  `{"backlog":N}`). Otherwise fall back to s.log.file.paste (kB, default 48).
 *  ansi: when false (clients that asked for {"ansi":0}, e.g. the on-device
 *  viewer), the tail is sent verbatim with no colour escapes. */
static void logPasteBack(int handle, long backlogBytes, bool ansi) {
  long readSize;
  if (backlogBytes > 0) {
    readSize = backlogBytes;
  } else {
    int pasteKB = storageGetInt("s.log.file.paste", 48);
    if (pasteKB <= 0) return;
    readSize = (long)pasteKB * 1024;
  }
  if (!logFilePath[0]) return;

  if (logFile >= 0) fs_stream_sync(logFile);

  int f = fs_open(logFilePath, "r");
  if (f < 0) return;

  fs_seek(f, 0, SEEK_END);
  long fileSize = fs_tell(f);
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

    if (ansi && lvlPos > 0) {
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

  /* Chunk into packets that fit the per-connection ITS buffer (2KB cap).
   * Split on newline boundaries so ANSI colour sequences stay inside a
   * single packet (browser xterm.js concats them sequentially). */
  constexpr size_t CHUNK = 1500;
  size_t off = 0;
  while (off < op) {
    size_t take = (op - off > CHUNK) ? CHUNK : (op - off);
    if (take < op - off) {
      size_t nl = take;
      while (nl > 0 && out[off + nl - 1] != '\n') nl--;
      if (nl > 0) take = nl;
    }
    if (itsSend(handle, out + off, take, pdMS_TO_TICKS(500)) != take) break;
    off += take;
  }
  heap_caps_free(out);
  heap_caps_free(in);
}

/* ---- Log ITS server callbacks ---- */

/** TCP (stream mode): net-forwarded for `nc` access. No ANSI — plain
 *  bytes so the pipe stays clean for line-oriented tools. */
static int logTcpConnect(int handle, const void* data, size_t len) {
  (void)data; (void)len;
  int s = logAllocSlot(handle);
  if (s < 0) return -1;
  logSlots[s].ansi = LOG_NO_ANSI;
  return s;
}

/** DC (packet mode): forwarded by webrtc_task from `log:1`. The DCEP
 *  protocol bytes may carry `{"backlog":N}` to override paste-back size. */
static int logDcConnect(int handle, const void* data, size_t len) {
  int s = logAllocSlot(handle);
  if (s < 0) return -1;
  logSlots[s].ansi = LOG_ANSI;
  long backlog = 0;
  if (data && len > 0 && len < 128) {
    char proto[128];
    memcpy(proto, data, len);
    proto[len] = '\0';
    /* Tiny parser over the DCEP protocol JSON. */
    const char* p = strstr(proto, "\"backlog\"");
    if (p && (p = strchr(p, ':'))) backlog = atol(p + 1);
    /* On-device LVGL viewers (and plain pipes) ask for {"ansi":0}: no colour
     * escapes in live lines OR paste-back. Browser xterm omits the key. */
    const char* a = strstr(proto, "\"ansi\"");
    if (a && (a = strchr(a, ':')) && atol(a + 1) == 0)
      logSlots[s].ansi = LOG_NO_ANSI;
  }
  /* Defer paste-back out of the connect handshake. It reads up to tens of KB
   * from the (now SD-backed, slow) log file and pushes it through the 2KB
   * connection buffer — but onConnect runs synchronously before the client's
   * ack is signalled, and the client is still blocked inside itsConnect, not
   * draining. Doing it here stalls the handshake past the client's connect
   * timeout (→ "[log: connect failed]") and leaks the server slot. The task
   * loop runs it once the client is connected and draining. */
  logSlots[s].pasteBacklog = backlog;
  logSlots[s].pastePending = true;
  return s;
}

static void logOnDisconnect(int ref) {
  if (ref >= 0 && ref < LOG_MAX_CONSUMERS) {
    logSlots[ref].itsHandle = -1;
    logSlots[ref].lineLen = 0;
    logSlots[ref].pastePending = false;
  }
}

/* ---- Inbound stream support ----
 * Consumers (browser log:1, plain TCP) may also send lines TO the log task.
 * Each complete line is fanned out to the OTHER consumers + log file as-is —
 * no timestamp / [task] / level char added, so the source can stamp its own.
 * ANSI-capable consumers still get coloring re-applied if the level char is
 * recognizable in the standard "<ts> L [tag] msg" position. */

/* Find the level char position (E/W/I/D/V) — same heuristic as lineLevel():
 * a single level char surrounded by spaces (or at start of line). */
static size_t findLevelCharPos(const char* line, size_t len) {
    for (size_t i = 0; i + 1 < len; i++) {
        char c = line[i];
        if (c != 'E' && c != 'W' && c != 'I' && c != 'D' && c != 'V') continue;
        bool leftOk  = (i == 0) || line[i - 1] == ' ';
        bool rightOk = line[i + 1] == ' ';
        if (leftOk && rightOk) return i;
    }
    return (size_t)-1;
}

/* Wrap a plain preformatted line in ANSI colors when a level char is present.
 * If no level char is found, copies the line unchanged. Adds a trailing '\n'.
 * Returns bytes written. */
static int colorizePreformatted(const char* in, size_t len, char* out, size_t outSize) {
    while (len > 0 && (in[len - 1] == '\n' || in[len - 1] == '\r')) len--;
    size_t lp = findLevelCharPos(in, len);
    if (lp == (size_t)-1)
        return snprintf(out, outSize, "%.*s\n", (int)len, in);
    char level = in[lp];
    /* Trim trailing space from the timestamp prefix (if any) */
    size_t prefixLen = lp;
    while (prefixLen > 0 && in[prefixLen - 1] == ' ') prefixLen--;
    if (prefixLen == 0) {
        return snprintf(out, outSize, "\033[%sm%.*s\033[0m\n",
                        logColor(level), (int)(len - lp), in + lp);
    }
    return snprintf(out, outSize, "\033[%sm%.*s\033[0m \033[%sm%.*s\033[0m\n",
                    logColors[5], (int)prefixLen, in,
                    logColor(level), (int)(len - lp), in + lp);
}

/* Fan one inbound line out to file + all OTHER consumers (skip the source). */
static void logInboundLineOut(int srcSlot, const char* line, size_t len) {
    if (len == 0) return;
    dbg("inbound from slot %d (%u bytes)\n", srcSlot, (unsigned)len);

    /* Plain version (ANSI-stripped) for file + non-ANSI consumers */
    char plain[LOG_INBOUND_BUF + 8];
    size_t plainLen = len;
    if (plainLen > sizeof(plain) - 2) plainLen = sizeof(plain) - 2;
    memcpy(plain, line, plainLen);
    plainLen = stripAnsi(plain, plainLen);
    plain[plainLen++] = '\n';
    plain[plainLen] = '\0';

    if (logFile >= 0) logFileWrite(plain, plainLen);

    /* ANSI version: if line already has escape sequences, pass through; else
     * re-apply color around level char (and grey on the timestamp prefix). */
    char ansi[LOG_INBOUND_BUF + 64];
    size_t ansiLen = 0;
    bool ansiDone = false;
    auto ensureAnsi = [&]() {
        if (ansiDone) return;
        if (containsAnsi(line, len)) {
            size_t n = len > sizeof(ansi) - 2 ? sizeof(ansi) - 2 : len;
            memcpy(ansi, line, n);
            ansi[n++] = '\n';
            ansiLen = n;
        } else {
            int n = colorizePreformatted(line, len, ansi, sizeof(ansi));
            ansiLen = (n > 0) ? (size_t)n : 0;
        }
        ansiDone = true;
    };

    for (int i = 0; i < LOG_MAX_CONSUMERS; i++) {
        if (i == srcSlot) continue;  /* don't echo back to sender */
        int h = logSlots[i].itsHandle;
        if (h < 0 || !itsConnected(h)) continue;
        if (logSlots[i].ansi == LOG_ANSI) {
            ensureAnsi();
            if (ansiLen > 0) itsSend(h, ansi, ansiLen, 0);
        } else {
            itsSend(h, plain, plainLen, 0);
        }
    }

    /* Serial console isn't an ITS consumer of the log task — it sees log
     * output via logVprintf's direct fwrite(stdout). Mirror that here so
     * inbound lines also reach the serial console. */
    if (!serialInCli) {
        ensureAnsi();
        if (ansiLen > 0) fwrite(ansi, 1, ansiLen, stdout);
    }
}

/* Drain inbound bytes from one slot, splitting into newline-terminated lines. */
static void logSlotDrainInbound(int slot) {
    int h = logSlots[slot].itsHandle;
    if (h < 0 || !itsConnected(h)) return;
    /* Packet-mode reads need the recv buffer to hold the entire packet at
     * once; size to match the slot's toSize (2048 at port-open). */
    char tmp[2048];
    size_t got = itsRecv(h, tmp, sizeof(tmp), 0);  /* non-blocking */
    if (got == 0) return;
    auto& s = logSlots[slot];
    for (size_t k = 0; k < got; k++) {
        char c = tmp[k];
        if (c == '\r') continue;
        if (c == '\n') {
            logInboundLineOut(slot, s.lineBuf, s.lineLen);
            s.lineLen = 0;
        } else if (s.lineLen + 1 < sizeof(s.lineBuf)) {
            s.lineBuf[s.lineLen++] = c;
        }
        /* Overflow → silently drop until next newline */
    }
}

/* ---- Log task: drains input stream → fan out to ITS consumers ---- */

static SemaphoreHandle_t logReadySem = nullptr;

static void logTaskFn(void* arg) {
  for (int i = 0; i < LOG_MAX_CONSUMERS; i++) logSlots[i].itsHandle = -1;
  /* Bigger inbox so long log lines don't truncate. */
  itsServerInit(1600);
  itsClientInit(1);  /* so logFileOpen() can itsConnect to fs stream server */
  /* 2KB each direction. Outbound (fromSize): log fanout uses timeout=0,
   * downstream layers (webrtc rexmit pool, TCP send buffer) absorb real
   * bursts — the per-handle buffer just covers the small gap between
   * itsSend and the consumer's drain. Inbound (toSize): same idea, browser
   * console bursts at session start are ~25 short lines, well within 2KB.
   *
   * Two ports: stream-mode for TCP nc/serial, packet-mode for DC clients
   * (`log:1` — browser xterm and the on-device Log program, which asks for
   * {"ansi":0}). LOG_MAX_CONSUMERS=5: up to 3 TCP + 2 DC. */
  itsServerPortOpen(LOG_PORT_TCP, /*packetBased=*/false, 3, 2048, 2048);
  itsServerOnConnect(LOG_PORT_TCP, logTcpConnect);
  itsServerOnDisconnect(LOG_PORT_TCP, logOnDisconnect);
  itsServerPortOpen(LOG_PORT_DC,  /*packetBased=*/true,  2, 2048, 2048);
  itsServerOnConnect(LOG_PORT_DC, logDcConnect);
  itsServerOnDisconnect(LOG_PORT_DC, logOnDisconnect);
  /* Unblock logInit() — server is open for clients (e.g. serial task). */
  if (logReadySem) xSemaphoreGive(logReadySem);

  logLoadColors();
  storageSubscribeChanges("s.log.colors.", ON_CHANGE { logLoadColors(); });

  /* Open log file if configured */
  { char lvl[16];
    storageGetStr("s.log.file.level", lvl, sizeof(lvl), "verbose");
    logFileLevelMax = parseLevelNum(lvl);
    logFileIntervalMs = storageGetInt("s.log.file.interval", 5) * 1000;
    char name[64];
    storageGetStr("s.log.file.name", name, sizeof(name));
    if (name[0]) {
      char dir[64]; storageGetStr("s.log.dir", dir, sizeof(dir), "/sdcard/log");
      if (strncmp(dir, "/sdcard/", 8) != 0 || sdAvailable()) fs_mkdirp(dir);
      char path[128]; snprintf(path, sizeof(path), "%s/%s", dir, name);
      logFileOpen(path);
    }
  }

  storageSubscribeChanges("s.log.file.name", ON_CHANGE {
    logFileClose();
    if (val[0]) {
      char dir[64]; storageGetStr("s.log.dir", dir, sizeof(dir), "/sdcard/log");
      if (strncmp(dir, "/sdcard/", 8) != 0 || sdAvailable()) fs_mkdirp(dir);
      char path[128]; snprintf(path, sizeof(path), "%s/%s", dir, val);
      logFileOpen(path);
    }
  });
  storageSubscribeChanges("s.log.file.level", ON_CHANGE {
    logFileLevelMax = parseLevelNum(val);
  });
  storageSubscribeChanges("s.log.file.interval", ON_CHANGE {
    logFileIntervalMs = atoi(val) * 1000;
  });

  /* Register TCP port (non-blocking: retry from main loop so serial log
   * connection is accepted immediately during boot). Browser side reaches
   * us via the webrtc task's generic `<task>:<port>` router — no WS path
   * registration. */
  bool netRegistered = false;

  char buf[512];
  for (;;) {
    pmPollUsb();
    while (itsPoll(pdMS_TO_TICKS(200))) {}

    /* Deferred paste-back: a DC connect was just accepted (logDcConnect) and
     * the client is now past itsConnect and draining. Send the scrollback here,
     * outside the handshake, so a slow SD read can't time out the connect.
     * Runs before the live fan-out so history precedes live lines. */
    for (int i = 0; i < LOG_MAX_CONSUMERS; i++) {
      if (logSlots[i].pastePending && logSlots[i].itsHandle >= 0) {
        logSlots[i].pastePending = false;
        logPasteBack(logSlots[i].itsHandle, logSlots[i].pasteBacklog,
                     logSlots[i].ansi == LOG_ANSI);
      }
    }

    if (!netRegistered) {
      net_port_msg_t reg = {};
      reg.itsPort = LOG_PORT_TCP;
      safeStrncpy(reg.nvsKey, "log_port", sizeof(reg.nvsKey));
      netRegistered = itsSendAux("net", NET_PORT_REG_PORT, &reg, sizeof(reg), 0);
    }

    /* Skip the drain entirely while there are no consumers and no log file.
     * The ring keeps its contents so they fan out the moment a consumer
     * (typically the serial task) connects. Without this, the boot script's
     * logs would silently disappear because the drain happens between hook
     * install and the first consumer's connect being processed. */
    bool hasConsumer = (itsServerActive() > 0);
    bool hasFile = (logFile >= 0);

    /* Drain any inbound bytes from connected consumers — each complete line
     * is fanned out to the OTHER consumers + log file as-is. Allows browser /
     * external clients to inject pre-stamped log lines. */
    if (hasConsumer)
      for (int i = 0; i < LOG_MAX_CONSUMERS; i++) logSlotDrainInbound(i);

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
        /* Non-blocking: drop if buffer full. Stream (TCP) and packet (DC)
           modes are transparent to itsSend here — one call, one logical
           unit (stream bytes / one DC message per line). */
        itsSend(h, out, outLen, 0);
      }
    }
    logFileFlush();
  }
}

/* ---- Init ---- */

/* Module config version. Bump when adding/changing defaults. See duckdns.cpp. */
#define LOG_VERSION 1

static void logInstallDefaults() {
  int v = storageGetInt("s.log.version", 0);
  if (v >= LOG_VERSION) return;

  storageDefaultTree("s.log", R"({
    "level": "info",
    "timestamp": 1,
    "dir": "/sdcard/log",
    "file":   {"name":"", "level":"verbose", "interval":5, "paste":48},
    "colors": {"error":"0;31", "warn":"0;33", "info":"0;32",
               "debug":"0;37", "verbose":"0;90", "timestamp":"0;90"}
  })");

  cronDefault("0 0 * * * A", "logrotate 7");
  storageSet("s.log.version", LOG_VERSION);
}

void logInit() {
  if (logInited) return;
  logInited = true;

  logInstallDefaults();

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
  logTaskHandle = spawnTask(logTaskFn, "log", 6144, nullptr, 1, 1);
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

bool logIsDebug(const char* tag) {
  /* esp_log_level_get returns the per-tag level if explicitly set
   * (via esp_log_level_set, which logApplyLevels does for every
   * `s.log.tag.<tag>` entry in storage); otherwise it falls back to
   * the "*" wildcard which logApplyLevels keeps in sync with the
   * global `s.log.level`. So this resolves "rnsd-specific first,
   * then global" in one call. */
  return tag && esp_log_level_get(tag) >= ESP_LOG_DEBUG;
}

const char* cfd(int fd) {
  static char buf[12];
  if (!logIsDebug()) return "";
  snprintf(buf, sizeof(buf), "{%d} ", fd);
  return buf;
}

/* ---- CLI commands: log, logfile, logrotate ---- */

static void cmdLogfile(const char* a) {
    if (strcmp(a, "help") == 0) {
        cliPrintf("  %-*s show current log-file status\n", CLI_HELP_COL, "logfile");
        cliPrintf("  %-*s enable, today's YYYYMMDD.log\n", CLI_HELP_COL, "logfile on");
        cliPrintf("  %-*s disable\n", CLI_HELP_COL, "logfile off");
        cliPrintf("  %-*s use given filename\n", CLI_HELP_COL, "logfile <name>");
        cliPrintf("  %-*s as above, with level (error/warn/info/debug/verbose)\n",
                  CLI_HELP_COL, "logfile <level> <name|on|off>");
        return;
    }

    /* logfile (no args) — status only */
    if (a[0] == '\0') {
        char name[64], level[16], dir[64];
        storageGetStr("s.log.file.name", name, sizeof(name));
        storageGetStr("s.log.file.level", level, sizeof(level), "verbose");
        storageGetStr("s.log.dir", dir, sizeof(dir), "/sdcard/log");
        if (name[0]) cliPrintf("  %s/%s (%s)\n", dir, name, level);
        else         cliPrintf("  off (level=%s when on)\n", level);
        return;
    }

    /* Parse optional level prefix, then the action token (on/off/<name>) */
    char first[24] = {};
    int i = 0;
    while (a[i] && a[i] != ' ' && i < 23) { first[i] = a[i]; i++; }
    first[i] = '\0';
    const char* rest = a + i;
    while (*rest == ' ') rest++;

    const char* level = nullptr;
    const char* action;
    if (first[0] && isLevelArg(first) && *rest) {
        level = first;
        action = rest;
    } else {
        action = a;
    }

    if (level) storageSet("s.log.file.level", level);

    if (strcmp(action, "off") == 0) {
        storageSet("s.log.file.name", "");
        cliPrintf("  off\n");
        return;
    }

    char name[64];
    if (strcmp(action, "on") == 0) {
        time_t now = time(nullptr);
        struct tm tm;
        localtime_r(&now, &tm);
        snprintf(name, sizeof(name), "%04d%02d%02d.log",
                 tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    } else {
        /* Extract basename if full path given */
        const char* base = strrchr(action, '/');
        safeStrncpy(name, base ? base + 1 : action, sizeof(name));
    }

    storageSet("s.log.file.name", name);
    char dir[64]; storageGetStr("s.log.dir", dir, sizeof(dir), "/sdcard/log");
    char curLevel[16];
    storageGetStr("s.log.file.level", curLevel, sizeof(curLevel), "verbose");
    cliPrintf("  %s/%s (%s)\n", dir, name, curLevel);
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
