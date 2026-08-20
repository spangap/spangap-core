/**
 * PM — power management locks, USB management, RTC stats.
 * CLI commands: pm, top, usb.
 */
#include "pm.h"
#include "mem.h"
#include "log.h"
#include "spangap.h"   /* humanDetected — a USB host is a person with a cable */
#include "cli.h"
#include "storage.h"
#include "compat.h"
#include "its.h"
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
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <cstdio>
#include <cstring>
#include <algorithm>

/* 1 Hz CPU/PM stats sampler. Not started by pmInit and not always-on: the
   sampler task and its history ring exist only while an Activity monitor is
   watching. A subscription on sys.stats.web_actmon / .lcd_actmon (set up on the
   log task, see pmStatsPoll) spawns them when a flag goes set and tears them down
   (freeing the ring) when the last watcher leaves. This is flag-driven on every
   build, so a headless node with the flag never set never allocates the ring nor
   wakes the sampler — yet can still be started by hand for its published
   averages. `top` samples inline when the background sampler isn't running. */

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
  /* Refresh the name in place (the lock holds it by reference, so this updates
   * the lock's displayed name too). Reuse a reclaimed slot's lock — a prior task
   * here released it on death — rather than leaking a fresh lock object each
   * stop/start cycle; only create one the first time a slot is used. */
  safeStrncpy(s_boostTasks[slot].name, pcTaskGetName(t), sizeof(s_boostTasks[slot].name));
  if (!s_boostTasks[slot].lock)
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

/* TLS deletion callback: fires when a task that still HOLDS its auto boost is
 * deleted (e.g. a lifecycle task woken by a stop-notify — which boosts — then
 * vTaskDelete). Without this its CPU_FREQ_MAX lock stays acquired for the life of
 * the process: the CPU never drops to light sleep (a ~28 mA floor) and a
 * re-spawned same-named task can't reclaim it (the slot is keyed by task handle).
 * `value` is the task's boost lock, or null if it died un-boosted (nothing to do).
 * Runs during the idle task's cleanup; the slot free is spinlock-guarded. */
static void boostTlsDelCb(int /*idx*/, void* value) {
  pm_lock_handle_t l = (pm_lock_handle_t)value;
  if (!l) return;
  boostReleaseLean(l);          /* drop the one auto count the TLS represented */
  portENTER_CRITICAL(&s_boostMux);
  for (int i = 0; i < BOOST_MAX_TASKS; i++)
    if (s_boostTasks[i].lock == l) { s_boostTasks[i].task = nullptr; break; }
  portEXIT_CRITICAL(&s_boostMux);
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
    /* AndDelCallback so a task deleted while boosted releases the lock (boostTlsDelCb). */
    vTaskSetThreadLocalStoragePointerAndDelCallback(NULL, TLS_PM_BOOST, l, boostTlsDelCb);
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

static void pmStatsPoll();   /* start/stop the sampler off the actmon flags */

/* True while `usb cdc` has the console on a TinyUSB CDC port, which owns the
 * USB PHY the USB-Serial-JTAG controller would otherwise be driving. */
extern "C" volatile bool consoleOnCdc;
extern "C" const char*   consoleModeName(void);
extern "C" int           consoleCdcPortCount(void);
extern "C" void          consoleCdcPortStats(int itf, uint32_t* rx, uint32_t* tx,
                                             uint32_t* rxEvt, uint32_t* dtrEvt);
extern "C" bool          serialPortIsClaimed(int port);
extern "C" bool          serialPortClaimTriggered(int port);
extern "C" bool          serialPortIsAttached(int port);
extern "C" void          serialTaskStats(uint32_t* loops, uint32_t* scans, uint32_t* scanBytes);
extern "C" const char*   consoleLastSwitchError(void);

void pmPollUsb() {
  pmStatsPoll();             /* runs every log-loop iteration, before USB early-outs */
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
  if (!usbLock) return;

  /* While the console is on CDC the USB-Serial-JTAG controller does not own the
   * PHY, so it reads permanently disconnected. Left to run, the recovery path
   * below would reset that peripheral and re-enable its PHY every 60 s, fighting
   * the OTG core for the pads. The CDC link holds its own PM lock. */
  if (consoleOnCdc) return;

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
  static bool     wasDown = false;     /* latched down long enough to matter */
  if (connected || inGrace) {
    downStreak = 0;
    lastRecoverMs = 0;
    if (!held) pmLockAcquire(usbLock);
    /* A host enumerating on the USB console is someone at a desk with a cable
     * in hand — not the unattended device the long holds are there to protect.
     * Not during the boot grace window, which asserts nothing about a host. */
    if (connected) humanDetected("usb");
    /* Rising edge after a real outage: the host is back (fresh plug-in or a
     * recovery that took). Log once here, not per 60 s retry. */
    if (connected && wasDown) { info("usb up\n"); wasDown = false; }
  } else {
    if (downStreak < 3) downStreak++;
    if (downStreak >= 3) {
      wasDown = true;
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

/* Serial task drops any open USB-console CLI session while this is set. */
extern "C" volatile bool cliUsbSerialLinkDown;

static void cliUsbDown() {
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
  info("usb down\n");
  cliUsbSerialLinkDown = true;   /* the session this command runs in must not outlive the link */
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
  cliUsbSerialLinkDown = false;   /* console link is back; serial CLI sessions allowed again */
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
  /* The peripheral reset above cleared int_ena. The installed driver arms the
   * RX interrupt (SERIAL_OUT_RECV_PKT) only once at install time, and its ISR
   * is what drains the hardware FIFO into the ring buffer that stdin reads from.
   * Without re-arming it here the console goes deaf — bytes sit in the FIFO and
   * never reach the ring. TX self-heals because write_bytes() re-enables its
   * interrupt on every call, which is why output survives a reset but input doesn't. */
  usb_serial_jtag_ll_ena_intr_mask(USB_SERIAL_JTAG_INTR_SERIAL_OUT_RECV_PKT);
  /* Mechanism trace only — the info-level "usb up" is emitted by pmPollUsb on
   * the rising edge when a host is actually seen, so the 60 s unplugged retry
   * doesn't spam the log. */
  dbg("usb up: re-enumerate\n");
#else
  dbg("usb up: no-op on UART console\n");
#endif
}

/* Point the USB PHY back at the USB-Serial-JTAG controller and re-arm it. The
 * routing is lost whenever another controller (the OTG core, driven by
 * TinyUSB) claims the PHY, and the RX interrupt does not survive that — same
 * recovery the connection monitor uses, exposed for the console transport
 * switch in usb_ports.cpp. */
void pmUsbSerialJtagReattach(void) {
  cliUsbUp();
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
  /* Drop whatever queued while this controller was off the pads. Those bytes
   * predate the session about to resume on it, and delivering them would open
   * a CLI nobody asked for out of keystrokes nobody typed. */
  uint8_t sink[64];
  for (int pass = 0; pass < 16; pass++)
    if (usb_serial_jtag_read_bytes(sink, sizeof sink, 0) <= 0) break;
  /* Push what is still queued rather than leave it for the next keystroke to
   * dislodge. This is output from before the controller left the pads — a
   * console blackout covers the gap itself, so nothing stale is waiting here. */
  consoleFlush();
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

/* ================= 1 Hz CPU / PM stats sampler =================
 *
 * A background task (alive only while an Activity monitor is watching — see the
 * lifecycle note above) samples per-task CPU, per-core busy, and PM-mode
 * residency once a second, diffing against the previous second. It:
 *   - publishes the latest figures to the ephemeral sys.stats.* storage subtree
 *     (sys.stats.cpu_pct.0/.1, sys.stats.SLEEP/APB_MAX/CPU_MAX) plus a
 *     sys.stats.ts unix-second heartbeat that stalls when sampling stops;
 *   - fills a ring of the last `s.sys.cpu_sample_buf` seconds (5 bytes/sample:
 *     core0, core1, SLEEP, APB_MAX, CPU_MAX — all integer percent; APB_MIN, the
 *     80 MHz line, and the APB-high line are derived at read time);
 *   - keeps the latest per-task delta table so `top` renders instantly while the
 *     sampler is up (when it's down, `top` takes its own one-second sample).
 *
 * Sources: per-task/-core figures come from uxTaskGetSystemState run-time
 * counters (see the FreeRTOS run-time-stats build option); mode residency from
 * pmParseModeStats (esp_pm's numeric counters, only under CONFIG_PM_PROFILING).
 */

/* Extra per-second samplers other straddles hang off the shared cadence (e.g.
 * -net's Wi-Fi traffic ring). Registered at init, invoked by statsTick once a
 * second — so they run only while the sampler runs (a UI consumer is watching),
 * and never on a headless build. onStart/onStop fire on the running-state
 * transitions so a piggy-backed sampler can alloc/free its buffers in lockstep.
 * Init-time registration is single-threaded. */
struct StatSampler { void (*tick)(void); void (*onStart)(void); void (*onStop)(void); };
static StatSampler s_samplers[4];
static int s_samplerN = 0;
void pmStatsAddSampler(void (*tick)(void), void (*onStart)(void), void (*onStop)(void)) {
  if (tick && s_samplerN < (int)(sizeof(s_samplers) / sizeof(s_samplers[0])))
    s_samplers[s_samplerN++] = { tick, onStart, onStop };
}

#if CONFIG_FREERTOS_USE_TRACE_FACILITY && CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS

/* Latest per-task CPU slice, refreshed each second. Heap/stack columns are not
   here — `top` joins those fresh at print time (they're point-in-time, not
   windowed). */
struct cpuSnap {
  TaskHandle_t h;
  char     name[configMAX_TASK_NAME_LEN];
  int      core, pri;
  uint16_t stack;
  char     stkMem;      /* 'D' internal / 'P' PSRAM — captured while the handle
                           is known-live, so top never derefs a stale handle */
  uint32_t delta;       /* run-counter delta over the last sample window */
};

/* Ring element is PmStatSample (pm.h) — one second, integer percent. */

/* Shared with cmdTop — guarded by s_statsMux. */
static SemaphoreHandle_t s_statsMux;
static cpuSnap* s_snap;            /* latest per-task table (PSRAM) */
static int      s_snapN;
static uint32_t s_snapWindow;      /* run-time ticks spanned by the last window */
static uint32_t s_snapIdle0, s_snapIdle1;  /* IDLE0/IDLE1 deltas → per-core busy */
static bool     s_snapReady;
static PmStatSample* s_ring;
static int s_ringCap, s_ringHead, s_ringCount;

/* Sampler lifecycle. The task + ring exist only while an Activity monitor is
 * watching. A subscription on the actmon flags (registered on the log task, see
 * pmStatsPoll) drives the transitions — on every build, UI or headless, so a
 * headless node can start it by hand (`store set sys.stats.web_actmon 1`) to make
 * it publish the sys.stats.avg.* figures. */
static volatile bool s_statsRunning = false;
static volatile bool s_statsStop    = false;
static TaskHandle_t  s_statsTask    = nullptr;

/* Averaging windows the sampler publishes (seconds). The web monitor's draggable
 * average pill picks one; 300 s (5 min) is the default. Kept in sync with the
 * browser's list. */
static const int AVG_WINDOWS[] = { 30, 60, 120, 180, 240, 300 };
static constexpr int N_AVG_WINDOWS = (int)(sizeof(AVG_WINDOWS) / sizeof(AVG_WINDOWS[0]));
static void pmStatsAvgSet(PmStatAvg* out);   /* one-pass multi-window; defined below */

/* Sampler-private previous snapshot (only the sampler task touches these). */
struct cpuPrev { TaskHandle_t h; uint32_t run; };
static cpuPrev* s_prevRun;
static int      s_prevRunN, s_prevRunCap;
static uint32_t s_prevWall;
#ifdef CONFIG_PM_PROFILING
static int64_t  s_prevGrand[PM_MODE_COUNT];
static bool     s_prevGrandValid;
#endif

static void statsTick(bool driveSamplers = true) {
  UBaseType_t ntask = uxTaskGetNumberOfTasks();
  int cap = (int)ntask + 8;                 /* headroom for tasks spawned mid-walk */
  auto* raw = (TaskStatus_t*)gp_alloc(cap * sizeof(TaskStatus_t));
  if (!raw) return;
  uint32_t wall = 0;
  int cnt = (int)uxTaskGetSystemState(raw, cap, &wall);

  auto* cur = (cpuSnap*)gp_alloc((cnt ? cnt : 1) * sizeof(cpuSnap));
  if (!cur) { free(raw); return; }

  uint32_t idle0 = 0, idle1 = 0;
  for (int i = 0; i < cnt; i++) {
    cur[i].h = raw[i].xHandle;
    safeStrncpy(cur[i].name, raw[i].pcTaskName, configMAX_TASK_NAME_LEN);
    cur[i].pri   = (int)raw[i].uxCurrentPriority;
    cur[i].stack = raw[i].usStackHighWaterMark;
    cur[i].stkMem = esp_ptr_external_ram(pxTaskGetStackStart(raw[i].xHandle)) ? 'P' : 'D';
#if CONFIG_FREERTOS_VTASKLIST_INCLUDE_COREID
    cur[i].core = raw[i].xCoreID == tskNO_AFFINITY ? -1 : (int)raw[i].xCoreID;
#else
    cur[i].core = -2;
#endif
    uint32_t prev = 0; bool found = false;
    for (int j = 0; j < s_prevRunN; j++)
      if (s_prevRun[j].h == raw[i].xHandle) { prev = s_prevRun[j].run; found = true; break; }
    cur[i].delta = found ? (raw[i].ulRunTimeCounter - prev) : 0;
    if      (strcmp(cur[i].name, "IDLE0") == 0) idle0 = cur[i].delta;
    else if (strcmp(cur[i].name, "IDLE1") == 0) idle1 = cur[i].delta;
  }

  uint32_t window = (s_prevWall && wall > s_prevWall) ? (wall - s_prevWall) : 0;

  /* Save this walk as next tick's baseline. */
  if (cnt > s_prevRunCap) {
    free(s_prevRun);
    s_prevRun = (cpuPrev*)gp_alloc(cnt * sizeof(cpuPrev));
    s_prevRunCap = s_prevRun ? cnt : 0;
  }
  s_prevRunN = 0;
  for (int i = 0; i < cnt && i < s_prevRunCap; i++) {
    s_prevRun[i].h = raw[i].xHandle; s_prevRun[i].run = raw[i].ulRunTimeCounter;
    s_prevRunN++;
  }
  s_prevWall = wall;
  free(raw);

  if (!window) { free(cur); return; }        /* first tick: baseline only */

  /* Any nonzero busy time rounds up to at least 1% so the graph always shows a
     pixel for a core that did any work this second. */
  unsigned b0 = 0, b1 = 0;
  if (window > idle0) { b0 = (unsigned)((uint64_t)(window - idle0) * 100 / window); if (!b0) b0 = 1; }
  if (window > idle1) { b1 = (unsigned)((uint64_t)(window - idle1) * 100 / window); if (!b1) b1 = 1; }

  int sleepPct = 0, apbMaxPct = 0, cpuMaxPct = 0;
#ifdef CONFIG_PM_PROFILING
  { int64_t modeUs[PM_MODE_COUNT] = {};
    pmParseModeStats(modeUs);
    int64_t grand[PM_MODE_COUNT];
    for (int i = 0; i < PM_MODE_COUNT; i++) grand[i] = rtcAccumModeUs[i] + modeUs[i];
    if (s_prevGrandValid) {
      int64_t d[PM_MODE_COUNT], da = 0;
      for (int i = 0; i < PM_MODE_COUNT; i++) { d[i] = grand[i] - s_prevGrand[i]; if (d[i] < 0) d[i] = 0; da += d[i]; }
      if (da > 0) {
        sleepPct  = (int)(d[PM_MODE_LIGHT_SLEEP] * 100 / da);
        apbMaxPct = (int)(d[PM_MODE_APB_MAX]     * 100 / da);
        cpuMaxPct = (int)(d[PM_MODE_CPU_MAX]     * 100 / da);
      }
    }
    for (int i = 0; i < PM_MODE_COUNT; i++) s_prevGrand[i] = grand[i];
    s_prevGrandValid = true;
  }
#endif

  /* Swap in the fresh snapshot + append the ring sample under the lock. */
  xSemaphoreTake(s_statsMux, portMAX_DELAY);
  cpuSnap* old = s_snap;
  s_snap = cur; s_snapN = cnt;
  s_snapWindow = window; s_snapIdle0 = idle0; s_snapIdle1 = idle1;
  s_snapReady = true;
  if (s_ring && s_ringCap > 0) {
    PmStatSample& e = s_ring[s_ringHead];
    e.core0 = (uint8_t)b0; e.core1 = (uint8_t)b1;
    e.sleep = (uint8_t)sleepPct; e.apbMax = (uint8_t)apbMaxPct; e.cpuMax = (uint8_t)cpuMaxPct;
    s_ringHead = (s_ringHead + 1) % s_ringCap;
    if (s_ringCount < s_ringCap) s_ringCount++;
  }
  xSemaphoreGive(s_statsMux);
  free(old);

  /* Drive any piggy-backed samplers (e.g. -net traffic) on the same 1 Hz beat.
     They run every tick to keep their own rings filled and self-gate publishing.
     Skipped on the `top` fallback path, which is a private point-in-time read
     that must not allocate or advance another straddle's ring. */
  if (driveSamplers)
    for (int i = 0; i < s_samplerN; i++) s_samplers[i].tick();

  /* Publish the latest second, coalesced into one commit. The web and on-device
     monitors each raise an ephemeral flag (sys.stats.web_actmon / .lcd_actmon)
     while open; those flags are also what keep this sampler alive, so the guard
     is normally true — it only goes false for the one teardown second between
     the last watcher leaving and the task noticing, which we skip publishing.

     sys.stats.* has no "s." prefix → ephemeral (never persisted), but mirrored
     to browsers. sys.stats.ts is a unix-second heartbeat: it advances once per
     published sample, so a browser that sees it stall knows the device stopped
     publishing rather than merely reporting an idle second. */
  if (storageGetInt("sys.stats.web_actmon", 0) || storageGetInt("sys.stats.lcd_actmon", 0)) {
    /* Averages for every window (single source of truth for the model), so the
       web monitor's draggable pill can switch windows with no round-trip. Keys
       are sys.stats.avg.w<secs>.{cpu_max,apb_max,apb_min,ma_x10}. */
    PmStatAvg avg[N_AVG_WINDOWS];
    pmStatsAvgSet(avg);
    storageBegin();
    storageSet("sys.stats.cpu_pct.0", (int)b0);
    storageSet("sys.stats.cpu_pct.1", (int)b1);
    storageSet("sys.stats.SLEEP",   sleepPct);
    storageSet("sys.stats.APB_MAX", apbMaxPct);
    storageSet("sys.stats.CPU_MAX", cpuMaxPct);
    storageSet("sys.stats.ts",      (int)time(nullptr));
    for (int i = 0; i < N_AVG_WINDOWS; i++) {
      char key[48]; int w = AVG_WINDOWS[i];
      snprintf(key, sizeof key, "sys.stats.avg.w%d.cpu_max", w); storageSet(key, avg[i].cpuMax);
      snprintf(key, sizeof key, "sys.stats.avg.w%d.apb_max", w); storageSet(key, avg[i].apbMax);
      snprintf(key, sizeof key, "sys.stats.avg.w%d.apb_min", w); storageSet(key, avg[i].apbMin);
      snprintf(key, sizeof key, "sys.stats.avg.w%d.ma_x10",  w); storageSet(key, avg[i].mA10);
    }
    storageEnd();
  }
}

/* Free the ring + per-task snapshot under the stats lock, so a concurrent
   pmStatsHistory/pmStatsAvg/`top` reader (which all take s_statsMux and re-check
   s_ring/s_snapReady) sees them gone rather than dangling. The mutex itself
   persists across restarts. */
static void statsFreeState() {
  xSemaphoreTake(s_statsMux, portMAX_DELAY);
  free(s_ring);  s_ring = nullptr; s_ringCap = 0; s_ringHead = 0; s_ringCount = 0;
  free(s_snap);  s_snap = nullptr; s_snapN = 0;   s_snapReady = false;
  xSemaphoreGive(s_statsMux);
  free(s_prevRun); s_prevRun = nullptr; s_prevRunN = 0; s_prevRunCap = 0;
  s_prevWall = 0;
#ifdef CONFIG_PM_PROFILING
  s_prevGrandValid = false;
#endif
}

static void statsTaskFn(void*) {
  for (;;) {
    if (s_statsStop) break;
    statsTick();
    delay(1000);
  }
  /* Nobody's watching: drop the piggy-backed rings, then our own, and vanish. */
  for (int i = 0; i < s_samplerN; i++) if (s_samplers[i].onStop) s_samplers[i].onStop();
  statsFreeState();
  s_statsTask = nullptr;
  s_statsRunning = false;      /* published last: pmStatsPoll may now respawn us */
  vTaskDelete(nullptr);
}

/* Allocate a fresh zeroed ring and spawn the sampler. Runs on the log task
   (pmStatsPoll) with no sampler task alive, so touching the statics is safe. */
static void statsStart() {
  if (s_statsRunning) return;
  int cap = storageGetInt("s.sys.cpu_sample_buf", 320);
  if (cap < 0) cap = 0;
  if (cap > 3600) cap = 3600;
  xSemaphoreTake(s_statsMux, portMAX_DELAY);
  s_ring = nullptr; s_ringCap = 0; s_ringHead = 0; s_ringCount = 0;
  if (cap > 0) {
    s_ring = (PmStatSample*)gp_alloc((size_t)cap * sizeof(PmStatSample));
    if (s_ring) { memset(s_ring, 0, (size_t)cap * sizeof(PmStatSample)); s_ringCap = cap; }
  }
  s_snap = nullptr; s_snapN = 0; s_snapReady = false;
  xSemaphoreGive(s_statsMux);
  s_prevRun = nullptr; s_prevRunN = 0; s_prevRunCap = 0;   /* freed at prior teardown */
  s_prevWall = 0;
#ifdef CONFIG_PM_PROFILING
  s_prevGrandValid = false;
#endif
  for (int i = 0; i < s_samplerN; i++) if (s_samplers[i].onStart) s_samplers[i].onStart();
  s_statsStop = false;
  s_statsRunning = true;
  s_statsTask = spawnTask(statsTaskFn, "cpustat", 6144, nullptr, 1, 0, STACK_PSRAM);
  if (!s_statsTask) { statsFreeState(); s_statsRunning = false; }   /* spawn failed */
}

/* Match the sampler's presence to whether any Activity monitor is watching.
   Runs on the log task: either from the change subscription below, or once at
   setup to honour a flag already set at boot. */
static void statsWatchApply() {
  /* A WEB monitor can only be watching if WiFi is up — a browser has no other
   * path in. So gate web_actmon on the live link: a tab that vanished without
   * clearing the flag (unclean disconnect / WiFi drop) otherwise leaves this 1 Hz
   * sampler running forever for a viewer that's gone. An LCD monitor is local and
   * needs no link. */
  bool wifiUp = storageGetInt("wifi.sta.up", 0) || storageGetInt("wifi.ap.up", 0);
  bool want = (storageGetInt("sys.stats.web_actmon", 0) && wifiUp) ||
              storageGetInt("sys.stats.lcd_actmon", 0);
  if (want) {
    if (s_statsStop)          s_statsStop = false;   /* cancel a teardown still pending */
    else if (!s_statsRunning) statsStart();
  } else if (s_statsRunning && !s_statsStop) {
    s_statsStop = true;                              /* task tears itself down within ~1s */
  }
}

/* Called every log-loop iteration; sets up the flag subscription once. The
   subscription must be registered from a task that runs itsPoll (that's where
   change callbacks are delivered) — the log task is that host, and it's present
   on every build, so the sampler is reachable even on a headless node. */
static void pmStatsPoll() {
  static bool inited = false;
  if (inited) return;
  inited = true;
  s_statsMux = xSemaphoreCreateMutex();
  storageDefault("s.sys.cpu_sample_buf", 320);   /* 320 = default screen width */
  storageSubscribeChanges("sys.stats.web_actmon", [](const char*, const char*) { statsWatchApply(); });
  storageSubscribeChanges("sys.stats.lcd_actmon", [](const char*, const char*) { statsWatchApply(); });
  /* Re-evaluate when WiFi comes up or drops: a drop must stop a web-gated sampler
   * even though web_actmon itself didn't change (stale-flag case). */
  storageSubscribeChanges("wifi.sta.up", [](const char*, const char*) { statsWatchApply(); });
  storageSubscribeChanges("wifi.ap.up",  [](const char*, const char*) { statsWatchApply(); });
  statsWatchApply();                             /* honour a flag already set at boot */
}

int pmStatsHistory(PmStatSample* out, int max) {
  if (!out || max <= 0 || !s_statsMux || !s_ring || s_ringCap <= 0) return 0;
  xSemaphoreTake(s_statsMux, portMAX_DELAY);
  int n = s_ringCount < max ? s_ringCount : max;
  int start = ((s_ringHead - n) % s_ringCap + s_ringCap) % s_ringCap;
  for (int i = 0; i < n; i++) out[i] = s_ring[(start + i) % s_ringCap];
  xSemaphoreGive(s_statsMux);
  return n;
}

/* Private one-second per-task delta for `top` when the background sampler isn't
   running (no Activity monitor watching). Two uxTaskGetSystemState walks a second
   apart into freshly-allocated buffers — it never touches the sampler's statics,
   so it can't race a sampler that starts mid-measurement. Returns a gp_alloc'd
   cpuSnap[*outN] (caller frees) or nullptr on allocation failure. */
static cpuSnap* topSample(int* outN, uint32_t* outWindow, uint32_t* outIdle0, uint32_t* outIdle1) {
  *outN = 0; *outWindow = 0; *outIdle0 = 0; *outIdle1 = 0;
  struct Prev { TaskHandle_t h; uint32_t run; };
  int cap0 = (int)uxTaskGetNumberOfTasks() + 8;
  auto* raw0 = (TaskStatus_t*)gp_alloc(cap0 * sizeof(TaskStatus_t));
  if (!raw0) return nullptr;
  uint32_t wall0 = 0;
  int cnt0 = (int)uxTaskGetSystemState(raw0, cap0, &wall0);
  auto* prev = (Prev*)gp_alloc((cnt0 ? cnt0 : 1) * sizeof(Prev));
  if (!prev) { free(raw0); return nullptr; }
  for (int i = 0; i < cnt0; i++) { prev[i].h = raw0[i].xHandle; prev[i].run = raw0[i].ulRunTimeCounter; }
  free(raw0);

  delay(1000);

  int cap = (int)uxTaskGetNumberOfTasks() + 8;
  auto* raw = (TaskStatus_t*)gp_alloc(cap * sizeof(TaskStatus_t));
  if (!raw) { free(prev); return nullptr; }
  uint32_t wall = 0;
  int cnt = (int)uxTaskGetSystemState(raw, cap, &wall);
  auto* cur = (cpuSnap*)gp_alloc((cnt ? cnt : 1) * sizeof(cpuSnap));
  if (!cur) { free(raw); free(prev); return nullptr; }
  uint32_t idle0 = 0, idle1 = 0;
  for (int i = 0; i < cnt; i++) {
    cur[i].h = raw[i].xHandle;
    safeStrncpy(cur[i].name, raw[i].pcTaskName, configMAX_TASK_NAME_LEN);
    cur[i].pri   = (int)raw[i].uxCurrentPriority;
    cur[i].stack = raw[i].usStackHighWaterMark;
    cur[i].stkMem = esp_ptr_external_ram(pxTaskGetStackStart(raw[i].xHandle)) ? 'P' : 'D';
#if CONFIG_FREERTOS_VTASKLIST_INCLUDE_COREID
    cur[i].core = raw[i].xCoreID == tskNO_AFFINITY ? -1 : (int)raw[i].xCoreID;
#else
    cur[i].core = -2;
#endif
    uint32_t p = 0; bool found = false;
    for (int j = 0; j < cnt0; j++)
      if (prev[j].h == raw[i].xHandle) { p = prev[j].run; found = true; break; }
    cur[i].delta = found ? (raw[i].ulRunTimeCounter - p) : 0;
    if      (strcmp(cur[i].name, "IDLE0") == 0) idle0 = cur[i].delta;
    else if (strcmp(cur[i].name, "IDLE1") == 0) idle1 = cur[i].delta;
  }
  free(raw); free(prev);
  *outN = cnt;
  *outWindow = (wall > wall0) ? (wall - wall0) : 0;
  *outIdle0 = idle0; *outIdle1 = idle1;
  return cur;
}

/* Nominal per-DFS-mode SoC current (mA) — rough ESP32-class figures to calibrate
 * against a real measurement. CPU_MAX=240 MHz, APB_MAX/APB_MIN=80 MHz (APB bus
 * high/low), SLEEP=light sleep. Radio/peripherals are not modelled. */
static const int MA_CPU_MAX = 40, MA_APB_MAX = 30, MA_APB_MIN = 25, MA_SLEEP = 2;

/* Average `n` samples' running sums into *out and derive the current estimate. */
static void avgFromSums(PmStatAvg* out, long sc, long sa, long sn, long ss, int n) {
  if (n <= 0) { out->cpuMax = out->apbMax = out->apbMin = out->sleep = 0; out->mA10 = 0; return; }
  int cpu  = (int)((sc + n / 2) / n);
  int apb  = (int)((sa + n / 2) / n);
  int amin = (int)((sn + n / 2) / n);
  int slp  = (int)((ss + n / 2) / n);
  out->cpuMax = (uint8_t)cpu; out->apbMax = (uint8_t)apb;
  out->apbMin = (uint8_t)amin; out->sleep = (uint8_t)slp;
  /* mA = Σ(pct · mA_mode) / 100, expressed in tenths (÷10 instead of ÷100). */
  int num = cpu * MA_CPU_MAX + apb * MA_APB_MAX + amin * MA_APB_MIN + slp * MA_SLEEP;
  out->mA10 = (num + 5) / 10;
}

int pmStatsAvg(PmStatAvg* out, int secs) {
  if (!out) return 0;
  if (secs <= 0) secs = 300;
  long sc = 0, sa = 0, sn = 0, ss = 0;
  int n = 0;
  if (s_statsMux && s_ring && s_ringCap > 0) {
    xSemaphoreTake(s_statsMux, portMAX_DELAY);
    n = s_ringCount < secs ? s_ringCount : secs;
    int start = ((s_ringHead - n) % s_ringCap + s_ringCap) % s_ringCap;
    for (int i = 0; i < n; i++) {
      const PmStatSample& e = s_ring[(start + i) % s_ringCap];
      int cpu = e.cpuMax, apb = e.apbMax, slp = e.sleep;
      int amin = 100 - slp - apb - cpu; if (amin < 0) amin = 0;
      sc += cpu; sa += apb; sn += amin; ss += slp;
    }
    xSemaphoreGive(s_statsMux);
  }
  avgFromSums(out, sc, sa, sn, ss, n);
  return n;
}

/* All AVG_WINDOWS in one backward pass: the windows are nested (30 ⊂ 60 ⊂ … ⊂
 * 300), so we accumulate newest→oldest and snapshot the average as the count
 * crosses each window boundary. Windows past the data we have get the average of
 * everything available. */
static void pmStatsAvgSet(PmStatAvg* out) {
  for (int i = 0; i < N_AVG_WINDOWS; i++) avgFromSums(&out[i], 0, 0, 0, 0, 0);
  long sc = 0, sa = 0, sn = 0, ss = 0;
  int cnt = 0, wi = 0;
  if (s_statsMux && s_ring && s_ringCap > 0) {
    xSemaphoreTake(s_statsMux, portMAX_DELAY);
    int total = s_ringCount < AVG_WINDOWS[N_AVG_WINDOWS - 1] ? s_ringCount : AVG_WINDOWS[N_AVG_WINDOWS - 1];
    for (int k = 0; k < total; k++) {
      int idx = ((s_ringHead - 1 - k) % s_ringCap + s_ringCap) % s_ringCap;
      const PmStatSample& e = s_ring[idx];
      int cpu = e.cpuMax, apb = e.apbMax, slp = e.sleep;
      int amin = 100 - slp - apb - cpu; if (amin < 0) amin = 0;
      sc += cpu; sa += apb; sn += amin; ss += slp; cnt++;
      while (wi < N_AVG_WINDOWS && cnt == AVG_WINDOWS[wi]) { avgFromSums(&out[wi], sc, sa, sn, ss, cnt); wi++; }
    }
    xSemaphoreGive(s_statsMux);
  }
  for (; wi < N_AVG_WINDOWS; wi++) avgFromSums(&out[wi], sc, sa, sn, ss, cnt);
}
#else
int pmStatsHistory(PmStatSample* out, int max) { (void)out; (void)max; return 0; }
int pmStatsAvg(PmStatAvg* out, int secs) {
  (void)secs;
  if (out) { out->cpuMax = out->apbMax = out->apbMin = out->sleep = 0; out->mA10 = 0; }
  return 0;
}
static void pmStatsPoll() {}
#endif  /* run-time stats */

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
  /* CONFIG_PM_SLP_DISABLE_GPIO isolates every pin to its sleep config on entering
   * automatic light sleep (the "isolate all GPIO pins in sleep state" boot line).
   * A wake pin isolated that way never sees its edge/level, so the wakeup silently
   * never fires. Exclude this pin from the sleep-config switch so it keeps its live
   * input + pull + interrupt config across light sleep and can actually wake us.
   * Restored with gpio_sleep_sel_en() in pmGpioWakeDisable. */
  gpio_sleep_sel_dis((gpio_num_t)pin);
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
  gpio_sleep_sel_en((gpio_num_t)pin);   /* re-join the sleep-config switch (undo pmGpioWakeEnable) */
  /* We do not turn off the global esp_sleep_enable_gpio_wakeup() — other
   * pins may still rely on it, and it's a no-op for unconfigured pins. */
}

/* ---- light-sleep wake callbacks ---- */

#define PM_WAKE_CB_MAX 4
static pm_wake_cb_t s_wakeCbs[PM_WAKE_CB_MAX] = {};

#if CONFIG_PM_LIGHT_SLEEP_CALLBACKS
/* Runs from IDLE-task context on every automatic light-sleep exit (clocks/cache
 * already restored). Fan out the wake cause to the registered callbacks. Kept
 * lean — most exits are timer wakes a subscriber ignores in a couple of ops. */
static esp_err_t pmLightSleepExit(int64_t /*slept_us*/, void* /*arg*/) {
  int cause = (int)esp_sleep_get_wakeup_cause();
  for (int i = 0; i < PM_WAKE_CB_MAX; i++)
    if (s_wakeCbs[i]) s_wakeCbs[i](cause);
  return ESP_OK;
}
#endif

void pmOnLightSleepWake(pm_wake_cb_t cb) {
  if (!cb) return;
#if CONFIG_PM_LIGHT_SLEEP_CALLBACKS
  int slot = -1;
  for (int i = 0; i < PM_WAKE_CB_MAX; i++) {
    if (s_wakeCbs[i] == cb) return;                 /* already registered */
    if (!s_wakeCbs[i] && slot < 0) slot = i;
  }
  if (slot < 0) { err("pmOnLightSleepWake: no free slot"); return; }
  s_wakeCbs[slot] = cb;

  static bool registered = false;
  if (!registered) {
    esp_pm_sleep_cbs_register_config_t cfg = {};
    cfg.exit_cb = pmLightSleepExit;
    esp_err_t r = esp_pm_light_sleep_register_cbs(&cfg);
    if (r != ESP_OK) { err("esp_pm_light_sleep_register_cbs: %s", esp_err_to_name(r)); return; }
    registered = true;
  }
#else
  err("pmOnLightSleepWake: CONFIG_PM_LIGHT_SLEEP_CALLBACKS disabled");
#endif
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
    if (strcmp(a, "down") == 0) { cliUsbDown(); return; }
    if (strcmp(a, "up") == 0) { cliUsbUp(); return; }
    /* Bare `usb` reports status; up/down are silent. */
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    /* Peer presence is a USB-Serial-JTAG notion — it counts SOF packets on a
     * controller that does not own the PHY once the console has moved. */
    if (consoleOnCdc) cliPrintf("usb: cdc attached\n");
    else cliPrintf("usb: %s\n", usb_serial_jtag_is_connected() ? "connected" : "disconnected");
    cliPrintf("console: %s", consoleModeName());
    if (consoleCdcPortCount()) cliPrintf(" (%d ports, console on cdc 0)", consoleCdcPortCount());
    cliPrintf("\n");
    /* Per-port shuttle state: the first question about a silent claimed port
     * is whether bytes move at all. rx = drained by a reader, tx = queued out,
     * both since boot; the claim/attach state comes from the serial registry. */
    for (int i = 0; i < consoleCdcPortCount(); i++) {
        uint32_t rx, tx, rxEvt, dtrEvt;
        consoleCdcPortStats(i, &rx, &tx, &rxEvt, &dtrEvt);
        cliPrintf("cdc %d: rx %u (%u events) tx %u dtr-edges %u%s%s\n", i,
                  (unsigned)rx, (unsigned)rxEvt, (unsigned)tx, (unsigned)dtrEvt,
                  serialPortIsClaimed(i)
                      ? (serialPortClaimTriggered(i) ? ", claimed (in-band trigger)"
                                                     : ", claimed (dtr)")
                      : "",
                  serialPortIsAttached(i) ? ", client attached" : "");
    }
    if (consoleCdcPortCount()) {
        uint32_t loops, scans, scanBytes;
        serialTaskStats(&loops, &scans, &scanBytes);
        cliPrintf("serial task: %u loops, %u spare-port scans, %u bytes scanned\n",
                  (unsigned)loops, (unsigned)scans, (unsigned)scanBytes);
    }
    /* A failed switch reports itself during a re-enumeration, when a host is
     * least likely to be showing anything. Repeat it here, where someone asking
     * why the console did not move will look. */
    const char* switchErr = consoleLastSwitchError();
    if (switchErr && switchErr[0]) cliPrintf("last switch: %s\n", switchErr);
#else
    cliPrintf("usb: n/a\n");
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
    struct snap {
        TaskHandle_t h;
        char name[configMAX_TASK_NAME_LEN];
        int core; int pri; uint16_t stack; char stkMem;
        /* Filled after matching against heap task totals */
        size_t dram, psram; uint16_t dblk, pblk;
        uint32_t delta;
    };
    /* When an Activity monitor is watching, the background 1 Hz sampler is
       running: copy its most recent (≤1 s old) per-task delta table under the
       stats lock — no sampling window of our own. When nobody's watching, the
       sampler is torn down, so take a private one-second sample and free it right
       after, leaving no sampling buffers alive on an idle node. */
    int n2 = 0; uint32_t deltaTotal = 0, idle0 = 0, idle1 = 0;
    cpuSnap* localSnap = nullptr;
    const bool live = s_statsRunning;
    if (live && !s_snapReady) { cliPrintf("top: CPU stats warming up, retry in ~1s\n"); return; }
    if (!live) {
        localSnap = topSample(&n2, &deltaTotal, &idle0, &idle1);
        if (!localSnap) { cliPrintf("top: sampling unavailable\n"); return; }
    }
    xSemaphoreTake(s_statsMux, portMAX_DELAY);
    if (live && !s_snap) {   /* sampler stopped between the check and the lock */
        xSemaphoreGive(s_statsMux);
        cliPrintf("top: CPU stats warming up, retry in ~1s\n"); return;
    }
    const cpuSnap* src = live ? s_snap : localSnap;
    if (live) { n2 = s_snapN; deltaTotal = s_snapWindow; idle0 = s_snapIdle0; idle1 = s_snapIdle1; }
    const int maxSnap = n2 + 4;   /* +headroom for the addAgg pseudo-rows */
    auto* s2 = (snap*)gp_alloc(maxSnap * sizeof(snap));
    if (!s2) { xSemaphoreGive(s_statsMux); free(localSnap); cliPrintf("top: out of memory\n"); return; }
    memset(s2, 0, maxSnap * sizeof(snap));
    for (int i = 0; i < n2; i++) {
        s2[i].h = src[i].h;
        safeStrncpy(s2[i].name, src[i].name, configMAX_TASK_NAME_LEN);
        s2[i].core = src[i].core; s2[i].pri = src[i].pri;
        s2[i].stack = src[i].stack; s2[i].stkMem = src[i].stkMem;
        s2[i].delta = src[i].delta;
    }
    xSemaphoreGive(s_statsMux);
    free(localSnap);   /* no-op on the live path */

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
        snprintf(stackBuf, sizeof(stackBuf), "%u %c", (unsigned)s2[i].stack, s2[i].stkMem);
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

    free(s2);
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
