/**
 * cron — minute-resolution task scheduler.
 *
 * Entries stored in /state/crontab file (one per line, standard unix cron
 * format: 5 time fields + command, no user field).
 * Commands execute via CLI stream.
 *
 * Deep sleep: when allowed and crontab has entries, the cron task enters deep sleep
 * until the next minute boundary (+1s).  cronWakeupHandler() checks on boot
 * whether there's cron work — stays awake if so, otherwise goes back to sleep.
 */
#ifndef SPANGAP_CRON_H
#define SPANGAP_CRON_H

/** Spawn cron task and create CLI command stream.
 *  Call from app_main() after fs_init(). */
void cronInit();

/** Check cron entries against current time.
 *  execute=true: update lastMinute RTC state and send matching commands to CLI.
 *  execute=false: dry-run (for deep sleep wakeup check).
 *  Returns true if any entry matched. */
bool cronPoll(bool execute);

/** Deep sleep wakeup handler.  Call early in app_main() after fs_init().
 *  If not a cron deep sleep wakeup: returns false.
 *  If wakeup with work: clears magic, returns true (stay awake).
 *  If wakeup without work: enters deep sleep (never returns). */
bool cronWakeupHandler();

/** Drain pending cron commands and execute via cliProcess().
 *  Called by CLI task in its main loop. */
void cronDrainCommands();

/** Add a crontab entry only if no existing line — active or commented out —
 *  contains the same command string. Schedule is the leading time/flags
 *  fields (e.g. "*\/15 * * * * N"); command is everything after that.
 *
 *  Modules call this from init() once per version bump (gated by their own
 *  s.<mod>.version key) to install their default periodic tasks. Idempotent
 *  across reboots; respects user edits (delete to disable, or comment out
 *  to disable while keeping the line as a hint). Returns true if written. */
bool cronDefault(const char* schedule, const char* command);

#endif
