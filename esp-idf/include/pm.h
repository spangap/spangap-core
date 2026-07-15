/**
 * PM — power management locks, USB management, deep sleep.
 */
#ifndef SPANGAP_PM_H
#define SPANGAP_PM_H

#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

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

/* ---- CPU boost (notify-driven) ----
 * Tasks run at the DFS floor (80 MHz) by default. itsPoll automatically boosts
 * to max for the handling of a wake that came from a *notify* (an ITS message,
 * an ISR like lora's DIO1, input) — i.e. a real event — and stays at the floor
 * for a wake that merely *timed out* (a routine housekeeping tick). The boost is
 * held from the notify-wake until the task's next block. Each task owns a
 * recursive CPU_FREQ_MAX lock named after itself (created lazily on first boost)
 * so `pm` shows per-task boost time; the per-task "do I currently hold the auto
 * count" marker lives in TLS slot TLS_PM_BOOST (slot 0 is left for IDF). See
 * docs/plans/pm-task-boost.md. */
#define TLS_PM_BOOST 1

/** Auto boost, used by itsPoll() and delay(): on=true raises (acquires) the
 *  current task's one auto count after a notify-wake; on=false drops it before a
 *  block. Idempotent + TLS-tracked, so take/drop stay balanced in any order.
 *  Manual pmBoost() counts are separate and survive across these. */
void pmBoostAuto(bool on);

/** True if the current task currently holds its auto boost count. */
static inline bool pmBoostHeld(void) {
  return pvTaskGetThreadLocalStoragePointer(NULL, TLS_PM_BOOST) != NULL;
}

/** Manual sustained boost — for heavy timeout-path work, or continuous loops
 *  (net's select, webrtc) that aren't notify-driven and want 240 MHz held across
 *  their own blocks. Recursive; pair each pmBoost() with a pmBoostEnd(). */
void pmBoost(void);
void pmBoostEnd(void);

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

/** Called on each automatic light-sleep exit, from IDLE-task context, with the
 *  wake cause (an esp_sleep_wakeup_cause_t, e.g. ESP_SLEEP_WAKEUP_GPIO). Runs
 *  after clocks/cache are restored; must not block. A level-triggered GPIO wake
 *  source whose ISR can miss the edge (a press that has bounced back to the
 *  inactive level by the time the post-wake interrupt is sampled) uses this to
 *  re-check the line on the wake itself. */
typedef void (*pm_wake_cb_t)(int cause);

/** Register a light-sleep wake callback. Idempotent per cb; a handful of slots.
 *  Needs CONFIG_PM_LIGHT_SLEEP_CALLBACKS + tickless idle (both on by default). */
void pmOnLightSleepWake(pm_wake_cb_t cb);

/** Record that we're about to enter deep sleep for durationUs microseconds.
 *  Call just before esp_deep_sleep_start(). Stats survive in RTC RAM. */
void pmRecordDeepSleep(int64_t durationUs);

/** Dump all heap state to the log at info level. Call on malloc failure.
 *  Shows DMA/INTERNAL/SPIRAM totals + largest free block + per-task owners
 *  (per-task requires CONFIG_HEAP_TASK_TRACKING). */
void heapDump(const char* reason);

#endif
