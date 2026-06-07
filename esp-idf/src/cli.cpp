/**
 * CLI — command registry, line editor, serial task, CLI task.
 */
#include "cli.h"
#include "fs.h"
#include "log.h"
#include "its.h"
#include "storage.h"
#include "cron.h"
#include "compat.h"
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
#include <driver/usb_serial_jtag.h>
#include <driver/usb_serial_jtag_vfs.h>
#include <hal/usb_serial_jtag_ll.h>
#endif
#include <esp_heap_caps.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <vector>
#include <dirent.h>
#include <sys/stat.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

/* ---- CLI command registry ----
 *
 * Was a fixed cli_cmd_entry_t[48] that silently dropped registrations
 * on overflow — once the table filled (rnsd-side consumers push it past
 * 48) the later `lxmf`/`tcp`/`lora` verbs vanished with no warning. Now
 * an unbounded vector, kept sorted on insert so `help` stays
 * alphabetical (the old behaviour). `cmd` is still an un-owned
 * `const char*`: every caller passes a string literal, stable forever.
 *
 * Construct-on-first-use: registration runs from module *Init() on the
 * main task before the CLI task exists (see the concurrency note in
 * cliTaskMain), so a function-local static is provably safe against
 * static-init ordering rather than incidentally so. Indices returned by
 * cliLongestCmdMatch are consumed immediately during dispatch; all
 * registration must remain init-time — a later insert reallocates and
 * would invalidate a held index. */

struct cli_cmd_entry_t {
    const char* cmd;
    cli_cmd_cb_t cb;
    int cmdLen;
};

static std::vector<cli_cmd_entry_t>& cliCmds() {
    static std::vector<cli_cmd_entry_t> v;
    return v;
}

bool cliWantsHelp(const char* args) {
    return strcmp(args, "help") == 0 || strcmp(args, "-h") == 0 || strcmp(args, "--help") == 0;
}

void cliRegisterCmd(const char* cmd, cli_cmd_cb_t cb) {
    auto& cmds = cliCmds();
    /* Insert sorted alphabetically (keeps `help` output ordered). */
    size_t pos = 0;
    while (pos < cmds.size() && strcmp(cmds[pos].cmd, cmd) < 0) pos++;
    cmds.insert(cmds.begin() + pos,
                cli_cmd_entry_t{ cmd, cb, (int)strlen(cmd) });
}

/* ---- CLI output routing ---- */

#define CYAN  "\033[36m"
#define RESET "\033[0m"

typedef void (*cli_write_fn)(const char* data, size_t len);

static void cliFlush() {
  fflush(stdout);
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
  /* USB Serial JTAG echo needs a TX FIFO flush — fflush(stdout) only pushes
   * into the USB FIFO, but bytes don't actually leave until either a newline
   * or this explicit flush. UART has no equivalent (writes leave immediately). */
  usb_serial_jtag_ll_txfifo_flush();
#endif
}

static void cronCliWrite(const char* data, size_t len);  /* forward */

static cli_write_fn cliOut = nullptr;

int cliPrintf(const char* fmt, ...) {
    if (!cliOut) return 0;
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) {
        size_t w = (size_t)n < sizeof(buf) ? (size_t)n : sizeof(buf) - 1;
        cliOut(buf, w);
    }
    return n;
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
#define CLI_MAX_CLIENTS 8    /* up to 6 TCP + 2 DC (browser session + on-device CLI) */
static struct cli_slot_t {
    int itsHandle;
    cli_edit edit;
    cli_mode_t mode;
    bool usbSerial;
    bool color;        /* emit ANSI color escapes (CLI_ANSI input echo) */
    bool noPrompt;     /* suppress connect-time prompt (one-shot exec clients) */
    char lineBuf[128];
    int lineLen;
    int cols, rows;    /* client terminal size (0 = unknown → 80x24); for ssh pty-req */
    char cwd[256];
    /* True when the slot wants to disconnect once its outgoing stream is
     * fully drained. Set by trailing-';' (LINE / ANSI hangup) and by serial
     * empty-enter. The main loop polls itsSendIsEmpty and tears the
     * connection down when it goes true — avoids the itsSendDrain blocking
     * race that lost bytes on the wire. */
    bool pendingClose;
} cliSlots[CLI_MAX_CLIENTS];

/** USB serial reconnects after each command when sticky=0 — keep cwd across sessions */
static char cliUsbPersistCwd[256];

/** Serial task disconnects CLI and reconnects log without "Exiting CLI" banner. */
static volatile bool cliUsbSerialAutoResumeLog = false;
static int cliActiveSlot = -1;

/* Defined further down (extern "C" so log.cpp can read it). Declared here so
 * the line editor's ';' handler can flip it before the command runs. */
extern "C" volatile bool serialInCli;

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

/** Resolved s.cli.start_dir — absolute, normalized, existing directory (else "/"). */
static void cliResolvedStartDir(char* out, size_t outLen) {
    char d[256];
    storageGetStr("s.cli.start_dir", d, sizeof(d), "/");
    if (d[0] != '/') safeStrncpy(d, "/", sizeof(d));
    char tmp[256];
    safeStrncpy(tmp, d, sizeof(tmp));
    if (!cliCollapseAbsolute(tmp, sizeof(tmp))) safeStrncpy(tmp, "/", sizeof(tmp));
    struct stat st;
    if (fs_stat(tmp, &st) != 0 || !S_ISDIR(st.st_mode)) safeStrncpy(tmp, "/", sizeof(tmp));
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
    if (fs_stat(tmp, &st) != 0 || !S_ISDIR(st.st_mode)) return false;
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

static void itsSendAll(int h, const char* data, size_t len) {
    while (len > 0) {
        size_t sent = itsSend(h, data, len, pdMS_TO_TICKS(100));
        if (sent == 0) break;
        data += sent;
        len -= sent;
    }
}

static void itsCliWrite(const char* data, size_t len) {
    if (cliActiveSlot < 0) return;
    auto& cl = cliSlots[cliActiveSlot];
    int h = cl.itsHandle;
    if (h < 0) return;
    /* SSH raw-PTY (ANSI mode, non-serial) has no ONLCR layer — lone '\n'
     * staircases. Translate to "\r\n" in-flight. Serial users have tio's
     * ONLCR; LINE mode (browser xterm, ssh exec) goes to a terminal whose
     * own ONLCR handles it. So only the ANSI+non-serial path needs CRLF. */
    bool addCr = cl.mode == CLI_ANSI && !cl.usbSerial;
    /* Stream-mode (TCP/serial) may partial-send under back-pressure; retry
       with a short timeout so verbose commands aren't truncated. Packet-mode
       (DC) either delivers the whole body or returns 0 — itsSendAll handles
       both: one non-zero return closes the packet, zero means try again. */
    if (!addCr) { itsSendAll(h, data, len); return; }
    /* Walk the buffer, flushing runs that don't contain a bare '\n', and
     * substituting "\r\n" for each one we find. A '\n' preceded by '\r' in
     * the same write is already CRLF and passes through unchanged. */
    const char* start = data;
    for (size_t i = 0; i < len; i++) {
        if (data[i] != '\n') continue;
        if (i > 0 && data[i - 1] == '\r') continue;
        if (i > (size_t)(start - data))
            itsSendAll(h, start, data + i - start);
        itsSendAll(h, "\r\n", 2);
        start = data + i + 1;
    }
    if ((size_t)(start - data) < len)
        itsSendAll(h, start, len - (size_t)(start - data));
}

/* Blocking single-line input read from the active slot — see cli.h. Runs on
 * the cli task inside a command callback; the main loop is parked in the
 * callback, so we read the slot's ITS handle directly (the same byte stream
 * the line editor consumes) and echo through cliWrite. */
int cliReadLine(char* out, size_t outLen, cli_echo_t echo) {
    if (!out || outLen == 0) return -1;
    out[0] = '\0';
    if (cliActiveSlot < 0 || cliActiveSlot >= CLI_MAX_CLIENTS) return -1;
    int h = cliSlots[cliActiveSlot].itsHandle;
    if (h < 0) return -1;
    size_t len = 0;
    /* Read into a full-size buffer: packet-mode slots (browser DataChannel)
     * deliver one whole packet body per itsRecv and DROP it if maxLen is
     * smaller than the body — reading 1 byte at a time would lose every
     * keystroke burst and spin forever. Size matches the main loop's buf. */
    char rb[128];
    /* Safety cap so a walked-away session can't wedge the cli task (and thus
     * every other CLI slot) indefinitely. */
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(90000);
    /* Escape-sequence skip state (persists across reads): 0 = normal,
     * 1 = just saw ESC, 2 = inside a CSI/SS3 sequence. Strips cursor keys,
     * function keys, and bracketed-paste markers (ESC [200~ … ESC [201~) so a
     * typed OR pasted secret isn't polluted by the printable tail of an escape
     * sequence (the '[', digits and '~' are >= 0x20 and would otherwise be
     * captured as input). */
    int esc = 0;
    for (;;) {
        if ((long)(deadline - xTaskGetTickCount()) <= 0) { out[0] = '\0'; return -1; }
        /* Keep pumping the cli task's inbox while we're parked here. The accept
         * path (processInboxMsg → cliDcConnect) only runs from itsPoll, and the
         * main loop is stuck in this command — so without this, every new
         * connection (browser/webrtc xterm, on-device LCD CLI; both dial cli:1,
         * 2 DC slots) goes unacked for the whole prompt and is rejected. itsPoll
         * dispatches connects/disconnects/data only — it runs no command — so
         * this is not reentrant. It also wakes on this slot's own input. */
        while (itsPoll(0)) {}
        size_t n = itsRecv(h, rb, sizeof(rb), 0);
        if (n == 0) {
            if (!itsConnected(h)) { out[0] = '\0'; return -1; }
            itsPoll(pdMS_TO_TICKS(200));   /* sleep until an inbox event or 200ms */
            continue;
        }
        for (size_t i = 0; i < n; i++) {
            char c = rb[i];
            if (esc == 1) { esc = (c == '[' || c == 'O') ? 2 : 0; continue; }
            if (esc == 2) { if ((unsigned char)c >= 0x40 && (unsigned char)c <= 0x7e) esc = 0; continue; }
            if (c == 0x1b) { esc = 1; continue; }   /* ESC — start of an escape seq */
            if (c == '\r' || c == '\n') { cliWrite("\r\n", 2); out[len] = '\0'; return (int)len; }
            if (c == 0x03) { cliWrite("\r\n", 2); out[0] = '\0'; return -1; }   /* Ctrl-C */
            if (c == 0x04) { if (len == 0) { out[0] = '\0'; return -1; } continue; } /* Ctrl-D */
            if (c == 0x7f || c == 0x08) {  /* backspace / DEL */
                if (len > 0) { len--; if (echo != CLI_ECHO_NONE) cliWrite("\b \b", 3); }
                continue;
            }
            if ((unsigned char)c < 0x20) continue;  /* ignore other control chars */
            if (len < outLen - 1) {
                out[len++] = c;
                if (echo == CLI_ECHO) cliWrite(&c, 1);
                else if (echo == CLI_ECHO_STARS) cliWrite("*", 1);
            }
        }
    }
}

/* Raw passthrough read from the active slot — see cli.h. Same direct-handle
 * read as cliReadLine but verbatim: no echo, no editing, no escape stripping,
 * and a caller-chosen (short) timeout so an interactive relay can poll input
 * and stream output on the same task without wedging on either. */
int cliReadRaw(char* out, size_t outLen, int timeoutMs) {
    if (!out || outLen == 0) return -1;
    if (cliActiveSlot < 0 || cliActiveSlot >= CLI_MAX_CLIENTS) return -1;
    int h = cliSlots[cliActiveSlot].itsHandle;
    if (h < 0) return -1;
    /* Same reasoning as cliReadLine: an interactive relay (live ssh shell) parks
     * the cli main loop in this command for its whole run, so service the inbox
     * here or new connections are rejected the entire session. Grab buffered
     * input first, else block in itsPoll (wakes on data OR a connect) up to the
     * caller's timeout, then drain any further pending connects. */
    while (itsPoll(0)) {}
    size_t n = itsRecv(h, out, outLen, 0);
    if (n == 0) {
        itsPoll(pdMS_TO_TICKS(timeoutMs));
        while (itsPoll(0)) {}
        n = itsRecv(h, out, outLen, 0);
    }
    if (n == 0) return itsConnected(h) ? 0 : -1;
    return (int)n;
}

/* Active slot's reported terminal size (see cli.h). 80x24 when unknown. */
void cliTermSize(int* cols, int* rows) {
    int c = 80, r = 24;
    if (cliActiveSlot >= 0 && cliActiveSlot < CLI_MAX_CLIENTS) {
        if (cliSlots[cliActiveSlot].cols > 0) c = cliSlots[cliActiveSlot].cols;
        if (cliSlots[cliActiveSlot].rows > 0) r = cliSlots[cliActiveSlot].rows;
    }
    if (cols) *cols = c;
    if (rows) *rows = r;
}

static bool cliIsAnsi() {
    return cliActiveSlot >= 0 &&
           cliSlots[cliActiveSlot].mode == CLI_ANSI;
}

/* True iff the active slot wants ANSI *color* (color flag set + ANSI mode).
 * Public (declared in cli.h) so commands can colorize their own output. */
bool cliWantsColor() {
    return cliActiveSlot >= 0 &&
           cliSlots[cliActiveSlot].mode == CLI_ANSI &&
           cliSlots[cliActiveSlot].color;
}

/* Write a color escape only when the active slot wants color; a no-op
 * otherwise. Cursor/line-edit sequences are written directly (never gated). */
static void cliColorWrite(cli_write_fn write, const char* seq, size_t len) {
    if (cliWantsColor()) write(seq, len);
}

/* Mark the active CLI slot for shutdown. Serial gets a "Returning to log"
 * banner first (via the caller). The main loop tears the ITS handle down
 * once the slot's outgoing stream is fully drained — no race-prone
 * itsSendDrain-then-disconnect inline. */
static void cliRequestSessionEnd() {
    if (cliActiveSlot >= 0) cliSlots[cliActiveSlot].pendingClose = true;
}

/** True if trimmed line ends with ';' — serial CLI's "run this and switch
 *  back to log output" signal. */
static bool cliLineEndsWithSemicolon(const char* line) {
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
  int f = fs_open(path, "r");
  if (f < 0) return;  /* silent if file doesn't exist (e.g. optional net_up) */
  /* Log only via info() — printf/cliFlush on USB races the log task and garbles lines.
   * Message is "cli: …" so the line reads once as [task] + cli: (no nested [cli]). */
  info("cli: run %s\n", path);
  char buf[128];
  size_t linePos = 0;
  for (;;) {
    size_t n = fs_read(buf + linePos, 1, sizeof(buf) - linePos - 1, f);
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
  fs_close(f);
  info("cli: end %s\n", path);
}

static void cronCliWrite(const char* data, size_t len) {
  info("cron: %.*s", (int)len, data);
}

/* ---- Line editor helpers ---- */

/** Build "<hostname> $ " into out; returns visible length. Hostname comes from
 *  s.net.hostname (netInit defaults it to CONFIG_SPANGAP_PROJECT_NAME on first
 *  boot — e.g. "reticulous", "seccam"). Re-read every prompt so a live
 *  `set s.net.hostname=…` is reflected immediately. The fallback only matters
 *  if storage is unreadable before netInit has run; we use the same project
 *  name so the prompt is never the misleading platform name "spangap". */
static int cliPromptBuild(char* out, size_t outSize) {
  char host[48];
  storageGetStr("s.net.hostname", host, sizeof(host), CONFIG_SPANGAP_FW_HOSTNAME);
  if (!host[0]) safeStrncpy(host, CONFIG_SPANGAP_FW_HOSTNAME, sizeof(host));
  int n = snprintf(out, outSize, "%s $ ", host);
  return n;
}

static void cliWritePrompt(cli_write_fn write) {
  char buf[64];
  int n = cliPromptBuild(buf, sizeof(buf));
  if (n <= 0) return;
  /* "host $ " — color the hostname bold-green, leave the " $ " separator plain.
   * The escapes are gated by cliColorWrite and have zero visible width, so the
   * line-editor's cursor math (cliPromptLen, which counts only buf) is unchanged. */
  int hostLen = n > 3 ? n - 3 : n;   /* trailing " $ " is 3 chars */
  cliColorWrite(write, CLI_C_HOST, sizeof(CLI_C_HOST) - 1);
  write(buf, hostLen);
  cliColorWrite(write, CLI_C_RESET, sizeof(CLI_C_RESET) - 1);
  if (n > hostLen) write(buf + hostLen, n - hostLen);
}

static int cliPromptLen() {
  char buf[64];
  return cliPromptBuild(buf, sizeof(buf));
}

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
    if (ansi) cliColorWrite(write, CYAN, sizeof(CYAN) - 1);
    write(e.buf, newLen);
  } else {
    if (ansi) cliColorWrite(write, RESET, sizeof(RESET) - 1);
  }
}

/** Commands whose arguments include filesystem paths (tab-complete files/dirs). */
static bool cliCmdWantsFileArgs(int cmdIdx) {
  if (cmdIdx < 0) return false;
  const char* c = cliCmds()[cmdIdx].cmd;
  static const char* const fs[] = {"ls", "cd", "mkdir", "rm", "cat", "df", "run", "logfile", nullptr};
  for (int i = 0; fs[i]; i++)
    if (strcmp(c, fs[i]) == 0) return true;
  return false;
}

/** Longest registered command match at start of line (after spaces); returns index or -1. */
static int cliLongestCmdMatch(const char* line, int* matchedLen) {
  while (*line == ' ') line++;
  int bestIdx = -1, bestLen = 0;
  auto& cmds = cliCmds();
  for (size_t i = 0; i < cmds.size(); i++) {
    auto& en = cmds[i];
    if (strncmp(line, en.cmd, en.cmdLen) == 0 && (line[en.cmdLen] == '\0' || line[en.cmdLen] == ' ')) {
      if (en.cmdLen > bestLen) {
        bestIdx = (int)i;
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
  constexpr int MAX = 128;
  auto* listing = (fs_listing_t*)heap_caps_malloc(MAX * sizeof(fs_listing_t), MALLOC_CAP_SPIRAM);
  if (!listing) return;
  int n = fs_listdir(dirPath, listing, MAX);
  char match[128] = {};
  int matchCount = 0;
  for (int i = 0; i < n; i++) {
    const char* name = listing[i].name;
    if (strncmp(name, prefix, prefixLen) != 0) continue;
    matchCount++;
    if (matchCount == 1) {
      safeStrncpy(match, name, sizeof(match));
    } else {
      int j = 0;
      while (match[j] && match[j] == name[j]) j++;
      match[j] = '\0';
    }
  }
  heap_caps_free(listing);

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

  /* ^D — end-of-input. Always closes (serial: returns to log; TCP/SSH/WS:
   * disconnects). Any in-progress line is discarded — readline-style
   * "forward-delete" isn't worth the complexity, and a user who hits ^D
   * usually wants to leave, not edit. */
  if (c == 0x04) {
    if (cliActiveSlot >= 0 && cliSlots[cliActiveSlot].usbSerial) {
      cliColorWrite(write, RESET, sizeof(RESET) - 1);
      write("\r\n\r\nReturning to log\r\n\r\n", 23);
    } else if (ansi) {
      write("\r\n", 2);
    }
    cliRequestSessionEnd();
    return;
  }

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
      /* Trailing ';' is the "run this and disconnect" signal:
       *   serial → return to log view (handoff to log task)
       *   anything else (TCP / WS / DC / SSH exec) → close the ITS connection
       * Lines without ';' stay connected (interactive). */
      bool stayCli = true;
      bool resumeLog = false;
      bool itsHangup = false;
      if (cliActiveSlot >= 0 && cliLineEndsWithSemicolon(lineCopy)) {
        stayCli = false;
        if (cliSlots[cliActiveSlot].usbSerial) resumeLog = true;
        else                                    itsHangup = true;
      }
      write("\r\n", 2);
      if (resumeLog) {
        /* Trailing ';' on serial: brief overwrite-style "Returning to log"
         * before the command runs, so the user knows the view switched. */
        cliColorWrite(write, RESET, sizeof(RESET) - 1);
        write("\rReturning to log\r\r", 17);
        serialInCli = false;
      }
      cliOut = write;
      cliProcess(lineCopy);
      /* If the just-run command (e.g. `exit`) requested session end, treat
       * it like a hangup — skip the prompt redraw. */
      if (stayCli && cliActiveSlot >= 0 && cliSlots[cliActiveSlot].pendingClose)
        stayCli = false;
      if (stayCli) {
        if (ansi) {
          cliColorWrite(write, RESET, sizeof(RESET) - 1);
          cliWritePrompt(write);
        }
        write("\033[0 q", 5);
      } else {
        /* Reset ANSI before handoff to log; command output already ends with newline */
        cliColorWrite(write, RESET, sizeof(RESET) - 1);
        if (resumeLog) cliUsbSerialAutoResumeLog = true;
        if (itsHangup) {
          /* Don't disconnect inline — let the main loop tear down once the
           * outbound stream has actually drained. itsSendDrain-here races
           * with the recv task and truncates verbose command output. */
          cliSlots[cliActiveSlot].pendingClose = true;
        }
      }
    } else {
      /* Empty enter */
      if (cliActiveSlot >= 0 && cliSlots[cliActiveSlot].usbSerial) {
        /* Serial: announce the switch then kick back to log view. The main
         * loop disconnects once the banner has drained. */
        cliColorWrite(write, RESET, sizeof(RESET) - 1);
        write("\r\n\r\nReturning to log\r\n\r\n", 23);
        cliRequestSessionEnd();
      } else if (ansi) {
        /* WS/TCP ANSI: just re-prompt */
        write("\r\n", 2);
        cliWritePrompt(write);
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
        int n = snprintf(tmp, sizeof(tmp), "\033[%dG\033[K", cliPromptLen() + 1);
        cliColorWrite(write, RESET, sizeof(RESET) - 1);
        write(tmp, n);
        write("\033[0 q", 5);
      } else {
        write("\033[D", 3);
        cliEditRefresh(e, e.cursor, write);
      }
    }
  } else if (c >= 0x20 && e.len < (int)sizeof(e.buf) - 1) {
    if (e.len == 0 && ansi)
      cliColorWrite(write, CYAN, sizeof(CYAN) - 1);
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
  /* Alias expansion: if the first word names a stored alias
   * (s.cli.aliases.<word>), replace it with the alias value and append
   * everything after the word as extra arguments. Done once — the expansion
   * falls straight into command dispatch below, never back through cliProcess,
   * so aliases are NOT recursively re-expanded (an alias whose value begins
   * with another alias name runs that command, it doesn't chain). */
  char expanded[384];
  {
    const char* sp = line;
    while (*sp && *sp != ' ') sp++;
    size_t wlen = (size_t)(sp - line);
    char key[80];
    if (wlen > 0 && (size_t)snprintf(key, sizeof(key), "s.cli.aliases.%.*s", (int)wlen, line) < sizeof(key) &&
        storageExists(key)) {
      char val[192];
      storageGetStr(key, val, sizeof(val), "");
      /* sp points at the first space or the terminating NUL — appending it
       * verbatim carries the original argument text (with its leading space). */
      snprintf(expanded, sizeof(expanded), "%s%s", val, sp);
      line = expanded;
    }
  }
  /* Try registered commands (longest match first) */
  { int bestIdx = -1, bestLen = 0;
    auto& cmds = cliCmds();
    for (size_t i = 0; i < cmds.size(); i++) {
      auto& e = cmds[i];
      if (strncmp(line, e.cmd, e.cmdLen) == 0 &&
          (line[e.cmdLen] == '\0' || line[e.cmdLen] == ' ') &&
          e.cmdLen > bestLen) {
        bestIdx = (int)i; bestLen = e.cmdLen;
      }
    }
    if (bestIdx >= 0) {
      const char* args = line + bestLen;
      while (*args == ' ') args++;
      cmds[bestIdx].cb(args);
      return;
    }
  }
  if (*line) cliPrintf("%s: unknown command. Type \"help\" for help.\n", line);
}

/* ---- alias / unalias ---- */

#define CLI_ALIAS_PREFIX "s.cli.aliases."

/* storageForEach callback — can't capture, so it prints directly. Strips the
 * s.cli.aliases. prefix so the listing shows the bare alias name. */
static bool cliAliasListedAny;
static void cliAliasPrint(const char* key, const char* val) {
  const char* name = strncmp(key, CLI_ALIAS_PREFIX, sizeof(CLI_ALIAS_PREFIX) - 1) == 0
                         ? key + sizeof(CLI_ALIAS_PREFIX) - 1 : key;
  cliPrintf("alias %s %s\n", name, val);
  cliAliasListedAny = true;
}

static void cmdAlias(const char* a) {
  if (cliWantsHelp(a)) {
    cliPrintf("%-*s define/list command aliases\n", CLI_HELP_COL, "alias [<name> <cmd>]");
    return;
  }
  /* Bare `alias` → list all defined aliases. */
  if (!*a) {
    cliAliasListedAny = false;
    storageForEach(CLI_ALIAS_PREFIX, cliAliasPrint);
    if (!cliAliasListedAny) cliPrintf("(no aliases)\n");
    return;
  }
  /* First word = alias name; everything after the following space = value. */
  const char* sp = a;
  while (*sp && *sp != ' ') sp++;
  size_t nlen = (size_t)(sp - a);
  char key[80];
  /* The name becomes a dot-notation storage key segment under
   * s.cli.aliases.<name>, so a '.' would nest the alias into config and the
   * first-word lookup in cliProcess could never address it — reject it. */
  if (memchr(a, '.', nlen)) { cliPrintf("alias: name may not contain '.'\n"); return; }
  if (nlen == 0 || (size_t)snprintf(key, sizeof(key), CLI_ALIAS_PREFIX "%.*s", (int)nlen, a) >= sizeof(key)) {
    cliPrintf("alias: bad name\n");
    return;
  }
  while (*sp == ' ') sp++;
  /* `alias <name>` with no value → show that one alias. */
  if (!*sp) {
    if (storageExists(key)) {
      char val[192];
      storageGetStr(key, val, sizeof(val), "");
      cliPrintf("alias %.*s %s\n", (int)nlen, a, val);
    } else {
      cliPrintf("alias: %.*s not set\n", (int)nlen, a);
    }
    return;
  }
  storageSet(key, sp);
}

static void cmdUnalias(const char* a) {
  if (cliWantsHelp(a)) {
    cliPrintf("%-*s remove a command alias\n", CLI_HELP_COL, "unalias <name>");
    return;
  }
  /* Name is the first word only (ignore any trailing junk). */
  const char* sp = a;
  while (*sp && *sp != ' ') sp++;
  size_t nlen = (size_t)(sp - a);
  char key[80];
  if (nlen == 0 || (size_t)snprintf(key, sizeof(key), CLI_ALIAS_PREFIX "%.*s", (int)nlen, a) >= sizeof(key)) {
    cliPrintf("usage: unalias <name>\n");
    return;
  }
  if (!storageExists(key)) { cliPrintf("unalias: %.*s not set\n", (int)nlen, a); return; }
  storageUnset(key);
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
    cliRegisterCmd("alias", cmdAlias);
    cliRegisterCmd("unalias", cmdUnalias);
    cliRegisterCmd("exit", [](const char* a) {
        if (cliWantsHelp(a)) { cliPrintf("%-*s end this CLI session\n", CLI_HELP_COL, "exit"); return; }
        if (cliActiveSlot >= 0 && cliSlots[cliActiveSlot].usbSerial) {
            cliPrintf("\r\n\r\nReturning to log\r\n\r\n");
        }
        cliRequestSessionEnd();
    });
    cliRegisterCmd("help", [](const char* a) {
        if (strcmp(a, "help") == 0) { cliPrintf("%-*s list commands\n", CLI_HELP_COL, "help [<cmd>]"); return; }
        auto& cmds = cliCmds();
        /* `help <cmd>` → that command's fuller (-h) help. */
        if (*a && strcmp(a, "-h") != 0 && strcmp(a, "--help") != 0) {
            for (auto& e : cmds)
                if (strcmp(e.cmd, a) == 0) { e.cb("-h"); return; }
            cliPrintf("unknown command: %s\n", a);
            return;
        }
        /* Bare `help` / `help -h` → one line per command. The banner appears
         * once here, not in every command's one-liner. */
        cliPrintf("Type '<command> -h' for more on any command.\n\n");
        for (auto& e : cmds) e.cb("help");
    });
}

/* ---- CLI ITS server callbacks ---- */

/** Shared slot finalisation after mode/usbSerial are set. */
static void cliInitSlot(cli_slot_t& cl, int slot) {
  if (cl.usbSerial) {
    if (cliUsbPersistCwd[0] == '/') {
      bool rootOnly = (cliUsbPersistCwd[1] == '\0');
      struct stat st;
      if (rootOnly || (fs_stat(cliUsbPersistCwd, &st) == 0 && S_ISDIR(st.st_mode)))
        safeStrncpy(cl.cwd, cliUsbPersistCwd, sizeof(cl.cwd));
      else
        cliApplyStartDir(cl);
    } else
      cliApplyStartDir(cl);
    safeStrncpy(cliUsbPersistCwd, cl.cwd, sizeof(cliUsbPersistCwd));
  } else
    cliApplyStartDir(cl);

  /* Send initial prompt for ANSI clients (non-serial) and for TCP LINE
   * clients (so a scripted nc client can read-until-prompt rather than
   * relying on timeouts). */
  if (cl.mode == CLI_ANSI && !cl.usbSerial) {
    cliActiveSlot = slot;
    cliWritePrompt(itsCliWrite);
    itsCliWrite("\033[0 q", 5);
    cliActiveSlot = -1;
  } else if (cl.mode == CLI_LINE && !cl.usbSerial && !cl.noPrompt) {
    cliActiveSlot = slot;
    cliWritePrompt(itsCliWrite);
    cliActiveSlot = -1;
  }
}

/** TCP (stream mode): net-forwarded TCP/TLS client (LINE mode), or the
 *  on-device serial task sending a cli_connect_t. */
static int cliTcpConnect(int handle, const void* data, size_t len) {
  int slot = cliAllocSlot(handle);
  if (slot < 0) return -1;
  auto& cl = cliSlots[slot];
  cl.edit = {};
  cl.lineLen = 0;
  cl.usbSerial = false;
  cl.color = true;          /* default on; a cli_connect_t may opt out */
  cl.noPrompt = false;      /* default: send the connect-time prompt */
  if (len == sizeof(cli_connect_t)) {
    const auto* cc = (const cli_connect_t*)data;
    cl.mode = cc->mode;
    cl.usbSerial = cc->from_usb_serial != 0;
    cl.color = (cc->color == CLI_COLOR);
    cl.noPrompt = cc->no_prompt != 0;
  } else if (len >= 1 && len < sizeof(cli_connect_t)) {
    cl.mode = *(const cli_mode_t*)data;
  } else {
    /* Empty, or a larger connect descriptor from a forwarder (net sends its
     * own net_connect_t on raw TCP/TLS) — line-oriented, no echo. The CLI
     * doesn't decode the forwarder's struct; it only needs LINE mode. */
    cl.mode = CLI_LINE;
  }
  cliInitSlot(cl, slot);
  return slot;
}

/** DC (packet mode): browser xterm.js. LINE mode — the browser does its own
 *  line editing and echo (see TerminalWindow.vue) and sends one finished
 *  command per newline; the device echoes nothing and just executes lines. */
static int cliDcConnect(int handle, const void* data, size_t len) {
  int slot = cliAllocSlot(handle);
  if (slot < 0) return -1;
  auto& cl = cliSlots[slot];
  cl.edit = {};
  cl.lineLen = 0;
  /* Optional connect payload "colsxrows" (e.g. "64x26") reports the client's
   * terminal size, used for the ssh pty-req. Defaults to 80x24 if absent. */
  cl.cols = 80; cl.rows = 24;
  if (data && len > 0 && len < 16) {
    char b[16]; memcpy(b, data, len); b[len] = '\0';
    int cc = 0, rr = 0;
    if (sscanf(b, "%dx%d", &cc, &rr) == 2 && cc > 0 && rr > 0) { cl.cols = cc; cl.rows = rr; }
  }
  cl.usbSerial = false;
  /* CLI_ANSI: the device owns echo + line editing + history, emitting ANSI the
   * client just renders. Clients (browser xterm, on-device LCD app) are dumb
   * terminals that echo nothing locally — so there's no double-echo, and an
   * interactive ssh shell needs no mode toggle (the remote pty echoes through).
   * The LCD can't render the cursor-addressing escapes, but strips them. */
  cl.color = true;          /* xterm renders colour; the LCD strips it */
  cl.noPrompt = false;
  cl.mode = CLI_ANSI;
  cliInitSlot(cl, slot);
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
  /* History buffer in PSRAM, allocated in task context so heap tracking
     attributes it to cli, not the main task that spawned us. */
  histBuf = (char(*)[128])heap_caps_calloc(HIST_SIZE, 128, MALLOC_CAP_SPIRAM);
  for (int i = 0; i < CLI_MAX_CLIENTS; i++) cliSlots[i].itsHandle = -1;
  itsServerInit();
  /* CLI commands sometimes need to itsConnect outwards (e.g. rnprobe → rnsd
   * RNSD_PORT_PACKET). Mark this task as a client too. 2 slots = current
   * command + headroom. */
  itsClientInit(2);
  /* Two ports because the transports frame differently — TCP/serial is a byte
   * stream (packetBased=false), the WebRTC DataChannel is message-oriented
   * (packetBased=true) — and a port has one framing mode, so they can't share.
   * Shared CLI_MAX_CLIENTS=8 slot pool — 6 TCP + 2 DC. DC has only two possible
   * consumers (the single browser webrtc session + the on-device CLI), so it's
   * capped at 2; the headroom goes to TCP (nc debug + sshd-in backends). The two
   * caps sum to the pool, so DC's 2 stay guaranteed even under a TCP flood. */
  itsServerPortOpen(CLI_PORT_TCP, /*packetBased=*/false, 6, 512, 2048);
  itsServerOnConnect(CLI_PORT_TCP, cliTcpConnect);
  itsServerOnDisconnect(CLI_PORT_TCP, cliOnDisconnect);
  itsServerPortOpen(CLI_PORT_DC,  /*packetBased=*/true,  2, 512, 2048);
  itsServerOnConnect(CLI_PORT_DC, cliDcConnect);
  itsServerOnDisconnect(CLI_PORT_DC, cliOnDisconnect);

  /* Builtin commands are registered on main task in cliInit() (before this
   * task exists) — cliRegisterCmd is unlocked and every module's Init()
   * registers commands on main task's context, so the main task's serial
   * flow is the one safe context. Registering them here would race. */

  /* The CLI's TCP endpoint (raw `nc` access, sshd-in backends) is exposed by
   * spangap-net, which registers CLI_PORT_TCP against this task on its behalf
   * — the core CLI knows nothing about TCP. A net-less image simply has no
   * TCP listener; serial, the browser DataChannel (cli:1), and the on-device
   * terminal reach the CLI directly over ITS regardless. */

  for (;;) {
    while (itsPoll(pdMS_TO_TICKS(50))) {}

    /* Process each active slot. Stream and packet modes both deliver bytes
       via itsRecv — packet mode returns exactly one message body per call,
       stream mode whatever's accumulated. The line editor doesn't care
       about message boundaries. */
    char buf[128];
    for (int s = 0; s < CLI_MAX_CLIENTS; s++) {
      int h = cliSlots[s].itsHandle;
      if (h < 0 || !itsConnected(h)) continue;
      size_t n = itsRecv(h, buf, sizeof(buf), 0);
      if (n == 0) continue;
      cliActiveSlot = s;
      auto& cl = cliSlots[s];
      if (cl.mode == CLI_ANSI) {
        for (size_t i = 0; i < n; i++)
          cliEditChar(cl.edit, buf[i], itsCliWrite);
      } else {
        /* LINE mode: buffer until newline. Trailing ';' is the "run this
         * and disconnect" signal (same convention serial uses for handing
         * back to log; see the line-editor path above). Used by ssh exec
         * to get one-shot command semantics. */
        bool hangup = false;
        for (size_t i = 0; i < n; i++) {
          char c = buf[i];
          if (c == 0x04) {  /* ^D — EOF, disconnect */
            hangup = true;
            break;
          }
          if (c == '\n' || c == '\r') {
            cl.lineBuf[cl.lineLen] = '\0';
            hangup = cliLineEndsWithSemicolon(cl.lineBuf);
            if (cl.lineLen > 0) {
              cliOut = itsCliWrite;
              cliProcess(cl.lineBuf);
            }
            cl.lineLen = 0;
            if (hangup) break;
            cliWritePrompt(itsCliWrite);
          } else if (cl.lineLen < (int)sizeof(cl.lineBuf) - 1) {
            cl.lineBuf[cl.lineLen++] = c;
          }
        }
        if (hangup) {
          /* Defer disconnect to the main loop (see pendingClose handling)
           * so output drains fully without racing the recv task. */
          cl.pendingClose = true;
        }
      }
      cliActiveSlot = -1;
    }

    /* Deferred-close sweep: any slot that asked to close (via trailing ';'
     * or serial empty-enter) gets torn down once its outgoing stream has
     * fully drained. itsSendIsEmpty mirrors itsIsEmpty on the peer's recv
     * direction, so this fires the instant the SSH/web/log consumer has
     * read the last byte of command output.
     *
     * ITS disconnect callbacks only fire on the REMOTE end of a closed
     * connection — when we close locally, cli's own onDisconnect doesn't
     * fire, so the slot has to be reset inline here or it leaks (and
     * after CLI_MAX_CLIENTS leaks, new sessions get "shell request failed"). */
    for (int s = 0; s < CLI_MAX_CLIENTS; s++) {
      if (!cliSlots[s].pendingClose) continue;
      int h = cliSlots[s].itsHandle;
      if (h < 0) { cliSlots[s].pendingClose = false; continue; }
      if (itsSendIsEmpty(h)) {
        itsDisconnect(h);
        cliSlots[s] = {};
        cliSlots[s].itsHandle = -1;
      }
    }

    /* Cron commands */
    cliOut = cronCliWrite;
    cronDrainCommands();
  }
}

/* ---- Serial task: byte shuttle between serial port and log/CLI ---- */

/** Write CLI bytes to the console verbatim. Newline translation (\n -> \r\n)
 * happens exactly once, in the USB-Serial-JTAG VFS TX layer (TX mode CRLF) —
 * the same single translation the log path (logVprintf fwrite) relies on.
 * Do NOT pre-expand here: serialEmit and the VFS both expanding produced
 * \r\r\n per line, which terminals render as a duplicated/overwritten
 * output region (looked like CLI output "reappearing"). */
static void serialEmit(const char* p, size_t n) {
  fwrite(p, 1, n, stdout);
}

/* Set true while in CLI mode so logVprintf suppresses its direct-stdout echo
 * (otherwise log lines would interleave with command output / prompts).
 * Declared extern in log.cpp; defined here without extern (definition-with-
 * initializer rule). */
extern "C" { volatile bool serialInCli = false; }

static void serialTaskFn(void* arg) {
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
  /* IDF 5.5 regression: the USB Serial JTAG VFS no-driver read path can't size
   * non-blocking reads — usb_serial_jtag_get_read_bytes_available() returns 0
   * unless the driver is installed (the LL only exposes a 1-bit "any byte"
   * flag, not a count), so non-blocking read() always returns -1 EWOULDBLOCK
   * even with bytes in the hardware FIFO. Installing the driver attaches an
   * ISR that drains LL → ringbuffer, and the VFS then reads from the ring. */
  usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
  usb_serial_jtag_driver_install(&cfg);
  usb_serial_jtag_vfs_use_driver();
#endif

  /* Set stdin non-blocking */
  int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
  fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);

  itsClientInit(1);
  int cliHandle = -1;

  for (;;) {
    while (itsPoll(pdMS_TO_TICKS(50))) {}

    /* Poll serial input */
    char c;
    while (read(STDIN_FILENO, &c, 1) == 1) {
      if (c == 0x03) {
        /* Ctrl-C on serial: abort any CLI line in flight, print a hint
         * (so the user doesn't think Ctrl-C exits the monitor — Ctrl-]
         * does), resume log. No-op for the CLI line abort if we're
         * already in log mode. */
        if (cliHandle >= 0) {
          itsDisconnect(cliHandle);
          cliHandle = -1;
        }
        serialInCli = false;
        printf("\033[0m\r\n\r\nPress Ctrl-] to exit monitor\r\n\r\n");
        fflush(stdout); cliFlush();
        continue;
      }
      if (cliHandle < 0 && c != '\n' && c != '\r') {
        /* Switch to CLI mode — suppress direct-stdout log echo */
        serialInCli = true;
        cli_connect_t req = { CLI_ANSI, 1, CLI_COLOR, 0 };
        cliHandle = itsConnect("cli", CLI_PORT_TCP, &req, sizeof(req), pdMS_TO_TICKS(500));
        if (cliHandle >= 0) {
          char host[48];
          storageGetStr("s.net.hostname", host, sizeof(host), CONFIG_SPANGAP_FW_HOSTNAME);
          if (!host[0]) safeStrncpy(host, CONFIG_SPANGAP_FW_HOSTNAME, sizeof(host));
          printf("\033[0m\r\n\r\nCLI mode, hit return on prompt to return to log\r\n\r\n%s $ ", host);
          fflush(stdout); cliFlush();
        } else {
          /* connect failed — abort CLI mode */
          serialInCli = false;
        }
      }
      if (cliHandle >= 0)
        itsSend(cliHandle, &c, 1, 0);
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
      /* Check if CLI kicked us. The CLI side already printed the handoff
       * banner ("Resuming log" / "Press Ctrl-] to exit monitor") before
       * disconnecting — stay silent here. */
      if (!itsConnected(cliHandle)) {
        cliHandle = -1;
        serialInCli = false;
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
        serialInCli = false;
      }
    }
  }
}

/* ---- Init ---- */

/* Module config version. Bump when adding/changing defaults. See duckdns.cpp.
 * v2: dropped s.cli.sticky — serial CLI is always sticky now, trailing ';' is
 * the explicit "run and return to log" signal. */
#define CLI_VERSION 2

void cliInit() {
  int v = storageGetInt("s.cli.version", 0);
  if (v < CLI_VERSION) {
    storageDefaultTree("s.cli", R"({
      "start_dir": "/"
    })");
    storageUnset("s.cli.sticky");
    storageSet("s.cli.version", CLI_VERSION);
  }

  /* Register builtin commands on main task's context, before spawning the cli
   * task. The cli command table is an unlocked static array — all registration
   * must happen serially on a single task, and the main task's module-init
   * chain is already that context. */
  cliBuiltinInit();

  cliTaskHandle = spawnTask(cliTaskFn, "cli", 6144, nullptr, 1, 1);
  /* 4096 instead of 3072 — apps linking C++ exception support (e.g.
   * reticulous + microReticulum) pay ~600B of libstdc++ unwinder stack
   * per dispatch, which the 3072 budget didn't allow for. */
  spawnTask(serialTaskFn, "serial", 4096, nullptr, 1, 1);
}
