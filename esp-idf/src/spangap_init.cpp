/**
 * spangapInit + spangapPostAppInit — bring up the spangap platform CORE.
 *
 * spangapInit() brings up only the foundational primitives that must exist
 * before anything else and have no sensible `init:`-hook ordering (fs, storage
 * load, log, cli, pm, auth). The storage *task* (storageInit) and cron
 * (cronInit) are NOT started here — like every sibling straddle, they declare
 * an `init:` hook in their straddle.yaml and are run by the build-generated
 * spangapInitStraddles() dispatcher. That keeps spangap-core free of compile-
 * AND link-time knowledge of which siblings exist.
 *
 * Consumer pattern is now just three platform calls (plus whatever board /
 * app-policy glue isn't itself a staged straddle):
 *
 *     spangapInit();            // core foundations (this file)
 *     spangapInitStraddles();   // generated dispatcher: every staged straddle's
 *                               //   init: hook in (order, dependency) order
 *     spangapPostAppInit();     // finalise: rtcRamSetValid, boot script, cronPoll
 */
#include "spangap.h"
#include "auth.h"

#include <cstdio>
#include <cstring>
#include <ctime>
#include "esp_littlefs.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* Build epoch — symbol comes from spangap_app_build_epoch.c, regenerated
 * each ninja invocation by scripts/write-build-epoch.py. */
extern "C" const uint32_t app_build_unix;

/* Build invocation identity — symbols come from spangap_app_build_info.c,
 * regenerated each ninja invocation by scripts/write-build-info.py from the
 * SPANGAP_BUILD_* env `spangap build` exports. */
extern "C" const char app_build_straddle[];
extern "C" const char app_build_version[];
extern "C" const char app_build_args[];

namespace {

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
    info("build: straddle %s v%s\n", app_build_straddle, app_build_version);
    info("build: invocation %s\n", app_build_args);
}

}  // namespace

extern "C" void spangapInit(void) {
    /* Line-buffer stdout so each \n flushes immediately (USB Serial JTAG
     * default is fully-buffered, hides log lines until full or close). */
    setvbuf(stdout, nullptr, _IOLBF, 0);
    printf("spangap: starting\n");
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
     * every sibling straddle come up next via spangapInitStraddles() (their
     * declared init: hooks) — see header docstring. */
}

extern "C" void spangapPostAppInit(void) {
    /* Mark RTC RAM valid so RTC vars survive deep-sleep wake correctly
     * (rtcRamValid() returns false after warm reboot, esp_restart, panic). */
    rtcRamSetValid();

    /* Run boot script — last because every CLI command must already be
     * registered by this point (both platform and consumer). */
    cliRunFile(fsStatePath("/boot").c_str());

    /* Boot-complete signal — modules subscribe via
     *   storageSubscribeChanges("sys.boot_complete", cb)
     * to defer activation until the boot script's customisations are in. */
    storageSet("sys.boot_complete", 1);

    logApplyLevels();
    info("spangap: ready\n");

    /* Run any cron entries that fall in the current minute (deep-sleep wake
     * may already have moved time forward through a scheduled minute). */
    cronPoll(true);
}
