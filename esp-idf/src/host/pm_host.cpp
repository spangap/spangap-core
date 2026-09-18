/**
 * pm_host.cpp — the power manager on a host that has no power to manage.
 *
 * Every symbol of pm.h, answering the way a station with a cable in it and
 * nothing to save answers: a USB host is attached (which is what keeps the
 * log's direct console echo live), no lock forbids anything because nothing
 * sleeps, and there are no residency figures because there are no power modes
 * to be resident in.
 */
#include "pm.h"

#include "compat.h"
#include "log.h"

/* RTC RAM is the memory that outlives a deep sleep. A process has none: every
 * boot starts with nothing carried over, which is exactly what "not valid"
 * means to every reader. */
bool rtcRamValid()    { return false; }
void rtcRamSetValid() {}

void pmInit() {}
void pmPollUsb() {}

/* The console is the process's own stdout and it is always there. */
bool pmUsbAttached() { return true; }

void pmRegisterCmds() {}
void pmUsbSerialJtagReattach(void) {}

void pmLockCreate(pm_lock_type_t, const char*, pm_lock_handle_t* out) {
    if (out) *out = nullptr;
}
void pmLockAcquire(pm_lock_handle_t) {}
void pmLockRelease(pm_lock_handle_t) {}

/* A process does not deep-sleep; it is stopped by whoever started it. */
bool deepSleepAllowed() { return false; }

void pmBoostAuto(bool) {}
void pmBoost(void) {}
void pmBoostEnd(void) {}

int  pmGpioWakeEnable(int, int) { return 0; }
void pmGpioWakeDisable(int) {}
void pmOnLightSleepWake(pm_wake_cb_t) {}
void pmRecordDeepSleep(int64_t) {}

void heapDump(const char* reason) {
    info("heap: not tracked on this target (%s)", reason ? reason : "");
}

void pmStatsAddSampler(void (*)(void), void (*)(void), void (*)(void)) {}
int  pmStatsHistory(PmStatSample*, int) { return 0; }
int  pmStatsAvg(PmStatAvg*, int) { return 0; }
