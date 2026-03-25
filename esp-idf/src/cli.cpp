/**
 * CLI — command registry, line editor, serial task, CLI task.
 * Split from ipc.cpp.
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

/* ---- CLI command registry ---- */

#define CLI_MAX_CMDS 32

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

/* ---- Command history (shared across serial + network) ---- */
static constexpr int HIST_SIZE = 20;
static char (*histBuf)[128];       /* allocated in PSRAM */
static int histCount = 0;          /* total entries stored */
static int histHead = 0;           /* next write slot (circular) */

static void histAdd(const char* cmd) {
  /* skip if same as last entry */
  int last = (histHead + HIST_SIZE - 1) % HIST_SIZE;
  if (histCount > 0 && strcmp(histBuf[last], cmd) == 0) return;
  strncpy(histBuf[histHead], cmd, sizeof(histBuf[0]) - 1);
  histBuf[histHead][sizeof(histBuf[0]) - 1] = '\0';
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
    char lineBuf[128];
    int lineLen;
} cliSlots[CLI_MAX_CLIENTS];
static int cliActiveSlot = -1;

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
  FILE* f = fopen(path, "r");
  if (!f) { cliPrintf("%s: file not found\n", path); return; }
  char line[128];
  while (fgets(line, sizeof(line), f)) {
    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
      line[--len] = '\0';
    if (len > 0) cliProcess(line);
  }
  fclose(f);
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

/* Tab completion: find longest common prefix of files matching current word */
static void cliTabComplete(cli_edit& e, cli_write_fn write) {
  e.buf[e.len] = '\0';
  /* Find the last word (after last space) */
  const char* wordStart = e.buf;
  for (int i = e.cursor - 1; i >= 0; i--) {
    if (e.buf[i] == ' ') { wordStart = e.buf + i + 1; break; }
  }
  int wordLen = e.buf + e.cursor - wordStart;
  if (wordLen <= 0) return;

  /* Determine directory and prefix */
  char dirPath[128] = "/sdcard";
  char prefix[80] = {};
  const char* lastSlash = nullptr;
  for (const char* p = wordStart; p < wordStart + wordLen; p++)
    if (*p == '/') lastSlash = p;

  if (lastSlash) {
    int dlen = lastSlash - wordStart;
    if (dlen == 0) { strcpy(dirPath, "/"); }
    else { memcpy(dirPath, wordStart, dlen); dirPath[dlen] = '\0'; }
    strncpy(prefix, lastSlash + 1, sizeof(prefix) - 1);
  } else {
    strncpy(prefix, wordStart, wordLen);
    prefix[wordLen] = '\0';
  }

  int prefixLen = strlen(prefix);
  DIR* dir = opendir(dirPath);
  if (!dir) return;

  char match[80] = {};
  int matchCount = 0;
  struct dirent* ent;
  while ((ent = readdir(dir)) != nullptr) {
    if (strncmp(ent->d_name, prefix, prefixLen) != 0) continue;
    matchCount++;
    if (matchCount == 1) {
      strncpy(match, ent->d_name, sizeof(match) - 1);
    } else {
      /* Shorten match to common prefix */
      int i = 0;
      while (match[i] && match[i] == ent->d_name[i]) i++;
      match[i] = '\0';
    }
  }
  closedir(dir);

  if (matchCount == 0 || (int)strlen(match) <= prefixLen) return;

  /* Insert the completion suffix */
  const char* suffix = match + prefixLen;
  int suffixLen = strlen(suffix);
  if (e.len + suffixLen >= (int)sizeof(e.buf) - 1) return;

  /* Insert at cursor */
  memmove(e.buf + e.cursor + suffixLen, e.buf + e.cursor, e.len - e.cursor);
  memcpy(e.buf + e.cursor, suffix, suffixLen);
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

  if (c == '\t') {
    cliTabComplete(e, write);
    return;
  }

  if (c == '\n' || c == '\r') {
    if (e.len > 0) {
      e.buf[e.len] = '\0';
      histAdd(e.buf);
      e.histIdx = -1;
      e.savedValid = false;
      e.len = 0;
      e.cursor = 0;
      write("\r\n", 2);
      cliOut = write;
      cliProcess(e.buf);
      if (ansi) {
        write(RESET, sizeof(RESET) - 1);
        write("$ ", 2);
      }
      write("\033[0 q", 5);
    } else {
      /* Empty enter — kick client (session done) */
      if (cliActiveSlot >= 0 && cliSlots[cliActiveSlot].itsHandle >= 0)
          itsServerKick(cliSlots[cliActiveSlot].itsHandle);
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
  while (*line == ' ') line++;
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

static int cliOnConnect(int handle, int itsPort, const void* data, size_t len) {
  int slot = cliAllocSlot(handle);
  if (slot < 0) return -1;
  auto& cl = cliSlots[slot];
  cl.edit = {};
  cl.lineLen = 0;
  cl.ws = false;
  if (len >= sizeof(net_connect_t) && ((const net_connect_t*)data)->ws) {
    if (!wsUpgrade(handle)) return -1;
    cl.ws = true;
    cl.mode = CLI_LINE;  /* WS clients are line-mode */
  } else if (len >= sizeof(cli_connect_t)) {
    cl.mode = ((const cli_connect_t*)data)->mode;
  } else {
    cl.mode = CLI_LINE;
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
  itsServerInit(CLI_MAX_CLIENTS, 512, 2048);
  itsServerOnConnect(cliOnConnect);
  itsServerOnDisconnect(cliOnDisconnect);

  /* Register TCP port with network */
  { net_port_msg_t reg = {};
    reg.itsPort = 8081;  /* convention for cli */
    strncpy(reg.nvsKey, "cli_port", sizeof(reg.nvsKey));
    while (!itsSendAux("net", &reg, sizeof(reg), pdMS_TO_TICKS(500)))
      vTaskDelay(pdMS_TO_TICKS(100));
  }
  /* Register WS path with web */
  { web_path_msg_t reg = {};
    reg.itsPort = 8081;
    strncpy(reg.path, "cli", sizeof(reg.path));
    while (!itsSendAux("web", &reg, sizeof(reg), pdMS_TO_TICKS(500)))
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
        else if (op < 0) { itsServerKick(h); continue; }
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
    logHandle = itsConnect("log", 0, &req, sizeof(req), pdMS_TO_TICKS(500));
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
        cli_connect_t req = { CLI_ANSI };
        cliHandle = itsConnect("cli", 0, &req, sizeof(req), pdMS_TO_TICKS(500));
        if (cliHandle >= 0) {
          printf("\n\nCLI mode. Press enter on empty line to resume log.\n\n$ ");
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
        buf[n] = '\0';
        printf("%s", buf);
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
        buf[n] = '\0';
        printf("%s", buf);
      }
      fflush(stdout);
      cliFlush();
      /* Check if CLI kicked us */
      if (!itsConnected(cliHandle)) {
        cliHandle = -1;
        printf("\r\033[K\nExiting CLI, resuming log.\n\n");
        fflush(stdout);
        cliFlush();
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
