/**
 * CLI — command registry, line editor, CLI/serial tasks.
 */
#ifndef SECCAM_CLI_H
#define SECCAM_CLI_H

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

/** Column width for help alignment. Usage: cliPrintf("%-*s description\n", CLI_HELP_COL, "cmd [args]"); */
#define CLI_HELP_COL 23

/** printf to the active CLI client (ITS handle or serial).
 *  Signature matches plain printf so it can be used as a drop-in `int (*)(const char*, ...)`
 *  print function pointer (e.g. itsStatus(cliPrintf)). Return value is the number of
 *  bytes formatted (pre-truncation), 0 if no output channel is set. */
int cliPrintf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

/** Raw write to active CLI client (e.g. cat). No-op if no session output. */
void cliWrite(const char* data, size_t len);

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

#endif
