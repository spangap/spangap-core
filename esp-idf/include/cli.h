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

typedef struct {
  cli_mode_t mode;
  /** 1 if this client is the device USB serial task — honors s.cli.sticky and trailing ';' to stay in CLI. */
  uint8_t from_usb_serial;
} cli_connect_t;

/* ---- CLI command API ---- */

/** CLI command callback. `args` is everything after the command name (trimmed).
 *  When args == "help", print a one-line description and return. */
typedef void (*cli_cmd_cb_t)(const char* args);

/** Register a CLI command. Sorted alphabetically. Longest-prefix match on dispatch. */
void cliRegisterCmd(const char* cmd, cli_cmd_cb_t cb);

/** Column width for help alignment. Usage: cliPrintf("  %-*s description\n", CLI_HELP_COL, "cmd [args]"); */
#define CLI_HELP_COL 23

/** printf to the active CLI client (ITS handle or serial). */
void cliPrintf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

/** Raw write to active CLI client (e.g. cat). No-op if no session output. */
void cliWrite(const char* data, size_t len);

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
