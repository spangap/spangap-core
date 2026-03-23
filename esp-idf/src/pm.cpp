/**
 * PM — power management locks, USB management, RTC stats.
 * Split from ipc.cpp. CLI commands: pm, top, usb.
 */
#include "ipc.h"
#include "pm.h"
#include "log.h"
#include "cli.h"
#include "cfg.h"
#include "compat.h"
#include <esp_system.h>
#include <esp_heap_caps.h>
#include <esp_wifi.h>
#include <esp_sleep.h>
#include <esp_pm.h>
#include <esp_timer.h>
#ifdef CONFIG_PM_PROFILING
#include <esp_private/pm_impl.h>
#endif
#include <esp_private/esp_clk.h>
#include <driver/usb_serial_jtag.h>
#include <hal/usb_serial_jtag_ll.h>
#include <cstdio>
#include <cstring>
#include <algorithm>

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
    ipcBroadcast(MSG_SYS_SLEEP);
}

static pm_lock_handle_t usbLock = nullptr;
RTC_DATA_ATTR static bool rtcUsbDisabled = false;

bool deepSleepAllowed() {
  for (pm_lock* l = pmLockList; l; l = l->next)
    if (l->count > 0) return false;
  return true;
}

static void pmDumpDeepSleepLocks(FILE* stream) {
  int64_t now = esp_timer_get_time();
  int64_t now_d100 = now / 100;
  bool any = false;
  for (pm_lock* l = pmLockList; l; l = l->next)
    if (l->type == PM_NO_DEEP_SLEEP) { any = true; break; }
  if (!any) return;
  fprintf(stream, "\nDeep sleep locks:\n");
#ifdef CONFIG_PM_PROFILING
  fprintf(stream, "%-15s %-14s  %-5s  %-8s  %-13s  %-14s  %-8s\n",
          "Name", "Type", "Arg", "Active", "Total_count", "Time(us)", "Time(%)");
  for (pm_lock* l = pmLockList; l; l = l->next) {
    if (l->type != PM_NO_DEEP_SLEEP) continue;
    int64_t held = l->time_held;
    if (l->count > 0) held += now - l->last_taken;
    fprintf(stream, "%-15.15s %-14s  %-5d  %-8d  %-13d  %-14lld  %-3lld%%\n",
            l->name, "NO_DEEP_SLEEP", 0, l->count, l->times_taken,
            held, now_d100 ? (held + now_d100 - 1) / now_d100 : 0LL);
  }
#else
  fprintf(stream, "%-15s %-14s  %-5s  %-8s\n", "Name", "Type", "Arg", "Active");
  for (pm_lock* l = pmLockList; l; l = l->next) {
    if (l->type != PM_NO_DEEP_SLEEP) continue;
    fprintf(stream, "%-15.15s %-14s  %-5d  %-8d\n",
            l->name, "NO_DEEP_SLEEP", 0, l->count);
  }
#endif
}

void pmInit() {
  esp_pm_config_t pm = { .max_freq_mhz = 240, .min_freq_mhz = 80, .light_sleep_enable = true };
  esp_err_t err = esp_pm_configure(&pm);
  dbg("pm: configured 240/80 MHz + light sleep (%s)\n", esp_err_to_name(err));

  pmLockCreate(PM_NO_LIGHT_SLEEP, "usb", &usbLock);
  if (rtcValid() && rtcUsbDisabled) {
    /* USB was disabled before deep sleep — keep it off */
    usb_serial_jtag_pull_override_vals_t vals = { .dp_pu = false, .dm_pu = false,
                                                   .dp_pd = false, .dm_pd = false };
    usb_serial_jtag_ll_phy_enable_pull_override(&vals);
  } else {
    rtcUsbDisabled = false;
    pmLockAcquire(usbLock);
  }
}

void pmPollUsb() {
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
}

static void cliUsbDown() {
  info("usb down\n");
  vTaskDelay(pdMS_TO_TICKS(10));
  /* Disable D+ pullup → host sees disconnect, stops SOF packets */
  usb_serial_jtag_pull_override_vals_t vals = { .dp_pu = false, .dm_pu = false,
                                                 .dp_pd = false, .dm_pd = false };
  usb_serial_jtag_ll_phy_enable_pull_override(&vals);
  rtcUsbDisabled = true;
  if (usbLock->count > 0)
    pmLockRelease(usbLock);
}

static void cliUsbUp() {
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
    { auto w = [](void*, const char* d, int l) -> int { cliPrintf("%.*s", l, d); return l; };
      FILE* f = funopen(nullptr, nullptr, w, nullptr, nullptr);
      if (f) { esp_pm_dump_locks(f); pmDumpDeepSleepLocks(f); fclose(f); }
    }
}

static void cmdTop(const char* args) {
    if (strcmp(args, "help") == 0) { cliPrintf("  %-*s tasks, CPU%%, heap, uptime\n", CLI_HELP_COL, "top"); return; }
#if CONFIG_FREERTOS_USE_TRACE_FACILITY && CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
    constexpr int MAX_SNAP = 32;
    struct snap { char name[configMAX_TASK_NAME_LEN]; uint32_t run; int core; int pri; uint16_t stack; };
    auto* s1 = (snap*)malloc(MAX_SNAP * sizeof(snap));
    auto* s2 = (snap*)malloc(MAX_SNAP * sizeof(snap));
    if (!s1 || !s2) { free(s1); free(s2); cliPrintf("top: out of memory\n"); return; }
    auto takeSnap = [](snap* out, int max, uint32_t& total) -> int {
        UBaseType_t n = uxTaskGetNumberOfTasks();
        auto* raw = (TaskStatus_t*)malloc(n * sizeof(TaskStatus_t));
        if (!raw) return 0;
        n = uxTaskGetSystemState(raw, n, &total);
        int cnt = n < (UBaseType_t)max ? (int)n : max;
        for (int i = 0; i < cnt; i++) {
            strncpy(out[i].name, raw[i].pcTaskName, configMAX_TASK_NAME_LEN - 1);
            out[i].name[configMAX_TASK_NAME_LEN - 1] = '\0';
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
    struct top_entry { const char* name; int core; int pri; uint16_t stack; uint32_t delta; };
    auto* entries = (top_entry*)malloc(n2 * sizeof(top_entry));
    if (!entries) { free(s1); free(s2); return; }
    int cnt = 0;
    for (int i = 0; i < n2; i++) {
        if (strncmp(s2[i].name, "IDLE", 4) == 0) continue;
        uint32_t prev = 0;
        for (int j = 0; j < n1; j++)
            if (strcmp(s1[j].name, s2[i].name) == 0) { prev = s1[j].run; break; }
        entries[cnt++] = { s2[i].name, s2[i].core, s2[i].pri, s2[i].stack, s2[i].run - prev };
    }
    std::sort(entries, entries + cnt, [](const top_entry& a, const top_entry& b) {
        int ca = a.core < 0 ? 99 : a.core, cb = b.core < 0 ? 99 : b.core;
        return ca != cb ? ca < cb : a.delta > b.delta;
    });
    cliPrintf("  %-12s %4s %3s %5s %8s\n", "Task", "Core", "Pri", "Stack", "CPU%");
    for (int i = 0; i < cnt; i++) {
        unsigned p = deltaTotal > 0 ? (unsigned)(entries[i].delta * 1000 / deltaTotal) : 0;
        char cb[16];
        if (entries[i].core < 0) strcpy(cb, entries[i].core == -2 ? " ?" : " *");
        else snprintf(cb, sizeof(cb), " %d", entries[i].core);
        cliPrintf("  %-12s %4s %3d %5u %4u.%u%%\n", entries[i].name, cb, entries[i].pri,
            (unsigned)entries[i].stack, p / 10, p % 10);
    }
    free(entries); free(s1); free(s2);
#endif
    { char f[16], t[16], m[16];
      cliPrintf("\n  Heap:\n");
      cliPrintf("    DRAM   free %s / %s  (min %s)\n",
          fmtSize(heap_caps_get_free_size(MALLOC_CAP_8BIT|MALLOC_CAP_INTERNAL), f, sizeof(f)),
          fmtSize(heap_caps_get_total_size(MALLOC_CAP_8BIT|MALLOC_CAP_INTERNAL), t, sizeof(t)),
          fmtSize(heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT|MALLOC_CAP_INTERNAL), m, sizeof(m)));
      cliPrintf("    PSRAM  free %s / %s  (min %s)\n",
          fmtSize(heap_caps_get_free_size(MALLOC_CAP_SPIRAM), f, sizeof(f)),
          fmtSize(heap_caps_get_total_size(MALLOC_CAP_SPIRAM), t, sizeof(t)),
          fmtSize(heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM), m, sizeof(m)));
      cliPrintf("    Total  free %s\n",
          fmtSize(heap_caps_get_free_size(MALLOC_CAP_8BIT), f, sizeof(f)));
    }
    uint32_t up = (uint32_t)(esp_timer_get_time() / 1000000ULL);
    char eb[32]; fmtElapsed(up, eb, sizeof(eb));
    cliPrintf("\n  Uptime: %s\n", eb);
}

void pmRegisterCmds() {
    cliRegisterCmd("pm", cmdPm);
    cliRegisterCmd("top", cmdTop);
    cliRegisterCmd("usb", cmdUsb);
}
