/**
 * cron — minute-resolution task scheduler.
 *
 * Entries stored in NVS as cron_0, cron_1, ... (sequential, stops at first
 * missing key).  Format: standard unix cron (5 time fields + command), no
 * user field.  Commands execute via CLI stream.
 *
 * Deep sleep: when allowed and cron_0 exists, the cron task enters deep sleep
 * until the next minute boundary (+1s).  cronWakeupHandler() checks on boot
 * whether there's cron work — stays awake if so, otherwise goes back to sleep.
 */
#ifndef SECCAM_CRON_H
#define SECCAM_CRON_H

/** Spawn cron task and create CLI command stream.
 *  Call from app_main() after nvsInit(). */
void cronInit();

/** Check cron entries against current time.
 *  execute=true: update lastMinute RTC state and send matching commands to CLI.
 *  execute=false: dry-run (for deep sleep wakeup check).
 *  Returns true if any entry matched. */
bool cronPoll(bool execute);

/** Deep sleep wakeup handler.  Call early in app_main() after nvsInit().
 *  If not a cron deep sleep wakeup: returns false.
 *  If wakeup with work: clears magic, returns true (stay awake).
 *  If wakeup without work: enters deep sleep (never returns). */
bool cronWakeupHandler();

/** Drain pending cron commands and execute via cliProcess().
 *  Called by CLI task in its main loop. */
void cronDrainCommands();

#endif
