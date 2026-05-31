/**
 * spangap.h — public CORE umbrella header.
 *
 * One include for the spangap-core API surface only: compat, pm, log, fs,
 * storage, its, cli, cron. Sibling straddles (spangap-net, spangap-web,
 * spangap-lcd, wg, upnp, duckdns, acme, ota, …) live in their own headers;
 * consumers `#include` them directly:
 *
 *     #include "spangap.h"
 *     #include "net.h"        // from spangap-net (if you require it)
 *     #include "web.h"        // from spangap-web
 *     #include "wg.h"         // from wg
 *     ...
 *
 *     extern "C" void app_main() {
 *         spangapInit();      // core foundations only
 *         tlsInit(); netInit(); ntpInit(); ntpApplyTimezone();
 *         mdnsInit(); wgInit(); upnpInit(); duckdnsInit(); acmeInit();
 *         otaInit(); webInit(); storageInit(); webrtcInit(); cronInit();
 *         #if CONFIG_SPANGAP_LCD
 *             lcdInit();
 *         #endif
 *         // ...consumer's own task graph...
 *         spangapPostAppInit();
 *     }
 *
 * Consumers compose around the platform via netRegister(NET_EV_*, cb),
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

#ifdef __cplusplus
extern "C" {
#endif

/** Bring up the spangap CORE platform:
 *
 *    line-buffered stdout → fs_init → optional fs_mount_sd (when
 *      CONFIG_SPANGAP_SDCARD=y) → fsSelectStateStore → storageLoad
 *      → project-mismatch factory reset → s.sys.banner default →
 *      logInit/cliInit/pmInit → cronWakeupHandler → resetOnOffHandler
 *      → publishBuildTimes
 *
 *  Returns. The consumer's app_main is then responsible for sibling
 *  straddle initialisation (see header docstring above for the typical
 *  ordering) and finally calls spangapPostAppInit() to finalise boot.
 *
 *  Build epoch (`app_build_unix`) is generated and linked in by
 *  spangap-core itself via scripts/write-build-epoch.py — consumers
 *  don't pass it. */
void spangapInit(void);

/** Run every staged straddle's declared `init:` hook (straddle.yaml), in
 *  (order, dependency-topo) order. DEFINED by the generated
 *  staging/spangap_init_dispatch.gen.cpp, which spangap-inside writes for
 *  every buildable (empty body when nothing declares init:). Call once from
 *  app_main, after the core/net foundation is up and before the consumer's
 *  own task graph. From then on a straddle that declares `init:` initializes
 *  automatically when staged — no app_main edit. The buildable wires the
 *  generated file into its main component's SRCS (guarded on EXISTS). */
void spangapInitStraddles(void);

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

#ifdef __cplusplus
}
#endif

#endif
