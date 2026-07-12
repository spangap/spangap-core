/**
 * PM — power management locks, USB management, RTC stats.
 * CLI commands: pm, top, usb.
 */
#include "pm.h"
#include "mem.h"
#include "log.h"
#include "cli.h"
#include "storage.h"
#include "compat.h"
#include <esp_system.h>
#include <esp_heap_caps.h>
#ifdef CONFIG_HEAP_TASK_TRACKING
#include <esp_heap_task_info.h>
#endif
#include <esp_wifi.h>
#include <esp_sleep.h>
#include <esp_pm.h>
#include <esp_timer.h>
#include <driver/gpio.h>
#ifdef CONFIG_PM_PROFILING
#include <esp_private/pm_impl.h>
#endif
#include <esp_private/esp_clk.h>
#include <esp_memory_utils.h>
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
#include <driver/usb_serial_jtag.h>
#include <hal/usb_serial_jtag_ll.h>
#endif
#include <cstdio>
#include <cstring>
#include <algorithm>

/* ---- Central RTC RAM validity ---- */
#define RTC_APP_MAGIC 0x5ECC0001
RTC_DATA_ATTR static uint32_t rtcAppMagic = 0;
bool rtcRamValid()    { return rtcAppMagic == RTC_APP_MAGIC; }
void rtcRamSetValid() { rtcAppMagic = RTC_APP_MAGIC; }

/* ---- Deep sleep + PM mode stats (RTC RAM — survive deep sleep) ---- */
RTC_DATA_ATTR static int32_t rtcDeepSleepCount = 0;
RTC_DATA_ATTR static int64_t rtcDeepSleepUs = 0;
RTC_DATA_ATTR static int32_t rtcPmDeepSleepCount = 0;
RTC_DATA_ATTR static int64_t rtcPmDeepSleepUs = 0;

/* Accumulated awake-mode times from all previous wake sessions */
RTC_DATA_ATTR static int64_t rtcAccumModeUs[PM_MODE_COUNT] = {};
/* Snapshot at last 'pm' command (grand totals at that point) */
RTC_DATA_ATTR static int64_t rtcPmModeUs[PM_MODE_COUNT] = {};
RTC_DATA_ATTR static bool rtcPmEverCalled = false;

/* ---- PM lock bookkeeping (linked list) ---- */

struct pm_lock {
  const char* name;
  pm_lock_type_t type;
  esp_pm_lock_handle_t esp_handle;  // non-null for ESP types, null for deep sleep
  int count;                        // current active count (recursive)
  int times_taken;                  // total acquisitions
  int64_t time_held;                // accumulated hold time (us)
  int64_t last_taken;               // timestamp of last acquire (when count 0→1)
  pm_lock* next;
};

static pm_lock* pmLockList = nullptr;
/* Guards the pmLockList head only. Most locks are created single-threaded at
 * init, but per-task boost locks (below) are created lazily from many tasks at
 * once — two concurrent prepends would otherwise lose-update the list head. */
static portMUX_TYPE pmListMux = portMUX_INITIALIZER_UNLOCKED;

static const esp_pm_lock_type_t espLockTypes[] = {
  ESP_PM_CPU_FREQ_MAX, ESP_PM_APB_FREQ_MAX, ESP_PM_NO_LIGHT_SLEEP
};

void pmLockCreate(pm_lock_type_t type, const char* name, pm_lock_handle_t* out) {
  auto* l = (pm_lock*)gp_calloc(1, sizeof(pm_lock));
  l->name = name;
  l->type = type;
  if (type != PM_NO_DEEP_SLEEP)
    esp_pm_lock_create(espLockTypes[type], 0, name, &l->esp_handle);
  portENTER_CRITICAL(&pmListMux);   /* link last — esp_pm_lock_create must run outside the spinlock */
  l->next = pmLockList;
  pmLockList = l;
  portEXIT_CRITICAL(&pmListMux);
  *out = l;
}

void pmLockAcquire(pm_lock_handle_t h) {
  if (!h) return;
  if (h->count == 0)
    h->last_taken = esp_timer_get_time();
  h->count++;
  h->times_taken++;
  if (h->esp_handle)
    esp_pm_lock_acquire(h->esp_handle);
}

void pmLockRelease(pm_lock_handle_t h) {
  if (!h || h->count <= 0) return;
  h->count--;
  if (h->count == 0)
    h->time_held += esp_timer_get_time() - h->last_taken;
  if (h->esp_handle)
    esp_pm_lock_release(h->esp_handle);
  /* Deep sleep is not supported at the moment. This was its sole trigger:
   * last NO_DEEP_SLEEP lock out → sys.going_down=1 → cron's subscription
   * enters deep sleep. Re-enable together with the going_down subscription
   * and cronDeepSleep() in cron.cpp. */
  // if (deepSleepAllowed())
  //   storageSet("sys.going_down", 1);
}

/* ---- CPU boost (notify-driven) ---- (see pm.h / docs/plans/pm-task-boost.md) */
#if CONFIG_FREERTOS_THREAD_LOCAL_STORAGE_POINTERS <= TLS_PM_BOOST
#error "pm boost needs CONFIG_FREERTOS_THREAD_LOCAL_STORAGE_POINTERS > TLS_PM_BOOST (bump it in sdkconfig.defaults.spangap)"
#endif

/* Each boosting task gets its OWN recursive CPU_FREQ_MAX lock, named after the
 * task and created lazily on first boost. A lock's count aggregates only that
 * task's auto count (0/1, tracked in TLS_PM_BOOST for idempotency) plus its
 * manual pmBoost() depth — the task runs at 240 MHz iff its own count > 0.
 * Per-task locks (vs one shared "boost" lock) mean `pm`/esp_pm_dump_locks list
 * each task's boost time + total_count separately, and a stuck count names its
 * own leaker. */
#define BOOST_MAX_TASKS 24
struct boost_task_t {
  TaskHandle_t     task;
  pm_lock_handle_t lock;
  /* Private copy of the task name: pmLockCreate stores the name pointer by
   * reference, and the TCB name can outlive the task (locks are never freed),
   * so we own a stable string for the lock's lifetime. */
  char             name[configMAX_TASK_NAME_LEN];
};
static boost_task_t s_boostTasks[BOOST_MAX_TASKS];
/* Guards free-slot claiming only — two different tasks must not grab the same
 * slot. Each claimed entry is thereafter private to its one (single-threaded)
 * task, so lookups/acquires need no further locking. */
static portMUX_TYPE s_boostMux = portMUX_INITIALIZER_UNLOCKED;

static pm_lock_handle_t boostLockFind(TaskHandle_t t) {
  for (int i = 0; i < BOOST_MAX_TASKS; i++)
    if (s_boostTasks[i].task == t) return s_boostTasks[i].lock;
  return nullptr;
}

/* Get-or-create the current task's boost lock. Registry full → nullptr and the
 * caller's boost no-ops (BOOST_MAX_TASKS covers every long-lived task with
 * margin). The slot is claimed under the spinlock; the esp_pm lock is created
 * outside it (pmLockCreate allocates + takes its own lock). */
static pm_lock_handle_t boostLockEnsure() {
  TaskHandle_t t = xTaskGetCurrentTaskHandle();
  pm_lock_handle_t l = boostLockFind(t);
  if (l) return l;
  int slot = -1;
  portENTER_CRITICAL(&s_boostMux);
  for (int i = 0; i < BOOST_MAX_TASKS; i++)
    if (!s_boostTasks[i].task) { s_boostTasks[i].task = t; slot = i; break; }
  portEXIT_CRITICAL(&s_boostMux);
  if (slot < 0) return nullptr;
  safeStrncpy(s_boostTasks[slot].name, pcTaskGetName(t), sizeof(s_boostTasks[slot].name));
  pmLockCreate(PM_CPU_FREQ_MAX, s_boostTasks[slot].name, &s_boostTasks[slot].lock);
  return s_boostTasks[slot].lock;
}

/* Lean release for the auto-boost hot path (runs on every itsPoll block): same
 * count/stats bookkeeping as pmLockRelease, but skips the deepSleepAllowed()
 * list-walk + going_down storageSet — boost locks are CPU_FREQ_MAX and never
 * gate deep sleep. (Acquire has no such side effect, so we reuse pmLockAcquire.) */
static void boostReleaseLean(pm_lock_handle_t h) {
  if (!h || h->count <= 0) return;
  h->count--;
  if (h->count == 0)
    h->time_held += esp_timer_get_time() - h->last_taken;
  if (h->esp_handle)
    esp_pm_lock_release(h->esp_handle);
}

/* Per-task auto boost. TLS slot holds this task's boost lock while it holds its
 * one auto count, else nullptr — so take/drop are idempotent and balanced
 * regardless of call order. Manual pmBoost() counts share the same per-task lock
 * but are NOT tracked in TLS, so they survive across blocks independently. */
void pmBoostAuto(bool on) {
  void* held = pvTaskGetThreadLocalStoragePointer(NULL, TLS_PM_BOOST);
  if (on && !held) {
    pm_lock_handle_t l = boostLockEnsure();
    if (!l) return;
    vTaskSetThreadLocalStoragePointer(NULL, TLS_PM_BOOST, l);
    pmLockAcquire(l);
  } else if (!on && held) {
    vTaskSetThreadLocalStoragePointer(NULL, TLS_PM_BOOST, nullptr);
    boostReleaseLean((pm_lock_handle_t)held);
  }
}

void pmBoost(void)    { pm_lock_handle_t l = boostLockEnsure(); if (l) pmLockAcquire(l); }
void pmBoostEnd(void) { pm_lock_handle_t l = boostLockFind(xTaskGetCurrentTaskHandle());
                        if (l && l->count > 0) pmLockRelease(l); }

static pm_lock_handle_t usbLock = nullptr;
RTC_DATA_ATTR static bool rtcUsbDisabled = false;

bool deepSleepAllowed() {
  for (pm_lock* l = pmLockList; l; l = l->next)
    if (l->count > 0) return false;
  return true;
}

static const char* pmTypeName(pm_lock_type_t t) {
  switch (t) {
    case PM_CPU_FREQ_MAX:  return "CPU_FREQ_MAX";
    case PM_APB_FREQ_MAX:  return "APB_FREQ_MAX";
    case PM_NO_LIGHT_SLEEP: return "NO_LIGHT_SLEEP";
    case PM_NO_DEEP_SLEEP:  return "NO_DEEP_SLEEP";
  }
  return "?";
}

/* Capture an esp_pm dump into a heap buffer so we can reformat / splice it. */
struct pm_buf_t { char* buf; size_t pos; size_t cap; };
static int pmBufWrite(void* cookie, const char* data, int len) {
  auto* s = (pm_buf_t*)cookie;
  size_t n = (s->pos + len < s->cap) ? (size_t)len : s->cap - s->pos - 1;
  memcpy(s->buf + s->pos, data, n);
  s->pos += n;
  s->buf[s->pos] = '\0';
  return len;
}

/** Dump every PM lock. esp_pm_dump_locks() lists the IDF-internal ones we
 *  otherwise can't see (wifi, rtos0/1, driver locks) and is NULL-name-safe
 *  since IDF 5.5 (prints lock@<ptr>). It can't see our PM_NO_DEEP_SLEEP locks
 *  (no esp_handle), so we splice those into its table. Under profiling its
 *  output also carries Mode + Sleep stats — we drop the Mode table (the `pm`
 *  summary already reports those counters, correctly bucketed) and keep Sleep
 *  stats, whose light-sleep reject count is the "why no light sleep" signal. */
static void pmDumpLocks() {
  char* buf = (char*)gp_alloc(4096);
  if (buf) {
    pm_buf_t st = { buf, 0, 4096 };
    FILE* f = funopen(&st, nullptr, pmBufWrite, nullptr, nullptr);
    if (f) { esp_pm_dump_locks(f); fclose(f); } else buf[0] = '\0';
  }
  if (!buf || !buf[0]) {                 /* fallback: at least our own locks */
    free(buf);
    cliPrintf("\nPM locks (own only):\n");
    for (pm_lock* l = pmLockList; l; l = l->next) {
      if (l->type == PM_NO_DEEP_SLEEP) continue;   /* deep sleep unsupported — hidden */
      cliPrintf("%-15.15s %-14s  %d\n", l->name ? l->name : "?",
                pmTypeName(l->type), l->count);
    }
    return;
  }

  /* esp_pm_dump_locks' lock table, then our NO_DEEP_SLEEP rows. Under profiling
   * its output also trails a "Mode stats" table and a "Sleep stats" block. We
   * DROP the Mode stats table: it's the same per-mode counters the summary above
   * already reports (correctly bucketed), and printing both raw-and-summary was
   * the source of the "two different numbers" confusion. We keep Sleep stats —
   * its light-sleep reject count is the "why no light sleep" signal. */
  char* mode      = strstr(buf, "\nMode stats:");
  char* sleepStat = strstr(buf, "\nSleep stats:");
  size_t head = mode ? (size_t)(mode - buf) : strlen(buf);
  cliPrintf("\n");
  cliWrite(buf, head);
  /* Deep sleep is not supported at the moment — the NO_DEEP_SLEEP rows (our
   * own locks, spliced in because esp_pm can't see them) are hidden. */
#if 0
  for (pm_lock* l = pmLockList; l; l = l->next) {
    if (l->type != PM_NO_DEEP_SLEEP) continue;   /* esp-backed locks already listed */
#ifdef CONFIG_PM_PROFILING
    int64_t now = esp_timer_get_time(), now_d100 = now / 100;
    int64_t held = l->time_held + (l->count > 0 ? now - l->last_taken : 0);
    cliPrintf("%-15.15s %-14s  %-5d  %-8d  %-13d  %-14lld  %-3lld%%\n",
              l->name ? l->name : "?", pmTypeName(l->type), 0,
              l->count, l->times_taken, held,
              now_d100 ? (held + now_d100 - 1) / now_d100 : 0LL);
#else
    cliPrintf("%-15.15s %-14s  %-5d  %-8d\n",
              l->name ? l->name : "?", pmTypeName(l->type), 0, l->count);
#endif
  }
#endif
  if (sleepStat) cliWrite(sleepStat, strlen(sleepStat));  /* Sleep stats only (Mode stats == summary above) */
  free(buf);
}

void pmInit() {
  esp_pm_config_t pm = { .max_freq_mhz = 240, .min_freq_mhz = 80, .light_sleep_enable = true };
  esp_err_t err = esp_pm_configure(&pm);
  dbg("pm: configured 240/80 MHz + light sleep (%s)\n", esp_err_to_name(err));

  pmLockCreate(PM_NO_LIGHT_SLEEP, "usb", &usbLock);
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
  if (rtcRamValid() && rtcUsbDisabled) {
    /* USB was disabled before deep sleep — keep it off */
    usb_serial_jtag_pull_override_vals_t vals = { .dp_pu = false, .dm_pu = false,
                                                   .dp_pd = false, .dm_pd = false };
    usb_serial_jtag_ll_phy_enable_pull_override(&vals);
  } else {
    rtcUsbDisabled = false;
    pmLockAcquire(usbLock);
  }
#else
  /* UART console: no USB peer state, no SOF traffic to gate light sleep on.
   * The "usb" lock stays released; light sleep is governed by other holders. */
  rtcUsbDisabled = false;
#endif
}

#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
static void cliUsbUp();   /* defined below; pmPollUsb auto-recovers with it */
#endif

void pmPollUsb() {
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
  if (!usbLock) return;

  /* Evaluate at ~1 Hz regardless of how often the log loop calls us.
   * usb_serial_jtag_is_connected() is SOF-based and noisy at sub-second
   * rates — a single false sample used to release the lock and latch
   * light sleep on. */
  static uint32_t lastEvalMs = 0;
  uint32_t now = millis();
  if (now - lastEvalMs < 1000) return;
  lastEvalMs = now;

  static const uint32_t USB_GRACE_MS = 5000;
  bool inGrace = now < USB_GRACE_MS;
  bool connected = usb_serial_jtag_is_connected();
  bool held = usbLock->count > 0;

  /* Explicit cliUsbDown(): release immediately, no debounce. */
  if (rtcUsbDisabled) {
    if (held) pmLockRelease(usbLock);
    return;
  }

  /* Up acts immediately (keep the console alive ASAP); down needs 3
   * consecutive 1 Hz samples so a brief SOF gap — e.g. a monitor
   * respawn around a reflash — can't latch light sleep on. */
  static uint8_t  downStreak = 0;
  static uint32_t lastRecoverMs = 0;   /* 0 = none attempted this outage */
  if (connected || inGrace) {
    downStreak = 0;
    lastRecoverMs = 0;
    if (!held) pmLockAcquire(usbLock);
  } else {
    if (downStreak < 3) downStreak++;
    if (downStreak >= 3) {
      if (!lastRecoverMs || now - lastRecoverMs >= 60000) {
        /* Sustained loss. Force a clean re-enumeration: if a host is
         * actually attached (slow monitor respawn, or the controller
         * wedged after a light-sleep nap), cliUsbUp() resets the
         * peripheral + re-asserts the D+ pull-up so the host re-detects
         * us. It also re-acquires the lock; give it ~3 s to come back.
         * Retried every 60 s, never just once: a wedge means the
         * controller can't see SOF *at all*, so if the single attempt
         * misses the host (or the host is plugged in only after we've
         * already slept) nothing would ever read connected again — the
         * console would be dead until reboot. Cost of retrying while
         * genuinely unplugged: ~3 s of held lock per minute. */
        lastRecoverMs = now ? now : 1;
        downStreak = 0;
        cliUsbUp();
      } else if (held) {
        /* Recovery didn't bring a host back — no host right now.
         * Release so light sleep can proceed; the 60 s retry re-checks. */
        pmLockRelease(usbLock);
      }
    }
  }
#else
  /* UART console: no peer presence to track. */
#endif
}

static void cliUsbDown() {
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
  info("usb down\n");
  vTaskDelay(pdMS_TO_TICKS(10));
  /* Disable D+ pullup → host sees disconnect, stops SOF packets */
  usb_serial_jtag_pull_override_vals_t vals = { .dp_pu = false, .dm_pu = false,
                                                 .dp_pd = false, .dm_pd = false };
  usb_serial_jtag_ll_phy_enable_pull_override(&vals);
  rtcUsbDisabled = true;
  if (usbLock->count > 0)
    pmLockRelease(usbLock);
#else
  info("usb down: no-op on UART console\n");
#endif
}

static void cliUsbUp() {
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
  /* Acquire lock first to prevent light sleep during re-enumeration */
  rtcUsbDisabled = false;
  if (usbLock && usbLock->count == 0)
    pmLockAcquire(usbLock);
  /* Reset the USB Serial JTAG peripheral — clears internal state machine
   * that may be confused after light sleep gated the USB clock. */
  int __DECLARE_RCC_ATOMIC_ENV __attribute__((unused));
  usb_serial_jtag_ll_reset_register();
  /* Re-enable internal PHY + pads (reset clears these) */
  usb_serial_jtag_ll_phy_enable_external(false);
  usb_serial_jtag_ll_phy_enable_pad(true);
  usb_serial_jtag_ll_phy_disable_pull_override();
  info("usb up\n");
#else
  info("usb up: no-op on UART console\n");
#endif
}

#ifdef CONFIG_PM_PROFILING
/* Parse esp_pm_impl_dump_stats' "Mode stats" table into per-mode microseconds. */
static void pmParseModeStats(int64_t modeUs[PM_MODE_COUNT]) {
  char buf[512] = {};
  pm_buf_t state = {buf, 0, sizeof(buf)};
  FILE* f = funopen(&state, nullptr, pmBufWrite, nullptr, nullptr);
  if (!f) return;
  esp_pm_impl_dump_stats(f);
  fclose(f);

  static const char* names[] = {"SLEEP", "APB_MIN", "APB_MAX", "CPU_MAX"};
  char* line = buf;
  while (line && *line) {
    char name[16] = {};
    long long us = 0;
    if (sscanf(line, " %15s %*[^M]M %lld", name, &us) == 2) {
      for (int i = 0; i < PM_MODE_COUNT; i++)
        if (strcmp(name, names[i]) == 0) modeUs[i] = us;
    }
    char* nl = strchr(line, '\n');
    line = nl ? nl + 1 : nullptr;
  }
}

static void pmPrintModeLines(int64_t mode[PM_MODE_COUNT], int64_t deepUs,
                             int64_t wall, int32_t dsCount = 0) {
  if (wall <= 0) return;
  int dsPct     = (int)(deepUs * 100 / wall);
  int sleepPct  = (int)(mode[PM_MODE_LIGHT_SLEEP] * 100 / wall);
  int cpuMaxPct = (int)(mode[PM_MODE_CPU_MAX] * 100 / wall);
  /* APB_MIN and APB_MAX BOTH run the CPU at the DFS floor (80 MHz); they differ
   * only in whether the APB bus is pinned high. So "80 MHz" is their sum and
   * only CPU_MAX is 240 MHz. (The old split folded APB_MAX — CPU 80 — into the
   * 240 MHz line, so a chip pinned at APB-max by a radio read as "240 MHz".) */
  int cpu80Pct  = 100 - dsPct - sleepPct - cpuMaxPct;
  /* Share of wall time the APB bus was pinned high while awake (APB_MAX +
   * CPU_MAX). This — not CPU speed — is what holds off light sleep: a high value
   * alongside a LOW 240 MHz figure is a peripheral/radio APB lock (Wi-Fi,
   * SoftAP), not CPU load. Orthogonal to the CPU lines, so it doesn't sum in. */
  int apbHiPct  = (int)((mode[PM_MODE_APB_MAX] + mode[PM_MODE_CPU_MAX]) * 100 / wall);
  /* Deep sleep is not supported at the moment — hide its always-0 line. dsPct
   * stays in the cpu80Pct math above so the split still sums when re-enabled. */
  (void)dsCount;
  // cliPrintf("deep sleep    %d%% (%d)\n", dsPct, (int)dsCount);
  cliPrintf("light sleep   %d%%\n", sleepPct);
  cliPrintf("CPU  80 MHz   %d%%\n", cpu80Pct);
  cliPrintf("CPU 240 MHz   %d%%\n", cpuMaxPct);
  cliPrintf("\nAPB_FREQ_MAX  %d%%  (prevents light sleep)\n", apbHiPct);
}
#endif

/* ---- GPIO wake source ---- */

static bool s_gpioWakeArmed = false;

int pmGpioWakeEnable(int pin, int wakeLevel) {
  if (wakeLevel != GPIO_INTR_HIGH_LEVEL && wakeLevel != GPIO_INTR_LOW_LEVEL) {
    err("pmGpioWakeEnable: pin %d wake level must be HIGH_LEVEL or LOW_LEVEL, got %d",
        pin, wakeLevel);
    return ESP_ERR_INVALID_ARG;
  }
  esp_err_t r = gpio_set_intr_type((gpio_num_t)pin, (gpio_int_type_t)wakeLevel);
  if (r != ESP_OK) {
    err("pmGpioWakeEnable: gpio_set_intr_type(%d): %s", pin, esp_err_to_name(r));
    return r;
  }
  r = gpio_wakeup_enable((gpio_num_t)pin, (gpio_int_type_t)wakeLevel);
  if (r != ESP_OK) {
    err("pmGpioWakeEnable: gpio_wakeup_enable(%d): %s", pin, esp_err_to_name(r));
    return r;
  }
  if (!s_gpioWakeArmed) {
    r = esp_sleep_enable_gpio_wakeup();
    if (r != ESP_OK) {
      err("pmGpioWakeEnable: esp_sleep_enable_gpio_wakeup: %s", esp_err_to_name(r));
      return r;
    }
    s_gpioWakeArmed = true;
  }
  dbg("pm: GPIO %d armed as wake source (level=%s)", pin,
      wakeLevel == GPIO_INTR_HIGH_LEVEL ? "HIGH" : "LOW");
  return ESP_OK;
}

void pmGpioWakeDisable(int pin) {
  gpio_wakeup_disable((gpio_num_t)pin);
  /* We do not turn off the global esp_sleep_enable_gpio_wakeup() — other
   * pins may still rely on it, and it's a no-op for unconfigured pins. */
}

void pmRecordDeepSleep(int64_t durationUs) {
  rtcDeepSleepCount++;
  rtcDeepSleepUs += durationUs;
#ifdef CONFIG_PM_PROFILING
  /* Capture current awake-mode stats and accumulate into RTC —
     ESP PM stats reset on deep sleep wake */
  int64_t modeUs[PM_MODE_COUNT] = {};
  pmParseModeStats(modeUs);
  for (int i = 0; i < PM_MODE_COUNT; i++)
    rtcAccumModeUs[i] += modeUs[i];
#endif
}

/* ---- CLI commands: pm, top, usb ---- */

static void cmdUsb(const char* a) {
    if (cliWantsHelp(a)) { cliPrintf("%-*s USB status; up/down to reconnect/disconnect\n", CLI_HELP_COL, "usb [up|down]"); return; }
    if (strcmp(a, "down") == 0) cliUsbDown();
    else if (strcmp(a, "up") == 0) cliUsbUp();
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    else cliPrintf("usb: %s\n", usb_serial_jtag_is_connected() ? "connected" : "disconnected");
#else
    else cliPrintf("usb: n/a\n");
#endif
}

static void cmdPm(const char* args) {
    if (cliWantsHelp(args)) { cliPrintf("%-*s power management status (-v: lock stats); wifi none|min|max\n", CLI_HELP_COL, "pm [-v] [wifi ...]"); return; }
    if (strstr(args, "allow") || strstr(args, "inhibit")) {
        static pm_lock_handle_t cliDeep = nullptr, cliLight = nullptr, cliSlow = nullptr;
        static bool deepHeld = false, lightHeld = false, slowHeld = false;
        if (!cliDeep) {
            pmLockCreate(PM_NO_DEEP_SLEEP, "cli", &cliDeep);
            pmLockCreate(PM_NO_LIGHT_SLEEP, "cli", &cliLight);
            pmLockCreate(PM_CPU_FREQ_MAX, "cli", &cliSlow);
        }
        bool inhibit = strstr(args, "inhibit") != nullptr;
        if (strstr(args, "deep")) {
            if (inhibit && !deepHeld) { pmLockAcquire(cliDeep); deepHeld = true; }
            if (!inhibit && deepHeld) { pmLockRelease(cliDeep); deepHeld = false; }
        } else if (strstr(args, "light")) {
            if (inhibit && !lightHeld) { pmLockAcquire(cliLight); lightHeld = true; }
            if (!inhibit && lightHeld) { pmLockRelease(cliLight); lightHeld = false; }
        } else if (strstr(args, "slow")) {
            if (inhibit && !slowHeld) { pmLockAcquire(cliSlow); slowHeld = true; }
            if (!inhibit && slowHeld) { pmLockRelease(cliSlow); slowHeld = false; }
        }
        return;
    }
    if (strncmp(args, "wifi", 4) == 0) {
        const char* sub = args + 4;
        while (*sub == ' ') sub++;
        if (strcmp(sub, "none") == 0) { esp_wifi_set_ps(WIFI_PS_NONE); cliPrintf("wifi ps: off\n"); }
        else if (strcmp(sub, "min") == 0) { esp_wifi_set_ps(WIFI_PS_MIN_MODEM); cliPrintf("wifi ps: min\n"); }
        else if (strcmp(sub, "max") == 0) { esp_wifi_set_ps(WIFI_PS_MAX_MODEM); cliPrintf("wifi ps: max\n"); }
        else { wifi_ps_type_t ps; esp_wifi_get_ps(&ps);
            cliPrintf("wifi ps: %s\n", ps == WIFI_PS_NONE ? "none" : ps == WIFI_PS_MIN_MODEM ? "min" : "max"); }
        return;
    }
    /* pm status */
    cliPrintf("Now:\n\n  CPU: %d MHz, APB: %d MHz\n",
        esp_clk_cpu_freq() / 1000000, esp_clk_apb_freq() / 1000000);
#ifdef CONFIG_PM_PROFILING
    { int64_t modeUs[PM_MODE_COUNT] = {};
      pmParseModeStats(modeUs);
      int64_t grand[PM_MODE_COUNT], grandAwake = 0;
      for (int i = 0; i < PM_MODE_COUNT; i++) { grand[i] = rtcAccumModeUs[i] + modeUs[i]; grandAwake += grand[i]; }
      if (rtcPmEverCalled) {
        int64_t delta[PM_MODE_COUNT], deltaAwake = 0;
        for (int i = 0; i < PM_MODE_COUNT; i++) { delta[i] = grand[i] - rtcPmModeUs[i]; if (delta[i]<0) delta[i]=0; deltaAwake += delta[i]; }
        int64_t dsUsDelta = rtcDeepSleepUs - rtcPmDeepSleepUs;
        int64_t wall = deltaAwake + dsUsDelta;
        char el[20]; fmtElapsed((uint32_t)(wall / 1000000), el, sizeof(el));
        cliPrintf("\nSince last 'pm' (%s ago):\n\n", el);
        pmPrintModeLines(delta, dsUsDelta, wall, rtcDeepSleepCount - rtcPmDeepSleepCount);
      }
      rtcPmEverCalled = true;
      for (int i = 0; i < PM_MODE_COUNT; i++) rtcPmModeUs[i] = grand[i];
      rtcPmDeepSleepCount = rtcDeepSleepCount; rtcPmDeepSleepUs = rtcDeepSleepUs;
      cliPrintf("\nSince boot:\n\n");
      pmPrintModeLines(grand, rtcDeepSleepUs, grandAwake + rtcDeepSleepUs, rtcDeepSleepCount);
    }
#endif
    if (strstr(args, "-v")) pmDumpLocks();   /* lock + mode + sleep stats are opt-in */
}

static void cmdTop(const char* args) {
    if (cliWantsHelp(args)) { cliPrintf("%-*s tasks, CPU%%, heap, uptime (-v: stack/byte/block detail + heap tables; human: readable sizes; -d/-p: sort by DRAM/PSRAM)\n", CLI_HELP_COL, "top [-v] [human] [-d|-p]"); return; }
    const bool verbose = (strstr(args, "-v") != nullptr);
    const bool human   = (strstr(args, "human") != nullptr);
    const bool byDram  = (strstr(args, "-d") != nullptr);
    const bool byPsram = (strstr(args, "-p") != nullptr);
    const int  bw = human ? 7 : 8;   /* byte-column width: fits 16MB raw, tighter for -h */
    /* Format a byte count: raw decimal by default, fmtSize ("198kB") with -h.
       Returns buf so it can be used inline as a printf %s arg. */
    auto fmtBytes = [human](char* buf, size_t bufsz, size_t bytes) -> const char* {
        if (human) fmtSize((uint32_t)bytes, buf, bufsz);
        else       snprintf(buf, bufsz, "%u", (unsigned)bytes);
        return buf;
    };
#if CONFIG_FREERTOS_USE_TRACE_FACILITY && CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
    /* Size the snapshot buffers to the live task count, plus headroom for tasks
       that may spawn during the 2s sampling window. A fixed cap (was 32) silently
       truncated the task set once the system grew past it, and since the order
       from uxTaskGetSystemState() isn't stable, a task could land in snap2 with
       no match in snap1 -> prev stayed 0 -> delta became the task's *lifetime*
       run counter instead of its 2s slice, printing bogus >100% rows. */
    const int maxSnap = (int)uxTaskGetNumberOfTasks() + 8;
    struct snap {
        TaskHandle_t h;
        char name[configMAX_TASK_NAME_LEN];
        uint32_t run;
        int core; int pri; uint16_t stack;
        /* Filled after matching against heap task totals */
        size_t dram, psram; uint16_t dblk, pblk;
        uint32_t delta;
    };
    auto* s1 = (snap*)gp_alloc(maxSnap * sizeof(snap));
    auto* s2 = (snap*)gp_alloc(maxSnap * sizeof(snap));
    if (!s1 || !s2) { free(s1); free(s2); cliPrintf("top: out of memory\n"); return; }
    memset(s1, 0, maxSnap * sizeof(snap));
    memset(s2, 0, maxSnap * sizeof(snap));
    auto takeSnap = [](snap* out, int max, uint32_t& total) -> int {
        UBaseType_t n = uxTaskGetNumberOfTasks();
        auto* raw = (TaskStatus_t*)gp_alloc(n * sizeof(TaskStatus_t));
        if (!raw) return 0;
        n = uxTaskGetSystemState(raw, n, &total);
        int cnt = n < (UBaseType_t)max ? (int)n : max;
        for (int i = 0; i < cnt; i++) {
            out[i].h = raw[i].xHandle;
            safeStrncpy(out[i].name, raw[i].pcTaskName, configMAX_TASK_NAME_LEN);
            out[i].run = raw[i].ulRunTimeCounter;
            out[i].pri = (int)raw[i].uxCurrentPriority;
            out[i].stack = raw[i].usStackHighWaterMark;
#if CONFIG_FREERTOS_VTASKLIST_INCLUDE_COREID
            out[i].core = raw[i].xCoreID == tskNO_AFFINITY ? -1 : (int)raw[i].xCoreID;
#else
            out[i].core = -2;
#endif
        }
        free(raw);
        return cnt;
    };
    uint32_t t1 = 0, t2 = 0;
    int n1 = takeSnap(s1, maxSnap, t1);
    vTaskDelay(pdMS_TO_TICKS(2000));
    int n2 = takeSnap(s2, maxSnap, t2);
    uint32_t deltaTotal = t2 - t1;

    /* Compute per-task CPU delta; also capture IDLE0/IDLE1 for per-core busy. */
    uint32_t idle0 = 0, idle1 = 0;
    for (int i = 0; i < n2; i++) {
        uint32_t prev = 0; bool found = false;
        for (int j = 0; j < n1; j++)
            if (s1[j].h == s2[i].h) { prev = s1[j].run; found = true; break; }
        /* No baseline (task spawned mid-window, or a reused handle) -> report 0%
           rather than letting delta default to the full lifetime counter. */
        s2[i].delta = found ? (s2[i].run - prev) : 0;
        if (strcmp(s2[i].name, "IDLE0") == 0) idle0 = s2[i].delta;
        else if (strcmp(s2[i].name, "IDLE1") == 0) idle1 = s2[i].delta;
    }

    /* Merge per-task heap totals into s2 by TaskHandle_t; accumulate unmatched. */
    size_t preDram = 0, preDblk = 0, preP = 0, prePblk = 0;
    size_t delDram = 0, delDblk = 0, delP = 0, delPblk = 0;
#ifdef CONFIG_HEAP_TASK_TRACKING
    { constexpr size_t MAX_HT = 32;
      PSRAM_BSS static heap_task_totals_t htotals[MAX_HT];
      memset(htotals, 0, sizeof(htotals));
      size_t ntot = 0;
      heap_task_info_params_t p = {};
      p.caps[0] = MALLOC_CAP_INTERNAL; p.mask[0] = MALLOC_CAP_INTERNAL;
      p.caps[1] = MALLOC_CAP_SPIRAM;   p.mask[1] = MALLOC_CAP_SPIRAM;
      p.totals = htotals; p.num_totals = &ntot; p.max_totals = MAX_HT;
      heap_caps_get_per_task_info(&p);
      for (size_t i = 0; i < ntot; i++) {
          if (htotals[i].task == nullptr) {
              preDram += htotals[i].size[0];  preDblk += htotals[i].count[0];
              preP    += htotals[i].size[1];  prePblk += htotals[i].count[1];
              continue;
          }
          bool found = false;
          for (int j = 0; j < n2; j++) {
              if (s2[j].h == htotals[i].task) {
                  s2[j].dram  = htotals[i].size[0];  s2[j].dblk = htotals[i].count[0];
                  s2[j].psram = htotals[i].size[1];  s2[j].pblk = htotals[i].count[1];
                  found = true; break;
              }
          }
          if (!found) {
              delDram += htotals[i].size[0];  delDblk += htotals[i].count[0];
              delP    += htotals[i].size[1];  delPblk += htotals[i].count[1];
          }
      }
    }
#endif

    /* Fold the pre-scheduler and deleted-task heap aggregates into the table as
       pseudo-rows (h == nullptr) so they sort by RAM alongside real tasks under
       -d/-p instead of always trailing at the bottom. */
    auto addAgg = [&](const char* nm, size_t dr, size_t db, size_t ps, size_t pb) {
        if ((!dr && !ps) || n2 >= maxSnap) return;
        snap& e = s2[n2++];
        memset(&e, 0, sizeof(e));
        safeStrncpy(e.name, nm, sizeof(e.name));
        e.h = nullptr;
        e.dram = dr; e.dblk = (uint16_t)db; e.psram = ps; e.pblk = (uint16_t)pb;
    };
    addAgg("pre-sched", preDram, preDblk, preP, prePblk);
    addAgg("(deleted)", delDram, delDblk, delP, delPblk);

    /* Sort (IDLE always last): -d/-p order by that RAM type desc; otherwise real
       tasks pinned by core then CPU desc, with the aggregate rows trailing. */
    std::sort(s2, s2 + n2, [byDram, byPsram](const snap& a, const snap& b) {
        bool ai = strncmp(a.name, "IDLE", 4) == 0;
        bool bi = strncmp(b.name, "IDLE", 4) == 0;
        if (ai != bi) return !ai;
        if (byDram)  return a.dram  > b.dram;
        if (byPsram) return a.psram > b.psram;
        bool as = a.h == nullptr, bs = b.h == nullptr;
        if (as != bs) return !as;
        int ca = a.core < 0 ? 99 : a.core, cb = b.core < 0 ? 99 : b.core;
        return ca != cb ? ca < cb : a.delta > b.delta;
    });

    /* Per-task percentages are each row's share of the total RAM of that type
     * physically detected on the chip (not just what's currently allocated), so
     * a column shows true occupancy and won't sum to 100%. */
    size_t totalDram  = heap_caps_get_total_size(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    size_t totalPsram = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    /* Byte count and its share of the column total, as two separate strings so
       the byte value sits under the DRAM/PSRAM header and the % gets its own. */
    auto fmtMem = [&fmtBytes](char* numOut, size_t numSz, char* pctOut, size_t pctSz,
                              size_t bytes, size_t total) {
        fmtBytes(numOut, numSz, bytes);
        unsigned p10 = total ? (unsigned)((uint64_t)bytes * 1000 / total) : 0;
        snprintf(pctOut, pctSz, "%u.%u%%", p10 / 10, p10 % 10);
    };

    /* Verbose: Task | Core | Pri | Stack | CPU% | DRAM % blk | PSRAM % blk.
       Default: Task | Core | Pri | CPU% | DRAM% | PSRAM% (percentages only). */
    if (verbose)
        cliPrintf("%-12s %4s  %3s  %7s %7s %*s %6s %5s %*s %6s %5s\n",
                  "Task", "Core", "Pri", "Stack", "CPU%",
                  bw, "DRAM", "%", "blk", bw, "PSRAM", "%", "blk");
    else
        cliPrintf("%-12s %4s  %3s  %7s %7s %7s\n",
                  "Task", "Core", "Pri", "CPU%", "DRAM%", "PSRAM%");
    for (int i = 0; i < n2; i++) {
        if (strncmp(s2[i].name, "IDLE", 4) == 0) continue;
        char drNum[16], drPct[10], psNum[16], psPct[10];
        fmtMem(drNum, sizeof(drNum), drPct, sizeof(drPct), s2[i].dram, totalDram);
        fmtMem(psNum, sizeof(psNum), psPct, sizeof(psPct), s2[i].psram, totalPsram);
        if (s2[i].h == nullptr) {     /* pre-sched / (deleted): no CPU/stack to show */
            if (verbose)
                cliPrintf("%-12s %4s  %3s  %7s %7s %*s %6s %5u %*s %6s %5u\n",
                    s2[i].name, "-", "-", "-", "-",
                    bw, drNum, drPct, (unsigned)s2[i].dblk,
                    bw, psNum, psPct, (unsigned)s2[i].pblk);
            else
                cliPrintf("%-12s %4s  %3s  %7s %7s %7s\n",
                    s2[i].name, "-", "-", "-", drPct, psPct);
            continue;
        }
        unsigned p10 = deltaTotal > 0 ? (unsigned)(s2[i].delta * 1000 / deltaTotal) : 0;
        char cb[16];
        if (s2[i].core < 0) strcpy(cb, s2[i].core == -2 ? " ?" : " *");
        else snprintf(cb, sizeof(cb), " %d", s2[i].core);
        char cpuBuf[16];
        snprintf(cpuBuf, sizeof(cpuBuf), "%u.%u%%", p10 / 10, p10 % 10);
        if (!verbose) {
            cliPrintf("%-12s %4s  %3d  %7s %7s %7s\n",
                s2[i].name, cb, s2[i].pri, cpuBuf, drPct, psPct);
            continue;
        }
        char stackBuf[12];
        const void* stkBase = pxTaskGetStackStart(s2[i].h);
        char stkMem = esp_ptr_external_ram(stkBase) ? 'P' : 'D';
        snprintf(stackBuf, sizeof(stackBuf), "%u %c", (unsigned)s2[i].stack, stkMem);
        cliPrintf("%-12s %4s  %3d  %7s %7s %*s %6s %5u %*s %6s %5u\n",
            s2[i].name, cb, s2[i].pri, stackBuf, cpuBuf,
            bw, drNum, drPct, (unsigned)s2[i].dblk,
            bw, psNum, psPct, (unsigned)s2[i].pblk);
    }

    /* Per-core CPU busy: deltaTotal on SMP FreeRTOS is wall-time ticks (not
     * summed across cores), so each IDLE task's delta is its core's idle
     * fraction of that same window. */
    unsigned b0 = (deltaTotal > idle0) ? (unsigned)((deltaTotal - idle0) * 1000 / deltaTotal) : 0;
    unsigned b1 = (deltaTotal > idle1) ? (unsigned)((deltaTotal - idle1) * 1000 / deltaTotal) : 0;
    cliPrintf("\nTotal CPU:\n");
    cliPrintf("  core0: %u.%u%%\n", b0/10, b0%10);
    cliPrintf("  core1: %u.%u%%\n", b1/10, b1%10);

    /* Used = allocated heap = total - free, from the same caps the Heap table
     * prints, so used + free == total and this can't disagree with the free line
     * below. Task stacks/TCBs are heap_caps allocations, so they're already in
     * the allocated figure (and show up per-creator, mostly under (deleted)) —
     * summing the per-task stack column on top would double-count them. */
    if (verbose) {
        size_t freeDram  = heap_caps_get_free_size(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
        size_t freePsram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        size_t ramDram  = totalDram  > freeDram  ? totalDram  - freeDram  : 0;
        size_t ramPsram = totalPsram > freePsram ? totalPsram - freePsram : 0;
        char rb[16], rt[16], pb[16];
        unsigned dp10 = totalDram  ? (unsigned)((uint64_t)ramDram  * 1000 / totalDram)  : 0;
        unsigned pp10 = totalPsram ? (unsigned)((uint64_t)ramPsram * 1000 / totalPsram) : 0;
        cliPrintf("\nTotal RAM usage:\n");
        snprintf(pb, sizeof(pb), "(%u.%u%%)", dp10 / 10, dp10 % 10);
        cliPrintf("  DRAM:  %*s / %*s %8s\n",
            bw, fmtBytes(rb, sizeof(rb), ramDram),  bw, fmtBytes(rt, sizeof(rt), totalDram),  pb);
        snprintf(pb, sizeof(pb), "(%u.%u%%)", pp10 / 10, pp10 % 10);
        cliPrintf("  PSRAM: %*s / %*s %8s\n",
            bw, fmtBytes(rb, sizeof(rb), ramPsram), bw, fmtBytes(rt, sizeof(rt), totalPsram), pb);
    }

    free(s1); free(s2);
#endif
    if (verbose) {
        cliPrintf("\nHeap:\n");
        char ha[16], hc[16], hd[16];
        cliPrintf("%-6s %*s %*s  %10s\n", "", bw, "free", bw, "lowest", "contiguous");
        cliPrintf("%-6s %*s %*s  %10s\n", "DRAM",
            bw, fmtBytes(ha, sizeof(ha), heap_caps_get_free_size(MALLOC_CAP_8BIT|MALLOC_CAP_INTERNAL)),
            bw, fmtBytes(hc, sizeof(hc), heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT|MALLOC_CAP_INTERNAL)),
            fmtBytes(hd, sizeof(hd), heap_caps_get_largest_free_block(MALLOC_CAP_8BIT|MALLOC_CAP_INTERNAL)));
        cliPrintf("%-6s %*s %*s  %10s\n", "PSRAM",
            bw, fmtBytes(ha, sizeof(ha), heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
            bw, fmtBytes(hc, sizeof(hc), heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM)),
            fmtBytes(hd, sizeof(hd), heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)));
        cliPrintf("%-6s %*s %*s  %10s\n", "DMA",
            bw, fmtBytes(ha, sizeof(ha), heap_caps_get_free_size(MALLOC_CAP_DMA)),
            bw, fmtBytes(hc, sizeof(hc), heap_caps_get_minimum_free_size(MALLOC_CAP_DMA)),
            fmtBytes(hd, sizeof(hd), heap_caps_get_largest_free_block(MALLOC_CAP_DMA)));
    } else {
        cliPrintf("\nUse top -v for more detailed memory information\n");
    }

    uint32_t up = (uint32_t)(esp_timer_get_time() / 1000000ULL);
    char eb[32]; fmtElapsed(up, eb, sizeof(eb));
    cliPrintf("\nUptime: %s\n", eb);
}

/* ---- Heap dump on malloc failure ---- */

static void heapRegionInfo(const char* tag, uint32_t caps) {
    multi_heap_info_t h;
    heap_caps_get_info(&h, caps);
    char f[16], t[16], m[16], lg[16];
    size_t total = h.total_free_bytes + h.total_allocated_bytes;
    info("  %-8s free %s / %s  min %s  largest %s  free_blk %u  tot_blk %u\n",
        tag,
        fmtSize(h.total_free_bytes, f, sizeof(f)),
        fmtSize(total, t, sizeof(t)),
        fmtSize(h.minimum_free_bytes, m, sizeof(m)),
        fmtSize(h.largest_free_block, lg, sizeof(lg)),
        (unsigned)h.free_blocks, (unsigned)h.total_blocks);
}

void heapDump(const char* reason) {
    info("heap dump: %s\n", reason ? reason : "(no reason)");
    heapRegionInfo("DMA",      MALLOC_CAP_DMA);
    heapRegionInfo("INTERNAL", MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    heapRegionInfo("SPIRAM",   MALLOC_CAP_SPIRAM);
#ifdef CONFIG_HEAP_TASK_TRACKING
    constexpr size_t MAX_TASKS = 24;
    static heap_task_totals_t totals[MAX_TASKS];
    memset(totals, 0, sizeof(totals));
    size_t ntotals = 0;
    heap_task_info_params_t p = {};
    p.caps[0] = MALLOC_CAP_INTERNAL; p.mask[0] = MALLOC_CAP_INTERNAL;
    p.caps[1] = MALLOC_CAP_SPIRAM;   p.mask[1] = MALLOC_CAP_SPIRAM;
    p.totals = totals;
    p.num_totals = &ntotals;
    p.max_totals = MAX_TASKS;
    heap_caps_get_per_task_info(&p);
    std::sort(totals, totals + ntotals,
        [](const heap_task_totals_t& a, const heap_task_totals_t& b) {
            return a.size[0] > b.size[0];   /* largest internal-DRAM user first */
        });
    info("  per-task (task: internal / spiram, blocks):\n");
    for (size_t i = 0; i < ntotals; i++) {
        const char* nm = totals[i].task ? pcTaskGetName(totals[i].task) : "pre-sched";
        char fi[16], fp[16];
        info("    %-12s %s / %s  (%u / %u blk)\n",
            nm,
            fmtSize(totals[i].size[0], fi, sizeof(fi)),
            fmtSize(totals[i].size[1], fp, sizeof(fp)),
            (unsigned)totals[i].count[0], (unsigned)totals[i].count[1]);
    }
#endif
}

void pmRegisterCmds() {
    cliRegisterCmd("pm", cmdPm);
    cliRegisterCmd("top", cmdTop);
    cliRegisterCmd("usb", cmdUsb);
}
