/**
 * PM — power management locks, USB management, RTC stats.
 * CLI commands: pm, top, usb.
 */
#include "pm.h"
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

static const esp_pm_lock_type_t espLockTypes[] = {
  ESP_PM_CPU_FREQ_MAX, ESP_PM_APB_FREQ_MAX, ESP_PM_NO_LIGHT_SLEEP
};

void pmLockCreate(pm_lock_type_t type, const char* name, pm_lock_handle_t* out) {
  auto* l = (pm_lock*)calloc(1, sizeof(pm_lock));
  l->name = name;
  l->type = type;
  if (type != PM_NO_DEEP_SLEEP)
    esp_pm_lock_create(espLockTypes[type], 0, name, &l->esp_handle);
  l->next = pmLockList;
  pmLockList = l;
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
  if (deepSleepAllowed())
    storageSet("sys.going_down", 1);
}

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

/** Dump every lock we track (name safe — it's our own list, never NULL).
 *  Avoids esp_pm_dump_locks() which crashes on IDF-internal NULL-name locks. */
static void pmDumpDeepSleepLocks(FILE* stream) {
  if (!pmLockList) return;
  int64_t now = esp_timer_get_time();
  int64_t now_d100 = now / 100;
  fprintf(stream, "\nPM locks:\n");
#ifdef CONFIG_PM_PROFILING
  fprintf(stream, "%-15s %-14s  %-8s  %-13s  %-14s  %-8s\n",
          "Name", "Type", "Active", "Total_count", "Time(us)", "Time(%)");
  for (pm_lock* l = pmLockList; l; l = l->next) {
    int64_t held = l->time_held;
    if (l->count > 0) held += now - l->last_taken;
    fprintf(stream, "%-15.15s %-14s  %-8d  %-13d  %-14lld  %-3lld%%\n",
            l->name ? l->name : "?", pmTypeName(l->type),
            l->count, l->times_taken,
            held, now_d100 ? (held + now_d100 - 1) / now_d100 : 0LL);
  }
#else
  fprintf(stream, "%-15s %-14s  %-8s\n", "Name", "Type", "Active");
  for (pm_lock* l = pmLockList; l; l = l->next) {
    fprintf(stream, "%-15.15s %-14s  %-8d\n",
            l->name ? l->name : "?", pmTypeName(l->type), l->count);
  }
#endif
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

void pmPollUsb() {
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
  if (!usbLock) return;
  static const uint32_t USB_GRACE_MS = 5000;
  bool inGrace = millis() < USB_GRACE_MS;
  bool connected = usb_serial_jtag_is_connected();
  bool wantLock = (connected || inGrace) && !rtcUsbDisabled;
  bool held = usbLock->count > 0;
  if (wantLock && !held)
    pmLockAcquire(usbLock);
  else if (!wantLock && held)
    pmLockRelease(usbLock);
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
/* Capture esp_pm_impl_dump_stats into buffer, parse mode times, print delta */
struct pm_buf_t { char* buf; size_t pos; size_t cap; };
static int pmBufWrite(void* cookie, const char* data, int len) {
  auto* s = (pm_buf_t*)cookie;
  size_t n = (s->pos + len < s->cap) ? (size_t)len : s->cap - s->pos - 1;
  memcpy(s->buf + s->pos, data, n);
  s->pos += n;
  s->buf[s->pos] = '\0';
  return len;
}

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
  int dsPct    = (int)(deepUs * 100 / wall);
  int sleepPct = (int)(mode[PM_MODE_LIGHT_SLEEP] * 100 / wall);
  int minPct   = (int)(mode[PM_MODE_APB_MIN] * 100 / wall);
  int maxPct   = 100 - dsPct - sleepPct - minPct;
  cliPrintf("  deep sleep    %d%% (%d)\n", dsPct, (int)dsCount);
  cliPrintf("  light sleep   %d%%\n", sleepPct);
  cliPrintf("  80 MHz        %d%%\n", minPct);
  cliPrintf("  240 MHz       %d%%\n", maxPct);
}
#endif

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
    if (strcmp(a, "help") == 0) { cliPrintf("  %-*s reconnect or disconnect USB\n", CLI_HELP_COL, "usb up|down"); return; }
    if (strcmp(a, "down") == 0) cliUsbDown();
    else if (strcmp(a, "up") == 0) cliUsbUp();
}

static void cmdPm(const char* args) {
    if (strcmp(args, "help") == 0) { cliPrintf("  %-*s power management\n", CLI_HELP_COL, "pm [wifi ...]"); return; }
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
    cliPrintf("\n");
    /* esp_pm_dump_locks crashes on internal IDF locks with NULL names — skip
     * until upstream is fixed or we replace it with our own iterator. */
    { auto w = [](void*, const char* d, int l) -> int { cliPrintf("%.*s", l, d); return l; };
      FILE* f = funopen(nullptr, nullptr, w, nullptr, nullptr);
      if (f) { pmDumpDeepSleepLocks(f); fclose(f); }
    }
}

static void cmdTop(const char* args) {
    if (strcmp(args, "help") == 0) { cliPrintf("  %-*s tasks, CPU%%, heap, uptime\n", CLI_HELP_COL, "top"); return; }
#if CONFIG_FREERTOS_USE_TRACE_FACILITY && CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
    constexpr int MAX_SNAP = 32;
    struct snap {
        TaskHandle_t h;
        char name[configMAX_TASK_NAME_LEN];
        uint32_t run;
        int core; int pri; uint16_t stack;
        /* Filled after matching against heap task totals */
        size_t dram, psram; uint16_t dblk, pblk;
        uint32_t delta;
    };
    auto* s1 = (snap*)malloc(MAX_SNAP * sizeof(snap));
    auto* s2 = (snap*)malloc(MAX_SNAP * sizeof(snap));
    if (!s1 || !s2) { free(s1); free(s2); cliPrintf("top: out of memory\n"); return; }
    memset(s1, 0, MAX_SNAP * sizeof(snap));
    memset(s2, 0, MAX_SNAP * sizeof(snap));
    auto takeSnap = [](snap* out, int max, uint32_t& total) -> int {
        UBaseType_t n = uxTaskGetNumberOfTasks();
        auto* raw = (TaskStatus_t*)malloc(n * sizeof(TaskStatus_t));
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
    int n1 = takeSnap(s1, MAX_SNAP, t1);
    vTaskDelay(pdMS_TO_TICKS(2000));
    int n2 = takeSnap(s2, MAX_SNAP, t2);
    uint32_t deltaTotal = t2 - t1;

    /* Compute per-task CPU delta; also capture IDLE0/IDLE1 for per-core busy. */
    uint32_t idle0 = 0, idle1 = 0;
    for (int i = 0; i < n2; i++) {
        uint32_t prev = 0;
        for (int j = 0; j < n1; j++)
            if (s1[j].h == s2[i].h) { prev = s1[j].run; break; }
        s2[i].delta = s2[i].run - prev;
        if (strcmp(s2[i].name, "IDLE0") == 0) idle0 = s2[i].delta;
        else if (strcmp(s2[i].name, "IDLE1") == 0) idle1 = s2[i].delta;
    }

    /* Merge per-task heap totals into s2 by TaskHandle_t; accumulate unmatched. */
    size_t preDram = 0, preDblk = 0, preP = 0, prePblk = 0;
    size_t delDram = 0, delDblk = 0, delP = 0, delPblk = 0;
#ifdef CONFIG_HEAP_TASK_TRACKING
    { constexpr size_t MAX_HT = 32;
      static heap_task_totals_t htotals[MAX_HT];
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

    /* Sort live tasks: pinned by core, then CPU desc; IDLE filtered out. */
    std::sort(s2, s2 + n2, [](const snap& a, const snap& b) {
        bool ai = strncmp(a.name, "IDLE", 4) == 0;
        bool bi = strncmp(b.name, "IDLE", 4) == 0;
        if (ai != bi) return !ai;
        int ca = a.core < 0 ? 99 : a.core, cb = b.core < 0 ? 99 : b.core;
        return ca != cb ? ca < cb : a.delta > b.delta;
    });

    /* Unified table: Task | Core | Pri | Stack | CPU% | DRAM | PSRAM | Dblk | Pblk */
    cliPrintf("  %-12s %4s %3s  %7s %7s %8s %8s %5s %5s\n",
              "Task", "Core", "Pri", "Stack", "CPU%", "DRAM", "PSRAM", "Dblk", "Pblk");
    for (int i = 0; i < n2; i++) {
        if (strncmp(s2[i].name, "IDLE", 4) == 0) continue;
        unsigned p10 = deltaTotal > 0 ? (unsigned)(s2[i].delta * 1000 / deltaTotal) : 0;
        char cb[16];
        if (s2[i].core < 0) strcpy(cb, s2[i].core == -2 ? " ?" : " *");
        else snprintf(cb, sizeof(cb), " %d", s2[i].core);
        char cpuBuf[16];
        snprintf(cpuBuf, sizeof(cpuBuf), "%u.%u%%", p10 / 10, p10 % 10);
        char stackBuf[12];
        const void* stkBase = pxTaskGetStackStart(s2[i].h);
        char stkMem = esp_ptr_external_ram(stkBase) ? 'P' : 'D';
        snprintf(stackBuf, sizeof(stackBuf), "%u %c", (unsigned)s2[i].stack, stkMem);
        cliPrintf("  %-12s %4s %3d  %7s %7s %8u %8u %5u %5u\n",
            s2[i].name, cb, s2[i].pri, stackBuf, cpuBuf,
            (unsigned)s2[i].dram, (unsigned)s2[i].psram,
            (unsigned)s2[i].dblk, (unsigned)s2[i].pblk);
    }
    /* Pre-scheduler + deleted-task heap aggregates (no CPU/stack to show). */
    if (preDram || preP) {
        cliPrintf("  %-12s %4s %3s  %7s %7s %8u %8u %5u %5u\n",
            "pre-sched", "-", "-", "-", "-",
            (unsigned)preDram, (unsigned)preP, (unsigned)preDblk, (unsigned)prePblk);
    }
    if (delDram || delP) {
        cliPrintf("  %-12s %4s %3s  %7s %7s %8u %8u %5u %5u\n",
            "(deleted)", "-", "-", "-", "-",
            (unsigned)delDram, (unsigned)delP, (unsigned)delDblk, (unsigned)delPblk);
    }

    /* Per-core CPU busy: deltaTotal on SMP FreeRTOS is wall-time ticks (not
     * summed across cores), so each IDLE task's delta is its core's idle
     * fraction of that same window. */
    unsigned b0 = (deltaTotal > idle0) ? (unsigned)((deltaTotal - idle0) * 1000 / deltaTotal) : 0;
    unsigned b1 = (deltaTotal > idle1) ? (unsigned)((deltaTotal - idle1) * 1000 / deltaTotal) : 0;
    cliPrintf("\n  Total CPU:   core0: %u.%u%%    core1: %u.%u%%\n",
              b0/10, b0%10, b1/10, b1%10);

    free(s1); free(s2);
#endif
    cliPrintf("\n  Heap:\n");
    cliPrintf("    DRAM   free %u / %u  min %u  largest %u\n",
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT|MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_total_size(MALLOC_CAP_8BIT|MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT|MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT|MALLOC_CAP_INTERNAL));
    cliPrintf("    PSRAM  free %u / %u  min %u  largest %u\n",
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
        (unsigned)heap_caps_get_total_size(MALLOC_CAP_SPIRAM),
        (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
    cliPrintf("    DMA    free %u  min %u  largest %u\n",
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
        (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_DMA),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));

    uint32_t up = (uint32_t)(esp_timer_get_time() / 1000000ULL);
    char eb[32]; fmtElapsed(up, eb, sizeof(eb));
    cliPrintf("\n  Uptime: %s\n", eb);
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
