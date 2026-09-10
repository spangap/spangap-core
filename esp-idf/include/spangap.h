/**
 * spangap.h — public CORE umbrella header.
 *
 * One include for the spangap-core API surface only: compat, pm, log, fs,
 * storage, its, cli, cron. Sibling straddles (spangap-net, spangap-web,
 * spangap-lcd, wg, upnp, duckdns, acme, ota, …) live in their own headers;
 * consumers `#include` them directly:
 *
 *     #include "spangap.h"
 *
 * A buildable ships NO app_main — spangap-inside generates the entire entry
 * point (staging/spangap_init_dispatch.gen.cpp): it constructs every staged
 * straddle's boot Service (service.h), registers them in one ordered registry,
 * and drives the boot phases:
 *
 *     spangapRegisterServices();  // construct + register all, in init_order
 *     serviceRunStart();          // onStart walk: bare hardware, before spangapInit()
 *     spangapInit();              // core foundations only
 *     serviceRunInit();           // onInit walk: ecosystem up
 *     spangapPostAppInit();       // finalise boot
 *
 * No per-straddle init calls and no #if CONFIG_SPANGAP_* guards anywhere:
 * registration order is init_order() — spangap's own components (core, net, web,
 * lcd) first, in that fixed order and each only if staged, then every other
 * staged straddle in dependency order. A straddle boots by declaring a
 * `services:` class (or a legacy start:/init: hook the generator wraps in an
 * adapter Service); see service.h and spangap-core/docs/init.md. Consumers
 * compose around the platform via netRegister(NET_EV_*, cb),
 * storageSubscribeChanges, cron entries, and /state/boot scripts.
 */
#ifndef SPANGAP_H
#define SPANGAP_H

#include "sdkconfig.h"

/* spangap-core foundation */
#include "compat.h"
#include "random.h"
#include "pm.h"
#include "log.h"
#include "fs.h"
#include "storage.h"
#include "its.h"
#include "cli.h"
#include "cron.h"
#include "service.h"    /* Service base + serviceRegister/serviceRun* (C++ only) */

#ifdef __cplusplus
extern "C" {
#endif

/** Bring up the spangap CORE platform:
 *
 *    randomInit (seed the DRBG before any radio or ADC) → line-buffered
 *      stdout → fs_init → optional fs_mount_sd (when
 *      CONFIG_SPANGAP_SDCARD=y) → fsSelectStateStore → storageLoad
 *      → project-mismatch factory reset →
 *      logInit/cliInit/pmInit → cronWakeupHandler → publishBuildTimes
 *
 *  Returns. The generated app_main then runs the serviceRunInit() walk
 *  (sibling straddle bring-up) and finally spangapPostAppInit() to finalise
 *  boot — see the header docstring above.
 *
 *  Build epoch (`app_build_unix`) is generated and linked in by
 *  spangap-core itself via scripts/write-build-epoch.py — consumers
 *  don't pass it. */
void spangapInit(void);

/* ---- Safe mode ---- */

/** Which state-store operation this boot exists to perform.
 *
 *  A storage flag names the operation, so there is no mode menu and no landing
 *  page — the device boots and does the thing:
 *
 *      set s.sys.backup=1          → stream the state store out as a .tgz
 *      set s.sys.restore=1         → take such an archive back in
 *      set s.sys.factory_reset=N   → 1 = flash, 2 = SD, 3 = both
 *
 *  Setting any of them from a running system saves and reboots at once; the
 *  next boot reads the flag, clears it (so a crash inside safe mode comes back
 *  into a normal boot), and reports it here. */
typedef enum {
    SAFE_MODE_NONE = 0,
    SAFE_MODE_BACKUP,
    SAFE_MODE_RESTORE,
    SAFE_MODE_FACTORY_RESET,
} safe_mode_t;

/** The operation this boot was asked to perform, or SAFE_MODE_NONE for an
 *  ordinary boot. Valid from the moment spangapInit() has read the flags —
 *  i.e. everywhere a service, a CLI command, or a web handler can run. Constant
 *  for the lifetime of the boot. */
safe_mode_t spangapSafeMode(void);

/** What `s.sys.factory_reset` asked to destroy: bit 0 = the on-flash `state`
 *  extent, bit 1 = /sdcard/state. Zero unless spangapSafeMode() is
 *  SAFE_MODE_FACTORY_RESET. */
#define SAFE_WIPE_FLASH 1
#define SAFE_WIPE_SD    2
int spangapFactoryResetTarget(void);

/** Watch the three safe-mode flags on the CALLING task and, when one is set on
 *  a running system, save and reboot into it. Called once, from the cron task's
 *  body: a storage subscription is delivered to the task that registered it, so
 *  it has to live on a task that outlives boot — and cron is core's own
 *  long-lived housekeeping actor (it already watches s.cron there). Never
 *  called in safe mode, where cron does not come up. */
void spangapWatchSafeModeFlags(void);

/* Boot participation is a Service (service.h), not a free-function dispatcher.
 * The generated app_main constructs every staged straddle's Service and walks
 * the registry: serviceRunStart() (onStart, bare hardware, before spangapInit)
 * then serviceRunInit() (onInit, ecosystem up). Those walk entry points live in
 * service.h; there is no consumer-called spangapStartStraddles/InitStraddles. */

/** Finalise platform startup after the consumer has brought up its own
 *  modules. Runs:
 *
 *    rtcRamSetValid → cliRunFile("/state/boot") → set sys.boot_complete
 *      = 1 → logApplyLevels → info("ready") → cronReschedule + cronPoll
 *
 *  Modules that registered for `sys.boot_complete` via storage subscribe
 *  fire here. After this call, IDF's main_task automatically deletes
 *  itself — no explicit vTaskDelete needed. */
void spangapPostAppInit(void);

/** Confirm this image is on the board it was built for, and halt the chip if it
 *  is not — the staged board straddle's detect_hw() read against the board baked
 *  into the image. A mismatch stops the device where it stands — this task
 *  blocks forever, awake, so the console stays enumerated and the reason stays
 *  readable — because every pin map in a wrong-board image belongs to someone
 *  else's hardware. Only a reset or a power cycle leaves that state.
 *
 *  Called by serviceRunStart(), BEFORE the first onStart(). That is the whole
 *  requirement: the probe opens an I2C bus and the SPI host itself, and a
 *  board's own onStart is what takes them — anything later loses the bus and
 *  reads as an unrecognised board. Nothing needs to have been brought up first;
 *  a probe drives the power rail it needs. */
void spangapConfirmBoard(void);

/** Say who this device is, as boot log lines:
 *
 *      build: hw hw-lilygo-tdeck
 *      build: catalogue stable
 *      build: datetime 20260814130700
 *
 *  The board (as the staged board straddle read it off the hardware this boot),
 *  the catalogue the image was published from, and the stamp of that run. A line
 *  is omitted when its fact does not exist — a generic image claims no board, an
 *  image from outside a catalogue run has no catalogue and no stamp — because
 *  absent is the honest answer and it is what tells a tool to go and look for
 *  itself. Boot only; a console that attaches is answered with
 *  spangapIdentityLine() instead. */
void spangapLogBuildIdentity(void);

/** Compose the console greeting's identity line into buf (NUL-terminated, no
 *  newline):
 *
 *      dev f9fb74, host tbeam, fw rop/reticulous_hw-lilygo-tbeam-supreme_20260814130700, ap "lab", ip 10.1.2.3
 *
 *  The physical unit (low three MAC bytes, the digits that lead the USB serial
 *  string), the hostname (same source as the CLI prompt), the image named
 *  exactly as its catalogue file — `<catalogue>/<project-slug>_<dist>_<stamp>`
 *  — and, while associated, the network and address the device is reachable
 *  at (the SSID is free text and therefore the one quoted field). A field
 *  whose fact does not exist is dropped; `hw <board>` is added only when the
 *  detected board differs from the dist (a variant entry whose name is not
 *  simply the board's). Emitted by the console for a bare Enter: a boot log
 *  is watched by almost nobody, and a tool that opens the port later learns
 *  everything from this one answer instead of interrogating the device. */
void spangapIdentityLine(char* buf, size_t n);

/** Block the calling task until the platform clock is known-valid — the storage
 *  key `sys.time.valid` flips to 1 when a time source syncs (SNTP in
 *  spangap-net, gps, spangap-rtc) — or until `timeout_s` elapses, whichever
 *  comes first. Returns true if time became valid, false on timeout.
 *
 *  `timeout_s <= 0` uses the operator-tunable default `s.sys.time_wait_s`
 *  (fallback 30 s). The wait is event-driven — signalTimeValid() wakes it the
 *  instant the clock becomes valid, so it blocks efficiently (light sleep, no
 *  poll) and returns within ms of an SNTP/browser/CLI time-set rather than a
 *  poll period late. It also self-skips when no time source can arrive: with
 *  WiFi disabled there is no NTP, so it returns immediately instead of burning
 *  the timeout (no need to hand-set `s.sys.time_wait_s = 0`). Holds a PM
 *  no-deep-sleep lock for the duration; safe from any task. Intended for the RNS
 *  startup paths so the first announces aren't stamped with the pre-sync 1970
 *  epoch. */
bool waitForTime(int timeout_s);

/** Wake every in-progress waitForTime() — the clock just became valid. Called by
 *  whoever sets `sys.time.valid` (net's NTP sync callback, browser/CLI time-set).
 *  Idempotent and safe from any context (including the tcpip task). */
void signalTimeValid(void);

/** Wake every waitForFlag(key, …) blocked on this boot flag — call it right
 *  after publishing the flag (e.g. after storageSet("rns.ready", 1)). `key` must
 *  be a static string. Idempotent; safe from any task. */
void signalFlag(const char* key);

/** Block until an ephemeral readiness flag `key` (a storage int) is non-zero,
 *  or `timeout_s` elapses (<= 0 = check once, don't wait). Returns true iff set.
 *  The boot-barrier primitive: e.g. waitForFlag("rns.ready", 120) at the top of
 *  an iface/client task body holds it quiet until the RNS universe has settled
 *  (clock valid, network up if configured, minimum settle elapsed). Safe from
 *  any task; holds a shared PM no-deep-sleep lock for the wait. */
bool waitForFlag(const char* key, int timeout_s);

/** Wake every waitForFlag(key, …) blocked on a flag that was just written by
 *  someone who never called signalFlag() — a browser config patch, a CLI `set`.
 *  Lookup-only: a key nobody is waiting on is ignored rather than claiming one
 *  of the few flag slots. Called by the storage change dispatcher for every
 *  change, so any flag becomes settable from off-device without its waiter
 *  falling back to the timeout. */
void signalFlagIfWaited(const char* key);

/** Report that a person is interacting with this device — a keystroke on a
 *  console, a screen woken by a touch, a click in the browser UI. Publishes the
 *  ephemeral flag `sys.human_detected` = 1 (sticky for the boot) and stamps
 *  `sys.human_last_s` with the uptime seconds of the most recent interaction
 *  (uptime, not wall clock, so it is meaningful before the clock syncs), then
 *  signals the flag.
 *
 *  Long holds that only exist to protect an unattended device — a startup
 *  quiet period, a slow retry ladder — wait on the flag with
 *  waitForFlag("sys.human_detected", …) and cut themselves short the moment
 *  somebody is at the controls. The browser writes the same key over the config
 *  channel, which reaches waiters through the storage change dispatcher.
 *
 *  Cheap enough for per-keystroke call sites: the first call publishes and logs
 *  the source, later ones coalesce to at most one write per 30 s. Any task, not
 *  from an ISR. `source` is a short static tag naming the input, for the log. */
void humanDetected(const char* source);

#ifdef __cplusplus
}
#endif

#endif
