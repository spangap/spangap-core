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

/** Is a USB-serial host attached? The debounced state pmPollUsb() tracks — the
 *  same one the `usb` NO_LIGHT_SLEEP lock follows, so it is true through the
 *  boot grace window and false after `usb down`. Raw
 *  usb_serial_jtag_is_connected() is SOF-based and noisy at sub-second rates;
 *  this is the settled answer, and the serial task is notified on the rising
 *  edge so it can park indefinitely while it reads false. */
bool pmUsbAttached();

/** Register PM CLI commands (pm, top, usb). */
void pmRegisterCmds();

/** Route the USB PHY back to the USB-Serial-JTAG controller and re-arm it.
 *  Needed after another controller has held the PHY — they share one, and the
 *  RX interrupt does not survive the handover. No-op on a UART console. */
void pmUsbSerialJtagReattach(void);

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

/* ---- CPU / PM activity ring (1 Hz history for the on-device activity graph) ----
 * A background sampler records one sample per second. Each holds integer-percent
 * figures; apbMin (the 80 MHz APB-locked-but-not-boosted residency) is derived by
 * the reader as 100 - sleep - apbMax - cpuMax.
 *
 * The sampler and its ring are flag-driven on every build: they exist only while
 * an Activity monitor is watching (sys.stats.web_actmon / .lcd_actmon set), and
 * when the last watcher leaves the task terminates and the ring is freed; the next
 * watcher respawns it with a fresh, zeroed ring. A headless node can therefore
 * start sampling — and publishing the sys.stats.avg.* figures — by setting a flag
 * by hand. */
struct PmStatSample { uint8_t core0, core1, sleep, apbMax, cpuMax; };

/** Register callbacks tied to the shared CPU/PM sampler.
 *  - tick fires once per second while the sampler runs (right after it records
 *    its own sample), letting another straddle piggy-back 1 Hz work on the
 *    shared cadence (e.g. -net's Wi-Fi traffic ring).
 *  - onStart/onStop (optional) fire on the transitions in and out of the
 *    running state — i.e. when the first watcher arrives and when the last one
 *    leaves — so a piggy-backed sampler can allocate and free its own buffers in
 *    lockstep with the CPU/PM ring. onStop runs on the sampler task just before
 *    it terminates; onStart runs on the task that spawns it.
 *  All fire only while a UI consumer is watching, never on a headless build.
 *  A few slots; call at init. */
void pmStatsAddSampler(void (*tick)(void), void (*onStart)(void) = nullptr,
                       void (*onStop)(void) = nullptr);

/** Copy up to `max` most-recent ring samples into out[], oldest first so
 *  out[n-1] is the latest second. Returns the count written (0 if no sample has
 *  been produced yet, the ring is disabled, or the build lacks run-time stats).
 *  Thread-safe. */
int pmStatsHistory(PmStatSample* out, int max);

/* Averaged PM-mode residency over the recent ring window, plus a rough current
 * estimate. Percentages are integer; the current is in tenths of a milliamp
 * (mA10 = 83 → 8.3 mA) so the whole path stays integer. */
struct PmStatAvg { uint8_t cpuMax, apbMax, apbMin, sleep; int mA10; };

/** Average the last `secs` ring samples (capped to what the ring holds; secs<=0
 *  → 300) into *out, and fill its mA10 current estimate. Returns the number of
 *  samples averaged (0 → *out is all-zero). Thread-safe. */
int pmStatsAvg(PmStatAvg* out, int secs);

#endif
