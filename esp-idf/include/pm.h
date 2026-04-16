/**
 * PM — power management locks, USB management, deep sleep.
 */
#ifndef SECCAM_PM_H
#define SECCAM_PM_H

#include <stdint.h>

/** Lock types. ESP types delegate to esp_pm; NO_DEEP_SLEEP is ours. */
enum pm_lock_type_t {
  PM_CPU_FREQ_MAX,      // keep CPU at max frequency
  PM_APB_FREQ_MAX,      // keep APB at max frequency
  PM_NO_LIGHT_SLEEP,    // prevent light sleep
  PM_NO_DEEP_SLEEP      // prevent deep sleep (our own bookkeeping)
};

typedef struct pm_lock* pm_lock_handle_t;

/** Initialize PM: configure DFS + light sleep, create USB lock.
 *  Call once early in app_main(), after fs_init(). */
void pmInit();

/** Poll USB connection state, manage USB PM lock.
 *  Called periodically from log task. */
void pmPollUsb();

/** Register PM CLI commands (pm, top, usb). */
void pmRegisterCmds();

/** Create a named PM lock. name must be a string literal or static. */
void pmLockCreate(pm_lock_type_t type, const char* name, pm_lock_handle_t* out);

/** Acquire (recursive). */
void pmLockAcquire(pm_lock_handle_t handle);

/** Release. */
void pmLockRelease(pm_lock_handle_t handle);

/** Returns true when no locks of any type are held. */
bool deepSleepAllowed();

/** Record that we're about to enter deep sleep for durationUs microseconds.
 *  Call just before esp_deep_sleep_start(). Stats survive in RTC RAM. */
void pmRecordDeepSleep(int64_t durationUs);

#endif
