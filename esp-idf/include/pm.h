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

/** Register a GPIO as a light-sleep wake source.
 *
 *  ESP-IDF couples GPIO wakeup config with the per-pin interrupt type:
 *  enabling wakeup forces the pin to `GPIO_INTR_HIGH_LEVEL` or
 *  `GPIO_INTR_LOW_LEVEL` (edges aren't detectable in light sleep — the
 *  GPIO peripheral clock is gated). Callers must therefore be ready for
 *  a level-triggered ISR: disable the GPIO interrupt inside the ISR (or
 *  rely on the registering HAL doing so), and re-enable it after
 *  servicing the peripheral so the line drops back to the inactive
 *  level.
 *
 *  The first call also enables `esp_sleep_enable_gpio_wakeup()`. Each
 *  call sets `gpio_wakeup_enable(pin, level)` and `gpio_set_intr_type`
 *  on the pin. Multiple pins may be registered.
 *
 *  @param pin        GPIO number (0..GPIO_NUM_MAX-1, must support RTC
 *                    GPIO wakeup on the target chip).
 *  @param wakeLevel  `GPIO_INTR_HIGH_LEVEL` or `GPIO_INTR_LOW_LEVEL`. */
int pmGpioWakeEnable(int pin, int wakeLevel);

/** Disable light-sleep wakeup on this pin. */
void pmGpioWakeDisable(int pin);

/** Record that we're about to enter deep sleep for durationUs microseconds.
 *  Call just before esp_deep_sleep_start(). Stats survive in RTC RAM. */
void pmRecordDeepSleep(int64_t durationUs);

/** Dump all heap state to the log at info level. Call on malloc failure.
 *  Shows DMA/INTERNAL/SPIRAM totals + largest free block + per-task owners
 *  (per-task requires CONFIG_HEAP_TASK_TRACKING). */
void heapDump(const char* reason);

/** Register a board callback that cuts power to the peripherals (e.g. drives a
 *  power-enable GPIO to its inactive level). resetOnOffHandler() invokes it
 *  just before deep sleep. Register before diptychInit(). NULL = no-op. */
void resetOnOffSetPowerOff(void (*fn)(void));

/** Reset button as on/off switch. Called inside diptychInit() (right after the
 *  deep-sleep wake handler) and a no-op unless s.sys.reset_on_off is set. On a
 *  reset-button press it toggles the persisted s.sys.power_on state: "on" lets
 *  boot continue; "off" cuts peripheral power (via resetOnOffSetPowerOff) and
 *  deep-sleeps until the next reset. State lives in flash, not RTC, because the
 *  EN-pin reset power-cycles the RTC domain. Software/panic/watchdog reboots
 *  and deep-sleep wakes are not button presses and keep the device on. */
void resetOnOffHandler();

#endif
