/**
 * CLI — command registry, line editor, serial task, CLI task.
 */
#include "cli.h"
#include "log.h"
#include "its.h"
#include "web.h"
#include "net.h"
#include "storage.h"
#include "cron.h"
#include "compat.h"
#include <driver/usb_serial_jtag.h>
#include <hal/usb_serial_jtag_ll.h>
#include <esp_heap_caps.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

/* ---- CLI command registry ---- */

#define CLI_MAX_CMDS 48

struct cli_cmd_entry_t {
    const char* cmd;
    cli_cmd_cb_t cb;
    int cmdLen;
};

static cli_cmd_entry_t cliCmds[CLI_MAX_CMDS];
static int cliCmdCount = 0;

void cliRegisterCmd(const char* cmd, cli_cmd_cb_t cb) {
    if (cliCmdCount >= CLI_MAX_CMDS) return;
    /* Insert sorted alphabetically */
    int pos = 0;
    while (pos < cliCmdCount && strcmp(cliCmds[pos].cmd, cmd) < 0) pos++;
    if (pos < cliCmdCount)
        memmove(&cliCmds[pos + 1], &cliCmds[pos], (cliCmdCount - pos) * sizeof(cli_cmd_entry_t));
    cliCmds[pos].cmd = cmd;
    cliCmds[pos].cb = cb;
    cliCmds[pos].cmdLen = strlen(cmd);
    cliCmdCount++;
}

/* ---- CLI output routing ---- */

#define CYAN  "\033[36m"
#define RESET "\033[0m"

typedef void (*cli_write_fn)(const char* data, size_t len);

static void cliFlush() {
  fflush(stdout);
  usb_serial_jtag_ll_txfifo_flush();
}

static void cronCliWrite(const char* data, size_t len);  /* forward */

static cli_write_fn cliOut = nullptr;

void cliPrintf(const char* fmt, ...) {
    if (!cliOut) return;
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) cliOut(buf, (size_t)n);
}

void cliWrite(const char* data, size_t len) {
    if (!cliOut || !data || !len) return;
    while (len > 0) {
        size_t chunk = len > 512 ? 512 : len;
        cliOut(data, chunk);
        data += chunk;
        len -= chunk;
    }
}

/* ---- Command history (shared across serial + network) ---- */
static constexpr int HIST_SIZE = 20;
static char (*histBuf)[128];       /* allocated in PSRAM */
static int histCount = 0;          /* total entries stored */
static int histHead = 0;           /* next write slot (circular) */

static void histAdd(const char* cmd) {
  /* skip if same as last entry */
  int last = (histHead + HIST_SIZE - 1) % HIST_SIZE;
  if (histCount > 0 && strcmp(histBuf[last], cmd) == 0) return;
  safeStrncpy(histBuf[histHead], cmd, sizeof(histBuf[0]));
  histHead = (histHead + 1) % HIST_SIZE;
  if (histCount < HIST_SIZE) histCount++;
}

/* Map browse index (0 = newest) to circular buffer index */
static const char* histGet(int browseIdx) {
  if (browseIdx < 0 || browseIdx >= histCount) return nullptr;
  int idx = (histHead - 1 - browseIdx + HIST_SIZE * 2) % HIST_SIZE;
  return histBuf[idx];
}

struct cli_edit {
    char buf[128];
    char saved[128];    /* saved current line when browsing history */
    int len = 0;
    int cursor = 0;
    int escState = 0;
    int histIdx = -1;   /* -1 = not browsing, 0 = newest, 1 = next older... */
    bool savedValid = false;
};

/* CLI ITS server: per-slot state */
#define CLI_MAX_CLIENTS 4
static struct cli_slot_t {
    int itsHandle;
    cli_edit edit;
    cli_mode_t mode;
    bool ws;
    bool usbSerial;
    char lineBuf[128];
    int lineLen;
    char cwd[256];
} cliSlots[CLI_MAX_CLIENTS];

/** USB serial reconnects after each command when sticky=0 — keep cwd across sessions */
static char cliUsbPersistCwd[256];

/** Serial task disconnects CLI and reconnects log without "Exiting CLI" banner. */
static volatile bool cliUsbSerialAutoResumeLog = false;
static int cliActiveSlot = -1;

/** Collapse /, ., .. in an absolute path in place. */
static bool cliCollapseAbsolute(char* path, size_t cap) {
    size_t inLen = strlen(path);
    if (inLen == 0 || path[0] != '/' || inLen >= cap) return false;
    char work[256];
    safeStrncpy(work, path, sizeof(work));
    char out[256];
    size_t olen = 1;
    out[0] = '/';
    out[1] = '\0';
    const char* p = work + 1;
    while (*p) {
        const char* tok = p;
        while (*p && *p != '/') p++;
        size_t tl = (size_t)(p - tok);
        while (*p == '/') p++;
        if (tl == 0) continue;
        if (tl == 1 && tok[0] == '.') continue;
        if (tl == 2 && tok[0] == '.' && tok[1] == '.') {
            if (olen <= 1) continue;
            olen--;
            while (olen > 1 && out[olen - 1] != '/') olen--;
            if (olen > 1) olen--;
            out[olen] = '\0';
            continue;
        }
        if (olen > 1) {
            if (olen + 1 >= sizeof(out)) return false;
            out[olen++] = '/';
        }
        if (olen + tl + 1 >= sizeof(out)) return false;
        memcpy(out + olen, tok, tl);
        olen += tl;
        out[olen] = '\0';
    }
    if (olen + 1 > cap) return false;
    memcpy(path, out, olen + 1);
    return true;
}

/** Resolved s.cli.start_dir — absolute, normalized, existing directory (else /sdcard). */
static void cliResolvedStartDir(char* out, size_t outLen) {
    char d[256];
    storageGetStr("s.cli.start_dir", d, sizeof(d), "/sdcard");
    if (d[0] != '/') safeStrncpy(d, "/sdcard", sizeof(d));
    char tmp[256];
    safeStrncpy(tmp, d, sizeof(tmp));
    if (!cliCollapseAbsolute(tmp, sizeof(tmp))) safeStrncpy(tmp, "/sdcard", sizeof(tmp));
    struct stat st;
    if (stat(tmp, &st) != 0 || !S_ISDIR(st.st_mode)) safeStrncpy(tmp, "/sdcard", sizeof(tmp));
    safeStrncpy(out, tmp, outLen);
}

static void cliApplyStartDir(cli_slot_t& cl) { cliResolvedStartDir(cl.cwd, sizeof(cl.cwd)); }

void cliCdToStartDir() {
    if (cliActiveSlot < 0 || cliActiveSlot >= CLI_MAX_CLIENTS) return;
    cliApplyStartDir(cliSlots[cliActiveSlot]);
    if (cliSlots[cliActiveSlot].usbSerial)
        safeStrncpy(cliUsbPersistCwd, cliSlots[cliActiveSlot].cwd, sizeof(cliUsbPersistCwd));
}

void cliGetCwd(char* out, size_t outLen) {
    if (cliActiveSlot >= 0 && cliActiveSlot < CLI_MAX_CLIENTS && cliSlots[cliActiveSlot].itsHandle >= 0 &&
        cliSlots[cliActiveSlot].cwd[0] == '/')
        safeStrncpy(out, cliSlots[cliActiveSlot].cwd, outLen);
    else
        cliResolvedStartDir(out, outLen);
}

bool cliSetCwd(const char* absolutePath) {
    if (cliActiveSlot < 0 || cliActiveSlot >= CLI_MAX_CLIENTS) return false;
    char tmp[256];
    safeStrncpy(tmp, absolutePath, sizeof(tmp));
    if (tmp[0] != '/') return false;
    if (!cliCollapseAbsolute(tmp, sizeof(tmp))) return false;
    /* VFS root: stat("/") often fails on ESP-IDF though /fixed, /state, … exist */
    if (tmp[0] == '/' && tmp[1] == '\0') {
        safeStrncpy(cliSlots[cliActiveSlot].cwd, "/", sizeof(cliSlots[0].cwd));
        if (cliSlots[cliActiveSlot].usbSerial) safeStrncpy(cliUsbPersistCwd, "/", sizeof(cliUsbPersistCwd));
        return true;
    }
    struct stat st;
    if (stat(tmp, &st) != 0 || !S_ISDIR(st.st_mode)) return false;
    safeStrncpy(cliSlots[cliActiveSlot].cwd, tmp, sizeof(cliSlots[0].cwd));
    if (cliSlots[cliActiveSlot].usbSerial) safeStrncpy(cliUsbPersistCwd, tmp, sizeof(cliUsbPersistCwd));
    return true;
}

bool cliResolveFsPath(const char* userPath, char* out, size_t outLen) {
    char cwd[256];
    cliGetCwd(cwd, sizeof(cwd));
    char work[320];
    if (!userPath || !*userPath) {
        safeStrncpy(out, cwd, outLen);
        return true;
    }
    if (userPath[0] == '/') {
        if (snprintf(work, sizeof(work), "%s", userPath) >= (int)sizeof(work)) return false;
    } else {
        if (snprintf(work, sizeof(work), "%s/%s", cwd, userPath) >= (int)sizeof(work)) return false;
    }
    if (!cliCollapseAbsolute(work, sizeof(work))) return false;
    if (strlen(work) >= outLen) return false;
    safeStrncpy(out, work, outLen);
    return true;
}

static int cliAllocSlot(int itsHandle) {
    for (int i = 0; i < CLI_MAX_CLIENTS; i++)
        if (cliSlots[i].itsHandle < 0) {
            cliSlots[i].itsHandle = itsHandle;
            return i;
        }
    return -1;
}

static void itsCliWrite(const char* data, size_t len) {
    if (cliActiveSlot < 0) return;
    int h = cliSlots[cliActiveSlot].itsHandle;
    if (h < 0) return;
    if (cliSlots[cliActiveSlot].ws) {
        wsSendText(h, data, len);
    } else {
        while (len > 0) {
            size_t sent = itsSend(h, data, len, pdMS_TO_TICKS(100));
            if (sent == 0) break;
            data += sent;
            len -= sent;
        }
    }
}

static bool cliIsAnsi() {
    return cliActiveSlot >= 0 &&
           cliSlots[cliActiveSlot].mode == CLI_ANSI;
}

/** True if trimmed line ends with ';' — one-shot stay in CLI when s.cli.sticky is 0. */
static bool cliLineOneShotSticky(const char* line) {
  const char* p = line;
  size_t n = strlen(p);
  while (n > 0 && (p[n - 1] == ' ' || p[n - 1] == '\t')) n--;
  return n > 0 && p[n - 1] == ';';
}

/* Redraw buffer from position 'from' to end, clear rest, restore cursor */
static void cliEditRefresh(cli_edit& e, int from, cli_write_fn write) {
  char tmp[192];
  int n = 0;
  int count = e.len - from;
  if (count > 0 && count <= (int)sizeof(tmp) - 20) {
    memcpy(tmp, e.buf + from, count);
    n += count;
  }
  n += snprintf(tmp + n, sizeof(tmp) - n, "\033[K");
  int back = e.len - e.cursor;
  if (back > 0) n += snprintf(tmp + n, sizeof(tmp) - n, "\033[%dD", back);
  write(tmp, n);
}

void cliRunFile(const char* path) {
  int f = storageFopen(path, "r");
  if (f < 0) return;  /* silent if file doesn't exist (e.g. optional net_up) */
  /* Log only via info() — printf/cliFlush on USB races the log task and garbles lines.
   * Message is "cli: …" so the line reads once as [task] + cli: (no nested [cli]). */
  info("cli: run %s\n", path);
  char buf[128];
  size_t linePos = 0;
  for (;;) {
    size_t n = storageFread(buf + linePos, sizeof(buf) - linePos - 1, f);
    if (n == 0 && linePos == 0) break;
    size_t end = linePos + n;
    buf[end] = '\0';
    /* Process complete lines */
    size_t start = 0;
    for (size_t i = 0; i < end; i++) {
      if (buf[i] == '\n' || buf[i] == '\r') {
        buf[i] = '\0';
        if (i > start) {
          const char* ln = buf + start;
          while (*ln == ' ') ln++;
          if (*ln && *ln != '#') {
            info("cli: %s\n", ln);
            vTaskDelay(pdMS_TO_TICKS(50)); /* let log task drain under WiFi/boot burst */
          }
          cliProcess(buf + start);
        }
        start = i + 1;
      }
    }
    /* Keep partial line for next read */
    if (start < end) {
      linePos = end - start;
      memmove(buf, buf + start, linePos);
    } else {
      linePos = 0;
    }
    if (n == 0) {
      /* EOF — process remaining partial line */
      if (linePos > 0) {
        buf[linePos] = '\0';
        const char* ln = buf;
        while (*ln == ' ') ln++;
        if (*ln && *ln != '#') {
          info("cli: %s\n", ln);
          vTaskDelay(pdMS_TO_TICKS(50));
        }
        cliProcess(buf);
      }
      break;
    }
  }
  storageFclose(f);
  info("cli: end %s\n", path);
}

static void cronCliWrite(const char* data, size_t len) {
  info("cron: %.*s", (int)len, data);
}

/* ---- Line editor helpers ---- */

static constexpr int PROMPT_LEN = 2;  /* "$ " */

/* Replace entire edit buffer, redraw line */
static void cliEditReplace(cli_edit& e, const char* text, cli_write_fn write) {
  bool ansi = cliIsAnsi();
  /* Move cursor to start of input (after prompt), clear line */
  if (e.cursor > 0) {
    char tmp[16]; int n = snprintf(tmp, sizeof(tmp), "\033[%dD", e.cursor);
    write(tmp, n);
  }
  write("\033[K", 3);
  int newLen = strlen(text);
  if (newLen > (int)sizeof(e.buf) - 1) newLen = sizeof(e.buf) - 1;
  memcpy(e.buf, text, newLen);
  e.buf[newLen] = '\0';
  e.len = newLen;
  e.cursor = newLen;
  if (newLen > 0) {
    if (ansi) write(CYAN, sizeof(CYAN) - 1);
    write(e.buf, newLen);
  } else {
    if (ansi) write(RESET, sizeof(RESET) - 1);
  }
}

/** Commands whose arguments include filesystem paths (tab-complete files/dirs). */
static bool cliCmdWantsFileArgs(int cmdIdx) {
  if (cmdIdx < 0) return false;
  const char* c = cliCmds[cmdIdx].cmd;
  static const char* const fs[] = {"ls", "cd", "mkdir", "rm", "cat", "df", "run", "logfile", nullptr};
  for (int i = 0; fs[i]; i++)
    if (strcmp(c, fs[i]) == 0) return true;
  return false;
}

/** Longest registered command match at start of line (after spaces); returns index or -1. */
static int cliLongestCmdMatch(const char* line, int* matchedLen) {
  while (*line == ' ') line++;
  int bestIdx = -1, bestLen = 0;
  for (int i = 0; i < cliCmdCount; i++) {
    auto& en = cliCmds[i];
    if (strncmp(line, en.cmd, en.cmdLen) == 0 && (line[en.cmdLen] == '\0' || line[en.cmdLen] == ' ')) {
      if (en.cmdLen > bestLen) {
        bestIdx = i;
        bestLen = en.cmdLen;
      }
    }
  }
  *matchedLen = bestLen;
  return bestIdx;
}

/* Tab completion: files when past a path-taking command; dirname via cwd / resolved path */
static void cliTabComplete(cli_edit& e, cli_write_fn write) {
  e.buf[e.len] = '\0';
  const char* wordStart = e.buf;
  for (int i = e.cursor - 1; i >= 0; i--) {
    if (e.buf[i] == ' ') {
      wordStart = e.buf + i + 1;
      break;
    }
  }
  int wordLen = (int)(e.buf + e.cursor - wordStart);
  if (wordLen < 0) return;

  const char* line0 = e.buf;
  while (*line0 == ' ') line0++;

  int cmdLen = 0;
  int cmdIdx = cliLongestCmdMatch(line0, &cmdLen);
  if (!cliCmdWantsFileArgs(cmdIdx)) return;
  int cmdEndIdx = (int)(line0 - e.buf) + cmdLen;
  if (e.cursor <= cmdEndIdx) return;
  /* Option token (e.g. rm -rf, ls -la, mkdir -p) */
  if (wordLen > 0 && wordStart[0] == '-') return;

  char dirPath[256];
  char prefix[128];
  const char* lastSlash = nullptr;
  for (const char* p = wordStart; p < wordStart + wordLen; p++)
    if (*p == '/') lastSlash = p;

  if (lastSlash) {
    char dirSeg[256];
    size_t dlen = (size_t)(lastSlash - wordStart);
    if (dlen >= sizeof(dirSeg)) return;
    memcpy(dirSeg, wordStart, dlen);
    dirSeg[dlen] = '\0';
    if (!cliResolveFsPath(dirSeg[0] ? dirSeg : ".", dirPath, sizeof(dirPath))) return;
    size_t pfxLen = (size_t)(wordStart + wordLen - (lastSlash + 1));
    if (pfxLen >= sizeof(prefix)) return;
    memcpy(prefix, lastSlash + 1, pfxLen);
    prefix[pfxLen] = '\0';
  } else {
    cliGetCwd(dirPath, sizeof(dirPath));
    if ((size_t)wordLen >= sizeof(prefix)) return;
    memcpy(prefix, wordStart, (size_t)wordLen);
    prefix[wordLen] = '\0';
  }

  size_t prefixLen = strlen(prefix);
  DIR* dir = opendir(dirPath);
  if (!dir) return;

  char match[128] = {};
  int matchCount = 0;
  struct dirent* ent;
  while ((ent = readdir(dir)) != nullptr) {
    if (strncmp(ent->d_name, prefix, prefixLen) != 0) continue;
    matchCount++;
    if (matchCount == 1) {
      safeStrncpy(match, ent->d_name, sizeof(match));
    } else {
      int i = 0;
      while (match[i] && match[i] == ent->d_name[i]) i++;
      match[i] = '\0';
    }
  }
  closedir(dir);

  if (matchCount == 0 || strlen(match) <= prefixLen) return;

  const char* suffix = match + prefixLen;
  int suffixLen = (int)strlen(suffix);
  if (e.len + suffixLen >= (int)sizeof(e.buf) - 1) return;

  memmove(e.buf + e.cursor + suffixLen, e.buf + e.cursor, e.len - e.cursor);
  memcpy(e.buf + e.cursor, suffix, (size_t)suffixLen);
  e.len += suffixLen;
  e.cursor += suffixLen;
  cliEditRefresh(e, e.cursor - suffixLen, write);
}

/* ---- Line editor ---- */

static void cliEditChar(cli_edit& e, char c, cli_write_fn write) {
  bool ansi = cliIsAnsi();

  /* Escape sequence state machine (arrow keys) */
  if (e.escState == 1) {
    e.escState = (c == '[') ? 2 : 0;
    return;
  }
  if (e.escState == 2) {
    e.escState = 0;
    if (c == 'D' && e.cursor > 0) {
      e.cursor--;
      write("\033[D\033[2 q", 8);
    } else if (c == 'C' && e.cursor < e.len) {
      e.cursor++;
      if (e.cursor == e.len)
        write("\033[C\033[0 q", 8);
      else
        write("\033[C", 3);
    } else if (c == 'A') {
      int next = e.histIdx + 1;
      const char* h = histGet(next);
      if (h) {
        if (e.histIdx < 0) {
          memcpy(e.saved, e.buf, e.len);
          e.saved[e.len] = '\0';
          e.savedValid = true;
        }
        e.histIdx = next;
        cliEditReplace(e, h, write);
      }
    } else if (c == 'B') {
      if (e.histIdx > 0) {
        e.histIdx--;
        const char* h = histGet(e.histIdx);
        if (h) cliEditReplace(e, h, write);
      } else if (e.histIdx == 0) {
        e.histIdx = -1;
        cliEditReplace(e, e.savedValid ? e.saved : "", write);
        e.savedValid = false;
      }
    }
    return;
  }
  if (c == '\033') { e.escState = 1; return; }

  /* ^A / ^E — beginning / end of line (readline) */
  if (c == 0x01) {
    if (e.cursor > 0) {
      char tmp[16];
      int n = snprintf(tmp, sizeof(tmp), "\033[%dD", e.cursor);
      write(tmp, n);
      e.cursor = 0;
      write("\033[0 q", 5);
    }
    return;
  }
  if (c == 0x05) {
    if (e.cursor < e.len) {
      char tmp[16];
      int n = snprintf(tmp, sizeof(tmp), "\033[%dC", e.len - e.cursor);
      write(tmp, n);
      e.cursor = e.len;
      write("\033[0 q", 5);
    }
    return;
  }

  if (c == '\t') {
    cliTabComplete(e, write);
    return;
  }

  if (c == '\n' || c == '\r') {
    if (e.len > 0) {
      e.buf[e.len] = '\0';
      histAdd(e.buf);
      char lineCopy[128];
      safeStrncpy(lineCopy, e.buf, sizeof(lineCopy));
      e.len = 0;
      e.cursor = 0;
      e.buf[0] = '\0';
      e.histIdx = -1;
      e.savedValid = false;
      bool stayCli = true;
      if (cliActiveSlot >= 0 && cliSlots[cliActiveSlot].usbSerial) {
        bool sticky = storageGetInt("s.cli.sticky", 0) != 0;
        stayCli = sticky || cliLineOneShotSticky(lineCopy);
      }
      write("\r\n", 2);
      cliOut = write;
      cliProcess(lineCopy);
      if (stayCli) {
        if (ansi) {
          write(RESET, sizeof(RESET) - 1);
          write("$ ", 2);
        }
        write("\033[0 q", 5);
      } else {
        /* Reset ANSI before handoff to log; command output already ends with newline */
        write(RESET, sizeof(RESET) - 1);
        cliUsbSerialAutoResumeLog = true;
      }
    } else {
      /* Empty enter */
      if (cliActiveSlot >= 0 && cliSlots[cliActiveSlot].usbSerial) {
        /* Serial: kick back to log view */
        if (cliSlots[cliActiveSlot].itsHandle >= 0)
          itsDisconnect(cliSlots[cliActiveSlot].itsHandle);
      } else if (ansi) {
        /* WS/TCP ANSI: just re-prompt */
        write("\r\n$ ", 4);
        write("\033[0 q", 5);
      }
    }
  } else if (c == 0x7F || c == 0x08) {
    if (e.cursor > 0) {
      memmove(e.buf + e.cursor - 1, e.buf + e.cursor, e.len - e.cursor);
      e.len--;
      e.cursor--;
      if (e.len == 0 && ansi) {
        char tmp[16];
        int n = snprintf(tmp, sizeof(tmp), "\033[%dG\033[K", PROMPT_LEN + 1);
        write(RESET, sizeof(RESET) - 1);
        write(tmp, n);
        write("\033[0 q", 5);
      } else {
        write("\033[D", 3);
        cliEditRefresh(e, e.cursor, write);
      }
    }
  } else if (c >= 0x20 && e.len < (int)sizeof(e.buf) - 1) {
    if (e.len == 0 && ansi)
      write(CYAN, sizeof(CYAN) - 1);
    memmove(e.buf + e.cursor + 1, e.buf + e.cursor, e.len - e.cursor);
    e.buf[e.cursor] = c;
    e.len++;
    e.cursor++;
    cliEditRefresh(e, e.cursor - 1, write);
  }
}

/* ---- CLI command dispatcher ---- */

void cliProcess(const char* line) {
  char trimmed[128];
  while (*line == ' ') line++;
  safeStrncpy(trimmed, line, sizeof(trimmed));
  size_t tl = strlen(trimmed);
  while (tl > 0 && (trimmed[tl - 1] == '\r' || trimmed[tl - 1] == '\n' || trimmed[tl - 1] == ' ' ||
                    trimmed[tl - 1] == '\t'))
    trimmed[--tl] = '\0';
  line = trimmed;
  if (*line == '#' || *line == '\0') return;
  /* Semicolon: split and execute each part */
  const char* semi = strchr(line, ';');
  if (semi) {
    char part[128];
    size_t plen = semi - line;
    if (plen >= sizeof(part)) plen = sizeof(part) - 1;
    memcpy(part, line, plen);
    part[plen] = '\0';
    cliProcess(part);
    cliProcess(semi + 1);
    return;
  }
  /* Try registered commands (longest match first) */
  { int bestIdx = -1, bestLen = 0;
    for (int i = 0; i < cliCmdCount; i++) {
      auto& e = cliCmds[i];
      if (strncmp(line, e.cmd, e.cmdLen) == 0 &&
          (line[e.cmdLen] == '\0' || line[e.cmdLen] == ' ') &&
          e.cmdLen > bestLen) {
        bestIdx = i; bestLen = e.cmdLen;
      }
    }
    if (bestIdx >= 0) {
      const char* args = line + bestLen;
      while (*args == ' ') args++;
      cliCmds[bestIdx].cb(args);
      return;
    }
  }
  if (*line) cliPrintf("%s: unknown command. Type \"help\" for help.\n", line);
}

/* ---- External CLI command init functions ---- */
extern void cliCmdFsInit();
extern void cliCmdSysInit();
extern void pmRegisterCmds();
extern void logRegisterCmds();

static void cliBuiltinInit() {
    storageRegisterCmds();
    cliCmdFsInit();
    cliCmdSysInit();
    pmRegisterCmds();
    logRegisterCmds();
    cliRegisterCmd("help", [](const char* a) {
        if (strcmp(a, "help") == 0) { cliPrintf("  %-*s show commands\n", CLI_HELP_COL, "help [<cmd>]"); return; }
        if (*a) {
            for (int i = 0; i < cliCmdCount; i++)
                if (strcmp(cliCmds[i].cmd, a) == 0) { cliCmds[i].cb("help"); return; }
            cliPrintf("unknown command: %s\n", a);
        } else {
            for (int i = 0; i < cliCmdCount; i++) cliCmds[i].cb("help");
        }
    });
}

/* ---- CLI ITS server callbacks ---- */

static int cliOnConnect(int handle, const void* data, size_t len) {
  int slot = cliAllocSlot(handle);
  if (slot < 0) return -1;
  auto& cl = cliSlots[slot];
  cl.edit = {};
  cl.lineLen = 0;
  cl.ws = false;
  cl.usbSerial = false;
  if (len >= sizeof(net_connect_t) && ((const net_connect_t*)data)->ws) {
    if (!wsUpgrade(handle)) return -1;
    cl.ws = true;
    cl.mode = CLI_ANSI;  /* WS clients get full terminal (xterm.js) */
  } else if (len >= sizeof(net_connect_t)) {
    /* Raw TCP/TLS from net — payload is net_connect_t, not cli_connect_t */
    cl.mode = CLI_LINE;
    cl.usbSerial = false;
  } else if (len >= sizeof(cli_connect_t)) {
    const auto* cc = (const cli_connect_t*)data;
    cl.mode = cc->mode;
    cl.usbSerial = cc->from_usb_serial != 0;
  } else if (len >= 1) {
    cl.mode = *(const cli_mode_t*)data;
    cl.usbSerial = false;
  } else {
    cl.mode = CLI_LINE;
    cl.usbSerial = false;
  }

  if (cl.usbSerial) {
    if (cliUsbPersistCwd[0] == '/') {
      bool rootOnly = (cliUsbPersistCwd[1] == '\0');
      struct stat st;
      if (rootOnly || (stat(cliUsbPersistCwd, &st) == 0 && S_ISDIR(st.st_mode)))
        safeStrncpy(cl.cwd, cliUsbPersistCwd, sizeof(cl.cwd));
      else
        cliApplyStartDir(cl);
    } else
      cliApplyStartDir(cl);
    safeStrncpy(cliUsbPersistCwd, cl.cwd, sizeof(cliUsbPersistCwd));
  } else
    cliApplyStartDir(cl);

  /* Send initial prompt for ANSI clients */
  if (cl.mode == CLI_ANSI && !cl.usbSerial) {
    cliActiveSlot = slot;
    itsCliWrite("$ ", 2);
    itsCliWrite("\033[0 q", 5);
    cliActiveSlot = -1;
  }

  return slot;
}

static void cliOnDisconnect(int ref) {
  if (ref >= 0 && ref < CLI_MAX_CLIENTS) {
    cliSlots[ref] = {};
    cliSlots[ref].itsHandle = -1;
  }
}

/* ---- CLI task ---- */

static TaskHandle_t cliTaskHandle = NULL;

static void cliTaskFn(void* arg) {
  for (int i = 0; i < CLI_MAX_CLIENTS; i++) cliSlots[i].itsHandle = -1;
  itsServerInit();
  itsServerPortOpen(CLI_PORT, CLI_MAX_CLIENTS, 512, 2048);
  itsServerOnConnect(CLI_PORT, cliOnConnect);
  itsServerOnDisconnect(CLI_PORT, cliOnDisconnect);

  /* Register TCP port with network */
  { net_port_msg_t reg = {};
    reg.itsPort = CLI_PORT;
    safeStrncpy(reg.nvsKey, "cli_port", sizeof(reg.nvsKey));
    while (!itsSendAux("net", NET_PORT_REG_PORT, &reg, sizeof(reg), pdMS_TO_TICKS(500)))
      vTaskDelay(pdMS_TO_TICKS(100));
  }
  /* Register WS path with web */
  { web_path_msg_t reg = {};
    reg.itsPort = CLI_PORT;
    safeStrncpy(reg.path, "cli", sizeof(reg.path));
    while (!itsSendAux("web", WEB_PATH_REG_PORT, &reg, sizeof(reg), pdMS_TO_TICKS(500)))
      vTaskDelay(pdMS_TO_TICKS(100));
  }

  cliBuiltinInit();  /* register built-in CLI commands */

  for (;;) {
    while (itsPoll(pdMS_TO_TICKS(50))) {}

    /* Process each active slot */
    char buf[128];
    for (int s = 0; s < CLI_MAX_CLIENTS; s++) {
      int h = cliSlots[s].itsHandle;
      if (h < 0 || !itsConnected(h)) continue;
      size_t n = 0;
      if (cliSlots[s].ws) {
        size_t outLen = 0;
        int op = wsReadFrame(h, (uint8_t*)buf, sizeof(buf), &outLen);
        if (op > 0) n = outLen;
        else if (op < 0) { itsDisconnect(h); continue; }
      } else {
        n = itsRecv(h, buf, sizeof(buf), 0);
      }
      if (n == 0) continue;
      cliActiveSlot = s;
      auto& cl = cliSlots[s];
      if (cl.mode == CLI_ANSI) {
        for (size_t i = 0; i < n; i++)
          cliEditChar(cl.edit, buf[i], itsCliWrite);
      } else {
        /* LINE mode: buffer until newline */
        for (size_t i = 0; i < n; i++) {
          char c = buf[i];
          if (c == '\n' || c == '\r') {
            cl.lineBuf[cl.lineLen] = '\0';
            if (cl.lineLen > 0) {
              cliOut = itsCliWrite;
              cliProcess(cl.lineBuf);
            }
            cl.lineLen = 0;
          } else if (cl.lineLen < (int)sizeof(cl.lineBuf) - 1) {
            cl.lineBuf[cl.lineLen++] = c;
          }
        }
      }
      cliActiveSlot = -1;
    }

    /* Cron commands */
    cliOut = cronCliWrite;
    cronDrainCommands();
  }
}

/* ---- Serial task: byte shuttle between serial port and log/CLI ---- */

/** USB serial: normalize newlines so raw LF/CR don't confuse host terminals */
static void serialEmit(const char* p, size_t n) {
  for (size_t i = 0; i < n; i++) {
    if (p[i] == '\r') {
      printf("\r");
      if (i + 1 < n && p[i + 1] == '\n') {
        i++;
        printf("\n");
      } else
        printf("\n");
    } else if (p[i] == '\n')
      printf("\r\n");
    else
      printf("%c", p[i]);
  }
}

static void serialTaskFn(void* arg) {
  /* Set stdin non-blocking */
  int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
  fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);

  itsClientInit(2);

  int logHandle = -1;
  int cliHandle = -1;

  /* Connect to log */
  auto connectLog = [&]() {
    log_connect_t req = { LOG_ANSI };
    logHandle = itsConnect("log", LOG_PORT, &req, sizeof(req), pdMS_TO_TICKS(500));
  };
  connectLog();

  for (;;) {
    while (itsPoll(pdMS_TO_TICKS(50))) {}

    /* Poll serial input */
    char c;
    while (read(STDIN_FILENO, &c, 1) == 1) {
      if (cliHandle < 0 && c != '\n' && c != '\r') {
        /* Switch from log to CLI */
        if (logHandle >= 0) { itsDisconnect(logHandle); logHandle = -1; }
        cli_connect_t req = { CLI_ANSI, 1 };
        cliHandle = itsConnect("cli", CLI_PORT, &req, sizeof(req), pdMS_TO_TICKS(500));
        if (cliHandle >= 0) {
          printf("\033[0m");
          if (storageGetInt("s.cli.sticky", 0) != 0)
            printf("\r\n\r\nCLI mode. Press enter on empty line to resume log.\r\n\r\n$ ");
          else
            printf("\r\n$ ");
          fflush(stdout); cliFlush();
        }
      }
      if (cliHandle >= 0)
        itsSend(cliHandle, &c, 1, 0);
    }

    /* Drain log → serial */
    if (logHandle >= 0) {
      char buf[256];
      for (;;) {
        size_t n = itsRecv(logHandle, buf, sizeof(buf) - 1, 0);
        if (n == 0) break;
        serialEmit(buf, n);
      }
      fflush(stdout);
      cliFlush();
    }

    /* Drain CLI → serial */
    if (cliHandle >= 0) {
      char buf[256];
      for (;;) {
        size_t n = itsRecv(cliHandle, buf, sizeof(buf) - 1, 0);
        if (n == 0) break;
        serialEmit(buf, n);
      }
      fflush(stdout);
      cliFlush();
      /* Check if CLI kicked us */
      if (!itsConnected(cliHandle)) {
        cliHandle = -1;
        printf("\033[0m\r\033[K\r\nExiting CLI, resuming log.\r\n\r\n");
        fflush(stdout);
        cliFlush();
        connectLog();
      }
    }

    /* After draining CLI: auto-resume log (must run after itsRecv so command output is not dropped) */
    if (cliUsbSerialAutoResumeLog) {
      cliUsbSerialAutoResumeLog = false;
      if (cliHandle >= 0) {
        printf("\033[0m");
        fflush(stdout);
        cliFlush();
        char drain[512];
        for (;;) {
          size_t m = itsRecv(cliHandle, drain, sizeof(drain) - 1, pdMS_TO_TICKS(30));
          if (m == 0) break;
          serialEmit(drain, m);
        }
        fflush(stdout);
        cliFlush();
        itsDisconnect(cliHandle);
        cliHandle = -1;
        connectLog();
      }
    }

    /* Retry log connection if not connected (boot ordering) */
    if (logHandle < 0 && cliHandle < 0)
      connectLog();
  }
}

/* ---- Init ---- */

void cliInit() {
  /* Allocate history buffer in PSRAM */
  histBuf = (char(*)[128])heap_caps_calloc(HIST_SIZE, 128, MALLOC_CAP_SPIRAM);

  xTaskCreatePinnedToCore(cliTaskFn, "cli", 6144, NULL, 1, &cliTaskHandle, 1);
  static TaskHandle_t serialTaskHandle = NULL;
  xTaskCreatePinnedToCoreWithCaps(serialTaskFn, "serial", 3072, NULL, 1, &serialTaskHandle, 1, MALLOC_CAP_SPIRAM);
}
