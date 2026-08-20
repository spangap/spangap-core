/**
 * CLI — command registry, line editor, CLI/serial tasks.
 */
#ifndef SPANGAP_CLI_H
#define SPANGAP_CLI_H

/* Not transitively: SPANGAP_CDC_BUILT below reads CONFIG_ symbols, and an
 * undefined one silently evaluates to 0 — the wrong answer, not an error. */
#include "sdkconfig.h"

#include <stddef.h>
#include <stdint.h>

/** CLI task's ITS server ports.
 *    CLI_PORT_TCP: stream-mode, for raw TCP `nc` access and the on-device
 *                  serial task.
 *    CLI_PORT_DC:  packet-mode, one DataChannel message per keystroke
 *                  burst in / output chunk out; addressed as `cli:1`. */
static constexpr uint16_t CLI_PORT_TCP = 8081;
static constexpr uint16_t CLI_PORT_DC  = 1;

/* ---- Connect payload for CLI ITS server ---- */

enum cli_mode_t : uint8_t {
    CLI_ANSI,       /* interactive: char-by-char, line editing, echo, ANSI colors */
    CLI_LINE,       /* line mode: complete lines, no echo, no prompt */
};

/* Whether the CLI emits ANSI *color* escapes (the cyan input echo + resets).
 * Orthogonal to cli_mode_t: cursor/line-edit control sequences are always sent
 * in CLI_ANSI mode — this gates color only. Mirrors log's LOG_ANSI/LOG_NO_ANSI.
 * Value 0 == CLI_COLOR so a zero-filled / legacy connect payload keeps color
 * (back-compat: interactive CLI_ANSI clients were colored by default). */
enum cli_color_t : uint8_t {
    CLI_COLOR,      /* emit color escapes */
    CLI_NO_COLOR,   /* suppress color; keep cursor/editing sequences */
};

typedef struct {
  /** CLI_ANSI or CLI_LINE for interactive or line mode. */ 
  cli_mode_t mode;
  /** 1 if this client is the device USB serial task — stays in CLI mode
   *  until an empty return, a trailing ';', or Ctrl-C switches back to log. */
  uint8_t from_usb_serial;
  /** Color policy; defaults to CLI_COLOR (0) when the field is absent/zeroed. */
  cli_color_t color;
  /** 1 = suppress the connect-time prompt. For one-shot clients (ssh `exec`,
   *  which sends "cmd;\n" and is closed by the trailing ';') the prompt would
   *  just prefix the command output. Interactive/`nc` clients leave it 0 so
   *  they can read-until-prompt. */
  uint8_t no_prompt;
  /** 1 = force an admin-password login before any command is accepted. The CLI
   *  prints "Enter admin password: " on connect and routes every byte to the
   *  password check (echoed as '*') until authLogin(pw,"admin") succeeds; no
   *  command runs until then, and three failures drop the session. The gate is
   *  enforced entirely server-side — a client that sets this bit cannot be
   *  circumvented by the far end. Used by rnsh's server to expose the CLI over
   *  Reticulum. Legacy/short payloads default to 0 (no login). */
  uint8_t login;
} cli_connect_t;

/* ---- CLI command API ---- */

/** CLI command callback. `args` is everything after the command name (trimmed).
 *
 *  Help convention (uniform across all commands):
 *    - args == "help"            → print ONE short line for the `help` listing.
 *    - args == "-h" / "--help"   → print fuller per-command help (usage,
 *                                  subcommands). For simple commands this is
 *                                  the same single line.
 *    - args == ""                → show status (no separate "status" verb).
 *  Use cliWantsHelp(args) to cover all three help spellings in one guard when a
 *  command's brief and detailed help are identical. */
typedef void (*cli_cmd_cb_t)(const char* args);

/** Register a CLI command. Sorted alphabetically. Longest-prefix match on dispatch. */
void cliRegisterCmd(const char* cmd, cli_cmd_cb_t cb);

/** True for "help", "-h", or "--help" — any help request. */
bool cliWantsHelp(const char* args);

/** True when `tok` is an abbreviation of `full`: a prefix of it, at least
 *  `minLen` characters long. This is how a verb takes a short form — `a` for
 *  `announce` — and `minLen` is what keeps a one-letter form from reaching a
 *  verb that shares its first letters with another. A longer word that merely
 *  starts the same way (`announces` against `announce`) is not a prefix and
 *  does not match, so the two stay distinct commands. */
bool cliVerbIs(const char* tok, const char* full, size_t minLen);

/** Column width for help alignment. Usage: cliPrintf("%-*s description\n", CLI_HELP_COL, "cmd [args]"); */
#define CLI_HELP_COL 23

/** printf to the active CLI client (ITS handle or serial).
 *  Signature matches plain printf so it can be used as a drop-in `int (*)(const char*, ...)`
 *  print function pointer (e.g. itsStatus(cliPrintf)). Return value is the number of
 *  bytes formatted (pre-truncation), 0 if no output channel is set. */
int cliPrintf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

/** Raw write to active CLI client (e.g. cat). No-op if no session output. */
void cliWrite(const char* data, size_t len);

/** True iff the active CLI slot wants ANSI *color* — i.e. it's in CLI_ANSI mode
 *  AND its connect payload asked for CLI_COLOR. Commands that colorize their own
 *  output (e.g. `ls` directory entries) gate the escapes on this so no-color /
 *  line-mode / dumb clients get clean plain text. Cursor/line-edit sequences are
 *  separate and not governed by this. */
bool cliWantsColor(void);

/* Shared ANSI color escapes for CLI output. Emit these only when
 * cliWantsColor() is true (the color escapes are inert width-0 sequences, so
 * they never disturb column math — but a no-color terminal would show the raw
 * bytes). */
#define CLI_C_RESET "\033[0m"
#define CLI_C_HOST  "\033[1;32m"   /* prompt hostname — bold green */
#define CLI_C_DIR   "\033[1;34m"   /* directories     — bold blue  */
#define CLI_C_INPUT "\033[36m"     /* input echo      — cyan       */

/** Echo policy for cliReadLine. */
enum cli_echo_t : uint8_t {
    CLI_ECHO,        /* echo each character back to the client */
    CLI_ECHO_STARS,  /* echo each character as '*' (password fields) */
    CLI_ECHO_NONE,   /* echo nothing (silent password entry) */
};

/** Blocking single-line input read from the active CLI client. Intended to be
 *  called from inside a CLI command callback (e.g. `ssh` prompting for a
 *  password); it reads the same input stream the line editor would, so it
 *  works for serial, TCP/nc and browser sessions alike. Handles backspace
 *  (BS/DEL) and ends on CR or LF. Ctrl-C and Ctrl-D (on an empty line) abort.
 *  Writes a trailing CRLF after the line. Returns the number of characters
 *  read (>= 0, NUL-terminated in `out`), or -1 if aborted / no active session.
 *  `out` is always NUL-terminated on return (empty string on -1). */
int cliReadLine(char* out, size_t outLen, cli_echo_t echo);

/** Raw read from the active CLI client: up to `outLen` bytes verbatim — no
 *  echo, no line editing, no escape stripping. Blocks at most `timeoutMs`.
 *  Returns the byte count (>0), 0 on timeout, or -1 if there's no active
 *  session / it closed. For char-level relays such as an interactive ssh
 *  shell, where keystrokes (incl. control/escape bytes) must pass through
 *  untouched and output streams back concurrently. */
int cliReadRaw(char* out, size_t outLen, int timeoutMs);

/** Active CLI client's terminal size (columns/rows), as reported at connect
 *  time; defaults to 80x24 when the client didn't say. Used by the ssh client
 *  for its pty-req so ncurses apps get the right geometry. */
void cliTermSize(int* cols, int* rows);

/** Session working directory (default /sdcard when no interactive slot, e.g. cron). */
void cliGetCwd(char* out, size_t outLen);

/** Set cwd to an absolute normalized path; directory must exist. */
bool cliSetCwd(const char* absolutePath);

/** Reset session cwd to s.cli.start_dir (default /sdcard). No-op if no active CLI slot. */
void cliCdToStartDir();

/** Resolve path relative to session cwd; empty userPath → cwd. Fails if result too long. */
bool cliResolveFsPath(const char* userPath, char* out, size_t outLen);

/** Process a single CLI command string (e.g. "set key=value"). */
void cliProcess(const char* line);

/** Run a file as a CLI script (one command per line). */
void cliRunFile(const char* path);

/** Create CLI and serial tasks. Call after logInit(). */
void cliInit();

/** Write straight to the serial console, bypassing both the CLI session and the
 *  log. For output that must survive a moment when the ordinary paths cannot
 *  carry it — notably just after a transport switch, where the log's route is
 *  gated on a connection flag that has not caught up yet and discards whole
 *  writes until it does. */
void consoleWriteRaw(const char* data, size_t len);

/** Push anything the console has buffered onto the wire — the stream buffer and,
 *  on USB-Serial-JTAG, the hardware TX FIFO, which holds its contents until a
 *  newline or an explicit flush. Needed after a transport switch, where output
 *  written while the console was moving otherwise waits for whatever writes
 *  next: press a key an hour later and the backlog arrives then. */
void consoleFlush(void);

/** End the serial console's CLI session and hand it back to the live log, the
 *  way a trailing ';' does. For a command whose consequences must be visible as
 *  log output, and which the CLI session itself cannot outlive. */
void cliSerialResumeLog(void);

/** The same, but before this call returns rather than after the command does.
 *  For a command that restarts the device: the deferred form is acted on by the
 *  CLI task once the command returns, and `reboot` / `reset factory` never do —
 *  so the session ends by the chip going away, leaving the host terminal in
 *  whatever colour the CLI set and the entire boot log that follows wearing it.
 *  Call it before esp_restart(). */
void cliSerialResumeLogNow(void);

/** Wake the CLI task. For producers that queue work the CLI task must drain but
 *  whose delivery carries no ITS notification of its own — currently cron, which
 *  writes commands into a raw stream buffer. Safe from any task; a no-op before
 *  the CLI task exists. */
void cliWake();

/** True when the `usb cdc` console transport is built (CONFIG_SPANGAP_USB_CDC,
 *  off by default, and only meaningful where the console is on USB). It is what
 *  puts the TinyUSB device stack in the image, and with it the second serial
 *  port; everything below reduces to one port when it is 0. */
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG && CONFIG_SPANGAP_USB_CDC
#define SPANGAP_CDC_BUILT 1
#else
#define SPANGAP_CDC_BUILT 0
#endif

/** Serial ports the image can ever present — the ceiling the registry is sized
 *  to, not what exists right now (`sys.usb.serial_ports` is that). */
#if SPANGAP_CDC_BUILT
#define SERIAL_PORT_COUNT 2
#else
#define SERIAL_PORT_COUNT 1
#endif

/* ---- Serial-port handlers ----
 *
 * A task can claim a serial port and become the endpoint for whatever attaches
 * to it, in place of the log/CLI console. Port 0 is the console port — the
 * USB-Serial-JTAG controller, or CDC 0 while the console runs on `usb cdc`.
 * Port 1 is the second CDC port, which exists only while the console is on CDC
 * and only in a build with SPANGAP_CDC_BUILT; `sys.usb.serial_ports` publishes
 * how many ports exist (1 or 2), so a claimant can re-apply its claim when the
 * transport changes.
 *
 * A claim is dormant until a client actually attaches, and how a client is
 * detected is the claimant's choice:
 *
 * - **In-band trigger** (a claim made with `trigger`/`triggerLen`): the port
 *   attaches when the trigger byte sequence arrives in the input stream, on
 *   any transport. On the console port, bytes extending a partial match are
 *   withheld and replayed on a mismatch, so console typing is unaffected; the
 *   matched trigger belongs to the client's stream and is forwarded to the
 *   handler. DTR is ignored for attach (a host merely opening the port is not
 *   a client) but a DTR drop on CDC still releases an attached session; on
 *   USB-Serial-JTAG, which has no line state at all, release comes from the
 *   handler's own disconnect or the USB link going down. This is the only
 *   detection that works on the console port of USB-Serial-JTAG, and the only
 *   one that works through a relay that forwards bytes but not line state.
 *
 * - **DTR** (a claim without a trigger, CDC ports only): the host's DTR rise
 *   attaches, its drop releases — every pyserial-class client raises DTR on
 *   open and drops it on close. Any terminal that opens the port is treated
 *   as a client.
 *
 * While a port is attached, its byte stream is connected to the handler task
 * over ITS (serial_handler_connect_t is the connect payload) and log/CLI are
 * detached from it; on release the console returns.
 *
 * The esptool reset convention on CDC 0 is suppressed while a session is
 * attached (a client close drops DTR before RTS, which is the reset
 * sequence's own shape) and, for DTR claims, while the claim exists at all.
 * A dormant trigger claim leaves it armed: until a client speaks, the port is
 * fully an ordinary console, auto-reset included.
 */

/** Claim serial port `port` for `task`, which must have an ITS server port
 *  `itsPort` open. `trigger`/`triggerLen` (≤ 8 bytes) select in-band attach
 *  detection — see above; without them a CDC port attaches on DTR and a
 *  USB-Serial-JTAG console port can never attach. Returns false (and warns)
 *  for an out-of-range port, for port 1 while only one serial port exists, or
 *  when another task already holds the port. Re-claiming with the same task,
 *  ITS port and trigger succeeds unchanged, so a claimant can re-apply on
 *  every config pass. */
bool serialPortClaim(int port, const char* task, uint16_t itsPort,
                     const uint8_t* trigger = nullptr, size_t triggerLen = 0);

/** True while a client session is attached on `port` (between the handler
 *  connect and the release). Safe from any task. */
extern "C" bool serialPortIsAttached(int port);

/** Drop a claim, ending any attached session and returning port 0 to the
 *  console. */
void serialPortRelease(int port);

/** Connect payload the serial machinery sends a handler task. Its length is
 *  what tells a handler this session came from a serial port rather than from
 *  a network transport dialling the same ITS port. */
typedef struct {
  /** Which serial port attached: 0 = console port, 1 = second CDC port. */
  uint8_t serialPort;
} serial_handler_connect_t;

#endif
