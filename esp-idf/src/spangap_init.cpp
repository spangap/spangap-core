/**
 * spangapInit + spangapPostAppInit — bring up the spangap platform CORE.
 *
 * spangapInit() brings up only the foundational primitives that must exist
 * before anything else and have no sensible service ordering (fs, storage load,
 * log, cli, pm, auth). The storage *task* (storageInit) and cron (cronInit) are
 * NOT started here — like every sibling straddle, they come up in the generated
 * serviceRunInit() walk (registered in the platform band). That keeps
 * spangap-core free of compile- AND link-time knowledge of which siblings exist.
 *
 * This file owns only the middle platform call; the buildable's app_main is
 * fully generated (staging/spangap_init_dispatch.gen.cpp) and drives:
 *
 *     spangapRegisterServices();  // construct + register every staged Service, in order
 *     serviceRunStart();          // onStart walk: bare hardware, before spangapInit()
 *     spangapInit();              // core foundations (this file)
 *     serviceRunInit();           // onInit walk: every straddle, ecosystem up
 *     spangapPostAppInit();       // finalise: rtcRamSetValid, boot script, cronPoll
 */
#include "spangap.h"
#include "auth.h"

#include <cstdio>
#include <cstring>
#include <ctime>
#include "esp_littlefs.h"
#include "esp_system.h"
#include "hal/wdt_hal.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

/* Build epoch — symbol comes from spangap_app_build_epoch.c, regenerated
 * each ninja invocation by scripts/write-build-epoch.py. */
extern "C" const uint32_t app_build_unix;

/* Build invocation identity — symbols come from spangap_app_build_info.c,
 * regenerated each ninja invocation by scripts/write-build-info.py from the
 * SPANGAP_BUILD_* env `spangap build` exports. */
extern "C" const char app_build_straddle[];
extern "C" const char app_build_version[];
extern "C" const char app_build_args[];
extern "C" const char app_build_datetime[];

namespace {

/* esp_reset_reason() → stable short slug, persisted as s.sys.reset_reason for
 * boot logs / telemetry. The reason is latched by the reset itself and read back
 * on the next boot, so a crash-looping node still reports why it went down. */
const char* resetReasonStr(esp_reset_reason_t r) {
    switch (r) {
        case ESP_RST_POWERON:    return "poweron";
        case ESP_RST_EXT:        return "ext";
        case ESP_RST_SW:         return "sw";
        case ESP_RST_PANIC:      return "panic";
        case ESP_RST_INT_WDT:    return "int_wdt";
        case ESP_RST_TASK_WDT:   return "task_wdt";
        case ESP_RST_WDT:        return "wdt";
        case ESP_RST_DEEPSLEEP:  return "deepsleep";
        case ESP_RST_BROWNOUT:   return "brownout";
        case ESP_RST_SDIO:       return "sdio";
        case ESP_RST_USB:        return "usb";
        case ESP_RST_JTAG:       return "jtag";
        case ESP_RST_EFUSE:      return "efuse";
        case ESP_RST_PWR_GLITCH: return "pwr_glitch";
        case ESP_RST_CPU_LOCKUP: return "cpu_lockup";
        case ESP_RST_UNKNOWN:
        default:                 return "unknown";
    }
}

/* --- Build identity (numeric + short string for 32-byte WS notify payload) --- */

void fmtEpochUtc(uint32_t epoch, char* buf, size_t len) {
    if (epoch == 0) {
        safeStrncpy(buf, "(none)", len);
        return;
    }
    time_t t = (time_t)epoch;
    struct tm tm {};
    gmtime_r(&t, &tm);
    strftime(buf, len, "%Y-%m-%d %H:%M:%S UTC", &tm);
}

void publishBuildTimes() {
    storageSet("sys.buildtime.app", (int)app_build_unix);
    uint32_t fixedMaxMtime = 0;
    uint32_t fixedImageUnix = 0;
    uint32_t webrootCrc32 = 0;
    bool haveWebrootCrc = false;
    int f = fs_open(FS_FIXED "/build_times", "rb");
    if (f >= 0) {
        uint8_t b[12];
        size_t n = fs_read(b, 1, sizeof(b), f);
        if (n >= 8) {
            fixedMaxMtime  = (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
            fixedImageUnix = (uint32_t)b[4] | ((uint32_t)b[5] << 8) | ((uint32_t)b[6] << 16) | ((uint32_t)b[7] << 24);
        }
        if (n >= 12) {
            webrootCrc32 = (uint32_t)b[8] | ((uint32_t)b[9] << 8) | ((uint32_t)b[10] << 16) | ((uint32_t)b[11] << 24);
            haveWebrootCrc = true;
        }
        fs_close(f);
    }
    storageBegin();
    storageSet("sys.buildtime.fixed", (int)fixedMaxMtime);
    if (haveWebrootCrc)
        storageSet("sys.buildtime.web", (int)webrootCrc32);
    else
        storageUnset("sys.buildtime.web");

    char sum[40];
    snprintf(sum, sizeof(sum), "a%u f%u w%u",
             (unsigned)app_build_unix, (unsigned)fixedMaxMtime,
             haveWebrootCrc ? (unsigned)webrootCrc32 : 0u);
    storageSet("sys.build_time", sum);

    char appTs[40], fixSrcTs[40], fixImgTs[40];
    fmtEpochUtc(app_build_unix, appTs, sizeof(appTs));
    fmtEpochUtc(fixedMaxMtime, fixSrcTs, sizeof(fixSrcTs));
    fmtEpochUtc(fixedImageUnix, fixImgTs, sizeof(fixImgTs));
    info("build: app (firmware) %s (%u)\n", appTs, (unsigned)app_build_unix);
    info("build: fixed source_mtime=%s (%u) image=%s (%u)\n",
         fixSrcTs, (unsigned)fixedMaxMtime, fixImgTs, (unsigned)fixedImageUnix);
    if (haveWebrootCrc)
        info("build: webroot crc32=0x%08x (reload SPA when this changes)\n",
             (unsigned)webrootCrc32);

    /* `spangap build` invocation identity (straddle/version/flags). */
    storageSet("sys.build.straddle", app_build_straddle);
    storageSet("sys.build.version", app_build_version);
    storageSet("sys.build.args", app_build_args);
    storageSet("sys.build.datetime", app_build_datetime);
    storageEnd();
    info("build: straddle %s v%s\n", app_build_straddle, app_build_version);
    info("build: invocation %s\n", app_build_args);
    /* The catalogue build stamp, when this image is one. A flasher tails this to
     * decide whether the catalogue holds a newer build than what's running. */
    if (app_build_datetime[0])
        info("build: datetime %s\n", app_build_datetime);
}

}  // namespace

/* Boot-barrier events: waitForTime()/waitForFlag() block on a per-flag-key bit
 * that the flag's setter raises via signalFlag(), so a waiter light-sleeps until
 * the flag lands instead of polling storage at 5 Hz. Keys map to bits lazily
 * (flag names must be static strings). Created in spangapInit(), before any
 * straddle task can wait. */
static EventGroupHandle_t s_bootEvents = nullptr;
#define MAX_BOOT_FLAGS 12
static const char*  s_flagKeys[MAX_BOOT_FLAGS];
static int          s_flagCount = 0;
static portMUX_TYPE s_flagMux = portMUX_INITIALIZER_UNLOCKED;

/* Find, or lazily assign, the event-group bit for a flag key. Returns 0 only if
 * the table is full, whereupon callers fall back to a slow poll. */
static EventBits_t flagBitFor(const char* key) {
    EventBits_t bit = 0;
    portENTER_CRITICAL(&s_flagMux);
    int i;
    for (i = 0; i < s_flagCount; i++)
        if (strcmp(s_flagKeys[i], key) == 0) break;
    if (i == s_flagCount && s_flagCount < MAX_BOOT_FLAGS)
        s_flagKeys[s_flagCount++] = key;
    if (i < MAX_BOOT_FLAGS) bit = (EventBits_t)1u << i;
    portEXIT_CRITICAL(&s_flagMux);
    return bit;
}

extern "C" void spangapInit(void) {
    /* The boot-barrier events must exist before any straddle task (which spawn
     * after this) can wait on them. */
    s_bootEvents = xEventGroupCreate();
    /* Line-buffer stdout so each \n flushes immediately (USB Serial JTAG
     * default is fully-buffered, hides log lines until full or close). */
    setvbuf(stdout, nullptr, _IOLBF, 0);
    /* No trailing \n: this fires before logInit() installs the log task, so it
     * goes through the native ESP-IDF logger, which appends its own newline. */
    info("spangap starting");
    vTaskDelay(pdMS_TO_TICKS(100));

    /* Filesystem first; SD mount needs fs_init; storageLoad needs the active
     * state store chosen. fs_mount_sd() is a no-op when CONFIG_SPANGAP_SDCARD
     * is off. fsSelectStateStore() must sit between the SD mount and
     * storageLoad(): it picks /state vs /sdcard/state (and seeds first boot),
     * and storageLoad() reads settings.json from whichever it picked. */
    fs_init();
    fs_mount_sd();
    fsSelectStateStore();
    storageLoad();

    /* Project-mismatch factory reset.
     *
     * `s.sys.project` is the immutable project identity (CONFIG_SPANGAP_PROJECT_NAME
     * at compile time). If /state was last written by a different spangap
     * project, the stored settings.json may carry incompatible keys, cron
     * lines, or boot scripts. Detect the mismatch and factory-reset before
     * any module reads the polluted tree.
     *
     * First boot OR first run of this project after a clean state: stored
     * value is empty, we install it. Reflash from project A → project B with
     * intact /state: stored "A" != configured "B" → format + reboot. */
    {
        char stored[64] = {};
        storageGetStr("s.sys.project", stored, sizeof(stored), "");
        if (stored[0] != '\0' && strcmp(stored, CONFIG_SPANGAP_PROJECT_NAME) != 0) {
            printf("project changed: stored '%s' != configured '%s' — factory resetting /state\n",
                   stored, CONFIG_SPANGAP_PROJECT_NAME);
            fflush(stdout);
            vTaskDelay(pdMS_TO_TICKS(200));
            esp_littlefs_format("state");
            esp_restart();
            /* unreachable */
        }
        if (stored[0] == '\0') {
            storageSet("s.sys.project", CONFIG_SPANGAP_PROJECT_NAME);
        }
    }

    /* The device identity (name / banner / stub) is immutable firmware
     * identity now — exposed read-only as the fw.* storage subtree from
     * CONFIG_SPANGAP_FW_*, not a mutable s.sys.banner setting. */

    /* Foundation tasks. Log timestamps start in UTC; once ntpInit() runs from
     * the dispatcher (it applies the persisted timezone at the end), they
     * switch to the persisted zone. */
    logInit();
    cliInit();
    pmInit();
    /* Bring up the realm/password/cookie store + CLI before sibling straddles
     * (sshd, web) — both need authLogin/authCheck. The HTTP face is wired by
     * spangap-web's authWebInit() inside webInit(). */
    authInit();

    /* Deep-sleep wake decision (may go straight back to sleep) + build IDs. */
    cronWakeupHandler();

    publishBuildTimes();

    /* That's the lot for core's eager foundations. The storage task, cron, and
     * every sibling straddle come up next in the generated serviceRunInit() walk
     * (their registered Services / adapted init: hooks) — see header docstring. */
}

extern "C" void spangapPostAppInit(void) {
#if CONFIG_BOOTLOADER_WDT_DISABLE_IN_USER_CODE
    /* Boot survived: every core foundation AND every straddle's hardware bring-up
     * (LCD/touch/LoRa/SD, PSRAM, …) ran without wedging. Disable the RTC
     * (bootloader) watchdog we kept armed across that whole window — IDF skips
     * its own init_disable_rtc_wdt when we own the disable, so we replicate it
     * here. Past this point the scheduler's int/task WDTs cover any hang. */
    {
        wdt_hal_context_t rwdt = RWDT_HAL_CONTEXT_DEFAULT();
        wdt_hal_write_protect_disable(&rwdt);
        wdt_hal_disable(&rwdt);
        wdt_hal_write_protect_enable(&rwdt);
    }
#endif

    /* Mark RTC RAM valid so RTC vars survive deep-sleep wake correctly
     * (rtcRamValid() returns false after warm reboot, esp_restart, panic). */
    rtcRamSetValid();

    /* Record why this boot happened (panic / int_wdt / brownout / poweron / …)
     * and persist it right now: a node that crash-loops still leaves the last
     * reason on disk for the next boot and for telemetry. storageSave() blocks on
     * the persist worker, so this must run here (post-init, storage task up), not
     * in the early spangapInit(). */
    {
        const char* reason = resetReasonStr(esp_reset_reason());
        info("reset reason: %s", reason);
        storageSet("s.sys.reset_reason", reason);
        storageSave();
    }

    /* Run boot script — last because every CLI command must already be
     * registered by this point (both platform and consumer). */
    cliRunFile(fsStatePath("/boot").c_str());

    /* Boot-complete signal — modules subscribe via
     *   storageSubscribeChanges("sys.boot_complete", cb)
     * to defer activation until the boot script's customisations are in. */
    storageSet("sys.boot_complete", 1);

    logApplyLevels();
    info("spangap ready\n");

    /* Run any cron entries that fall in the current minute (deep-sleep wake
     * may already have moved time forward through a scheduled minute). */
    cronPoll(true);
}

extern "C" void signalFlag(const char* key) {
    if (s_bootEvents) xEventGroupSetBits(s_bootEvents, flagBitFor(key));
}

extern "C" void signalTimeValid(void) { signalFlag("sys.time.valid"); }

extern "C" bool waitForTime(int timeout_s) {
    /* Fast path: clock already known-valid (warm boot carrying an RTC time, or
     * an SNTP/GPS sync earlier this session). */
    if (storageGetInt("sys.time.valid", 0)) return true;

    /* timeout_s <= 0 → operator-tunable default. s.sys.time_wait_s = 0 skips. */
    if (timeout_s <= 0) timeout_s = storageGetInt("s.sys.time_wait_s", 30);
    if (timeout_s <= 0) return false;

    /* Nothing can set the clock without networking: NTP needs WiFi, and the
     * browser/CLI time-set paths are post-boot user actions, not something to
     * block boot on. So an offline node skips the wait outright instead of
     * spinning out the full timeout for a sync that will never come. */
    if (storageGetInt("s.net.wifi.enable", 0) == 0) return false;

    /* Keep a power-managed device awake for the wait. One shared recursive lock
     * across all callers — rnsd and the transports race here at boot, and the
     * count aggregates so deep sleep stays blocked while any wait is live. */
    static pm_lock_handle_t s_lock = nullptr;
    if (!s_lock) pmLockCreate(PM_NO_DEEP_SLEEP, "waittime", &s_lock);
    /* Capture locally: two boot tasks racing the lazy-create could each write
     * s_lock, so acquire and release must name the same handle this call saw. */
    pm_lock_handle_t lock = s_lock;
    if (lock) pmLockAcquire(lock);

    /* Block on the event, not a poll: signalTimeValid() (net's NTP sync
     * callback / browser / CLI) raises the sys.time.valid bit the moment the
     * clock is set, so this wakes within ms of sync and light-sleeps until then
     * instead of a 5 Hz storage poll that pinned every boot task awake. The bit
     * latches (xClearOnExit false), so a late waiter and the fast path above
     * stay coherent. */
    EventBits_t bit = s_bootEvents ? flagBitFor("sys.time.valid") : 0;
    if (bit)
        xEventGroupWaitBits(s_bootEvents, bit, pdFALSE, pdTRUE,
                            pdMS_TO_TICKS(timeout_s * 1000));
    bool valid = storageGetInt("sys.time.valid", 0) != 0;

    if (lock) pmLockRelease(lock);

    /* Log line is tagged with the calling task name (rnsd, tcp, lora, …). */
    if (valid) info("waitForTime: clock valid\n");
    else       warn("waitForTime: no valid time after %d s — proceeding (clock may read ~1970)\n", timeout_s);
    return valid;
}

extern "C" bool waitForFlag(const char* key, int timeout_s) {
    /* Block until an ephemeral readiness flag (storage int, non-zero) is set, or
     * `timeout_s` elapses. The boot-barrier primitive: rns publishes rns.ready,
     * net publishes net.up, …; ifaces/clients gate on them. timeout_s <= 0 means
     * "check once, don't wait". Returns true iff the flag was set. Holds one
     * shared PM no-deep-sleep lock for the wait (aggregated across the several
     * boot tasks that wait in parallel) so deep sleep can't latch mid-barrier.
     * `key` must be a static string (its bit is registered by pointer-kept name).
     * Not an ITS round-trip, so a waiter is NOT registered as an ITS task. */
    if (storageGetInt(key, 0)) return true;
    if (timeout_s <= 0) return false;

    static pm_lock_handle_t s_lock = nullptr;
    if (!s_lock) pmLockCreate(PM_NO_DEEP_SLEEP, "waitflag", &s_lock);
    pm_lock_handle_t lock = s_lock;
    if (lock) pmLockAcquire(lock);

    /* Event-driven: the flag's setter calls signalFlag(key) right after
     * publishing it, waking us at once — so we light-sleep here instead of the
     * old 5 Hz storage poll that kept every boot task (and the whole chip) awake.
     * The setter storageSet()s before it signals, and the bit latches, so the
     * check-then-block window can't miss the signal. Table-full → slow poll. */
    bool set = false;
    EventBits_t bit = s_bootEvents ? flagBitFor(key) : 0;
    if (bit) {
        xEventGroupWaitBits(s_bootEvents, bit, pdFALSE, pdTRUE,
                            pdMS_TO_TICKS(timeout_s * 1000));
        set = storageGetInt(key, 0) != 0;
    } else {
        uint32_t start = millis();
        while (!(set = storageGetInt(key, 0) != 0)) {
            if ((int)(millis() - start) >= timeout_s * 1000) break;
            delay(pdMS_TO_TICKS(500));
        }
    }

    if (lock) pmLockRelease(lock);
    if (!set) warn("waitForFlag: '%s' not set after %d s — proceeding\n", key, timeout_s);
    return set;
}
