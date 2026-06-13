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
 *     extern "C" void app_main() {
 *         spangapInit();           // core foundations only
 *         spangapInitStraddles();  // every staged straddle's init: hook
 *         // ...consumer's own task graph...
 *         spangapPostAppInit();
 *     }
 *
 * No per-straddle init calls and no #if CONFIG_SPANGAP_* guards in app_main:
 * spangapInitStraddles() (generated per buildable by spangap-inside) brings up
 * spangap's own components — core, net, web, lcd, in that fixed order, each only
 * if staged — and then every other staged straddle in dependency order. A board
 * may bracket spangapInit() with its own pre/post HAL bring-up (see hw-tdeck's
 * tdeckStart/tdeckInit). Consumers compose around the platform via
 * netRegister(NET_EV_*, cb), storageSubscribeChanges, cron entries, and
 * /state/boot scripts.
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
 *      → project-mismatch factory reset →
 *      logInit/cliInit/pmInit → cronWakeupHandler → publishBuildTimes
 *
 *  Returns. The consumer's app_main is then responsible for sibling
 *  straddle initialisation (see header docstring above for the typical
 *  ordering) and finally calls spangapPostAppInit() to finalise boot.
 *
 *  Build epoch (`app_build_unix`) is generated and linked in by
 *  spangap-core itself via scripts/write-build-epoch.py — consumers
 *  don't pass it. */
void spangapInit(void);

/** Run every staged straddle's declared `start:` hook (straddle.yaml) as the
 *  FIRST statement in app_main, BEFORE spangapInit() — i.e. before fs/storage/
 *  log/cli exist. This is the bare-hardware band for board bring-up the
 *  platform itself depends on: powering a peripheral rail before spangapInit()'s
 *  fs_mount_sd() touches the SD bus, registering the display/touch HAL before
 *  spangap-lcd's lcdInit(). CONTRACT (the strict inverse of
 *  spangapInitStraddles): a start hook runs on bare hardware — no info(),
 *  storage, fs_* helpers, or ITS are up yet; raw peripherals only. Ordered through the same
 *  init_order() walk as init: (a board lands early because its dependents
 *  require: it). By convention a start hook is named xStart.
 *
 *  DEFINED by the generated staging/spangap_init_dispatch.gen.cpp (empty body
 *  when nothing declares start:). Call once, as app_main's first statement. */
void spangapStartStraddles(void);

/** Run every staged straddle's declared `init:` hook (straddle.yaml) in two
 *  bands: first spangap's own platform components (core, net, web, lcd) in that
 *  fixed order — each only if staged — then every other staged straddle in
 *  dependency-topo order (a straddle inits after the ones it requires:). The
 *  platform band is the contract: anything in the second band can count on
 *  storage/cron, the IP stack, the web stack, and the LCD launcher already
 *  being up, so it neither defers nor orders itself against them.
 *
 *  DEFINED by the generated staging/spangap_init_dispatch.gen.cpp, which
 *  spangap-inside writes for every buildable (empty body when nothing declares
 *  init:). Call once from app_main, right after spangapInit() and before the
 *  consumer's own task graph. From then on a straddle that declares `init:`
 *  initializes automatically when staged — no app_main edit. The buildable
 *  wires the generated file into its main component's SRCS (guarded on
 *  EXISTS). */
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

/** Block the calling task until the platform clock is known-valid — the storage
 *  key `sys.time.valid` flips to 1 when a time source syncs (SNTP in
 *  spangap-net, GPS/RTC in hw-tdeck) — or until `timeout_s` elapses, whichever
 *  comes first. Returns true if time became valid, false on timeout.
 *
 *  `timeout_s <= 0` uses the operator-tunable default `s.sys.time_wait_s`
 *  (fallback 30 s); set that key to 0 on an offline node with no time source to
 *  skip the wait entirely (it would never sync, so the delay buys nothing).
 *  Holds a PM no-deep-sleep lock for the duration and is safe to call from any
 *  task. Intended for the RNS startup paths (rnsd + the transports) so the
 *  first announces and path-table entries aren't stamped with the pre-sync
 *  1970 epoch. */
bool waitForTime(int timeout_s);

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
