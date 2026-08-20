/**
 * cron — minute-resolution task scheduler.
 *
 * Entries are storage keys: s.cron.tab.<name> holds one line in standard unix
 * cron format (5 time fields + flags + command, no user field). Owners create
 * their key while their feature is enabled and delete it when disabled;
 * users add ad-hoc jobs the same way. Commands execute via CLI stream.
 *
 * Scheduling is next-minute based: whenever an entry is added, changed or
 * removed, cron computes the first minute one or more entries must run and
 * stores it in RTC RAM; the periodic check (and a deep-sleep wake) merely
 * compares the clock against it.
 */
#ifndef SPANGAP_CRON_H
#define SPANGAP_CRON_H

/** Spawn cron task and create CLI command stream.
 *  Call from app_main() after fs_init(). */
void cronInit();

/** Run entries due since the last serviced minute (flags applied), then
 *  recompute the stored next-run minute. A cheap compare-and-return while the
 *  stored minute hasn't arrived. Called from the cron task every ~30s and once
 *  at the end of boot (a deep-sleep wake may already have moved time forward
 *  through a scheduled minute). */
void cronPoll();

/** Recompute the stored next-run minutes from the s.cron.tab.* keys and
 *  refresh the deep-sleep lock. Runs on the cron task's s.cron subscription;
 *  the end-of-boot call resyncs entries written before that subscription was
 *  registered. */
void cronReschedule();

/** Deep sleep wakeup handler.  Call early in app_main() after fs_init().
 *  If not a cron deep sleep wakeup: returns false.
 *  If the target minute has arrived: returns true (stay awake).
 *  If woken early (skew margin): re-enters deep sleep (never returns). */
bool cronWakeupHandler();

/** Drain pending cron commands and execute via cliProcess().
 *  Called by CLI task in its main loop. */
void cronDrainCommands();

#endif
