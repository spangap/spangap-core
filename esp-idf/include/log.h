#ifndef SECCAM_LOG_H
#define SECCAM_LOG_H

#include <stdint.h>

/* Connect payload for log ITS server */
enum log_ansi_t : uint8_t {
    LOG_ANSI,
    LOG_NO_ANSI,
};

typedef struct {
    log_ansi_t ansi;
} log_connect_t;

#endif
