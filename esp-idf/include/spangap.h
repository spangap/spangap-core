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
 *    line-buffered stdout → fs_init → optional fs_mount_sd (when
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

/* Boot participation is a Service (service.h), not a free-function dispatcher.
 * The generated app_main constructs every staged straddle's Service and walks
 * the registry: serviceRunStart() (onStart, bare hardware, before spangapInit)
 * then serviceRunInit() (onInit, ecosystem up). Those walk entry points live in
 * service.h; there is no consumer-called spangapStartStraddles/InitStraddles. */

/** Finalise platform startup after the consumer has brought up its own
 *  modules. Runs:
 *
 *    rtcRamSetValid → cliRunFile("/state/boot") → set sys.boot_complete
 *      = 1 → logApplyLevels → info("ready") → cronPoll(true)
 *
 *  Modules that registered for `sys.boot_complete` via storage subscribe
 *  fire here. After this call, IDF's main_task automatically deletes
 *  itself — no explicit vTaskDelete needed. */
void spangapPostAppInit(void);

/** Block the calling task until the platform clock is known-valid — the storage
 *  key `sys.time.valid` flips to 1 when a time source syncs (SNTP in
 *  spangap-net, GPS/RTC in hw-lilygo-tdeck) — or until `timeout_s` elapses, whichever
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

#ifdef __cplusplus
}
#endif

#endif
