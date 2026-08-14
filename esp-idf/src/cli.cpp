/**
 * CLI — command registry, line editor, serial task, CLI task.
 */
#include "cli.h"
#include "auth.h"
#include "fs.h"
#include "spangap.h"   /* humanDetected — a keystroke is a person at the controls */
#include "log.h"
#include "its.h"
#include "storage.h"
#include "cron.h"
#include "compat.h"
#include "mem.h"
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
#include <string>
#include <deque>
#include <dirent.h>
#include <sys/stat.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

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

bool cliVerbIs(const char* tok, const char* full, size_t minLen) {
    size_t n = strlen(tok);
    return n >= minLen && n <= strlen(full) && strncmp(tok, full, n) == 0;
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

/* Set by `usb cdc` while the console runs on a TinyUSB CDC port instead of
 * the USB-Serial-JTAG controller. The two share one USB PHY, so exactly one is
 * on the wire; every console I/O site below branches on this. */
extern "C" volatile bool consoleOnCdc;
extern "C" volatile bool consoleSwitchPending;
extern "C" volatile bool consoleWriteDead;
extern "C" void consoleCdcFlush(void);
extern "C" int  consoleCdcRead(char* out);
/* Block read/write on a named CDC port, for the serial-handler shuttle. The
 * console's byte-at-a-time path cannot carry a handler's stream: a client that
 * expects its reply within a quarter second gets nowhere at one byte per poll
 * interval, and a handler's bytes must reach the wire unaltered (no CRLF
 * translation, which serialEmit applies to console output). */
extern "C" int  consoleCdcReadPort(int itf, uint8_t* out, size_t max);
extern "C" int  consoleCdcWritePort(int itf, const uint8_t* data, size_t len);

static void cliFlush() {
  fflush(stdout);
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
  /* USB Serial JTAG echo needs a TX FIFO flush — fflush(stdout) only pushes
   * into the USB FIFO, but bytes don't actually leave until either a newline
   * or this explicit flush. UART has no equivalent (writes leave immediately).
   * CDC has the same trailing-write problem and its own flush call. */
  if (consoleOnCdc) consoleCdcFlush();
  else              usb_serial_jtag_ll_txfifo_flush();
#endif
}

static void cronCliWrite(const char* data, size_t len);  /* forward */

static cli_write_fn cliOut = nullptr;

int cliPrintf(const char* fmt, ...) {
    if (!cliOut) return 0;
    /* Format on the heap, not the stack. This is the deepest common frame of
     * every CLI output path, and `help` prints one line per registered command
     * through here in a loop — a 256 B stack buffer at this point tipped the
     * (shallow) cli task over its stack limit. A heap buffer keeps the peak flat
     * regardless of how much a command prints. */
    constexpr size_t CAP = 256;
    std::vector<char> buf(CAP);
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf.data(), CAP, fmt, ap);
    va_end(ap);
    if (n > 0) {
        size_t w = (size_t)n < CAP ? (size_t)n : CAP - 1;
        cliOut(buf.data(), w);
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
/* Command history, newest at front — dynamically sized std::strings, so no fixed
 * per-line cap and no clients×history static waste. */
static std::deque<std::string> histDq;

static void histAdd(const char* cmd) {
  if (!histDq.empty() && histDq.front() == cmd) return;   /* skip dup of last */
  histDq.emplace_front(cmd);
  if ((int)histDq.size() > HIST_SIZE) histDq.pop_back();
}

/* Browse index 0 = newest; nullptr past the end. */
static const char* histGet(int browseIdx) {
  if (browseIdx < 0 || browseIdx >= (int)histDq.size()) return nullptr;
  return histDq[browseIdx].c_str();
}

struct cli_edit {
    std::string buf;            /* current line (dynamic, no fixed cap) */
    std::string saved;          /* saved current line when browsing history */
    int cursor = 0;
    /* 0 = normal, 1 = ESC seen, 2 = in CSI (first param), 3 = in SS3,
     * 4 = in CSI past the first parameter */
    int escState = 0;
    int escParam = 0;   /* first numeric CSI parameter (0 = none given) */
    int histIdx = -1;   /* -1 = not browsing, 0 = newest, 1 = next older... */
    bool savedValid = false;
};

/* CLI ITS server: per-slot state */
#define CLI_MAX_CLIENTS 10   /* up to 8 TCP + 2 DC (browser session + on-device CLI) */
PSRAM_BSS static struct cli_slot_t {
    int itsHandle;
    cli_edit edit;
    cli_mode_t mode;
    bool usbSerial;
    bool color;        /* emit ANSI color escapes (CLI_ANSI input echo) */
    bool noPrompt;     /* suppress connect-time prompt (one-shot exec clients) */
    std::string lineBuf;   /* LINE-mode accumulation (dynamic) */
    int cols, rows;    /* client terminal size (0 = unknown → 80x24); for ssh pty-req */
    char cwd[256];
    /* True when the slot wants to disconnect once its outgoing stream is
     * fully drained. Set by trailing-';' (LINE / ANSI hangup) and by serial
     * empty-enter. The main loop polls itsSendIsEmpty and tears the
     * connection down when it goes true — avoids the itsSendDrain blocking
     * race that lost bytes on the wire. */
    bool pendingClose;
    /* Admin-password login gate (cli_connect_t.login). loginRequired is set
     * from the connect payload; authed starts false when a login is required
     * and flips true only after authLogin(pw,"admin") succeeds. While
     * loginRequired && !authed, every received byte is consumed by the
     * password handler and NO command is dispatched. */
    bool loginRequired;
    bool authed;
    int  pwTries;
    std::string pwBuf;
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

/* Write the whole buffer, applying BACKPRESSURE: when the channel is full,
 * block and retry rather than drop. This throttles a producer (e.g. `cat` of a
 * big file) to the channel's drain rate instead of flooding the CLI→DataChannel
 * path — an unthrottled flood backed the WebRTC/SCTP send up until it wedged.
 * Guards keep it from hanging: stop if the peer disconnects, and give up after a
 * long stall (no drain) so a dead-but-not-closed channel can't block forever. */
static void itsSendAll(int h, const char* data, size_t len) {
    int stalls = 0;
    while (len > 0) {
        size_t sent = itsSend(h, data, len, pdMS_TO_TICKS(200));
        if (sent == 0) {
            if (!itsConnected(h)) break;       /* peer gone — drop the rest */
            if (++stalls > 150) break;         /* ~30 s with no drain — give up */
            continue;                          /* full: wait, then retry (backpressure) */
        }
        stalls = 0;
        data += sent;
        len  -= sent;
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

/* Single source of truth for the serial CLI's "Returning to log" banner. Every
 * serial path that hands the console back to the live log view routes through
 * here — ^D, empty Enter, the `exit` command, and the trailing-';' run-then-
 * resume. The blank line *after* the message is NOT emitted here: it's written
 * by the serial task at the moment log output resumes (cliEmitLogResumeGap),
 * because trailing newlines tacked onto the banner can land after the first
 * resumed log line — the CLI ITS stream drains a beat late while the (separate)
 * log task starts writing. */
static void cliReturnToLog(cli_write_fn write) {
    cliColorWrite(write, RESET, sizeof(RESET) - 1);
    write("\r\n\r\nReturning to log\r\n", 22);
}

/** True if trimmed line ends with ';' — serial CLI's "run this and switch
 *  back to log output" signal. */
static bool cliLineEndsWithSemicolon(const char* line) {
  const char* p = line;
  size_t n = strlen(p);
  while (n > 0 && (p[n - 1] == ' ' || p[n - 1] == '\t')) n--;
  return n > 0 && p[n - 1] == ';';
}

/* ---- wrap-aware cursor math ----
 * A line can wrap across terminal rows (prompt + text > width), so cursor moves
 * and clears must reckon in rows, not just columns — otherwise a multi-row
 * history entry doesn't clear and the cursor walks downward. Width comes from the
 * client's connect payload (colsxrows); the LCD and xterm both report it. */
static int cliPromptLen();   /* defined below */

static int cliTermWidth() {
  if (cliActiveSlot >= 0 && cliSlots[cliActiveSlot].cols > 0) return cliSlots[cliActiveSlot].cols;
  return 80;
}

/* Move the terminal cursor from input position `fromPos` to `toPos` (offsets
 * into the line; 0 = first char), crossing wrapped rows as needed. */
static void cliMoveCursor(int fromPos, int toPos, cli_write_fn write) {
  if (fromPos == toPos) return;
  int w = cliTermWidth(); if (w < 1) w = 80;
  int p = cliPromptLen();
  int frRow = (p + fromPos) / w, toRow = (p + toPos) / w;
  int toCol = (p + toPos) % w;
  char buf[24]; int n = 0;
  if (toRow < frRow)      n += snprintf(buf + n, sizeof(buf) - n, "\033[%dA", frRow - toRow);
  else if (toRow > frRow) n += snprintf(buf + n, sizeof(buf) - n, "\033[%dB", toRow - frRow);
  n += snprintf(buf + n, sizeof(buf) - n, "\033[%dG", toCol + 1);   /* absolute column (1-based) */
  if (n > 0) write(buf, n);
}

/* Redraw the whole line (wrap-aware) and leave the cursor at e.cursor. `from` is
 * the input position the terminal cursor is currently at. Clears with \033[J so
 * any wrapped continuation rows of the previous content are removed. */
static void cliEditRefresh(cli_edit& e, int from, cli_write_fn write) {
  cliMoveCursor(from, 0, write);            /* to start of input */
  write("\033[J", 3);                        /* clear to end of screen */
  if (!e.buf.empty()) write(e.buf.data(), e.buf.size());
  cliMoveCursor((int)e.buf.size(), e.cursor, write);   /* end -> cursor */
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
            delay(50); /* let log task drain under WiFi/boot burst */
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
          delay(50);
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
  /* Move to start of input (wrap-aware) and clear all wrapped rows below. */
  cliMoveCursor(e.cursor, 0, write);
  write("\033[J", 3);
  e.buf = text;
  e.cursor = (int)e.buf.size();
  if (!e.buf.empty()) {
    if (ansi) cliColorWrite(write, CYAN, sizeof(CYAN) - 1);
    write(e.buf.data(), e.buf.size());
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
  const char* base = e.buf.c_str();
  const char* wordStart = base;
  for (int i = e.cursor - 1; i >= 0; i--) {
    if (base[i] == ' ') {
      wordStart = base + i + 1;
      break;
    }
  }
  int wordLen = (int)(base + e.cursor - wordStart);
  if (wordLen < 0) return;

  const char* line0 = base;
  while (*line0 == ' ') line0++;

  int cmdLen = 0;
  int cmdIdx = cliLongestCmdMatch(line0, &cmdLen);
  if (!cliCmdWantsFileArgs(cmdIdx)) return;
  int cmdEndIdx = (int)(line0 - base) + cmdLen;
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
  if (suffixLen <= 0) return;

  e.buf.insert((size_t)e.cursor, suffix, (size_t)suffixLen);
  e.cursor += suffixLen;
  cliEditRefresh(e, e.cursor - suffixLen, write);
}

/* ---- Line editor ---- */

/* Move the input cursor to `to` (already clamped) and pick the cursor shape:
 * a bar while inside the line, the terminal default once at the end. */
static void cliEditGoto(cli_edit& e, int to, cli_write_fn write) {
  if (to == e.cursor) return;
  cliMoveCursor(e.cursor, to, write);
  e.cursor = to;
  write(e.cursor < (int)e.buf.size() ? "\033[2 q" : "\033[0 q", 5);
}

/* Act on a decoded escape sequence: `final` is the terminating byte, `param` the
 * first numeric parameter (0 when the sequence carried none). Shared by CSI
 * (ESC [ …) and SS3 (ESC O …, which cursor keys use in application mode). */
static void cliEditEscKey(cli_edit& e, char final, int param, cli_write_fn write) {
  bool ansi = cliIsAnsi();
  switch (final) {
    case 'D':                                     /* left */
      if (e.cursor > 0) cliEditGoto(e, e.cursor - 1, write);
      return;
    case 'C':                                     /* right */
      if (e.cursor < (int)e.buf.size()) cliEditGoto(e, e.cursor + 1, write);
      return;
    case 'A': {                                   /* up — older history */
      int next = e.histIdx + 1;
      const char* h = histGet(next);
      if (h) {
        if (e.histIdx < 0) {
          e.saved = e.buf;
          e.savedValid = true;
        }
        e.histIdx = next;
        cliEditReplace(e, h, write);
      }
      return;
    }
    case 'B':                                     /* down — newer history */
      if (e.histIdx > 0) {
        e.histIdx--;
        const char* h = histGet(e.histIdx);
        if (h) cliEditReplace(e, h, write);
      } else if (e.histIdx == 0) {
        e.histIdx = -1;
        cliEditReplace(e, e.savedValid ? e.saved.c_str() : "", write);
        e.savedValid = false;
      }
      return;
    case 'H':                                     /* Home (xterm/SS3 form) */
      cliEditGoto(e, 0, write);
      return;
    case 'F':                                     /* End (xterm/SS3 form) */
      cliEditGoto(e, (int)e.buf.size(), write);
      return;
    case '~':
      /* Numbered keypad forms. 3 = DEL: delete the character under the cursor,
       * leaving the cursor put. Everything else (PgUp/PgDn, bracketed-paste
       * markers 200/201, …) is swallowed rather than typed into the line. */
      if (param == 3 && e.cursor < (int)e.buf.size()) {
        e.buf.erase((size_t)e.cursor, 1);
        if (e.buf.empty() && ansi) {
          cliColorWrite(write, RESET, sizeof(RESET) - 1);
          write("\033[J", 3);
          write("\033[0 q", 5);
        } else {
          cliEditRefresh(e, e.cursor, write);     /* terminal cursor hasn't moved */
          if (e.cursor == (int)e.buf.size()) write("\033[0 q", 5);
        }
      } else if (param == 1 || param == 7) {      /* Home */
        cliEditGoto(e, 0, write);
      } else if (param == 4 || param == 8) {      /* End */
        cliEditGoto(e, (int)e.buf.size(), write);
      }
      return;
    default:
      return;
  }
}

static void cliEditChar(cli_edit& e, char c, cli_write_fn write) {
  bool ansi = cliIsAnsi();

  /* Escape sequence state machine: CSI (ESC [ params final) and SS3 (ESC O
   * final). Parameter bytes are accumulated so multi-byte keys such as DEL
   * (ESC [ 3 ~) are decoded whole instead of spilling their tail into the
   * line as text. */
  if (e.escState == 1) {
    e.escParam = 0;
    e.escState = (c == '[') ? 2 : (c == 'O') ? 3 : 0;
    return;
  }
  if (e.escState == 2 || e.escState == 4) {
    if (c >= 0x30 && c <= 0x3f) {                 /* parameter / private bytes */
      if (e.escState == 2) {
        if (c >= '0' && c <= '9') {
          if (e.escParam < 10000) e.escParam = e.escParam * 10 + (c - '0');
        } else {
          e.escState = 4;   /* ';' or a private marker — only param 1 matters */
        }
      }
      return;
    }
    if (c >= 0x20 && c <= 0x2f) return;           /* intermediate bytes */
    e.escState = 0;
    if (c >= 0x40 && c <= 0x7e) cliEditEscKey(e, c, e.escParam, write);
    return;
  }
  if (e.escState == 3) {                          /* SS3 — single final byte */
    e.escState = 0;
    cliEditEscKey(e, c, 0, write);
    return;
  }
  if (c == '\033') { e.escState = 1; return; }

  /* ^D — end-of-input. Always closes (serial: returns to log; TCP/SSH/WS:
   * disconnects). Any in-progress line is discarded — readline-style
   * "forward-delete" isn't worth the complexity, and a user who hits ^D
   * usually wants to leave, not edit. */
  if (c == 0x04) {
    if (cliActiveSlot >= 0 && cliSlots[cliActiveSlot].usbSerial) {
      cliReturnToLog(write);
    } else if (ansi) {
      write("\r\n", 2);
    }
    cliRequestSessionEnd();
    return;
  }

  /* ^A / ^E — beginning / end of line (readline) */
  if (c == 0x01) { cliEditGoto(e, 0, write); return; }
  if (c == 0x05) { cliEditGoto(e, (int)e.buf.size(), write); return; }

  if (c == '\t') {
    cliTabComplete(e, write);
    return;
  }

  if (c == '\n' || c == '\r') {
    if (!e.buf.empty()) {
      histAdd(e.buf.c_str());
      std::string lineCopy = e.buf;
      e.buf.clear();
      e.cursor = 0;
      e.histIdx = -1;
      e.savedValid = false;
      /* Trailing ';' is the "run this and disconnect" signal:
       *   serial → return to log view (handoff to log task)
       *   anything else (TCP / WS / DC / SSH exec) → close the ITS connection
       * Lines without ';' stay connected (interactive). */
      bool stayCli = true;
      bool resumeLog = false;
      bool itsHangup = false;
      if (cliActiveSlot >= 0 && cliLineEndsWithSemicolon(lineCopy.c_str())) {
        stayCli = false;
        if (cliSlots[cliActiveSlot].usbSerial) resumeLog = true;
        else                                    itsHangup = true;
      }
      write("\r\n", 2);
      if (resumeLog) {
        /* Trailing ';' on serial: announce the switch back to log before the
         * command runs (its output follows the banner). */
        cliReturnToLog(write);
        serialInCli = false;
      }
      cliOut = write;
      cliProcess(lineCopy.c_str());
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
        cliReturnToLog(write);
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
      e.buf.erase((size_t)(e.cursor - 1), 1);
      e.cursor--;
      if (e.buf.empty() && ansi) {
        cliMoveCursor(e.cursor + 1, 0, write);   /* old pos -> input start */
        cliColorWrite(write, RESET, sizeof(RESET) - 1);
        write("\033[J", 3);
        write("\033[0 q", 5);
      } else {
        /* terminal cursor is one right of the new cursor; full wrap-aware redraw */
        cliEditRefresh(e, e.cursor + 1, write);
      }
    }
  } else if (c >= 0x20 && e.buf.size() < 4096) {   /* sane upper bound */
    bool atEnd = (e.cursor == (int)e.buf.size());
    if (e.buf.empty() && ansi)
      cliColorWrite(write, CYAN, sizeof(CYAN) - 1);
    e.buf.insert(e.buf.begin() + e.cursor, c);
    e.cursor++;
    if (atEnd) {
      /* Fast path — appending at the end of the line. The terminal cursor is
       * already where the glyph goes, so just emit it and let the terminal
       * advance (autowrap handles the wrap column). This avoids the
       * move-to-start + \033[J clear + full-buffer rewrite that otherwise
       * repaints the whole input row on every keystroke. SGR state is already
       * the input colour (set on the first char / left as terminal state). */
      write(&c, 1);
    } else {
      /* Mid-line insert: wrap-aware redraw of the tail past the cursor. */
      cliEditRefresh(e, e.cursor - 1, write);
    }
  }
}

/* ---- CLI command dispatcher ---- */

void cliProcess(const char* line) {
  while (*line == ' ') line++;
  std::string trimmed(line);
  while (!trimmed.empty() && (trimmed.back() == '\r' || trimmed.back() == '\n' ||
                              trimmed.back() == ' '  || trimmed.back() == '\t'))
    trimmed.pop_back();
  line = trimmed.c_str();          /* points into `trimmed` for the rest of the fn */
  if (*line == '#' || *line == '\0') return;
  /* Semicolon: split and execute each part */
  const char* semi = strchr(line, ';');
  if (semi) {
    std::string part(line, (size_t)(semi - line));
    cliProcess(part.c_str());
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
extern void cliCmdMountInit();
extern void pmRegisterCmds();
extern void logRegisterCmds();
extern void usbPortsRegisterCmds();

static void cliBuiltinInit() {
    storageRegisterCmds();
    cliCmdFsInit();
    cliCmdSysInit();
    cliCmdMountInit();
    pmRegisterCmds();
    logRegisterCmds();
    usbPortsRegisterCmds();
    cliRegisterCmd("alias", cmdAlias);
    cliRegisterCmd("unalias", cmdUnalias);
    cliRegisterCmd("exit", [](const char* a) {
        if (cliWantsHelp(a)) { cliPrintf("%-*s end this CLI session\n", CLI_HELP_COL, "exit"); return; }
        if (cliActiveSlot >= 0 && cliSlots[cliActiveSlot].usbSerial) {
            cliReturnToLog(cliOut);
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

  /* Admin-password gate: instead of the command prompt, demand the password
   * up front. Subsequent bytes are consumed by cliHandleLoginInput until
   * authLogin succeeds; no command runs before then. */
  if (cl.loginRequired && !cl.authed) {
    cliActiveSlot = slot;
    itsCliWrite("Enter admin password: ", 22);
    cliActiveSlot = -1;
    return;
  }

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
  cl.lineBuf.clear();
  cl.usbSerial = false;
  cl.color = true;          /* default on; a cli_connect_t may opt out */
  cl.noPrompt = false;      /* default: send the connect-time prompt */
  cl.loginRequired = false; /* default: no admin gate */
  cl.pwTries = 0;
  cl.pwBuf.clear();
  if (len == sizeof(cli_connect_t)) {
    const auto* cc = (const cli_connect_t*)data;
    cl.mode = cc->mode;
    cl.usbSerial = cc->from_usb_serial != 0;
    cl.color = (cc->color == CLI_COLOR);
    cl.noPrompt = cc->no_prompt != 0;
    cl.loginRequired = cc->login != 0;
  } else if (len >= 1 && len < sizeof(cli_connect_t)) {
    cl.mode = *(const cli_mode_t*)data;
  } else {
    /* Empty, or a larger connect descriptor from a forwarder (net sends its
     * own net_connect_t on raw TCP/TLS) — line-oriented, no echo. The CLI
     * doesn't decode the forwarder's struct; it only needs LINE mode. */
    cl.mode = CLI_LINE;
  }
  cl.authed = !cl.loginRequired;
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
  cl.lineBuf.clear();
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
  /* The browser DC transport carries no cli_connect_t, so it can't request the
   * login gate; browser CLI auth is handled at the web layer. */
  cl.loginRequired = false;
  cl.authed = true;
  cl.pwTries = 0;
  cl.pwBuf.clear();
  cliInitSlot(cl, slot);
  return slot;
}

static void cliOnDisconnect(int ref) {
  if (ref >= 0 && ref < CLI_MAX_CLIENTS) {
    cliSlots[ref] = {};
    cliSlots[ref].itsHandle = -1;
  }
}

/* Admin-password login gate. Consumes bytes for a slot whose loginRequired is
 * set and authed is not yet true, accumulating a password line (echoed as '*'
 * in ANSI mode). On CR/LF it checks authLogin(pw,"admin"): success flips authed
 * and drops the command prompt; three failures tear the session down. No
 * command is ever dispatched while unauthenticated — the gate is server-side
 * and cannot be bypassed by the remote end. Caller sets cliActiveSlot. */
#define CLI_LOGIN_MAX_TRIES 3
static void cliHandleLoginInput(cli_slot_t& cl, const char* buf, size_t n) {
  bool ansi = (cl.mode == CLI_ANSI);
  for (size_t i = 0; i < n; i++) {
    char c = buf[i];
    if (c == 0x03 || c == 0x04) {          /* ^C / ^D — abort */
      itsCliWrite("\r\n", 2);
      cl.pendingClose = true;
      cl.pwBuf.clear();
      return;
    }
    if (c == '\r' || c == '\n') {
      std::string pw = cl.pwBuf;
      cl.pwBuf.clear();
      if (ansi) itsCliWrite("\r\n", 2);
      std::string realm, cookie;
      if (authLogin(pw.c_str(), "admin", realm, cookie) == AUTH_OK) {
        cl.authed = true;
        /* Drop the interactive prompt and hand off to normal processing. Any
         * bytes after this newline in the same recv are intentionally
         * discarded so a pipelined "pw\ncommand" can't run a command on the
         * auth packet. */
        cliWritePrompt(itsCliWrite);
        if (ansi) itsCliWrite("\033[0 q", 5);
        return;
      }
      if (++cl.pwTries >= CLI_LOGIN_MAX_TRIES) {
        itsCliWrite("Authentication failed.\r\n", 24);
        cl.pendingClose = true;
        return;
      }
      itsCliWrite("Enter admin password: ", 22);
      continue;
    }
    if (c == 0x08 || c == 0x7f) {           /* backspace / DEL */
      if (!cl.pwBuf.empty()) {
        cl.pwBuf.pop_back();
        if (ansi) itsCliWrite("\b \b", 3);
      }
      continue;
    }
    if ((uint8_t)c < 0x20) continue;        /* ignore other control bytes */
    if (cl.pwBuf.size() < 128) {
      cl.pwBuf.push_back(c);
      if (ansi) itsCliWrite("*", 1);
    }
  }
}

/* ---- CLI task ---- */

static TaskHandle_t cliTaskHandle = NULL;

static void serialEmit(const char* p, size_t n);   /* defined with the serial task */

void consoleWriteRaw(const char* data, size_t len) {
  serialEmit(data, len);
  fflush(stdout);
  cliFlush();
}

void consoleFlush(void) {
  fflush(stdout);
  cliFlush();
}

void cliSerialResumeLog(void) {
  cliUsbSerialAutoResumeLog = true;
}

void cliWake() {
  if (cliTaskHandle) xTaskNotifyGive(cliTaskHandle);
}

static void cliTaskFn(void* arg) {
  /* History buffer in PSRAM, allocated in task context so heap tracking
     attributes it to cli, not the main task that spawned us. */
  /* history is a std::deque now — no preallocation needed */
  for (int i = 0; i < CLI_MAX_CLIENTS; i++) cliSlots[i].itsHandle = -1;
  itsServerInit();
  /* CLI commands sometimes need to itsConnect outwards (e.g. rnprobe → rnsd
   * RNSD_PORT_PACKET). Mark this task as a client too. 2 slots = current
   * command + headroom. */
  itsClientInit(2);
  /* Two ports because the transports frame differently — TCP/serial is a byte
   * stream (packetBased=false), the WebRTC DataChannel is message-oriented
   * (packetBased=true) — and a port has one framing mode, so they can't share.
   * Shared CLI_MAX_CLIENTS=10 slot pool — 8 TCP + 2 DC. DC has only two possible
   * consumers (the single browser webrtc session + the on-device CLI), so it's
   * capped at 2; the headroom goes to TCP (nc debug, sshd-in backends, and the
   * serial task's framed-RPC exec). TCP is 8 rather than 6 so a full house of
   * ssh sessions cannot starve the RPC of a slot. The two caps sum to the pool,
   * so DC's 2 stay guaranteed even under a TCP flood. */
  itsServerPortOpen(CLI_PORT_TCP, /*packetBased=*/false, 8, 512, 2048);
  itsServerOnConnect(CLI_PORT_TCP, cliTcpConnect);
  itsServerOnDisconnect(CLI_PORT_TCP, cliOnDisconnect);
  itsServerPortOpen(CLI_PORT_DC,  ITS_PACKET,  2, 512, 2048, /*depth=*/0, /*maxMsg=*/2048);
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
    /* Fully event-driven: ITS delivery notifies this task per session byte, and
     * cron wakes us via cliWake() after queueing a command (its stream buffer
     * carries no notification of its own). So park until a real event rather than
     * polling — an idle console then adds zero wakes and both cores can light-
     * sleep. The lone exception is a slot waiting to close once its output has
     * drained to the peer (pendingClose): that drain-complete transition isn't a
     * notify, so fall back to a short re-check tick only while one is pending. */
    bool draining = false;
    for (int s = 0; s < CLI_MAX_CLIENTS; s++)
      if (cliSlots[s].pendingClose) { draining = true; break; }
    itsPoll(draining ? pdMS_TO_TICKS(20) : portMAX_DELAY);
    while (itsPoll(0)) {}

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
      /* Bytes on a console session are somebody typing — on serial, over the
       * network, at the password prompt. Ahead of the auth gate deliberately:
       * an attempt at the password is a person either way. */
      humanDetected("console");
      cliActiveSlot = s;
      auto& cl = cliSlots[s];
      if (cl.loginRequired && !cl.authed) {
        /* Unauthenticated: every byte goes to the password gate, never to the
         * command path. */
        cliHandleLoginInput(cl, buf, n);
        cliActiveSlot = -1;
        continue;
      }
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
            hangup = cliLineEndsWithSemicolon(cl.lineBuf.c_str());
            if (!cl.lineBuf.empty()) {
              cliOut = itsCliWrite;
              cliProcess(cl.lineBuf.c_str());
            }
            cl.lineBuf.clear();
            if (hangup) break;
            cliWritePrompt(itsCliWrite);
          } else if (cl.lineBuf.size() < 4096) {
            cl.lineBuf.push_back(c);
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

/** Write CLI bytes to the console.
 *
 * On USB-Serial-JTAG we bypass stdout and write straight to the driver's TX
 * ring buffer via usb_serial_jtag_write_bytes(). We do NOT go through the VFS
 * write() wrapper, because usb_serial_jtag_write() gates on
 * usb_serial_jtag_is_connected() and returns -1 — discarding the WHOLE chunk —
 * whenever that flag reads false. The flag is maintained by a FreeRTOS
 * tick-hook that watches for SOF packets, and it goes stale after ANY light
 * sleep: the tick-hook is frozen while asleep, so the flag stays latched false
 * until a tick runs post-wake. Output emitted in that window (the *front* of a
 * burst that follows an idle gap) was silently dropped. `top` hits this every
 * time: its uxTaskGetSystemState() critical section delays a tick, the monitor
 * — whose tolerance ALLOWED_NO_SOF_TICKS is pdMS_TO_TICKS(3) == 0 at
 * CONFIG_FREERTOS_HZ=100, i.e. zero — declares a spurious disconnect, releases
 * its own NO_LIGHT_SLEEP lock, and the device light-sleeps right as the table
 * starts printing; header + first rows vanish, tail survives.
 *
 * write_bytes() carries no is_connected gate: it queues into the ring and the
 * TX ISR drains to the FIFO whenever the host is actually reading. A host that
 * has genuinely stopped reading fills the ring and applies real backpressure
 * (bounded by the timeout below) — the honest signal — instead of a stale
 * connection flag causing a blind drop. Because we no longer pass through the
 * VFS TX layer, we apply its \n -> \r\n translation here (skipping any \n that
 * is already part of a \r\n, so editor output isn't doubled).
 *
 * (The log path still writes via stdout/the VFS, so it retains the old gate;
 * but the monitor holds NO_LIGHT_SLEEP while a host is connected, so a live
 * log session never sleeps mid-stream — only an interactive command burst
 * after the top-style spurious disconnect was exposed.) */
static void serialEmit(const char* p, size_t n) {
  /* Mid-move: neither transport can carry this, and the queue it would sit in
   * is drained into the next session that attaches. Drop it. */
  if (consoleWriteDead) return;
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
  /* None of the driver-level bypass below applies on CDC: the TinyUSB VFS does
   * its own line-ending translation and has no is_connected gate to route
   * around, so stdout is the correct path there. */
  if (consoleOnCdc) { fwrite(p, 1, n, stdout); return; }
  auto put = [](const char* d, size_t len) {
    size_t off = 0;
    while (off < len) {
      /* Blocks up to 250 ms: a reading host drains the FIFO so this returns at
       * once; only a host that has stopped reading times out, at which point
       * dropping the rest is correct (nobody's listening) rather than wedging
       * the serial task — which would stall every other console consumer. */
      int w = usb_serial_jtag_write_bytes(d + off, len - off, pdMS_TO_TICKS(250));
      if (w <= 0) break;
      off += (size_t)w;
    }
  };
  const char* start = p;
  for (size_t i = 0; i < n; i++) {
    if (p[i] != '\n') continue;
    if (i > 0 && p[i - 1] == '\r') continue;      /* already CRLF — leave in run */
    if (p + i > start) put(start, (size_t)(p + i - start));
    put("\r\n", 2);
    start = p + i + 1;
  }
  if (start < p + n) put(start, (size_t)(p + n - start));
#else
  fwrite(p, 1, n, stdout);
#endif
}

/* Emit the blank line that separates a "Returning to log" banner from the
 * resumed log output, and flush it. Called on the serial task right before
 * serialInCli is cleared, so it lands while the log task is still suppressed —
 * the gap is therefore guaranteed to sit between the banner and the first
 * resumed log line (a trailing newline on the banner itself can drain late and
 * land after that line instead). A single '\n' is enough: the USB-Serial-JTAG
 * VFS expands it to "\r\n" exactly as it does for log output. */
static void cliEmitLogResumeGap() {
  serialEmit("\n", 1);
  fflush(stdout);
  cliFlush();
}

/* Set true while in CLI mode so logVprintf suppresses its direct-stdout echo
 * (otherwise log lines would interleave with command output / prompts).
 * Declared extern in log.cpp; defined here without extern (definition-with-
 * initializer rule). */
extern "C" { volatile bool serialInCli = false; }

/* Set while a registered handler owns the console port. Suppresses the same log
 * mirror serialInCli does, and CLI entry with it: the port is carrying a
 * client's protocol, and console bytes pushed into that stream would corrupt
 * it. A third variable rather than a reuse of serialInCli, which a trailing-';'
 * command clears mid-session and which the CLI's own paths write. */
extern "C" { volatile bool serialInHandler = false; }

/* ---- Serial-port handler registry (contract in cli.h) ----
 *
 * Claims are posted from the claimant's task and read by the serial task
 * through bare volatile flags — the mechanism consoleSwitchPending already
 * uses here. Every flag is a single-writer edge the serial task clears, and
 * the registry itself changes only when a claim is taken or dropped, so there
 * is nothing a lock would add. */

/* SERIAL_PORT_COUNT is in cli.h: 2 where the CDC transport is built, 1 where it
 * is not, so a one-port image carries no port-1 registry at all. */

static struct serial_claim_t {
    char     task[16];
    uint16_t itsPort;
    bool     claimed;
} serialClaims[SERIAL_PORT_COUNT];

/* A claim was taken or dropped; the serial task re-reads the registry. */
static volatile bool serialClaimChanged = false;
/* A host attached to / detached from a claimed CDC port (DTR edge, raised on
 * the TinyUSB task). The USB-Serial-JTAG path has no line state and attaches in
 * band instead — see cli.h. */
static volatile bool serialAttachReq[SERIAL_PORT_COUNT];
static volatile bool serialDetachReq[SERIAL_PORT_COUNT];

static TaskHandle_t serialTaskHandle = NULL;

/* ---- Console write lock ----
 *
 * Two tasks write the console on two paths that share no ordering: the log task
 * echoes each line straight to stdout from its own context (logVprintf), and
 * the serial task writes framed-RPC replies through the driver. A log line
 * landing inside a length-counted reply frame is unrecoverable for the host —
 * it counts the log bytes as payload and shows the displaced reply bytes as
 * garbage, and resync-on-magic cannot help once the length has been read. The
 * echo and each whole reply frame are therefore serialised here.
 *
 * The direct echo itself stays: it is what lets logs reach the wire when the
 * serial task is wedged or not yet up, and what lets the idle serial task park
 * on the driver's RX ring instead of running a notify-and-poll loop. The cost
 * is that the echo can stall for the duration of one frame write, bounded by
 * the host draining the port.
 *
 * Created in cliInit(), before either task exists. A null handle simply does
 * not lock, which covers the early-boot window in which no frame can exist.
 *
 * Recursive, because anything the frame write touches — a driver, the CDC
 * layer — may log, and a log call re-entering this lock on the task that
 * already holds it would deadlock the console outright. Interleaving one line
 * into one frame is the lesser failure by far. */
extern "C" { SemaphoreHandle_t consoleWriteMutex = NULL; }

extern "C" void consoleWriteLock(void) {
    if (consoleWriteMutex) xSemaphoreTakeRecursive(consoleWriteMutex, portMAX_DELAY);
}
extern "C" void consoleWriteUnlock(void) {
    if (consoleWriteMutex) xSemaphoreGiveRecursive(consoleWriteMutex);
}

/* ---- Framed RPC: a host tool's side-channel on the console port ----
 *
 * One frame layout, both directions:
 *
 *     <magic:4> <id:1> <len:2 big-endian> <payload:len>
 *
 * host → device the payload is a command line; device → host it is that
 * command's output. The magic leads with 0xF5, which cannot appear in valid
 * UTF-8, so every byte of ordinary console traffic fails the match on byte one.
 * Frames are never echoed, never enter the line editor and never flip the
 * console from log into CLI mode, so a host can read device state while a
 * person is typing at the same port.
 *
 * The id is opaque to the device — copied from request to reply. It identifies
 * *what was asked*, so a host that times out may retry with the same id and
 * take a late reply to the first attempt as an answer to the second.
 *
 * A zero-length reply is a real answer: it means the command printed nothing,
 * as distinct from the device not answering at all. A command that fails still
 * answers, with whatever it printed; the frame carries no status of its own.
 *
 * There is no integrity check beyond the magic. The peer on the serial console
 * can already type `reset factory`; there is nothing to defend against here,
 * only line noise to recover from.
 *
 * Frames reach this code only while port 0 is a console: once a serial handler
 * owns the port its bytes go to hdlPump and never reach handleChar. That is
 * deliberate — the handler mechanism exists for Reticulum clients, and a port
 * claimed for one is not carrying a flasher. */
static const uint8_t rpcMagic[4] = { 0xF5, 'S', 'G', 0x01 };

/* The 2-byte length allows 64 KB. Frame buffers come from PSRAM at that cap;
 * 64 KB of internal DRAM is far too much on a device where every task stack
 * comes out of it. A board without PSRAM gets a small internal buffer instead
 * of making PSRAM a dependency of the transport — every reply a host actually
 * reads fits in a screenful, and truncation has defined behaviour below. */
#define RPC_CAP_INTERNAL  (8 * 1024)
/* No progress for this long abandons the frame and resyncs on the magic. What
 * this guards against is a corrupted length — a flaky cable, not an adversary —
 * leaving the device allocated and waiting for bytes that never arrive. */
#define RPC_ASSEMBLE_MS   1000
/* Bound on the outbound half: a command that never finishes would otherwise
 * wedge the relay, and the console with it. */
#define RPC_EXEC_MS       5000

static char* rpcAlloc(size_t want, size_t* got) {
#if CONFIG_SPIRAM
    if (char* psram = (char*)heap_caps_malloc(want, MALLOC_CAP_SPIRAM)) {
        *got = want;
        return psram;
    }
#endif
    size_t small = want < RPC_CAP_INTERNAL ? want : (size_t)RPC_CAP_INTERNAL;
    char* p = (char*)heap_caps_malloc(small, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    *got = p ? small : 0;
    return p;
}

/* Sniffer state. It lives out here rather than inside handleChar because
 * handleChar is called from two places — the idle blocking-read path and the
 * active drain path — and portRead delivers 128-byte chunks, so one frame can
 * straddle both a chunk boundary and a change of path. Owned by the serial
 * task; nothing else touches it. */
static enum : uint8_t {
    RPC_PREAMBLE,   /* matching the magic; rpcMatched bytes have agreed */
    RPC_HEADER,     /* magic matched; collecting id + length */
    RPC_BODY,       /* collecting the payload */
    RPC_DISCARD,    /* no buffer for this payload — swallow it and resync */
} rpcPhase = RPC_PREAMBLE;
static uint8_t    rpcMatched = 0;
static uint8_t    rpcHead[3];
static uint8_t    rpcHeadLen = 0;
static uint8_t    rpcId = 0;
static uint16_t   rpcWant = 0;
static uint16_t   rpcGot = 0;
static char*      rpcCmd = NULL;
static TickType_t rpcDeadline = 0;

/* True while a frame is part-assembled — including a partially matched magic,
 * whose held bytes are owed to the console. The idle read must not park
 * forever in that state or an abandoned frame is never timed out. */
static bool rpcAssembling(void) {
    return rpcPhase != RPC_PREAMBLE || rpcMatched != 0;
}

static void rpcReset(void) {
    if (rpcCmd) { free(rpcCmd); rpcCmd = NULL; }
    rpcPhase   = RPC_PREAMBLE;
    rpcMatched = 0;
    rpcHeadLen = 0;
    rpcWant = rpcGot = 0;
}

extern "C" int consoleCdcPortCount(void);

/* Serial ports the hardware presents right now. consoleCdcPortCount() reports 0
 * while the console is not on CDC, which is the one-port USB-Serial-JTAG case. */
static int serialPortCount(void) {
    int n = consoleCdcPortCount();
    return n > 0 ? n : 1;
}

/* Wake the serial task. Safe from any task, including the TinyUSB one. */
extern "C" void serialPortWake(void) {
    if (serialTaskHandle) xTaskNotifyGive(serialTaskHandle);
}

extern "C" bool serialPortIsClaimed(int port) {
    return port >= 0 && port < SERIAL_PORT_COUNT && serialClaims[port].claimed;
}

extern "C" void serialPortHostAttached(int port) {
    if (!serialPortIsClaimed(port)) return;
    serialAttachReq[port] = true;
    serialPortWake();
}

extern "C" void serialPortHostDetached(int port) {
    if (!serialPortIsClaimed(port)) return;
    serialDetachReq[port] = true;
    serialPortWake();
}

bool serialPortClaim(int port, const char* task, uint16_t itsPort) {
    if (port < 0 || port >= SERIAL_PORT_COUNT || !task || !*task) {
        warn("serial: port %d cannot be claimed (0 = console port%s)", port,
             SERIAL_PORT_COUNT > 1 ? ", 1 = second cdc port"
                                   : "; this build has no second port — "
                                     "CONFIG_SPANGAP_USB_CDC is off");
        return false;
    }
    int have = serialPortCount();
    if (port >= have) {
        warn("serial: port %d claim refused — this console presents %d serial port%s; "
             "`usb cdc` presents two", port, have, have == 1 ? "" : "s");
        return false;
    }
    auto& c = serialClaims[port];
    if (c.claimed) {
        /* Re-applying an identical claim is how a claimant reacts to a
         * transport switch, so it must not read as a conflict. */
        if (strcmp(c.task, task) == 0 && c.itsPort == itsPort) return true;
        warn("serial: port %d already claimed by %s", port, c.task);
        return false;
    }
    safeStrncpy(c.task, task, sizeof(c.task));
    c.itsPort = itsPort;
    c.claimed = true;
    serialAttachReq[port] = false;
    serialDetachReq[port] = false;
    serialClaimChanged = true;
    serialPortWake();
    info("serial: port %d claimed by %s:%u", port, task, (unsigned)itsPort);
    return true;
}

void serialPortRelease(int port) {
    if (port < 0 || port >= SERIAL_PORT_COUNT || !serialClaims[port].claimed) return;
    serialClaims[port].claimed = false;
    serialClaimChanged = true;
    serialPortWake();
    info("serial: port %d released", port);
}

/* Shuttle carry: bytes read off a port that the handler's ITS stream had no
 * room for yet. Stream-mode itsSend can accept part of a write, and a protocol
 * framed on the wire cannot survive losing the middle of a frame — so the
 * remainder is carried forward rather than dropped. Owned by the serial task. */
static uint8_t serialHdlPend[SERIAL_PORT_COUNT][128];
static size_t  serialHdlPendLen[SERIAL_PORT_COUNT];

/* Set by the pm layer when the USB console link drops (the `usb down` command,
 * or a debounced unplug). Level-triggered: while true, the serial task tears
 * down any open CLI session and returns to the blocking log read, so a session
 * left open by the very command that dropped USB can't keep polling a
 * disconnected console at 20 Hz. Cleared by pm when USB comes back up. */
extern "C" { volatile bool cliUsbSerialLinkDown = false; }

static void serialTaskFn(void* arg) {
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
  /* IDF 5.5 regression: the USB Serial JTAG VFS no-driver read path can't size
   * non-blocking reads — usb_serial_jtag_get_read_bytes_available() returns 0
   * unless the driver is installed (the LL only exposes a 1-bit "any byte"
   * flag, not a count), so non-blocking read() always returns -1 EWOULDBLOCK
   * even with bytes in the hardware FIFO. Installing the driver attaches an
   * ISR that drains LL → ringbuffer, and the VFS then reads from the ring. */
  usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
  /* Force the driver's TX/RX ring buffers into INTERNAL RAM. The driver builds
   * them with plain xRingbufferCreate (no caps), so with SPIRAM_MALLOC_ALWAYSINTERNAL=0
   * they land in PSRAM — but the ring buffer is touched by the driver's ISR, and
   * its struct holds a function pointer (vCopyItem) + spinlock. An IRAM ISR
   * mauling a PSRAM ring during a flash-op cache-disable window corrupts that
   * pointer → a later xRingbufferSend on the serial task calls a garbage PSRAM
   * address → InstructionFetchError (deterministic, reproduced via `cat` of a state file).
   * Buffers are tiny (256 B TX + 256 B RX default), so internal cost is ~1 KB.
   * extmem_enable sets the runtime alwaysinternal threshold; restore it after. */
  heap_caps_malloc_extmem_enable(32 * 1024);
  usb_serial_jtag_driver_install(&cfg);
  usb_serial_jtag_vfs_use_driver();
  heap_caps_malloc_extmem_enable(CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL);
#endif

  /* Set stdin non-blocking */
  int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
  fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);

  /* Three client slots, and the cap is hard — itsConnect fails past it. In the
   * worst case all three are live: a handler session on a claimed port, the
   * `cli:1` connection a console CLI session holds, and the framed-RPC exec. */
  itsClientInit(3);
  int cliHandle = -1;
  /* Per-port handler session; -1 when the port is a console (or unclaimed). */
  int hdlHandle[SERIAL_PORT_COUNT];
  for (int p = 0; p < SERIAL_PORT_COUNT; p++) hdlHandle[p] = -1;

  /* Raw port I/O for the shuttle — no line-ending translation and no console
   * gating, because what crosses here is a client's protocol, not text. */
  auto portRead = [&](int port, uint8_t* out, size_t max) -> int {
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
      if (consoleOnCdc) return consoleCdcReadPort(port, out, max);
      if (port != 0) return 0;
      int n = usb_serial_jtag_read_bytes(out, max, 0);
      return n > 0 ? n : 0;
#else
      if (port != 0) return 0;
      int n = (int)read(STDIN_FILENO, out, max);
      return n > 0 ? n : 0;
#endif
  };
  auto portWrite = [&](int port, const uint8_t* data, size_t len) {
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
      if (consoleOnCdc) { consoleCdcWritePort(port, data, len); return; }
      if (port != 0) return;
      size_t off = 0;
      while (off < len) {
        int w = usb_serial_jtag_write_bytes((const char*)data + off, len - off,
                                            pdMS_TO_TICKS(250));
        if (w <= 0) break;
        off += (size_t)w;
      }
      usb_serial_jtag_ll_txfifo_flush();
#else
      if (port != 0) return;
      fwrite(data, 1, len, stdout);
      fflush(stdout);
#endif
  };

  /* End a handler session and, for port 0, hand the console back to the log. */
  auto hdlDetach = [&](int port) {
      if (hdlHandle[port] < 0) return;
      itsDisconnect(hdlHandle[port]);
      hdlHandle[port] = -1;
      serialHdlPendLen[port] = 0;
      if (port == 0) serialInHandler = false;
  };

  /* A client has appeared on a claimed port: connect it to the handler task and
   * stop treating the port as a console. `first` is the byte that revealed the
   * client on an in-band attach, which belongs to its stream and is forwarded.
   * A rejected connect (the handler already has a session) leaves the port
   * exactly as it was — on port 0, still a console. */
  auto hdlAttach = [&](int port, const uint8_t* first, size_t nFirst) -> bool {
      if (!serialPortIsClaimed(port) || hdlHandle[port] >= 0) return false;
      serial_handler_connect_t hc = { (uint8_t)port };
      int h = itsConnect(serialClaims[port].task, serialClaims[port].itsPort,
                         &hc, sizeof(hc), pdMS_TO_TICKS(500));
      if (h < 0) return false;
      hdlHandle[port] = h;
      serialHdlPendLen[port] = 0;
      if (port == 0) {
        /* A CLI session cannot outlive the port it runs on. Drop it silently —
         * the banner would go to the client, not to a person. */
        if (cliHandle >= 0) { itsDisconnect(cliHandle); cliHandle = -1; }
        serialInCli    = false;
        serialInHandler = true;
        /* Frames are dead while a handler owns the port; a part-assembled one
         * must not survive to be completed by the client's own bytes. */
        rpcReset();
      }
      if (nFirst) itsSend(h, first, nFirst, pdMS_TO_TICKS(50));
      return true;
  };

  /* Shuttle one port's bytes in both directions. */
  auto hdlPump = [&](int port) {
      int h = hdlHandle[port];
      if (h < 0) return;
      /* Carry first: bytes reach the handler in order or not at all. */
      if (serialHdlPendLen[port]) {
        size_t sent = itsSend(h, serialHdlPend[port], serialHdlPendLen[port],
                              pdMS_TO_TICKS(20));
        if (sent >= serialHdlPendLen[port]) {
          serialHdlPendLen[port] = 0;
        } else {
          if (sent) {
            memmove(serialHdlPend[port], serialHdlPend[port] + sent,
                    serialHdlPendLen[port] - sent);
            serialHdlPendLen[port] -= sent;
          }
          return;   /* still backed up — leave the port unread this pass */
        }
      }
      uint8_t rb[128];
      int n = portRead(port, rb, sizeof(rb));
      if (n > 0) {
        size_t sent = itsSend(h, rb, (size_t)n, pdMS_TO_TICKS(20));
        if (sent < (size_t)n) {
          size_t rem = (size_t)n - sent;
          if (rem > sizeof(serialHdlPend[port])) rem = sizeof(serialHdlPend[port]);
          memcpy(serialHdlPend[port], rb + sent, rem);
          serialHdlPendLen[port] = rem;
        }
      }
      uint8_t ob[256];
      for (;;) {
        size_t m = itsRecv(h, ob, sizeof(ob), 0);
        if (m == 0) break;
        portWrite(port, ob, m);
      }
      if (!itsConnected(h)) hdlDetach(port);
  };

  /* Handle one inbound console byte: enter CLI mode on the first keystroke,
   * forward keys to the CLI, treat Ctrl-C as "drop back to log". Reached only
   * for bytes the frame sniffer in handleChar() below did not claim. */
  auto consoleChar = [&](char c) {
      /* In-band attach for a claimed console port on USB-Serial-JTAG, which
       * offers no DTR to watch. 0xC0 is the KISS frame delimiter and opens
       * every such client's first burst; no console keystroke produces it. The
       * byte belongs to the client, so it is forwarded, not swallowed. */
      if ((uint8_t)c == 0xC0 && !consoleOnCdc &&
          serialPortIsClaimed(0) && hdlHandle[0] < 0) {
        uint8_t b = 0xC0;
        if (hdlAttach(0, &b, 1)) return;
      }
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
        return;
      }
      if (cliHandle < 0 && (c == '\n' || c == '\r')) {
        /* Enter is the one key that does not open a session — it is how a
         * session is left, so treating it as the first keystroke of a new one
         * would make leaving impossible. Say what the console is doing instead
         * of swallowing the key, which reads as an unresponsive terminal.
         *
         * The hostname is in the line because Enter is what people press to find
         * out which device they are talking to, and otherwise the only way to
         * learn that is to open a CLI session and read it off the prompt. Same
         * source as the prompt (cliPromptBuild), so the two never disagree. */
        {
          char host[48];
          storageGetStr("s.net.hostname", host, sizeof(host), CONFIG_SPANGAP_FW_HOSTNAME);
          if (!host[0]) safeStrncpy(host, CONFIG_SPANGAP_FW_HOSTNAME, sizeof(host));
          char hint[160];
          int n = snprintf(hint, sizeof hint,
                           "\r\n" RESET "Spangap console on serial %s of '%s'. "
                           "Start typing to enter CLI\r\n",
                           consoleOnCdc ? "cdc 0" : "jtag", host);
          if (n > 0) serialEmit(hint, (size_t)n < sizeof hint ? (size_t)n : sizeof hint - 1);
        }
        /* …and say who we are. A bare Enter is how something announces itself on
         * the other end of the wire, and this is the one moment we know someone
         * is listening — the boot log said all this already, to an empty room.
         * Repeating it here is what lets a flasher identify the board without
         * asking a question, without opening a CLI session, and above all
         * without resetting the device to read it off the chip. */
        spangapLogBuildIdentity();
        cliFlush();
        return;
      }
      if (cliHandle < 0) {
        /* Switch to CLI mode — suppress direct-stdout log echo */
        serialInCli = true;
        cli_connect_t req = { CLI_ANSI, 1, CLI_COLOR, 0, /*login*/0 };
        cliHandle = itsConnect("cli", CLI_PORT_TCP, &req, sizeof(req), pdMS_TO_TICKS(500));
        if (cliHandle >= 0) {
          char host[48];
          storageGetStr("s.net.hostname", host, sizeof(host), CONFIG_SPANGAP_FW_HOSTNAME);
          if (!host[0]) safeStrncpy(host, CONFIG_SPANGAP_FW_HOSTNAME, sizeof(host));
          /* One \r\n, not two: this runs at column 0 (the log line before it
           * ended with a newline), where each \r\n is a blank line of its own.
           *
           * The hostname carries the same bold green cliWritePrompt gives it —
           * this is the one prompt that function does not draw, and spelling it
           * plainly here is what left the first prompt of a session uncoloured
           * while every later one was green. */
          printf("\033[0m\r\nCLI mode, hit return on prompt to return to log\r\n\r\n"
                 CLI_C_HOST "%s" CLI_C_RESET " $ ", host);
          fflush(stdout); cliFlush();
        } else {
          /* connect failed — abort CLI mode */
          serialInCli = false;
        }
      }
      if (cliHandle >= 0) {
        /* Block briefly rather than timeout-0: the cli task's input stream can
         * be momentarily full while it finishes a command, and a 0-timeout send
         * would silently drop the keystroke — the serial console "going deaf"
         * for that key. A short wait rides out the transient without stalling
         * the output-drain below for long. */
        itsSend(cliHandle, &c, 1, pdMS_TO_TICKS(50));
      }
  };

  /* Write one whole reply frame, under the console lock so no log line can land
   * inside it. Raw port I/O, not serialEmit: the payload is length-counted and
   * must not be touched by the console's \n -> \r\n translation. */
  auto rpcReply = [&](uint8_t id, const char* body, size_t len) {
      uint8_t hdr[7];
      memcpy(hdr, rpcMagic, sizeof(rpcMagic));
      hdr[4] = id;
      hdr[5] = (uint8_t)(len >> 8);
      hdr[6] = (uint8_t)(len & 0xff);
      consoleWriteLock();
      portWrite(0, hdr, sizeof(hdr));
      if (len) portWrite(0, (const uint8_t*)body, len);
      consoleWriteUnlock();
  };

  /* Run one command and frame its output back.
   *
   * This reuses the CLI's existing one-shot exec path rather than adding one:
   * a second client connection in LINE mode with the prompt suppressed, fed
   * "<cmd>;\n", where the trailing ';' is the CLI's "run this and close the
   * session" signal — exactly what ssh `exec` does. Its own ITS session means
   * a frame arriving while someone has a console CLI session open leaves that
   * user's line in progress untouched.
   *
   * Synchronous on the serial task by design: a retry that arrives mid-exec
   * waits in the driver's buffer until this returns, so both frames get
   * answered and the host's duplicate-reply rule absorbs the extra. An async
   * implementation would have to choose a policy there instead. */
  auto rpcRun = [&](uint8_t id, const char* cmd, size_t cmdLen) {
      size_t cap = 0;
      char* out = rpcAlloc(0xffff, &cap);
      if (!out) { rpcReply(id, NULL, 0); return; }
      size_t len = 0;
      bool   cut = false;

      cli_connect_t cc = { CLI_LINE, /*from_usb_serial=*/0, CLI_NO_COLOR,
                           /*no_prompt=*/1, /*login=*/0 };
      int h = itsConnect("cli", CLI_PORT_TCP, &cc, sizeof(cc), pdMS_TO_TICKS(500));
      if (h >= 0) {
          std::string line(cmd, cmdLen);
          line += ";\n";
          const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(RPC_EXEC_MS);
          size_t off = 0;
          while (off < line.size() && (int32_t)(xTaskGetTickCount() - deadline) < 0)
              off += itsSend(h, line.data() + off, line.size() - off, pdMS_TO_TICKS(50));
          char buf[256];
          while ((int32_t)(xTaskGetTickCount() - deadline) < 0) {
              itsPoll(pdMS_TO_TICKS(20));
              size_t n = itsRecv(h, buf, sizeof(buf), 0);
              if (n == 0) {
                  if (!itsConnected(h)) break;
                  continue;
              }
              size_t room = cap - len;
              if (n > room) { n = room; cut = true; }
              memcpy(out + len, buf, n);
              len += n;
          }
          /* On the deadline this drops the session mid-command and answers with
           * whatever had been printed — the console is worth more than the
           * remainder of one runaway command's output. */
          itsDisconnect(h);
      }
      /* Truncation is not signalled, so cut at the last complete line. A
       * mid-line cut turns `state=ap` into `state=a`, which parses as a valid
       * but wrong value; dropping the partial tail leaves only the missing-key
       * case that every reader already treats as unknown. */
      if (cut) while (len && out[len - 1] != '\n') len--;
      rpcReply(id, out, len);
      free(out);
  };

  /* Feed one byte to the frame state machine. Returns true when the byte
   * belonged to a frame, false when it is the console's. */
  auto rpcFeed = [&](uint8_t b) -> bool {
      rpcDeadline = xTaskGetTickCount() + pdMS_TO_TICKS(RPC_ASSEMBLE_MS);
      switch (rpcPhase) {
      case RPC_PREAMBLE:
          if (b == rpcMagic[rpcMatched]) {
              if (++rpcMatched == sizeof(rpcMagic)) {
                  rpcPhase   = RPC_HEADER;
                  rpcHeadLen = 0;
              }
              return true;
          }
          /* A failed match replays, it does not drop: the bytes withheld while
           * the magic was partially matched belong to the normal path. The
           * 0xF5 lead makes false starts rare — it cannot occur in UTF-8 text
           * — but pasted garbage exists. */
          {
              uint8_t held = rpcMatched;
              rpcMatched = 0;
              for (uint8_t i = 0; i < held; i++) consoleChar((char)rpcMagic[i]);
          }
          /* The disagreeing byte can itself open a frame (…F5 F5 53 47…). */
          if (b == rpcMagic[0]) { rpcMatched = 1; return true; }
          return false;

      case RPC_HEADER:
          rpcHead[rpcHeadLen++] = b;
          if (rpcHeadLen < sizeof(rpcHead)) return true;
          rpcId   = rpcHead[0];
          rpcWant = (uint16_t)((rpcHead[1] << 8) | rpcHead[2]);
          rpcGot  = 0;
          /* An empty request is not a command; answer it empty rather than
           * running the CLI's own idea of what a bare line means. */
          if (rpcWant == 0) { rpcReply(rpcId, NULL, 0); rpcReset(); return true; }
          {
              size_t got = 0;
              rpcCmd = rpcAlloc(rpcWant, &got);
              /* A command that doesn't fit is a corrupted length, not a real
               * request — swallow the payload so it can't be typed at the CLI,
               * and let the host time out. */
              if (rpcCmd && got < rpcWant) { free(rpcCmd); rpcCmd = NULL; }
          }
          rpcPhase = rpcCmd ? RPC_BODY : RPC_DISCARD;
          return true;

      case RPC_BODY:
          rpcCmd[rpcGot++] = (char)b;
          if (rpcGot < rpcWant) return true;
          {
              uint8_t  id  = rpcId;
              char*    cmd = rpcCmd;
              uint16_t n   = rpcWant;
              rpcCmd = NULL;       /* ownership moves out of the state machine */
              rpcReset();
              rpcRun(id, cmd, n);
              free(cmd);
          }
          return true;

      case RPC_DISCARD:
          if (++rpcGot >= rpcWant) rpcReset();
          return true;
      }
      return false;
  };

  /* Abandon a frame whose remainder never arrived, replaying any bytes the
   * partial magic match is holding on the console's behalf. */
  auto rpcCheckTimeout = [&]() {
      if (!rpcAssembling()) return;
      if ((int32_t)(xTaskGetTickCount() - rpcDeadline) < 0) return;
      uint8_t held = (rpcPhase == RPC_PREAMBLE) ? rpcMatched : 0;
      rpcReset();
      for (uint8_t i = 0; i < held; i++) consoleChar((char)rpcMagic[i]);
  };

  /* Every inbound console byte passes the frame sniffer first. It sits ahead of
   * both the line editor and the log/CLI switch, so a frame arriving mid-line
   * doesn't disturb the editor and one arriving in log mode doesn't flip modes.
   * It also sits ahead of the 0xC0 attach check, so an id or length byte that
   * happens to be 0xC0 cannot open a handler session mid-frame; idle, the 0xC0
   * check keeps its place. */
  auto handleChar = [&](char c) {
      if (rpcFeed((uint8_t)c)) return;
      consoleChar(c);
  };

  /* Console input that predates the console. The USB-Serial-JTAG controller
   * survives the reset that starts this firmware — that is what holds the USB
   * link up across a restart — so its receive path still holds whatever a host
   * wrote while the ROM loader, or a RAM-loaded image, was the thing on the
   * chip. flashmon's peripheral detection is exactly that shape: it uploads a
   * detector to RAM, talks to the port around it, then resets into the real
   * firmware. Handed to consoleChar those bytes read as keystrokes — a CLI
   * session opens on a character nobody typed and the boot log disappears
   * behind it. Nothing that arrived before this point was addressed to this
   * firmware, so drop it. Bounded, so a host that streams continuously cannot
   * hold the task here. */
  {
    const TickType_t until = xTaskGetTickCount() + pdMS_TO_TICKS(50);
    char drop[64];
    while ((int32_t)(xTaskGetTickCount() - until) < 0) {
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
      /* One blocking pass: the driver's ISR has to move the hardware FIFO into
       * its ring before there is anything to read. */
      if (usb_serial_jtag_read_bytes(drop, sizeof(drop), pdMS_TO_TICKS(10)) <= 0) break;
#else
      if (read(STDIN_FILENO, drop, sizeof(drop)) <= 0) break;
#endif
    }
  }

  /* The sniffer is armed from here on. A host tool sends a frame only after it
   * has seen this line, because firmware without the sniffer would take the
   * frame as keystrokes typed at the console — opening a CLI session and
   * suppressing the log, on a device that was never going to answer anyway. So
   * the capability is advertised, never probed. This is a load-bearing log line
   * and an API on both sides: one bit plus a version, emitted before anything
   * can be in flight. */
  info("serial: framed rpc v1\n");

  for (;;) {
    rpcCheckTimeout();
    /* ---- serial-handler bookkeeping, ahead of every console mode ----
     * A claimed port 1 must be shuttled whether or not the console has a CLI
     * session open, and a claim can be taken or dropped from another task at
     * any moment. */
    bool anyHdl = false;
    for (int p = 0; p < SERIAL_PORT_COUNT; p++) anyHdl |= hdlHandle[p] >= 0;
    if (anyHdl) {
      /* Keep this task's ITS inbox drained: a handler-side disconnect arrives
       * as an inbox message, and itsConnected() only reflects it once polled. */
      while (itsPoll(0)) {}
    }
    if (serialClaimChanged) {
      serialClaimChanged = false;
      for (int p = 0; p < SERIAL_PORT_COUNT; p++)
        if (hdlHandle[p] >= 0 && !serialPortIsClaimed(p)) hdlDetach(p);
    }
    /* A port that no longer exists (`usb jtag` took the second CDC port away)
     * cannot carry a session; the claim stays and goes dormant until the
     * claimant re-applies it on the next `usb cdc`. */
    for (int p = serialPortCount(); p < SERIAL_PORT_COUNT; p++) hdlDetach(p);
    for (int p = 0; p < SERIAL_PORT_COUNT; p++) {
      /* Detach before attach: a fast close-then-open leaves both edges posted,
       * and the session that survives must be the newer one. */
      if (serialDetachReq[p]) { serialDetachReq[p] = false; hdlDetach(p); }
      if (serialAttachReq[p]) { serialAttachReq[p] = false; hdlAttach(p, nullptr, 0); }
    }
    /* `usb down` — level-triggered, and last, so nothing re-attaches behind it
     * while the link is gone. */
    if (cliUsbSerialLinkDown)
      for (int p = 0; p < SERIAL_PORT_COUNT; p++) hdlDetach(p);
    hdlPump(1);

    if (hdlHandle[0] >= 0) {
      /* The console port belongs to a handler: no log mirror, no CLI, and no
       * console read — hdlPump has taken the bytes. Waited on a notification so
       * a handler write (ITS) or CDC rx wakes us at once; the timeout is what
       * covers USB-Serial-JTAG, whose ISR-fed ring raises none. */
      hdlPump(0);
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10));
      continue;
    }

#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    if (cliHandle < 0) {
      /* Idle / log mode: log fanout goes direct-to-stdout, so nothing is queued
       * to this task over ITS until a CLI session opens — only a keystroke moves
       * us forward, and there is nothing to poll for (USB recovery is driven by
       * the log task's pmPollUsb, not us). So park indefinitely on the driver's
       * ISR-fed RX ring: a keystroke wakes us at once, and an idle console adds
       * zero wakes — this task drops off the wake path entirely so both cores
       * can light-sleep. */
      char c;
      pmBoostAuto(false);
      /* The auto-resume latch belongs to a session; carrying it into log mode
       * would end the *next* session the moment it opened. cliSerialResumeLog()
       * sets it unconditionally, and `usb cdc` calls that with no session open. */
      cliUsbSerialAutoResumeLog = false;
      if (consoleOnCdc || consoleSwitchPending) {
        /* CDC has no ISR-fed ring to park on, so this path polls. It also covers
         * the switch itself: parking on the old transport while the console is
         * moving would strand this task on a controller that is about to lose
         * the bus. Waited on a notification so a claimed port's rx callback (or
         * a claim change) is serviced without waiting out the interval. */
        if (consoleCdcRead(&c)) { pmBoostAuto(true); handleChar(c); }
        else ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(50));
      } else {
        /* Park indefinitely only when no frame is part-assembled: nothing else
         * wakes this task, so an abandoned frame would never be timed out. */
        TickType_t wait = rpcAssembling() ? pdMS_TO_TICKS(50) : portMAX_DELAY;
        if (usb_serial_jtag_read_bytes(&c, 1, wait) == 1) {
          pmBoostAuto(true);
          handleChar(c);
        }
      }
      continue;
    }

    /* USB console link dropped while this CLI session was open — typically the
     * `usb down` command, which is itself typed on this console, so the session
     * outlives the link. Tear it down and loop back to the blocking log read
     * above instead of polling a disconnected console at 20 Hz. */
    if (cliUsbSerialLinkDown) {
      itsDisconnect(cliHandle);
      cliHandle = -1;
      serialInCli = false;
      continue;
    }
#endif

    while (itsPoll(pdMS_TO_TICKS(50))) {}

    /* Poll serial input from the driver rather than fd 0. A console that has
     * been through a transport switch no longer has its descriptor there —
     * freopen() reopens stdin onto a fresh one, which also leaves the
     * O_NONBLOCK set at task start behind on the old. */
    char c;
    if (consoleOnCdc) { while (consoleCdcRead(&c)) handleChar(c); }
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    else              { while (usb_serial_jtag_read_bytes(&c, 1, 0) == 1) handleChar(c); }
#else
    else              { while (read(STDIN_FILENO, &c, 1) == 1) handleChar(c); }
#endif

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
      /* Check if CLI kicked us. The CLI side already printed the "Returning to
       * log" banner before disconnecting; emit the trailing blank line here so
       * it can't drain late and land after the resumed log output. */
      if (!itsConnected(cliHandle)) {
        cliHandle = -1;
        cliEmitLogResumeGap();
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
        cliEmitLogResumeGap();
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
  /* Before either writer task exists (see consoleWriteLock above). */
  consoleWriteMutex = xSemaphoreCreateRecursiveMutex();

  int v = storageGetInt("s.cli.version", 0);
  if (v < CLI_VERSION) {
    storageBegin();
    storageDefaultTree("s.cli", R"({
      "start_dir": "/"
    })");
    storageUnset("s.cli.sticky");
    storageSet("s.cli.version", CLI_VERSION);
    storageEnd();
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
  serialTaskHandle = spawnTask(serialTaskFn, "serial", 4096, nullptr, 1, 1);
}
