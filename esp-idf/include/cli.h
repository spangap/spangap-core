#ifndef SECCAM_CLI_H
#define SECCAM_CLI_H

#include <stdint.h>

/* Connect payload for CLI ITS server */
enum cli_mode_t : uint8_t {
    CLI_ANSI,       /* interactive: char-by-char, line editing, echo, ANSI colors */
    CLI_LINE,       /* line mode: complete lines, no echo, no prompt */
};

typedef struct {
    cli_mode_t mode;
} cli_connect_t;

#endif
