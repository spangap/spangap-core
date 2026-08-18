/**
 * timezones.h — the firmware's built-in IANA→POSIX timezone table.
 *
 * Two parallel rodata arrays generated at release time by
 * scripts/update-zones.py (`make timezones`) into src/timezones_gen.c and
 * checked in. Nothing is parsed at runtime and nothing lives in RAM: the
 * strings sit in flash, tzLookup() binary-searches the names (the table is
 * sorted by strcmp), and zone data refreshes with every firmware update.
 */
#ifndef SPANGAP_TIMEZONES_H
#define SPANGAP_TIMEZONES_H

#ifdef __cplusplus
extern "C" {
#endif

extern const char* const TZ_NAMES[];   /* IANA names, strcmp-sorted */
extern const char* const TZ_POSIX[];   /* POSIX TZ strings, 1:1 with TZ_NAMES */
extern const int TZ_COUNT;

/** The POSIX TZ string for an IANA name, or NULL if the table lacks it.
 *  Points into rodata — never freed, always valid. */
const char* tzLookup(const char* iana);

#ifdef __cplusplus
}
#endif

#endif
