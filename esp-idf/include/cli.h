/**
 * CLI — command registry, line editor, CLI/serial tasks.
 */
#ifndef SECCAM_CLI_H
#define SECCAM_CLI_H

#include <stdint.h>

/* ---- Connect payload for CLI ITS server ---- */

enum cli_mode_t : uint8_t {
    CLI_ANSI,       /* interactive: char-by-char, line editing, echo, ANSI colors */
    CLI_LINE,       /* line mode: complete lines, no echo, no prompt */
};

typedef struct {
    cli_mode_t mode;
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

/** Process a single CLI command string (e.g. "set key=value"). */
void cliProcess(const char* line);

/** Run a file as a CLI script (one command per line). */
void cliRunFile(const char* path);

/** Create CLI and serial tasks. Call after logInit(). */
void cliInit();

#endif
