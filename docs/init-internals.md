# init — internals

Maintainer reference for platform bring-up. The [operator guide](init.md) covers
the four-call model and the boot barriers; this file is the exact order and the
contracts that order encodes. Source: [`spangap_init.cpp`](../esp-idf/src/spangap_init.cpp),
[`spangap.h`](../esp-idf/include/spangap.h).

## 1. What this adds

- **`spangapInit()`** — the eager core-foundation bring-up (filesystem, state
  store, config tree, log/CLI/pm/auth, deep-sleep wake decision, build identity).
- **`spangapPostAppInit()`** — boot finalisation (RTC watchdog disable, RTC-RAM
  validity, boot script, `sys.boot_complete`, first cron poll).
- **`spangapStartStraddles()` / `spangapInitStraddles()`** — declared here,
  **defined by the build-generated** `staging/spangap_init_dispatch.gen.cpp`
  (`spangap-inside` writes one per buildable; an empty body when nothing declares
  the hook). They are the seam that keeps spangap-core free of compile- *and*
  link-time knowledge of which siblings exist.
- **`waitForTime` / `waitForFlag`** — the boot-barrier primitives, with the
  shared power-management no-deep-sleep lock.
- The **project-mismatch factory reset** and the **`fw.*` identity** model.
- Build-identity publication (`publishBuildTimes`) from linked-in epoch/info
  symbols.

## 2. `spangapInit()` order

Exactly this sequence, and the ordering is load-bearing:

1. `setvbuf(stdout, _IOLBF)` — line-buffer stdout so each `\n` flushes (USB Serial
   JTAG is fully-buffered by default and would hide log lines).
2. `fs_init()` — mount `/fixed` (read-only) and the on-flash `/state` (always),
   start the fs worker tasks. `statePartitionEnsure()` runs at the top of this
   (see [flash-partitions](flash-partitions.md)).
3. `fs_mount_sd()` — no-op unless `CONFIG_SPANGAP_SDCARD`.
4. `fsSelectStateStore()` — pick `/state` vs `/sdcard/state` and seed the store on
   first boot. **Must sit between the SD mount and `storageLoad()`**: it decides
   which directory `storageLoad()` reads `storage/root.json` from.
5. `storageLoad()` — read the persisted config tree.
6. **Project-mismatch factory reset.** Read `s.sys.project`; if non-empty and
   `!= CONFIG_SPANGAP_PROJECT_NAME`, `esp_littlefs_format("state")` + `esp_restart()`
   (runs *before any module reads the polluted tree*). If empty, install the
   compile-time value.
7. `logInit()`, `cliInit()`, `pmInit()`, `authInit()` — the foundation tasks.
   `authInit()` comes up before sibling straddles (sshd, web) because they need
   `authLogin`/`authCheck`; the HTTP face is wired later by spangap-web's
   `authWebInit()` inside `webInit()`.
8. `cronWakeupHandler()` — the deep-sleep wake fast-path (below).
9. `publishBuildTimes()` — populate the `sys.build*` / `sys.buildtime.*`
   telemetry from the linked-in `app_build_*` symbols and `/fixed/build_times`.

The storage *task* (`storageInit`) and `cronInit` are deliberately **not** here —
they declare `init:` hooks and come up via `spangapInitStraddles()`, like every
sibling. Log timestamps start in UTC and switch to the persisted zone once the
time straddle's `init:` hook applies it.

## 3. `spangapInitStraddles()` ordering contract

Two bands, in order:

1. spangap's own platform components — **core, net, web, lcd, fixed order**, each
   only if staged.
2. every other staged straddle in dependency-topological order (`require:`
   relationships), the same `init_order()` walk that orders the `start:` band.

The contract band-2 code relies on: storage/cron, the IP stack, the web stack,
and the LCD shell are **already up** by the time a band-2 `init:` hook runs, so a
consumer neither defers nor self-orders against them. `start:` hooks run the
strict inverse — bare hardware, no platform services — and are ordered through
the same walk (a board lands early because its dependents `require:` it).

## 4. `spangapPostAppInit()`

1. If `CONFIG_BOOTLOADER_WDT_DISABLE_IN_USER_CODE`: disable the RTC (bootloader)
   watchdog kept armed across the whole boot window. Surviving to here means every
   core foundation **and** every straddle's hardware bring-up ran without wedging;
   past this point the scheduler's int/task watchdogs cover any hang.
2. `rtcRamSetValid()` — mark RTC RAM valid so RTC vars survive a deep-sleep wake
   (it reads false after a warm reboot, `esp_restart`, or panic).
3. `cliRunFile(fsStatePath("/boot"))` — the boot script runs **last** so every CLI
   command (platform and consumer) is already registered.
4. `storageSet("sys.boot_complete", 1)` — fires `sys.boot_complete` subscribers.
5. `logApplyLevels()`, then `cronPoll(true)` — run any cron entries that fall in
   the current minute (a deep-sleep wake may already have moved time past a
   scheduled minute).

IDF's `main_task` auto-deletes after this returns — no explicit `vTaskDelete`.

## 5. Boot barriers

`waitForTime` and `waitForFlag` both poll storage directly (a locked read, not an
ITS round-trip — a waiter is **not** registered as an ITS task merely by waiting)
and both hold a single shared `PM_NO_DEEP_SLEEP` lock, lazily created and
aggregated across the several boot tasks that wait in parallel (rnsd and the
interfaces race here at boot), so deep sleep stays blocked while any wait is live.
The lock handle is captured into a local before acquire/release because two boot
tasks racing the lazy-create could each write the static — acquire and release
must name the same handle the call saw.

## 6. Deep-sleep wake fast-path

`cronWakeupHandler()` runs inside `spangapInit()` (step 8), early enough that a
timer wake with no cron work this minute can go **straight back to sleep without
fully booting**. See [cron-internals](cron-internals.md) for the wake-decision
logic; the only init-side fact is its position — it must run after `fs_init()`
(it reads the crontab) and before the heavyweight foundation work that a
back-to-sleep wake would waste.

## 7. Build identity

`app_build_unix` (epoch) and `app_build_straddle` / `_version` / `_args` are
`extern "C"` symbols defined in generated files (`spangap_app_build_epoch.c`,
`spangap_app_build_info.c`) that `spangap build` regenerates each ninja
invocation. `publishBuildTimes()` also reads `/fixed/build_times` (a 12-byte blob:
`fixed` source mtime, image epoch, optional webroot CRC32) to surface
`sys.buildtime.fixed` / `.web`. A webroot CRC32 change is the browser's cue to
reload the SPA.

## 8. Pitfalls

- **`fsSelectStateStore()` must run between `fs_mount_sd()` and `storageLoad()`.**
  Move it and `storageLoad()` reads the wrong store (or no store).
- **The project-mismatch reset must precede any module read of the tree.** It runs
  inside `spangapInit()` before the foundation tasks for exactly this reason — a
  module that cached a key from a foreign project's `/state` would survive the
  format.
- **Never hand-write `app_main` or call a straddle's `xInit()` from it.** The
  dispatcher is generated; adding a manual call double-inits or breaks the
  band-ordering contract. A straddle runs at boot by declaring an `init:`/`start:`
  hook, nothing else.
- **`start:` hooks touch only raw hardware.** No `info()`, storage, `fs_*`, or ITS
  exist yet in that band; using them there crashes or silently no-ops.
- **`fw.*` is synthesized, never stored.** It is built into the storage dump from
  ROM constants and is not in `cfgRoot`; do not add an `s.sys.banner`-style
  mutable mirror of it.
