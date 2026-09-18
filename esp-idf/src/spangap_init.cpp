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
#include "esp_mac.h"
#include "esp_system.h"
/* The state store's own format call and the bootloader watchdog are both
 * flash-and-silicon; the host has neither. */
#if !CONFIG_IDF_TARGET_LINUX
#include "esp_littlefs.h"
#include "hal/wdt_hal.h"
#endif
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
extern "C" const char app_build_dist[];
extern "C" const char app_build_catalogue[];
extern "C" const char app_build_hw[];

/* The staged board straddle's one self-assertion: its own `hw-<straddle>` string
 * when the hardware under us is that board, NULL when it is not. Weak, so a
 * build with no board straddle — the generic image — links a null here and the
 * check below simply does not apply. See spangap-core/include/detect_probe.h. */
extern "C" const char* detect_hw(void) __attribute__((weak));

/* What detect_hw() answered this boot, for publishBuildTimes() to surface as
 * sys.hw. The probe runs before storage exists, so the answer is held rather
 * than published where it is found. Empty when no board straddle is staged. */
static const char* s_detectedHw = "";

/* Who this device is, as boot log lines.
 *
 *     build: hw hw-lilygo-tdeck
 *     build: catalogue stable
 *     build: datetime 20260814130700
 *
 * The board, the catalogue that published this image, and the stamp of that
 * run — everything flashmon, reading a boot log, needs to decide whether it
 * holds something newer. One fact per line, each self-describing, because the
 * reader is a line parser on the other side of a stream it does not control —
 * it may join mid-line, and a line it does not recognise must cost it nothing.
 *
 * Lines are omitted rather than emitted empty when the fact does not exist: a
 * generic image claims no board, and an image that did not come from a catalogue
 * run has no catalogue and no stamp. Absent is the honest answer, and it is what
 * tells flashmon to go and look for itself.
 *
 * Boot only. A console that ATTACHES is answered with the identity line below
 * instead (cli.cpp emits it for a bare Enter) — that is a message to the
 * console, not a log event, and dressing it as a log line put timestamps and
 * task tags on what a person reads as a greeting. */
extern "C" void spangapLogBuildIdentity(void) {
    if (s_detectedHw[0])        info("build: hw %s\n", s_detectedHw);
    if (app_build_catalogue[0]) info("build: catalogue %s\n", app_build_catalogue);
    if (app_build_datetime[0])  info("build: datetime %s\n", app_build_datetime);
}

/* The one-line identity a console greeting carries:
 *
 *     dev f9fb74, host tbeam, fw rop/reticulous_hw-lilygo-tbeam-supreme_20260814130700, ap "lab", ip 10.1.2.3
 *
 * Simple comma-separated fields, all the facts at once. `dev` is the low three
 * bytes of the MAC — the same six digits that lead the USB serial string, and
 * the one fact that names WHICH physical unit answered: USB descriptors do not
 * reach every consumer, and a host that reopens a port after the device
 * re-enumerated sees only the byte stream. `host` is the hostname, the same
 * source as the CLI prompt. `fw` is the image exactly as its catalogue file is
 * named — `<catalogue>/<project-slug>_<dist>_<stamp>` — so what a device
 * reports and what a builds directory lists read as the same thing; the slug
 * lowercases the project name the way make-builds does. Absent facts drop
 * their field: no catalogue run, no `fw`. An `hw <board>` field appears only
 * when the detected board differs from the dist — a variant entry whose name
 * is not simply the board's; a generic image stages no board straddle and so
 * claims no board at all. */
extern "C" void spangapIdentityLine(char* buf, size_t n) {
    uint8_t mac[6] = {};
    esp_efuse_mac_get_default(mac);
    char host[48];
    storageGetStr("s.net.hostname", host, sizeof host, CONFIG_SPANGAP_FW_HOSTNAME);
    if (!host[0]) snprintf(host, sizeof host, "%s", CONFIG_SPANGAP_FW_HOSTNAME);
    size_t o = (size_t)snprintf(buf, n, "dev %02x%02x%02x, host %s",
                                mac[3], mac[4], mac[5], host);
    if (s_detectedHw[0] && strcmp(s_detectedHw, app_build_dist) != 0 && o < n)
        o += (size_t)snprintf(buf + o, n - o, ", hw %s", s_detectedHw);
    if (app_build_dist[0] && app_build_datetime[0] && o < n) {
        char slug[40];
        size_t s = 0;
        for (const char* p = CONFIG_SPANGAP_PROJECT_NAME; *p && s + 1 < sizeof slug; p++) {
            char c = *p;
            if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
            if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) slug[s++] = c;
            else if (s && slug[s - 1] != '-') slug[s++] = '-';
        }
        while (s && slug[s - 1] == '-') s--;
        slug[s] = '\0';
        o += (size_t)snprintf(buf + o, n - o, ", fw ");
        if (app_build_catalogue[0] && o < n)
            o += (size_t)snprintf(buf + o, n - o, "%s/", app_build_catalogue);
        if (o < n)
            o += (size_t)snprintf(buf + o, n - o, "%s_%s_%s",
                     s ? slug : "builds", app_build_dist, app_build_datetime);
    }
    /* Online, and where: the association and address the network component
     * publishes into the ephemeral tree — absent on a build without one,
     * empty while unassociated, and either way the fields simply drop. The
     * pair is what tells a host the device is reachable over the network
     * without asking it anything. The SSID is free text, commas and spaces
     * included, so it is the one quoted field; a reader anchors on the
     * `", ip` that follows it. */
    char ssid[40] = "";
    char ip[48]   = "";
    storageGetStr("wifi.sta.ssid", ssid, sizeof ssid, "");
    storageGetStr("wifi.sta.ip",   ip,   sizeof ip,   "");
    if (ssid[0] && ip[0] && o < n)
        snprintf(buf + o, n - o, ", ap \"%s\", ip %s", ssid, ip);
}

/* Confirm the image is on the board it was built for, and halt if it is not.
 *
 * `app_build_hw` is the board straddle baked in at build time, as the
 * `<org>/hw-<board>` the invocation named; detect_hw() is that same board
 * straddle reading the hardware it is actually sitting on. When the two
 * disagree, every pin map in this image describes some other board — the LoRa
 * CS is someone's power enable, the display reset is someone's radio — so the
 * only safe thing left to do is stop touching the hardware.
 *
 * Halting is this task blocking forever, awake. The device stops without going
 * dark: the console peripheral stays powered, so the port stays enumerated and
 * the verdict stays readable — which matters, because the message IS the whole
 * value of stopping. A reboot loop would be worse than useless, re-driving the
 * same wrong pins every few seconds forever on hardware nobody is watching, and
 * deep sleep would take the explanation down with the port.
 *
 * Both halves must be present for the check to mean anything:
 *   * No detect_hw (the generic image, which stages no board straddle) — there
 *     is nothing to ask, and a generic image is by definition not claiming a
 *     board.
 *   * No app_build_hw (a build that named no board, or one that did not come
 *     from `spangap build`) — there is no claim to check the answer against.
 *
 * WHERE THIS RUNS is load-bearing: serviceRunStart(), before the first
 * onStart() — so before ANY straddle has touched the hardware. The probe opens
 * an I2C bus and the SPI host itself, and a board's own onStart is exactly what
 * takes those: hw-lilygo-tdeck creates the shared I2C0 bus there, so anything
 * later than this fails with "I2C bus id(0) has already been acquired" and reads
 * as a board it does not recognise. It does not need the board's bring-up
 * either — a probe drives the power rail it needs itself.
 *
 * That is also before storage and the log task exist, so the verdict goes
 * straight to the console through the native ESP-IDF logger, and the answer is
 * held in s_detectedHw until publishBuildTimes() can surface it as sys.hw.
 */
extern "C" void spangapConfirmBoard(void) {
    /* `app_build_hw` carries the org (`spangap/hw-lilygo-tdeck`); detect_hw()
     * answers the bare straddle name. Compare on the part after the slash, which
     * is the board — the org says who publishes the straddle, not what the
     * hardware is. */
    const char* claim = strrchr(app_build_hw, '/');
    claim = claim ? claim + 1 : app_build_hw;

    if (!detect_hw) {
        /* No board straddle staged (the generic image) is the ordinary case and
         * says nothing. A build that NAMES a board and still has no detect_hw
         * is not: either the straddle does not define it, or it does and the
         * linker did not extract the object holding it (see the `-u detect_hw`
         * in spangap-core's CMakeLists). Both leave the weak symbol null and
         * this check inert, and a safety check that looks enforced and is not is
         * worse than none — so it is stated out loud. */
        if (claim[0]) warn("no detect_hw for %s — cannot confirm this is the right board", claim);
        return;
    }
    const char* found = detect_hw();
    s_detectedHw = found ? found : "";

    if (claim[0] == '\0') return;              // board-less build: nothing claimed

    if (found && strcmp(found, claim) == 0) {
        info("board confirmed: %s", found);
        return;
    }
    err("WRONG BOARD: this image is built for %s, but the hardware reads as %s",
        claim, found ? found : "no board this straddle knows");
    err("halting — every pin map in this image belongs to a different board");
    fflush(stdout);

    /* Stop here, awake. Deep sleep would power down the console peripheral, so
     * the port drops off the host in the same breath as the message explaining
     * why — leaving someone holding a device that is simply dead, with the one
     * line that would have told them why already gone. Parking this task instead
     * keeps the port enumerated and the verdict readable for as long as the
     * device is plugged in.
     *
     * Two things are needed for that to actually hold.
     *
     * The RTC watchdog the bootloader armed is normally disabled by
     * spangapPostAppInit, at the far end of a boot this one never reaches — so
     * it has to be disabled here or it fires CONFIG_BOOTLOADER_WDT_TIME_MS after
     * boot and resets the chip. A halt that reboots every few seconds is worse
     * than no halt at all: the message scrolls past in a loop and the port
     * re-enumerates under whoever is trying to talk to it, which is exactly when
     * flashmon needs the device to hold still. */
#if CONFIG_BOOTLOADER_WDT_DISABLE_IN_USER_CODE
    {
        wdt_hal_context_t rwdt = RWDT_HAL_CONTEXT_DEFAULT();
        wdt_hal_write_protect_disable(&rwdt);
        wdt_hal_disable(&rwdt);
        wdt_hal_write_protect_enable(&rwdt);
    }
#endif

    /* And the verdict is re-stated on a slow beat rather than said once into a
     * console nobody was attached to yet. The reason someone plugs in after a
     * halt is to find out why it halted; printing only at the moment of failure
     * answers that question for everyone except the person asking it.
     *
     * The delay is what makes this gentle: the task yields, the idle task feeds
     * the task watchdog, and the console keeps servicing the port. Nothing else
     * has started (this runs ahead of the first onStart), so there is no state
     * machine to return to and nothing that could touch the wrong board's pins.
     * Only a reset or a power cycle leaves this. */
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        err("WRONG BOARD: image built for %s, hardware reads as %s — halted",
            claim, found ? found : "no board this straddle knows");
    }
}

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
    /* Which distribution this image is (the catalogue entry name), which
     * catalogue published it (`stable`, `dev`, …) and which board it was built
     * for. flashmon matches on the set: same catalogue, same dist, newer
     * datetime. All three are empty for a build that didn't come from a
     * catalogue run, which is a distinct state rather than a missing value. */
    storageSet("sys.build.dist", app_build_dist);
    storageSet("sys.build.catalogue", app_build_catalogue);
    storageSet("sys.build.hw", app_build_hw);
    /* Which board this actually IS, as the staged straddle's own detect_hw()
     * read it off the hardware at the top of this boot (confirmBoard). A device
     * that is running has already proved the two agree — it would have halted
     * otherwise — so this is the one key a tool can ask instead of probing the
     * chip itself. Empty for the generic image, which stages no board straddle
     * and so has nothing to read. */
    storageSet("sys.hw", s_detectedHw);
    storageEnd();
    info("build: straddle %s v%s\n", app_build_straddle, app_build_version);
    info("build: invocation %s\n", app_build_args);
    spangapLogBuildIdentity();
}

/* What the mount made of the flash, as ephemeral `sys.flash.*`. A booted device
 * knows more here than host-side chip detection does: not just the chip size,
 * but the floor of the image actually on it, the /state geometry that resulted,
 * and — implicitly — that this image boots at all.
 *
 * Ephemeral by prefix: keys outside the persisted ones are in-memory only and
 * recomputed every boot, which is exactly what these are. Published from here
 * rather than from fs_init(), which runs before storage is up. */
void publishFlashGeometry() {
    uint32_t size = 0, floor = 0, stateStart = 0, stateSize = 0;
    fsFlashGeometry(&size, &floor, &stateStart, &stateSize);
    storageBegin();
    storageSet("sys.flash.size",        (int)size);
    storageSet("sys.flash.floor",       (int)floor);
    storageSet("sys.flash.state_start", (int)stateStart);
    storageSet("sys.flash.state_size",  (int)stateSize);
    /* Where this boot's state actually lives, and whether there is a card at
     * all. A factory reset has to be told what to destroy, and that question
     * only has more than one answer on a device with an SD card — so the UI
     * asking it needs to know before it asks. */
    storageSet("sys.state.on_sd", fsStateOnSd() ? 1 : 0);
    storageSet("sys.sd.present",  sdAvailable() ? 1 : 0);
    storageEnd();
}

/* ---- Safe mode ----
 *
 * Three device operations — back the state store up, restore one, factory-reset
 * — all need the same thing: a running system that is not doing anything else
 * to the state store. Safe mode is that boot. A storage flag names the
 * operation, so there is no mode menu and no landing page.
 *
 * The flag is read, cleared and flushed HERE, before anything else happens, so
 * a crash anywhere inside safe mode comes back into a normal boot. This one
 * flush is the only write safe mode makes to the store outside the operation
 * the operator asked for. */

const char* const SAFE_KEY_BACKUP  = "s.sys.backup";
const char* const SAFE_KEY_RESTORE = "s.sys.restore";
const char* const SAFE_KEY_FACTORY = "s.sys.factory_reset";

safe_mode_t s_safeMode  = SAFE_MODE_NONE;
int         s_wipeTarget = 0;

/* Read the three flags, clear whichever are set, flush once. Called from
 * spangapInit() immediately after storageLoad() — before the project-identity
 * check, before logInit(), before any module can read the tree.
 *
 * CLEARING IS THE POINT, not bookkeeping. These flags are edge-triggered
 * commands carried in a value, and the storage actor dedups a SET whose value
 * already equals the committed one — no change, no notification. A flag left at
 * 1 therefore swallows every later write of 1, so a stale flag is not merely
 * untidy: it is a state in which the operation can never be requested again.
 * The clear here is what guarantees no boot ever leaves one behind.
 *
 * More than one set (an operator queueing two, or a crash between two writes)
 * resolves by destructiveness, most destructive first: a factory reset makes
 * the other two meaningless, and a restore supersedes a backup of the store it
 * is about to replace. */
void readSafeModeFlags() {
    int backup  = storageGetInt(SAFE_KEY_BACKUP, 0);
    int restore = storageGetInt(SAFE_KEY_RESTORE, 0);
    int factory = storageGetInt(SAFE_KEY_FACTORY, 0);
    if (!backup && !restore && !factory) return;

    if (factory) {
        s_safeMode = SAFE_MODE_FACTORY_RESET;
        /* 1 = flash, 2 = SD, 3 = both. Anything else means a caller wrote a
         * value we don't know; the flash store is the one every board has. */
        s_wipeTarget = factory & (SAFE_WIPE_FLASH | SAFE_WIPE_SD);
        if (!s_wipeTarget) s_wipeTarget = SAFE_WIPE_FLASH;
    } else if (restore) {
        s_safeMode = SAFE_MODE_RESTORE;
    } else {
        s_safeMode = SAFE_MODE_BACKUP;
    }
    if ((backup ? 1 : 0) + (restore ? 1 : 0) + (factory ? 1 : 0) > 1)
        warn("safe mode: several operations requested, taking the most destructive");

    storageBegin();
    if (backup)  storageUnset(SAFE_KEY_BACKUP);
    if (restore) storageUnset(SAFE_KEY_RESTORE);
    if (factory) storageUnset(SAFE_KEY_FACTORY);
    storageEnd();
    /* The persist worker does not exist yet (storageInit runs in the
     * serviceRunInit walk), so this flushes inline on this task. It must be
     * durable before we go any further: the flag has to be gone from disk even
     * if the operation below panics. */
    storageSave();

    /* No trailing \n on either line: this runs before logInit() installs the
     * log task, so both go through the native ESP-IDF logger, which appends
     * its own — same convention as the two lines at the top of spangapInit(). */
    const char* what = s_safeMode == SAFE_MODE_BACKUP  ? "backup"
                     : s_safeMode == SAFE_MODE_RESTORE ? "restore"
                                                       : "factory reset";
    info("safe mode: %s", what);
}

/* The wipe itself. Runs on a DRAM stack — an SPI-flash erase disables the PSRAM
 * cache, so both the task's stack and the random source buffer have to be
 * internal. Nothing waits on it and nothing can stop it. */
void factoryResetTask(void*) {
    int target = s_wipeTarget;
    if (target & SAFE_WIPE_SD) {
        if (fsClearSdState()) info("factory reset: /sdcard/state cleared\n");
        else                  warn("factory reset: no SD card to clear\n");
    }
    if (target & SAFE_WIPE_FLASH) {
        fsWipeFlashState([](uint32_t done, uint32_t total) {
            /* Roughly every 10%, so a minute of flash work isn't a minute of
             * silence on the console. */
            static uint32_t lastPct = 0;
            uint32_t pct = total ? done * 100 / total : 100;
            /* Published every callback, not every tenth: this is what a screen
             * draws a progress bar from, and a bar that moves in ten steps is a
             * worse answer to "is it stuck?" than one that moves. Ephemeral
             * (bare `sys.` prefix), so it is an in-RAM write — nothing reaches
             * the partition being erased, and flushing has already stopped. */
            storageSet("sys.wipe.percent", (int)pct);
            if (pct >= lastPct + 10 || pct == 100) {
                lastPct = pct - (pct % 10);
                info("factory reset: %u%%\n", (unsigned)pct);
            }
        });
    }
    delay(200);
    esp_restart();
}

}  // namespace

extern "C" safe_mode_t spangapSafeMode(void) { return s_safeMode; }
extern "C" int spangapFactoryResetTarget(void) { return s_wipeTarget; }

extern "C" void spangapWatchSafeModeFlags(void) {
    /* Setting a flag on a RUNNING system means "do it now": persist it and
     * reboot, so the operator's next contact with the device is already the
     * safe-mode boot. That is what makes the browser button, an rnsh `set`, and
     * a `/state/boot` line all one mechanism — the write IS the request.
     *
     * `s.sys.*` scope, not the individual keys: one subscription, and the
     * handler is the only thing that decides what counts. The clearing writes
     * readSafeModeFlags() makes arrive as val="" and are ignored; they also
     * happen long before this subscription exists, and never at all in safe
     * mode, where cron does not come up. */
    storageSubscribeChanges("s.sys.", ON_CHANGE {
        /* Every `s.sys.*` change this watcher is handed, before any filtering
         * — at debug level, because it is the one thing that tells "the button
         * did nothing" apart from "the write never arrived", and those look
         * identical from the browser. */
        dbg("safe-mode watch: %s=%s\n", key, val ? val : "(null)");
        if (!val || atoi(val) == 0) return;
        if (strcmp(key, SAFE_KEY_BACKUP)  != 0 &&
            strcmp(key, SAFE_KEY_RESTORE) != 0 &&
            strcmp(key, SAFE_KEY_FACTORY) != 0) return;
        info("%s=%s — rebooting into safe mode\n", key, val);
        /* A console CLI session may be open even though this request came from
         * somewhere else — end it here, or the safe-mode boot's output arrives
         * in the colour that session was using. A no-op when there is none. */
        cliSerialResumeLogNow();
        storageSave();
        delay(200);
        esp_restart();
    });
}

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
    /* First, before any driver can claim the ADC or a radio: seed the DRBG
     * every key on the device is drawn from (include/random.h). */
    randomInit();
    /* The boot-barrier events must exist before any straddle task (which spawn
     * after this) can wait on them. */
    s_bootEvents = xEventGroupCreate();
    /* Line-buffer stdout so each \n flushes immediately (USB Serial JTAG
     * default is fully-buffered, hides log lines until full or close). */
    setvbuf(stdout, nullptr, _IOLBF, 0);
    /* No trailing \n: this fires before logInit() installs the log task, so it
     * goes through the native ESP-IDF logger, which appends its own newline. */
    info("spangap starting");
    /* The chip's index within its OUI block, and the same six digits that lead
     * the USB serial string. Emitted every boot, on whatever console is
     * attached, so a host that can read the log can identify which physical
     * unit it is holding — USB descriptors do not reach every consumer. The
     * `dev` spelling is the one the fact has everywhere: the same field the
     * console greeting's identity line leads with. */
    {
        uint8_t mac[6] = {};
        esp_efuse_mac_get_default(mac);
        info("dev %02x%02x%02x", mac[3], mac[4], mac[5]);
    }
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

    /* Safe mode, before anything else can read or write the tree: read the
     * operation flag, clear it, flush. Everything after this point asks
     * spangapSafeMode() what kind of boot this is. */
    readSafeModeFlags();

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
#if CONFIG_IDF_TARGET_LINUX
            fsFormatFlash();       /* the store is a directory: empty it */
#else
            esp_littlefs_format("state");
#endif
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

    /* Deep-sleep wake decision (may go straight back to sleep) + build IDs.
     * Skipped in safe mode: an operator is waiting on the other end of a
     * transfer, and the one thing this call may do is go back to sleep. */
    if (s_safeMode == SAFE_MODE_NONE) cronWakeupHandler();

    publishBuildTimes();
    publishFlashGeometry();

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
     * registered by this point (both platform and consumer). Skipped in safe
     * mode: the script customises a system whose straddles aren't running, and
     * a broken boot script is one of the things a restore exists to repair. */
    if (spangapSafeMode() == SAFE_MODE_NONE)
        cliRunFile(fsStatePath("/boot").c_str());

    /* Boot-complete signal — modules subscribe via
     *   storageSubscribeChanges("sys.boot_complete", cb)
     * to defer activation until the boot script's customisations are in.
     * Published in safe mode too: what is up is up, and web's own readiness
     * hangs off it. */
    storageSet("sys.boot_complete", 1);

    logApplyLevels();
    info("spangap ready\n");

    /* A factory reset starts here and needs no client: bring up the band, start
     * wiping, and serve the estimate page to whoever turns up. That is what
     * makes it work on a headless or LoRa-only node — and what makes it still
     * happen when net or web failed to come up at all. Flushing stops first:
     * the in-RAM tree must not land in a partition being erased. */
    if (spangapSafeMode() == SAFE_MODE_FACTORY_RESET) {
        storageStopFlushing();
        spawnTask(factoryResetTask, "wipe", 4096, nullptr, 1, 0, STACK_DRAM);
    }

    /* Resync cron with everything boot wrote (owners install their
     * s.cron.tab.* entries during the serviceRunInit walk, possibly before the
     * cron task's own subscription was registered), then run any entries that
     * fall in the current minute (deep-sleep wake may already have moved time
     * forward through a scheduled minute). Not in safe mode — firing scheduled
     * commands in a recovery mode is wrong, and cron is not up there to run
     * them anyway. */
    if (spangapSafeMode() == SAFE_MODE_NONE) {
        cronReschedule();
        cronPoll();
    }
}

extern "C" void signalFlag(const char* key) {
    if (s_bootEvents) xEventGroupSetBits(s_bootEvents, flagBitFor(key));
}

extern "C" void signalFlagIfWaited(const char* key) {
    /* Every storage change passes through here, so this must never register a
     * bit: only a key some task is already blocked on has one, and the table is
     * a handful of slots. A waiter registers its bit before it blocks, so a flag
     * written from off-device after the wait began always finds it. */
    if (!s_bootEvents) return;
    EventBits_t bit = 0;
    portENTER_CRITICAL(&s_flagMux);
    for (int i = 0; i < s_flagCount; i++)
        if (strcmp(s_flagKeys[i], key) == 0) { bit = (EventBits_t)1u << i; break; }
    portEXIT_CRITICAL(&s_flagMux);
    if (bit) xEventGroupSetBits(s_bootEvents, bit);
}

extern "C" void signalTimeValid(void) { signalFlag("sys.time.valid"); }

/* Coalescing window for humanDetected(): a person types faster than any waiter
 * needs to hear about, and each publish is a config write plus a browser patch. */
#define HUMAN_PUBLISH_MIN_MS 30000

extern "C" void humanDetected(const char* source) {
    /* Plain statics, no lock: call sites are several tasks (console, screen,
     * USB poll), and the worst a race can do is publish the same truth twice. */
    static bool     s_announced   = false;
    static uint32_t s_lastPublish = 0;
    uint32_t now = millis();
    if (s_announced && (now - s_lastPublish) < HUMAN_PUBLISH_MIN_MS) return;
    s_lastPublish = now;
    if (!s_announced) {
        s_announced = true;
        info("human detected (%s)\n", source ? source : "?");
    }
    storageBegin();
    storageSet("sys.human_detected", 1);
    storageSet("sys.human_last_s", (int)(now / 1000));
    storageEnd();
    signalFlag("sys.human_detected");
}

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
