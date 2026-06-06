# Straddles: distributable feature/app units that span firmware + browser

Status: planning, not implemented. Captures a design discussion around how third-party (and first-party) features that span *several* surfaces — firmware, browser UI, on-device LCD UI — can be packaged, versioned, named, distributed, and combined into a buildable device image, without forcing users to assemble a dependency graph by hand and without inviting registry-namespace attacks.

This plan extends [`Kconfig.md`](Kconfig.md). That document fixes how spangap-core's *own* modules toggle and initialize (per-consumer Kconfig + single `spangapInit()`). This document is the *next layer*: how external feature/app packages compose with spangap-core, how they're identified, fetched, staged, and brought up.

## Note to the agent executing this plan

**All work lands on the `straddles` branch, not `main`.** The plan
renames workspace dirs and adds manifests in place — confirm the active
branch before any commit. `seccam/` is in scope from Phase 3 onward.

Phase boundaries are hard. The user flashes the result on real hardware
and reports back; only then does the next phase open. You don't run
`idf.py flash` or build/flash automatically — make the edits, hand them
off, wait.

Some steps are user-executed, not agent-executed: npm / components.espressif.com
name claims (step 1), GitHub org creation, any push to a public registry. Draft
the artifacts and the exact commands, then hand off.

Workspace gotchas:

- Builds run inside the `spangap-build-env:dev` Docker container via the
  `spangap` host-side shim ([`spangap/spangap`](../../../spangap/spangap)).
  The Alpine host has no ESP-IDF / Node / Python / cairo installed — everything
  toolchain-side lives in the container.
- Reference clones (microReticulum, NomadNet upstream, ratdeck, …) live in
  [`../../../research/`](../../../research/) — gitignored, multi-GB,
  re-cloneable. Read-only; license-check before borrowing code.
- The user's memory file (`MEMORY.md`) carries durable preferences worth
  honouring up-front: NEVER use `gh` (use WebFetch/WebSearch for GitHub); NEVER
  call ExitPlanMode; bundle hardware-cycle changes; discuss before coding when
  results surprise.

This document is the authoritative spec — read it end-to-end before starting.
*Plan for execution* below is the concrete step list; everything above is
rationale you'll need when judgement calls come up mid-step.

## Motivation

A spangap feature is intrinsically multi-bodied: it almost always has firmware code (an ESP-IDF component) plus paired browser code (a Vue/Quasar package), and increasingly also on-device LCD UI (LVGL programs). All of these are only correct *together*. Shipping a feature as "an npm package + an ESP-IDF component (+ another component for the LCD bits)" forces every consumer to re-establish that pairing by hand at matching versions, with nothing enforcing it — and turns one feature into N parallel repos. The unit you actually want to ship and version is the **feature**, not its halves.

A *straddle* is that unit: a single, single-versioned, single-named distributable that holds all the surfaces of one feature in one repo, plus its docs/tools/tests. Today's `seccam` and `reticulous` are already proto-straddles — each is a firmware tree plus a browser tree paired by hand against spangap. This plan formalizes that into a declared, versioned, machine-resolvable container so the same arrangement scales to third-party authors and to a build-your-own-device kit.

## Goals

1. **One name, one version, one repo per feature.** A straddle is a github repo. Its firmware, browser, and on-device LCD halves live inside as subdirs at the same version, atomically. Nobody ever has to keep separately-versioned packages "in sync," and one feature never sprawls into three parallel `-web` / `-lcd` repos.
2. **The resolver solves dependency graphs, not the human.** Users pick apps and features by name; transitive firmware-component, npm-package, and other-straddle deps are filled in automatically.
3. **Coding inside a straddle stays unchanged from spangap-core conventions.** Authors write `xInit()` functions, `s.x.*` storage keys, `#include "x/x.h"` — same shape as today's modules. No new C++ namespace ceremony, no new macros required.
4. **One npm CLI as the front door.** Beginner-friendly install (`npm install -g spangap`), then `spangap init/make/flash`. The CLI hides Docker, the dual ESP-IDF/npm build worlds, the staging dance, and the host/VM serial split.
5. **No registry trust expansion.** Adding straddles to the ecosystem doesn't require every author to claim and police namespaces on npm and components.espressif.com. One defensive scope, claimed once by spangap, protects everyone.
6. **Curated profiles, not free-for-all.** A small, named, tested set of build profiles ("core only," "core + net," "everything") rather than a combinatorial pick-list of modules. Modularity that nobody tests rots.

## Terminology

- **Straddle** — a single github repo holding any combination of *firmware half* (`esp-idf/`, optionally with an `esp-idf/lcd/` sub-dir for on-device LVGL UI sources) and *browser half* (`browser/`) of one feature/app at one version. The unit of distribution. A straddle may have either, both, or only the LCD slice.
- **Buildable straddle** — a straddle that declares image-level globals (partition layout, image name, etc.) and produces a flashable device image. `seccam`, `hw-tdeck`, a future `calendar-kiosk` are buildable straddles.
- **Feature straddle** — a straddle without app globals. A reusable library/feature that other straddles pull in (e.g., `spangap-core`, `spangap-web`, `reticulous` (the rnsd protocol layer), `lxmf`).
- **UI activator** — a special pair of feature straddles, `spangap/spangap-web` and `spangap/spangap-lcd`, whose presence in the dep graph *activates* compilation of every other straddle's `browser/` half and `esp-idf/lcd/` slice respectively. Without the activator in the graph (or with `--no-web-ui` / `--no-lcd` on the build), those halves sit dormant in the source tree. See *UI activators* below.
- **Board straddle** — a special case of buildable straddle that pairs a board's HAL (display, input, partition layout, GPIO wiring) with a curated set of feature straddles. `hw-tdeck` is the canonical example; `someone/reticulous-heltec` would be the natural way to bring up the same protocol stack on a Heltec board.
- **Project home repo** — by convention, `<org>/<org>` is the org's umbrella repo (CLI shim, docs, install instructions, container Dockerfile), *not* a straddle. `spangap/spangap` is the canonical example; nothing `requires:` it.
- **Manifest** — a `straddle.yaml` at the straddle's repo root declaring identity, deps, supported platforms, and (for buildable straddles) globals.

## The straddle as a unit

### Repo layout

```
github.com/<org>/<repo>/
  ├── straddle.yaml           ← the manifest
  ├── esp-idf/                ← ESP-IDF component (firmware half)
  │     ├── CMakeLists.txt
  │     ├── idf_component.yml
  │     ├── include/
  │     │     └── <prefix>/
  │     │           └── <name>.h
  │     ├── src/
  │     │     └── <name>.cpp
  │     └── lcd/              ← on-device LVGL UI sources — still firmware, just
  │           ├── include/      a slice of the component that's only included
  │           │     └── <prefix>/   when the spangap/spangap-lcd activator is in the graph
  │           │           └── <name>_lcd.h
  │           └── src/
  │                 └── <name>_lcd.cpp
  ├── browser/                ← Vue/Quasar package (browser half)
  │     ├── package.json      ← built only when spangap-web activator is in graph
  │     └── src/
  ├── docs/
  ├── tools/
  └── tests/
```

`esp-idf/lcd/` is *firmware* — it compiles into the ESP-IDF component alongside `esp-idf/src/`. It's a sub-dir, not a sibling of `esp-idf/`, because there's no separate platform here: the LCD code is C++ that runs on the ESP32. What makes it special is only that the CLI omits it from the component's source/include lists unless `spangap/spangap-lcd` is reachable in the dep graph (and `--no-lcd` isn't set). When activated, the LCD slice's headers (`esp-idf/lcd/include/`) join the component's include path and its sources (`esp-idf/lcd/src/`) join the source list. There's no second ESP-IDF component, no separate library — same component, two source dirs, one of them gated on the activator.

Default subdir names are `esp-idf/`, `esp-idf/lcd/`, and `browser/`; the manifest can override them. The halves are independently optional — a pure-firmware library has no `browser/` or `esp-idf/lcd/`, a pure-browser piece no `esp-idf/`, etc. The `browser/` half and the `esp-idf/lcd/` slice are *passive*: they sit in the repo until an activator (`spangap/spangap-web` or `spangap/spangap-lcd`) appears in the dep graph and turns them on across every straddle in the build. The manifest declares supported platforms.

Why `esp-idf/` and not `esp32/`: the subdir names a *build system / toolchain*, not a chip family. ESP-IDF supports many ESP32 variants under one tree (s3, c6, p4, h2, c3 …), while Zephyr can also target esp32 — naming by build system stays unambiguous if/when other ports appear (`pico/`, `zephyr/`, …).

### The manifest (`straddle.yaml`)

Tiny by design. It carries only what the *resolver* needs:

```yaml
name: spangap/calendar
prefix: calendar                   # short identity: symbol prefix, include subdir, browser import name
version: 0.3.0

# where the halves live (defaults shown — usually omitted)
firmware: esp-idf/
firmware_lcd: esp-idf/lcd/         # sub-dir of firmware; still C++ code in the same ESP-IDF component
browser: browser/

# other straddles this one depends on; resolver pulls them transitively.
# spangap/spangap-web and spangap/spangap-lcd are UI activators — naming them
# here is how a build opts into compiling everyone's browser/ or esp-idf/lcd/
# slice.
requires:
  - spangap/spangap-core           # offline foundation
  - spangap/spangap-net            # IP stack: net/tls/ntp/mdns (transitively pulled by spangap-web)
  - spangap/spangap-web            # activates browser/ across the graph; brings web/auth/webrtc + UI shell
  - spangap/spangap-lcd            # activates esp-idf/lcd/ across the graph; on-device LVGL launcher
  - spangap/calendar-shared-ui     # a reusable feature straddle

# which platform builds this straddle supports
platforms: [esp-idf]

# buildable globals — present only on buildable straddles
buildable:
  name: "Calendar"
  partition_layout: standard-8mb
  ota_pubkey: keys/ota.pem
```

What the manifest deliberately does *not* contain:

- No `hooks:` section. Hook bindings (when used at all) live in code, inside `init()`, not in YAML. The manifest declares deps and identity only.
- No CLI/web/menu registration entries — those are direct function calls inside `init()`.
- No version range solver. Pin a tag, branch, or commit SHA; the resolver fetches exactly that.

### Init: just `<prefix>Init()`

The init mechanism matches the existing spangap-core convention exactly. A straddle defines one function — same shape as `storageInit`, `webInit`, `cliInit` already in the platform today:

```cpp
// esp-idf/src/calendar.cpp
#include "calendar/calendar.h"

static void onNetUp() { /* ... */ }

void calendarInit() {
    cliRegisterCmd("cal", calCliHandler);
    storageSubscribeChanges("s.calendar.", calOnConfigChange);
    netRegister(NET_EV_UPSTREAM_UP, onNetUp);
    // or, for a documented post-init lifecycle hook:
    spangapOn(Hook::BootComplete, &onBootComplete);
}
```

No C++ namespace wrapping per straddle. No macros required. The `<prefix>` (here `calendar`) is unique by convention — same way `storageGet` and `cliRegister` already don't collide because no other module uses those prefixes.

The build generates a flat dispatcher that calls each straddle's `<prefix>Init()` in topo-sorted dep order, and (when `spangap/spangap-lcd` is in the graph) each straddle's `<prefix>LcdInit()` immediately after — but only for straddles that actually ship an `esp-idf/lcd/` slice. The CLI knows the dep graph at staging time, so the dispatcher is emitted with exactly the right set of calls; no preprocessor gating in the generated code:

```cpp
// generated — example output with spangap/spangap-lcd activator in graph
extern void calendarSharedUiInit();
extern void calendarInit();
extern void calendarLcdInit();
// ...

void spangapRunStraddles() {
    calendarSharedUiInit();
    calendarInit();
    calendarLcdInit();        // only present when spangap/spangap-lcd is in the graph
    // ...
}
```

With `--no-lcd` (or `spangap/spangap-lcd` not reachable), the same generator omits every `*LcdInit()` line and the corresponding `esp-idf/lcd/` slices are absent from each firmware component's source list. The dispatcher and the source list change together — one source of truth (the resolved graph).

`spangapRunStraddles()` is called from `spangapPostAppInit()` (or equivalent), after the platform's own `spangapInit()` has brought up the modules from [Kconfig.md](Kconfig.md). See *Relationship to Kconfig.md* below.

### Hooks: small post-init lifecycle set, opt-in inside `init()`

Most coordination needs are *not* hooks. Specifically:

- **"Run me after spangap is up"** → falls out of topo-sorted init order. By the time your straddle's `init()` runs, everything it `requires` has already initialized. No hook needed.
- **"Wait until B has finished some async readiness step"** → ephemeral storage key + `storageSubscribeChanges` (see *Inter-straddle coordination* below). No hook needed.
- **"Register a CLI command / web route / menu item / settings panel"** → direct function call inside `init()` against the present module (`cliRegisterCmd`, `webRoute`, `menuRegistry.register`, …). Not a hook; just code.

What *is* a hook: a small set of named **post-init asynchronous lifecycle events** that aren't natural storage vars. Initial candidates:

| Hook | Fires when |
|---|---|
| `Hook::BootComplete` | After all straddles' `Init()` have returned and `/state/boot` has run |
| `Hook::NetUp` / `Hook::NetDown` | Wraps the existing `NET_EV_UPSTREAM_UP/DOWN` for straddles that don't want to depend on `net.h` directly |
| `Hook::Shutdown` | Pre–deep-sleep / clean-shutdown notification |

Callback signature is `void(void)`. The dispatcher iterates registered callbacks per hook; the function-pointer table is populated at init time from inside straddles' `init()` bodies. No opaque `void*` payload, no per-hook signature variants — if a future event genuinely needs a payload it earns a typed overload at that time.

### Inter-straddle coordination

Two tiers, each reusing existing machinery — no new primitive is introduced:

1. **Static ordering** — declared in the manifest as `requires`. The CLI topo-sorts and emits init calls in dependency order. "A's `init()` must see B already initialized" is a `requires: [B]` line.

2. **Dynamic readiness** — handled via the existing storage subscription pattern with NOW_AND_ON_CHANGE semantics. A component publishes a key when it becomes ready, others subscribe; subscribers that arrive *before* the publish still get the value the moment it's written (no startup race), subscribers that arrive *after* it's written get the current value immediately. Per CLAUDE.md storage conventions, **rendezvous keys are non-`s.` (ephemeral, RAM-only)** so they can't survive a reboot to go stale, and publishing them costs no flash writes.

   Two components agree on a key string (e.g., `calendar.db_ready`); spangap never learns what the string means. The opaque value under the key is the (firmware-only, within-one-boot) escape hatch for any "the other component must give me a handle" case — though wherever possible, rendezvous on plain data (a port, a path, a flag) and let the waiter open its own handle, to avoid cross-straddle lifetime/ownership concerns.

### Additive registration: just function calls

CLI commands, web routes, menu items, settings panels — these are not hooks and not declarative entries. They're plain function calls inside `init()` against whichever modules are present:

```cpp
void calendarInit() {
    // CLI surface
    cliRegisterCmd("cal", calCliHandler);

    // browser-facing settings panel: registered via the menu store
    // (browser-side registration; see browser-half code)

    // a WebRTC data-channel endpoint for live updates
    itsServerPortOpen(CAL_DC_PORT, 1, 2048, 4096);
    itsServerOnConnect(CAL_DC_PORT, calOnDcConnect);
}
```

If a straddle wants to be usable on a profile that doesn't include `web` or `cli`, it guards those calls (`#if CONFIG_SPANGAP_WEB`) — same pattern Kconfig.md establishes for spangap-core. Most straddles will require the bundles they need and not bother.

## Browser-side parallel structure

The browser half of each straddle is built and orchestrated by an exact mirror of the firmware mechanism — same idea, but ES modules and Vite handle the isolation work that naming conventions and CMake components do on the firmware side. The discipline a straddle author needs to learn is *less* on the browser side, not more.

Everything in this section is gated on the **`spangap/spangap-web` activator** being in the dep graph (and `--no-web-ui` not being passed to `spangap make`). When the activator is absent or suppressed, the CLI skips browser staging, doesn't emit `straddle_dispatch.ts`, and doesn't invoke Vite/quasar at all — the firmware build proceeds untouched. A straddle's `browser/` subdir is dormant source in that mode.

### Each straddle's browser package exports an `init()`

The browser-half public entry point is `browser/src/index.ts`, exporting one function — the direct analog of firmware's `<prefix>Init()`:

```ts
// browser/src/index.ts in github.com/spangap/calendar
import CalendarPanel from "./panels/CalendarPanel.vue";
import { menuRegistry } from "spangap-browser";

export function init() {
    menuRegistry.register({
        group: "Apps",
        id: "calendar",
        label: "Calendar",
        component: () => CalendarPanel,
    });
    // ...subscribe to storage keys, register Pinia stores, register WebRTC channels, etc.
}
```

No `<prefix>` discipline on the function name (unlike firmware): ES modules already namespace exports per module. `calendar`'s `init` is a different symbol from `coolapp`'s `init`; the module path disambiguates. Plain `init` is the convention.

### The CLI generates a dispatcher (`straddle_dispatch.ts`)

Symmetric with the firmware-side `straddle_dispatch.cpp`:

```ts
// staging/generated/straddle_dispatch.ts (generated)
import { init as calendarSharedUiInit } from "calendar-shared-ui";
import { init as calendarInit } from "calendar";
import { init as coolappInit } from "coolapp";

export function runStraddleInits() {
    calendarSharedUiInit();    // topo-sorted dep order — same as firmware
    calendarInit();
    coolappInit();
}
```

The app's `main.ts` (scaffolded by `spangap init`) calls `runStraddleInits()` once after Quasar/Pinia setup:

```ts
import { createApp } from "vue";
// ...Quasar + Pinia bootstrap...
import { runStraddleInits } from "./generated/straddle_dispatch";
runStraddleInits();
app.mount("#app");
```

Bare-specifier imports (`"calendar"`) resolve through the Vite alias config the CLI writes — same role as ESP-IDF's include-path resolution on the firmware side. Never via the npm registry. The encoded `@spangap/spangap__calendar` `package.json` name exists only for 404-safe leak prevention; nothing imports by that string.

### Tree-shaking is the disable mechanism

Dropping a straddle from the manifest = its import vanishes from the generated dispatcher = Vite tree-shakes its entire package out of the bundle. No `import.meta.env` gating needed on the dispatcher side; selection-by-presence-in-manifest does the work.

(The Kconfig.md plan's `VITE_SPANGAP_*` env-var bridge stays in use for *spangap-core's own internal modules* — duckdns panel, wireguard panel, etc. Those gate on per-consumer Kconfig flags. The straddle dispatcher is the gate for *external* straddles. The two mechanisms compose: a straddle's `init()` might still internally check `import.meta.env.VITE_SPANGAP_WG` before registering a WG-related sub-panel.)

### What the browser side does NOT need

Several pieces of firmware-side machinery have no browser-side counterpart, because the JS module system, Pinia, and Vue's reactivity already do that work:

- **No `<prefix>` symbol convention.** ES modules namespace exports per module. Two straddles' `init`s, helpers, types, components, and stores don't collide; they live in separate module scopes. Authors write whatever names they like; isolation is structural.
- **No anonymous-namespace / `static` discipline for privates.** Anything not exported from `index.ts` is module-local by default.
- **Fewer hooks.** Most "would-be hooks" are already subscriptions to Pinia stores or storage keys — `device.connected`, `auth.userId`, `storage.subscribe("s.cal.due")`. The browser-side parallel of the firmware hook set is small and grown on demand; the existing reactive primitives cover the rest.

### Cross-side rendezvous bridges for free

A non-`s.` (ephemeral) storage key the firmware half publishes lands in the browser's storage tree automatically via the existing `storage:1` DC sync. So both halves of one straddle can rendezvous through the same key with no new plumbing — firmware writes `calendar.db_ready`, browser subscribes to `calendar.db_ready` — and the rendezvous *also* works across halves of *different* straddles. This is the real payoff of reusing storage as the coordination primitive: it's the one that already bridges the firmware/browser boundary, so making it the rendezvous primitive means coordination works across firmware↔firmware, browser↔browser, and firmware↔browser identically. No second mechanism needed.

### The one wrinkle: Vue global component name collisions

The one place browser-side code can collide despite ES module isolation is `app.component("MyButton", ...)` — Vue's global component registration uses a string name in a single shared registry. Two straddles each globally registering `<MyButton>` would conflict.

Convention: prefer **local imports** inside components (Quasar's default style), and if global registration is genuinely needed, **prefix the component name** with the straddle's short identity (`<CalendarButton>`, `<CoolAppButton>`). It's the same `<prefix>` convention as firmware public APIs, applied only to the narrow case of globally-registered Vue components. Rarely needed in practice; documented as a straddle-author convention.

### Symmetry at a glance

| Concern | Firmware | Browser |
|---|---|---|
| Public entry | `void <prefix>Init()` | `export function init(): void` from package `index.ts` |
| Generated dispatcher | `straddle_dispatch.cpp` | `straddle_dispatch.ts` |
| Resolution mechanism | ESP-IDF component dirs + `#include` paths | Vite aliases + ES module imports |
| Symbol namespacing | Naming convention (`<prefix>`) | ES module scope (free) |
| Private internals | `static` / anonymous namespace | Unexported from `index.ts` |
| Hook set | Small documented enum (`Hook::NetUp`, …) | Smaller still — Pinia/storage subscriptions cover most |
| Inter-straddle rendezvous | Storage NOW_AND_ON_CHANGE on ephemeral key | Same key, synced automatically by `storage:1` DC |
| Disable mechanism | Source-exclude or stub-body (per [Kconfig.md](Kconfig.md)) | Tree-shake on unimported modules |

End result: the browser side is built from the same dependency graph as the firmware side, generates the same shape of dispatcher, and is expressed in *less* author code — no namespace, no macros, no scope discipline. Just one exported function per straddle and whatever else its `init()` happens to register.

## LCD slice: firmware code, activator-gated source list

The on-device LCD code is *firmware* — same language, same toolchain, same ESP-IDF component as the rest of `esp-idf/`. It lives as a sub-dir, `esp-idf/lcd/`, only so that the CLI can include or exclude the whole slice cleanly based on whether `spangap/spangap-lcd` is in the dep graph (and `--no-lcd` isn't set). Conceptually it's not a third half alongside firmware and browser — it's a *gated portion* of the firmware half.

Each straddle that wants to expose on-device UI ships an `esp-idf/lcd/` slice with one or more LVGL programs and/or settings panes, plus a single public entry point:

```cpp
// esp-idf/lcd/src/calendar_lcd.cpp
#include "calendar/calendar_lcd.h"
#include "lcd.h"

static void calLauncher() { /* ... */ }

void calendarLcdInit() {
    lcdRegister("Calendar", calIcon, calLauncher);
    lcdRegisterSettings("Apps/Calendar", "Calendar", calLcdSettings);
}
```

When `spangap/spangap-lcd` is reachable, `spangap make` adds `esp-idf/lcd/include/` to the straddle's ESP-IDF component include path and `esp-idf/lcd/src/*` to its source list, then emits the call to `<prefix>LcdInit()` into the generated firmware dispatcher in topo-sorted dep order — directly after each straddle's `<prefix>Init()`. When the activator is absent or suppressed, the slice is simply not added; no symbols, no overhead.

Disable mechanism is *source-list exclusion*, not Kconfig ifdefs in author code: a straddle's LCD code can call `lcdRegister`/`lcdRegisterSettings` and `#include "lcd.h"` unconditionally, because the file only compiles in builds where the activator is on (and so `lcd.h` is reachable). This is a niceness over the firmware-half + `#if CONFIG_SPANGAP_LCD` guard pattern Kconfig.md establishes for spangap-core's own modules.

Same rendezvous primitives apply: an LCD program rendezvous with the rest of the firmware via the same ephemeral storage keys; everything `storage`/`its`/`net` already does is available unchanged — there's no boundary to cross, it's all one component.

## Platform decomposition: `spangap-core`, the net layer, the two activators, granular extras

The spangap platform is **not a single straddle**. It's a small set of straddles whose dep graph naturally produces the "offline-only," "with IP networking," "with browser UI," "with on-device UI," "everything" layers without any tier machinery in the build system. Two of those straddles — `spangap/spangap-web` and `spangap/spangap-lcd` — additionally play the role of *UI activators* (see *UI activators* below): naming them in `requires:` is what tells the build to compile every other straddle's `browser/` half or `esp-idf/lcd/` slice.

- **`spangap/spangap-core`** — offline-complete foundation. Firmware-only (`platforms: [esp-idf]`, no browser half).
  Members: `its`, `fs`, `spi_helper`, `storage`, `log`, `cli`, `cron`, `pm`, `compat`.
  A device built with only `spangap-core` is fully functional offline — serial console, local config, scheduling, persistent state. Sensor logger, dev board, or a LoRa-only mesh node (the LoRa transport rides on SPI through `spangap-core`; no IP needed). The `prefix:` of this straddle is the empty string by convention: its symbols (`storageGet`, `cliRegisterCmd`, `info()`, …) read as language primitives, not as a namespaced library.

- **`spangap/spangap-net`** — IP networking + TLS. Firmware-only, no browser half, **not an activator**. `requires: [spangap/spangap-core]`.
  Members: `net`, `tls`, `ntp`, `spangap_mdns`.
  The foundation for anything that needs WiFi + TCP/UDP — RNS-over-TCP, the AutoInterface UDP-multicast peer discovery, MQTT clients, the HTTPS server. A LoRa- or ESPNOW-only node skips this entirely; a node with WiFi but no browser server still pulls it.

- **`spangap/spangap-web`** — HTTPS + auth + WebRTC + the shared browser UI shell, **and the browser-side activator**. `requires: [spangap/spangap-net]`.
  Firmware members: `web`, `auth`, `webrtc_task`, `webrtc_sctp`.
  Browser half: the shared UI shell — `FloatingWindow`, `MenuBar`, `SettingX` controls, `TerminalWindow`, `LogWindow`, `EditorWindow`, the WebRTC session, auth flow, storage sync, menu registry, Pinia stores.
  Activator role: when reachable in the dep graph (and not suppressed by `--no-web-ui`), the build picks up every other straddle's `browser/` subdir and includes its `init()` in the generated browser dispatcher.
  `auth` bundling with `web` is structural: enabling browser access is exactly when remote-storage protection must apply, and the bundle enforces it.

- **`spangap/spangap-lcd`** — LVGL launcher + on-device shell, **and the LCD-side activator**. `requires: [spangap/spangap-core]`. Parallel to (not built atop) `spangap-web` — the LCD launcher is its own UI surface.
  Activator role: when reachable in the dep graph (and not suppressed by `--no-lcd`), the build folds every other straddle's `esp-idf/lcd/` slice into that straddle's firmware component and calls its `<prefix>LcdInit()` from the generated dispatcher.

- **Granular extras** — each its own straddle. `spangap/wg`, `spangap/upnp`, `spangap/duckdns`, `spangap/acme`, `spangap/ota`. Each declares the minimum it needs in `requires:` — all currently need `spangap/spangap-net`; none requires `spangap/spangap-web` directly. They may carry a `browser/` half and/or an `esp-idf/lcd/` slice for their config panels; those compile only when the matching activator is in the graph. `spangap/acme` is the one to watch here: its HTTP-01 challenge handler is passive code that only compiles when `spangap-web` is in the graph, so a `spangap-web`-less build silently runs DNS-01-only and will error at runtime if no TXT-capable provider is configured.

The dep graph stays **monotonic**: anything that needs HTTPS pulls `spangap-web` which pulls `spangap-net` which pulls `spangap-core`. Users put whichever extras they want into `requires:` and the resolver fills in below. No `profile:` field in `straddle.yaml`, no `tier:` field — "profiles" are just opinionated `requires:` lists in the install docs ("offline-only kit," "kit with networking," "kit with browser UI," "kit with on-device UI," "full kit"). Two build-time flags (`--no-web-ui`, `--no-lcd`) let any of those profiles strip back to a cmdline-only firmware without editing the manifest.

The fine-grained intra-straddle separability mentioned earlier (`log` degrades to serial `printf`, `cli` is ifdef-removable, `cron` is purely additive) remains real and exploitable *inside* each straddle via the Kconfig.md mechanism — but those are knobs *within* a straddle, not its presence/absence in the dep graph.

### Why not one platform straddle with internal toggles?

Pre-decomposition, the platform could have lived as a single `spangap/spangap` straddle with Kconfig toggles selecting the `core`/`net`/granular tiers from within. That model was rejected for three reasons:

1. **Independent versioning.** `spangap-core` and `spangap-web` evolve at different rates; tying them to a single repo's version forces lockstep where there's no semantic need for it.
2. **Pure-offline builds.** A LoRa-only sensor node shouldn't pull a multi-megabyte browser-UI source tree it has no intention of compiling. Separate straddles eliminate the dead-code-elimination question entirely.
3. **Third-party swap-out.** A custom DDNS provider becomes `someone/myddns` as a clean drop-in replacement for `spangap/duckdns`. With everything in one platform straddle, that swap would be a fork rather than a dep change.

The `<org>/<org>` repo (`spangap/spangap`) is repurposed accordingly: it's the **project home / installer**, holding the CLI shim, brew formula, install script, top-level docs, operator tools (`flasher`, `spangap-cli`, `reallyclean.sh`), and the build-container Dockerfile (`ghcr.io/spangap/build-env`). It is not a straddle and is not in the dep graph; the CLI consults it once at install time and never again.

## Multi-straddle apps: protocol layers + transports + board straddle

Apps of non-trivial scope decompose into *protocol straddles*, *transport straddles*, and an *app-board straddle*. Each is one repo carrying its firmware (`esp-idf/`, optionally with an `esp-idf/lcd/` slice for on-device UI) and its browser UI (`browser/`) at one version. The `browser/` half and the `esp-idf/lcd/` slice are passive: they compile into the build only when the corresponding *UI activator* is in the dep graph (see *UI activators* below).

Reticulum on spangap is the canonical example; the same shape applies to seccam, a future calendar app, or any third-party app of comparable scope:

- **Protocol straddle** — pure protocol logic, no opinion about how packets move. Has its own browser/LCD UI for protocol state.
  `reticulous/rns` (prefix: `rns`) — `esp-idf/`: cmdline `rnsd` core + the µReticulum fork; `browser/`: RNS state/management panels; `esp-idf/lcd/`: on-device RNS UI. `requires: [spangap/spangap-core]` only — no IP layer at the protocol level so a LoRa-only or ESPNOW-only build pulls no networking weight it doesn't need.

- **Transport straddles** — one per transport mechanism. Each plugs an interface into `rns` and contributes its own configuration UI on the browser/LCD activators. All four are in scope today; today's `reticulous/main/*` carries each of them in some form.
  `reticulous/iface-tcp` (prefix: `rns_tcp`) — RNS-over-TCP client + server interfaces. `requires: [reticulous/rns, spangap/spangap-net]`.
  `reticulous/iface-auto` (prefix: `rns_auto`) — UDP-multicast AutoInterface for LAN peer discovery. `requires: [reticulous/rns, spangap/spangap-net]`.
  `reticulous/iface-espnow` (prefix: `rns_espnow`) — ESPNOW peer-to-peer over the WiFi radio. Brings the radio up itself; no IP stack required. `requires: [reticulous/rns]`.
  `reticulous/iface-lora` (prefix: `rns_lora`) — SX1262 LoRa over SPI. `requires: [reticulous/rns]`.

- **Layered-protocol straddle** — one repo per layered protocol, riding RNS the way TCP/UDP ride IP.
  `reticulous/lxmf` (prefix: `lxmf`) — LXMF messaging: `esp-idf/` + `browser/` panels + `esp-idf/lcd/` slice. `requires: [reticulous/rns]`.

- **Buildable board straddle** — partition layout, board HAL, plus the `requires:` line that names which protocol/transport straddles to include and which UI activators to turn on. **This is the straddle that actually produces a flashable device image.**
  `reticulous/hw-tdeck` — `requires: [reticulous/rns, reticulous/lxmf, reticulous/iface-tcp, reticulous/iface-auto, reticulous/iface-espnow, reticulous/iface-lora, spangap/spangap-web, spangap/spangap-lcd]` gives a T-Deck with the full RNS + LXMF stack over all four transports plus both UI surfaces.

A board straddle picks whichever combination of protocol/transport straddles it wants and whichever UI activators apply; the resolver assembles them. A LoRa-only sensor board with no WiFi is simply a board straddle that requires `rns + iface-lora` and skips `spangap-net` entirely.

### UI activators: one dep, every straddle's UI

`spangap/spangap-web` and `spangap/spangap-lcd` play a special role in the dep graph: they're *activators* for browser-side and LCD-side UI compilation respectively. When `spangap/spangap-web` is reachable from the buildable straddle's `requires:` (and `--no-web-ui` is not set), the build picks up the `browser/` half of *every* straddle in the graph and wires it into the browser dispatcher. Same for `spangap/spangap-lcd` and the `esp-idf/lcd/` slice of every straddle's firmware component. Build-time flags suppress either or both:

```
spangap make                       # whatever requires: lists
spangap make --no-web-ui           # skip the browser build entirely
spangap make --no-lcd              # skip every esp-idf/lcd/ slice
spangap make --no-web-ui --no-lcd  # cmdline-only firmware (serial + remote CLI; no UI at all)
```

This is the deliberate inversion of an earlier sketch: rather than `reticulous/rns + reticulous/reticulous-web + reticulous/reticulous-lcd` (three repos per protocol layer, multiplied across every transport and the LXMF layer), one protocol/transport gets one straddle, and the activators turn UI compilation on across the whole graph. Triple-repo-per-feature was the failure mode the activator design exists to prevent — it defeated the one-name-one-version-one-repo argument that motivates straddles in the first place.

A consequence: a straddle that has *only* a `browser/` half (no firmware code at all) is unusual but supported — it ships UI panels that decorate other straddles' firmware. Its presence in `requires:` is itself enough to opt in; activation still requires `spangap/spangap-web`.

### Prefix vs github name

The github repo name is a brand identity (`reticulous/rns`); the in-code `prefix:` is what shows up in source as `prefixInit()`, `cliRegisterCmd("prefix-foo", …)`, and `s.prefix.*` storage keys. These should diverge when the brand name doesn't describe the symbols well.

`reticulous/rns`'s `prefix:` is **`rns`** — that's what the symbols actually are (`rnsInit`, the rnsd cmdline). `reticulous/lxmf` uses `prefix: lxmf`. The transport straddles use `rns_tcp`, `rns_auto`, `rns_espnow`, `rns_lora` — separable surfaces within the RNS family, distinct enough to need their own symbols but clearly part of one family in the prefix. Two straddles under the same brand can't both use the same prefix anyway (they'd collide on symbols and CLI commands), so the brand-vs-prefix split is forced as soon as an org has more than one straddle.

### Board straddles are special-case app straddles

The `hw-tdeck` pattern generalizes: a board straddle pairs a board's HAL (display, input, partition layout, GPIO wiring) with a curated set of feature straddles. Boards become pluggable in the same way apps are. The CLI doesn't need a separate "board" concept — board straddles are just buildable straddles whose primary content is the board HAL. A user wanting reticulous on a different board makes `someone/reticulous-heltec` with the same shape; the CLI doesn't care.

## Distribution: GitHub as the source

The unit of distribution is the github repo. A straddle's canonical identifier everywhere — manifests, CLI output, logs — is its `<org>/<repo>` path. No registry lookups are involved for straddle fetching.

Both ecosystems natively support fetching components by location:

- **ESP-IDF** components can be declared with `path:` or `git:` source in `idf_component.yml`. Local-path overrides win over the registry.
- **npm packages** sit in `node_modules/`; presence wins over registry resolution.

The spangap CLI clones the straddle's repo (at the manifest-pinned tag/branch/SHA), reads `straddle.yaml`, and stages each half into the build's native shape (see *Staging layout*). Neither registry is contacted for the straddle itself; both registries are contacted only for genuine third-party shared deps that the straddle's own `idf_component.yml` / `package.json` declares (e.g., cJSON, lwIP add-ons, lodash).

Versioning is git-native: a manifest pins a tag (`spangap/calendar@v0.3.0`), a branch (`@main`), or a commit SHA. Equivalent to `npm install github:...#tag` and `idf_component.yml`'s `git+version`.

### The defensive scope umbrella

The unavoidable problem with GitHub-as-source is that the staged packages still have `package.json`/`idf_component.yml` `name` fields, and any tooling that ever hits a registry for those names is exposed to dependency confusion / namespace squatting (the Birsan 2021 class of attack). Solution: claim a single `spangap` scope on each registry, and adopt one rule — **official spangap publishes never contain `__` in the name**. That makes the same scope serve as both publish home and defensive umbrella:

| Registry | Scope/namespace | Purpose |
|---|---|---|
| npm | `@spangap` | Real publishable first-party things (`@spangap/cli`, `@spangap/browser` if/when published standalone). Doubles as the defensive umbrella: owning the scope blocks all squatters under it, and the "no `__` in official names" rule keeps encoded staged names (`@spangap/<org>__<repo>`) cleanly distinguishable from real ones. |
| npm | `spangap` (unscoped) | Reserved so `npm install -g spangap` is the CLI install path and can't be squatted |
| ESP-IDF registry | `spangap` | Real publishable first-party components. Doubles as the defensive umbrella the same way. |

A staged calendar straddle's `package.json` `name` is `@spangap/spangap__calendar`. The `__` separator inside the package name is structurally unusual — it makes the name read as "this is a generated artifact, not a hand-install target," even to a human glancing manually. Because the `@spangap` scope is owned and no official publish ever uses `__`, any *accidental* registry lookup for `@spangap/anything__else` returns 404 — fails *safely* instead of installing whatever an attacker has put there.

Third-party straddle authors do **not** need to claim their own scope. Their github org name appears *inside* a package name (`@spangap/joe__coolapp`), under spangap's scope, not as a separate npm scope. One scope claim by spangap protects the entire ecosystem — including straddles that don't exist yet.

The scope is an operational responsibility: the account that owns `@spangap` / `spangap` becomes a security-critical asset (strong 2FA, hardware-backed if possible, no third-party tokens). Compromise of that account = ability to publish under the scope = able to inject things the CLI doesn't expect. The threat surface is small (the CLI never asks npm for `@spangap/*__*` names; injected packages mostly sit harmless), but the scope is only as trustworthy as its keys.

### Staging layout

After `spangap make`:

```
<project-dir>/
  staging/
    components/
      spangap__spangap-core/      ← from github.com/spangap/spangap (platform)
      spangap__calendar/          ← from github.com/spangap/calendar
      joe__coolapp/               ← third-party
      …
    browser/
      (Vite-resolved via aliases; see below)
    generated/
      straddle_dispatch.cpp       ← flat list of `xInit()` calls in dep order
      straddle_dispatch.ts        ← browser-side equivalent
```

Each firmware component sits as a normal ESP-IDF component dir; the ESP-IDF build picks them up via the standard `EXTRA_COMPONENT_DIRS` mechanism (or by living under `staging/components/` which is already on the search path). Filename collisions across straddles are structurally impossible because each component is in its own directory.

### Include paths and aliases

Source-code identity is decoupled from distribution identity. Authors lay out their headers under their declared `prefix:`:

```
esp-idf/include/
  calendar/
    calendar.h
```

ESP-IDF mounts `include/` on the include path, so consumers write the short form:

```cpp
#include "calendar/calendar.h"
```

The org-prefixed form (`spangap/calendar/…`) and the encoded form (`spangap__calendar/…`) **never appear in source**. The encoded form exists only as a directory name in `staging/components/`, as a `name:` field in machine-generated `idf_component.yml` entries, and in any `package.json` `name:` field for the browser half. None of those are read by human code. The org belongs in `straddle.yaml` (in `name:` and consumers' `requires:` lines), nowhere else — so a fork from `spangap/calendar` to `me/calendar` requires zero source edits.

For the browser half, the same decoupling holds via Vite alias config. The CLI writes:

```ts
// vite.config.ts (generated)
resolve.alias = {
  'calendar': '/abs/path/to/staging/browser/spangap__calendar/src',
  // ...
}
```

So browser-half source code imports the short form:

```ts
import { CalendarPanel } from "calendar"
```

— never `@spangap/spangap__calendar`. The encoded npm name is purely a 404-safe leak prevention.

### Symbol and filename collision handling

The straddle model deliberately avoids the "flatten everything into one staging blob" mental model. Instead, both ecosystems' native packaging is the isolation mechanism:

- **Filenames** — each straddle is its own ESP-IDF component directory and its own npm package directory. Two straddles can both have `helpers.h` because they live in different `include/` trees. Cross-includes always include via the chosen subdir (`#include "calendar/helpers.h"`), never bare basename.
- **Public symbols** — each straddle's public API uses the `<prefix>` naming convention (`calendarShowEvent`, `calendarInit`), same as today's spangap-core (`storageGet`, `webInit`). For something larger than the spangap ecosystem, the convention evolves to `<org>_<prefix>_…` — but for today's scale, prefix-by-convention is enough.
- **Private internals** — `static` functions / anonymous-namespace types are translation-unit-local by definition; they can't collide.
- **Cross-straddle symbol surface is tiny by design.** Straddles don't `#include` each other's headers and don't call each other's symbols directly except through declared deps. The hook pattern uses function-pointer registration, so the only globally-visible symbols per straddle are its `<prefix>Init` plus whatever it explicitly exposes via its public header.
- **Linker as safety net** — accidental global-symbol leaks across straddles surface as `multiple definition of foo` link-time errors pointing at exact files. That's a fine DX — loud build error, not silent corruption.

For *vendored* third-party C code (where two straddles independently vendor the same library and produce identical symbols), the manifest rule is "shared C deps go through declared ESP-IDF components, not vendored copies." A CI linter that flags duplicate vendored libs is cheap insurance and fits naturally into `spangap make`'s pre-build phase.

## The CLI

The CLI is delivered as an npm package — the lowest-friction global-CLI channel that's already in the stack via Quasar. It is *not* where spangap lives; it's a thin orchestrator that knows the compatibility matrix (which platform version pairs with which ESP-IDF version, which straddle versions, etc.) and drives the existing dual build systems.

```
npm install -g spangap
# (or: npm install -g @spangap/cli — see umbrella table above)
```

Core commands:

- **`spangap init`** — scaffold the current dir as a buildable straddle. Writes a starter `straddle.yaml` with the default profile (`net` + the platform), creates the layout (`esp-idf/`, `browser/`, `straddle.yaml`), pins versions.
- **`spangap make`** — resolve the straddle dep graph from the manifest, fetch missing straddles from github (clone or tarball, cached), topo-sort, stage into `staging/`, emit `straddle_dispatch.cpp` and `straddle_dispatch.ts`, drive `quasar build` for the browser (only if `spangap/spangap-web` is reachable and `--no-web-ui` isn't set), then `idf.py build` for the firmware (folding each straddle's `esp-idf/lcd/` slice into its component when `spangap/spangap-lcd` is reachable and `--no-lcd` isn't set).
  Flags: `--no-web-ui` skips the browser build (and the browser dispatcher) entirely. `--no-lcd` skips every `esp-idf/lcd/` slice even when `spangap/spangap-lcd` is in `requires:`. Both together yield a cmdline-only firmware — remote CLI works, no UI surfaces are compiled. Override semantics only; the manifest stays untouched.
- **`spangap flash <port>`** — flash from the local environment over USB.
- **`spangap flash -d <port>`** — "device is on the *host*, build was elsewhere." Used when the build ran inside a container/VM and the USB serial is on the host (the common Docker-on-Mac case; matches today's flasher daemon split).
- **`spangap doctor`** — preflight: check for ESP-IDF (correct version), npm/Node, Docker (if using container builds). Guide through Espressif's installer where missing.

The CLI is the *only* tool a user has to know about. Beneath it, `idf.py`, `quasar`, `esptool`, `docker` are present but driven by the CLI, not by hand. A user who knows none of those should still be able to `spangap init && spangap make && spangap flash`.

## Onboarding: Docker as the easy path

The most demanding part of the spangap first-run experience is the **ESP-IDF toolchain bootstrap** — gigabytes, Espressif's installer, a Python env, toolchains. `npm install -g spangap` is two seconds; ESP-IDF setup can be 20+ minutes and is the #1 spot beginners bounce. The CLI can guide it via `spangap doctor`, but it can't make it instant.

The strongest version of "even if you know little, you succeed" is: **the user does not install ESP-IDF at all.** The build runs inside a Docker container provided by spangap; the host only needs Docker and a USB serial port.

This matches the existing builder-VM pattern. Docker Desktop on Mac runs the container inside a small Linux VM (a real Linux kernel virtualized via Apple's Virtualization.framework — not instruction-level emulation), and on Apple Silicon the VM is ARM64 native, so a firmware compile runs at near-native speed. Docker Desktop does *not* support USB passthrough on Mac — but that's exactly the architecture: the container builds, the host flashes via `spangap flash -d`. The biggest Docker-on-Mac limitation is the reason the design is shaped this way, not a problem to solve.

Onboarding flow target:

1. `npm install -g spangap`
2. `spangap doctor` — ensures Docker Desktop is installed (one Mac app), pulls the spangap build container.
3. `spangap init` in an empty dir — writes a starter buildable straddle.
4. `spangap make` — runs the build inside the container.
5. `spangap flash -d /dev/tty.usbmodem…` — flashes from the host.

The user never types `idf.py`, `docker`, `quasar`, or `esptool`. They never install ESP-IDF.

Expert/advanced users who want a local ESP-IDF install retain that option; `spangap make` without `--docker` runs the build directly. Same for OrbStack as a lighter Docker Desktop alternative on Mac. Defaults aim at beginners; escape ramps preserved.

## Drawbacks (acknowledged, accepted)

- **GitHub-as-registry binds identity to location.** If `spangap/calendar` moves to `someoneelse/calendar`, every manifest referencing it breaks. Real registries decouple (the name stays even if source moves). For a small/closed ecosystem this is rare in practice; the lightest mitigation if needed later is a tiny redirect file (`name → current git URL`) hosted on GitHub Pages, no server. Defer until the URL-rot problem materializes.
- **Defensive umbrella is an operational responsibility.** The `@spangap` account credentials are security-critical (see above). Adds an ongoing care commitment, not just a one-time setup.
- **Curated profiles can lag.** Three named profiles (`core`, `net`, `full`) are easy to test; a combinatorial pick-list isn't. Authors who genuinely need a non-profile combination will work harder. Trade accepted — the kit experience comes first.
- **Container builds add Docker as a prerequisite.** For users who balk at Docker, the local ESP-IDF install path remains. But documentation will heavily push the container path because it's the one that works first-time.
- **Encoded staged names look ugly.** `@spangap/spangap__calendar` is not pretty. It is, however, never seen in code; only in tooling output / generated files. Accept the aesthetic cost as the price of one-claim ecosystem protection.

## Relationship to [`Kconfig.md`](Kconfig.md)

Kconfig.md governs how spangap-core's *own* modules toggle on/off per consumer and how `spangapInit()` brings them up. That layer doesn't go away — it stays exactly as that plan describes for the platform's intrinsic modules (`storage`, `log`, `cli`, `web`, `auth`, …).

This plan adds a second orchestration layer *on top* of that:

```
app_main()                            ← unchanged shape from Kconfig.md
  spangapInit()                       ← platform modules, in fixed platform-curated order
  spangapRunStraddles()               ← generated; calls each straddle's <prefix>Init() in dep order
  cliRunFile("/state/boot")           ← unchanged
```

Concretely:

- Kconfig.md's `CONFIG_SPANGAP_*` toggles continue to govern intra-straddle modules (inside `spangap-core`, `spangap-web`, etc.).
- A straddle's `requires` list resolves to ESP-IDF components staged under `staging/components/`, which the `idf.py build` picks up via the existing managed-components mechanism.
- A straddle's firmware code can `#include "spangap-core/spangap-core.h"` (or `spangap-web/spangap-web.h`) to access platform APIs — same as a consumer's own `main/` code today does via `spangap.h`.
- `spangap/spangap-core` pins the matching ESP-IDF version in its `straddle.yaml`. `spangap/spangap-web` pins to a compatible `spangap-core@vN`. Each granular extra pins to a compatible `spangap-web@vN`. The version-compatibility chain is resolved by `requires:` graph walk, not by a single platform straddle holding all the pins.

The two plans together describe the full picture: Kconfig.md = the platform's intrinsic modularity; this plan = the ecosystem layer that lets external features plug in.

## Summary of moving parts

| Layer | What | Where |
|---|---|---|
| Identity | `<org>/<repo>` on GitHub | The straddle's repo URL |
| Manifest | `name`, `version`, `requires`, `platforms`, `app` | `straddle.yaml` at straddle repo root |
| Firmware half | ESP-IDF component | `esp-idf/` (or repo-specific override) |
| Browser half | Vue/Quasar package | `browser/` (or repo-specific override) |
| LCD slice (firmware sub-dir) | LVGL programs + settings panes | `esp-idf/lcd/` (or repo-specific override) |
| Init function (firmware) | `void <prefix>Init()` | Straddle's firmware source |
| Init function (browser) | `export function init()` from package entry | `browser/src/index.ts` |
| Init function (LCD slice) | `void <prefix>LcdInit()` | Straddle's `esp-idf/lcd/` source, called from firmware dispatcher when `spangap/spangap-lcd` is in graph |
| Hook subscription | `spangapOn(Hook::*, &cb)` (firmware) / Pinia + storage subscriptions (browser) | Inside `init()` |
| Static ordering | `requires:` declared deps, topo-sorted | Manifest + CLI resolver |
| Dynamic readiness | NOW_AND_ON_CHANGE on ephemeral storage key | Existing storage subscriptions |
| Tier selection | One of `core` / `net` / granular profile | Buildable straddle manifest |
| UI activators | `spangap/spangap-web` (browser/), `spangap/spangap-lcd` (lcd/) | Named in `requires:`; suppressed at build with `--no-web-ui` / `--no-lcd` |
| Staging dir | `staging/components/<org>__<repo>/` + Vite aliases | Per-project build artifact |
| Generated dispatcher | Flat `<prefix>Init()` (+ optional `<prefix>LcdInit()`) calls in topo order | `staging/generated/straddle_dispatch.cpp/.ts` |
| Defensive umbrella | `@spangap` (npm) / `spangap` (ESP-IDF) | Same scope as real publishes; the "no `__` in official names" rule makes encoded staged names safely 404 |
| Encoded staged name | `@spangap/<org>__<repo>` | Generated; not in source code |
| Include path in source | `#include "<prefix>/<name>.h"` | Author's chosen header layout under `include/<prefix>/` |
| Browser import path | `import { X } from "<prefix>"` via Vite alias | Vite alias generated by CLI |
| CLI front door | `npm install -g spangap` (or `@spangap/cli`) | Distributed via npm |
| Build orchestration | `spangap init / make / flash [-d]` | The CLI |
| Container build | Docker Desktop (or OrbStack) on host | Standard Docker, no custom infra |

## Open questions for implementation pass

1. ~~**Subdir name: `idf/` vs `esp-idf/` vs `esp32/`.**~~ Resolved — `esp-idf/`. Names the build system / toolchain, not the chip family; longest but the most accurate.
2. **The exact hook set.** `BootComplete`, `NetUp/Down`, `Shutdown` is a starting list. Grown by demand. Decide each addition deliberately — the hook taxonomy is the long-term API contract.
3. **Macro sugar for init.** `SPANGAP_INIT_START(prefix) / END` was sketched, judged optional. Author writing `void calendarInit() { ... }` directly matches spangap-core convention and needs no sugar. Probably skip the macro entirely; revisit only if real boilerplate emerges.
4. **`straddle.yaml` schema and validator.** Needs a real schema (likely just a JSON Schema) so the CLI can give clear errors on malformed manifests. Pin schema version in the manifest itself for future evolution.
5. **Documented preset `requires:` lists.** With profiles being docs-only (opinionated `requires:` lines for "offline-only," "with browser UI," "full kit"), the question is just which presets to ship in the getting-started page. Probably three is right; resist adding more until a real combination earns a name through repeated use.
6. ~~**The platform decomposition sequencing.**~~ Resolved — see *Plan for execution*. CLI + container land first (Phase 1) against today's tree wrapped as one seed straddle, validated against a throwaway "hello world" feature straddle to exercise the dep graph. The real platform split (Phase 2) follows once the mechanism is proven on real hardware.
7. **Container image curation.** The Docker image is part of the trust chain — what's in it, who builds it, how it's pinned to platform-straddle versions. Probably `ghcr.io/spangap/build-env:<platform-version>`, built from a Dockerfile in the platform-straddle repo, signed.
8. **CLI<->container handoff for `flash -d`.** The container produces the binary at a known path; the host CLI picks it up. Sharing the staging dir via bind-mount is the simplest mechanism; need to confirm Vite's performance on bind-mounted source on Mac is acceptable (was historically the slowest part of Docker Desktop).
9. **Browser-side runtime safety check** (parallel to Kconfig.md §"Optional safety net"). A staged browser package checking on first connect that its expected spangap version actually matches the device's reported version — cheap belt-and-suspenders against staging-vs-flashed-firmware drift.
10. **Third-party CI linter.** Out of scope for v1 but worth designing: a github action that any straddle author can drop into their repo to validate manifest, naming conventions, no-duplicate-vendored-libs, etc.

## What today's tree becomes

Paths on the left reflect the post-Phase-1 workspace (after the `spangap/` → `spangap-core/` rename and the inner `spangap-core/` → `esp-idf/` rename).

| Today (post-Phase-1 rename) | Future repo |
|---|---|
| `spangap-core/esp-idf/src/{cli,cli_cmd_fs,cli_cmd_sys,storage,log,fs,its,cron,pm,spi_helper,compat}.cpp` + paired headers | `spangap/spangap-core` |
| `spangap-core/esp-idf/src/{net,tls,ntp,spangap_mdns}.cpp` + paired headers | `spangap/spangap-net` |
| `spangap-core/esp-idf/src/{web,auth,webrtc_task,webrtc_sctp}.cpp` + paired headers | `spangap/spangap-web` (firmware half, `esp-idf/`) |
| `spangap-core/browser/src/{components,lib,stores,modules,panels}/*` (shared UI shell) | `spangap/spangap-web` (browser half, `browser/`) |
| `spangap-core/esp-idf/src/lcd_ui/*` + `include/lcd.h` + `include/lcd_board.h` | `spangap/spangap-lcd` |
| `spangap-core/esp-idf/src/{wg,upnp,duckdns,acme,ota}.cpp` + paired headers | `spangap/wg`, `spangap/upnp`, `spangap/duckdns`, `spangap/acme`, `spangap/ota` (one repo each; each carries its `browser/` settings panel inline) |
| `spangap-core/scripts/{flasher,spangap-cli,reallyclean.sh}` | `spangap/spangap` (alongside CLI shim, brew formula, install script, build-container Dockerfile) |
| `reticulous/main/{rnsd,ports}.*` + `reticulous/components/{microreticulum,bzip2}/*` | `reticulous/rns`'s `esp-idf/` (prefix: `rns`) |
| `reticulous/main/{tcp}.*` | `reticulous/iface-tcp`'s `esp-idf/` (prefix: `rns_tcp`) |
| `reticulous/main/{auto}.*` | `reticulous/iface-auto`'s `esp-idf/` (prefix: `rns_auto`) |
| `reticulous/main/{espnow}.*` | `reticulous/iface-espnow`'s `esp-idf/` (prefix: `rns_espnow`) |
| `reticulous/main/{lora,esp_idf_hal}.*` | `reticulous/iface-lora`'s `esp-idf/` (prefix: `rns_lora`) |
| `reticulous/main/{lxmf,lxmf_lcd}.*` | `reticulous/lxmf`'s `esp-idf/` (prefix: `lxmf`) |
| `reticulous/main/{nomad,nomad_lcd}.*` | `reticulous/nomad`'s `esp-idf/` (prefix: `nomad`) |
| `reticulous/web-interface/src/*` RNS state/management panels | `reticulous/rns`'s `browser/` |
| `reticulous/web-interface/src/*` per-transport config panels | each transport straddle's `browser/` (split per transport) |
| `reticulous/web-interface/src/*` LXMF-management panels | `reticulous/lxmf`'s `browser/` |
| `reticulous/web-interface/src/*` Nomad browser/announces panels | `reticulous/nomad`'s `browser/` |
| reticulous on-device LCD UI for RNS state | `reticulous/rns`'s `esp-idf/lcd/` |
| reticulous on-device LCD UI per transport | each transport straddle's `esp-idf/lcd/` |
| reticulous on-device LCD UI for LXMF | `reticulous/lxmf`'s `esp-idf/lcd/` |
| reticulous on-device LCD UI for Nomad | `reticulous/nomad`'s `esp-idf/lcd/` |
| `reticulous/main/{tdeck,gps,main}.*` + `partitions.csv` + `ota_pubkey.h` + `sdkconfig.defaults` + top-level `CMakeLists.txt` | `reticulous/hw-tdeck` (the buildable straddle that builds the T-Deck device image) |
| `reticulous/main/{maps}.*` + `scripts/maketiles.py` | `reticulous/maps` (feature straddle, no device image of its own — consumed by buildable straddles like `hw-tdeck`. Independent of the RNS family; `requires: [spangap/spangap-core, spangap/spangap-lcd]`. Reads GPS via ephemeral storage; GPS service abstraction deferred until a second consumer.) |
| `seccam/*` | `seccam/seccam` (all halves in one repo; further decomposition into sub-protocols can come later if the codebase grows that way) |

The spangap side splits cleanly because the code is already module-bounded — each `<mod>.cpp` + `<mod>.h` pair moves as a unit. The reticulous side isn't sliced this way today; expect real code movement, especially separating each transport into its own straddle, lifting the on-device LCD UI and browser panels out of the protocol-core code, and pulling the T-Deck-specific board HAL into its own straddle.

## Plan for execution (deferred)

Phases cluster steps that share a goal; **do not start a phase until the previous one is end-to-end verified on real hardware.**

Versions are an afterthought at this stage. There's one user (the author), no shipped consumers, and the eventual public release will likely be a squashed initial commit anyway. Straddles carry a `version:` field because the schema requires it; pick whatever feels right and bump when something breaks. Breaking changes are free.

### Phases 1, 2, and Phase-3 step 10 — done (2026-05-27)

Phase 1 (mechanism validation): `straddle.yaml` schema, the `spangap/` installer repo (host shim, in-container CLI, build-env Dockerfile), the `spangap-build-env:dev` image, the workspace renames (`spangap/`→`spangap-core/` outer, `spangap-core/`→`esp-idf/` inner), the `reticulous/reticulous` ur-straddle manifest, and end-to-end verification on the T-Deck — all landed. The only remaining Phase-1 step is **claiming the public names** (`@spangap` and unscoped `spangap` on npm; `spangap` on components.espressif.com), which is user-executed and deferred until Phase 4.

Phase 2 (real platform split): `spangap-core` narrowed to its foundation (`its`, `fs`, `spi_helper`, `storage`, `log`, `cli`, `cron`, `pm`, `compat`), eight new sibling straddles broken out — `spangap-net`, `spangap-web`, `spangap-lcd`, `wg`, `upnp`, `duckdns`, `acme`, `ota` — each with its own `straddle.yaml`, `esp-idf/idf_component.yml`, and `esp-idf/CMakeLists.txt`. Cross-straddle wiring built out beyond the spec: short, fork-friendly component names via `<consumer>/staging/components/<short>/` symlinks (the spangap CLI populates them pre-build; the consumer's project CMakeLists picks them up via `EXTRA_COMPONENT_DIRS`). The `spangap_init.cpp` reverse-dependency was rewritten — `spangapInit()` is now core-only, sibling `xInit()` calls live in the consumer's `app_main`, and `spangap.h` no longer pulls in sibling headers — so spangap-core no longer compile- or link-knows that any sibling exists. Build verified end-to-end on the (still-unsplit) reticulous buildable straddle.

### Phase 3 — Real apps

**First order of business: decompose `reticulous`.** It's the only straddle whose code is bigger than its split lines, and the post-Phase-2 platform stops carrying the layering workarounds the moment its real consumer is in shape.

10. **Decompose `reticulous` into the multi-straddle-app pattern.** ✓ Done (2026-05-27). Real code movement, not just file relocation — separating each transport into its own straddle, splitting LXMF, Nomad, and the offline-maps app out of the protocol-core code, lifting on-device LCD UI and browser panels into each straddle's `esp-idf/lcd/` slice and `browser/` half, and pulling the T-Deck-specific board HAL into its own buildable straddle. No separate `-web` or `-lcd` repos per protocol or transport: the activator pattern compiles each one's UI on demand.

    Notes from the landing:
    - `MapWindow.vue` ended up in `rns`, not `maps`. Despite the name it's an RNS path-table view, not a map render; classifying it with `maps` would have inverted the dep direction (`maps` doesn't `require:` rnsd state). `maps` ships no browser half today — its only UI surface is the on-device LCD launcher.
    - `AnnouncesWindow.vue` lives in `lxmf`, not `rns`. It's tightly coupled to the `AnnouncesView` chat component, the `useLxmf()` composable, and the shared `messagesVisible` ref (clicking an announce opens that peer's thread). Putting it in `rns` would have forced core to depend on lxmf — wrong direction.
    - `iface-espnow` `requires: spangap-net` despite the spec saying "Brings the radio up itself; no IP stack required" — today's code still pulls `net.h` for `netIsUp()` and `NET_EV_UP/DOWN` subscription. Defer the radio-ownership rework until a real driver lands.
    - `iface-lora` does the same `PRIVATE` include trick `ota` uses — `target_include_directories(${COMPONENT_LIB} PRIVATE "${CMAKE_SOURCE_DIR}/main")` — so `lora.cpp` can `#include "tdeck.h"` for the `BOARD_LORA_*` pin map without a circular dep on `hw-tdeck`. A future "board HAL service" abstraction would lift this; not yet motivated.
    - LCD slices live at `esp-idf/lcd/src/<x>_lcd.cpp` (per the spec's layout) but the CLI doesn't yet implement activator-driven source-list exclusion; until then the slice's `.cpp` body stays wrapped in `#if CONFIG_SPANGAP_LCD` and the consumer's `app_main` keeps the same `#if` guard around `xLcdRegister()` calls. The slice's public `xLcdRegister()` declaration moved into each straddle's main header so `main.cpp` no longer carries forward `extern "C"` declarations.
    - `rns` vendors `microreticulum/` + `bzip2/` as **nested sub-components** under its `esp-idf/components/`. EXTRA_COMPONENT_DIRS doesn't recurse, so the consumer's top-level `CMakeLists.txt` adds each `staging/components/*/components/` to the search path explicitly (a one-line `file(GLOB)` per buildable straddle). This is the only structural deviation from one-component-per-straddle in the workspace today.
    - Cross-straddle browser imports use bare-specifier package names (`import { useWinZoom } from 'rns/lib/winZoom'`). The CLI doesn't yet write Vite aliases for these — they resolve via npm `file:` deps in `hw-tdeck/web-interface/package.json`, mirroring the existing `spangap-browser` symlink pattern.
    - The third-party managed-component deps that used to live in `reticulous/main/idf_component.yml` (`jgromes/radiolib`, `lvgl/lvgl`, `espressif/esp_lcd_touch_gt911`, `espressif/esp_jpeg`) all migrated to `hw-tdeck/esp-idf/main/idf_component.yml` rather than to the feature straddles that actually use them. The IDF component manager doesn't walk the deps of locally-staged components, so the buildable straddle has to surface every transitive third-party managed dep.

    **Spec for reference (matches what landed):**

    **Feature straddles (firmware-only, no device image):**
    - `reticulous/rns` (`prefix: rns`) — rnsd core + the µReticulum fork + vendored bzip2. `requires: [spangap/spangap-core]`.
    - `reticulous/iface-tcp` (`prefix: rns_tcp`) — TCP transport. `requires: [reticulous/rns, spangap/spangap-net]`.
    - `reticulous/iface-auto` (`prefix: rns_auto`) — UDP-multicast AutoInterface (LAN peer discovery). `requires: [reticulous/rns, spangap/spangap-net]`.
    - `reticulous/iface-espnow` (`prefix: rns_espnow`) — ESPNOW transport; brings the WiFi radio up itself. `requires: [reticulous/rns]`.
    - `reticulous/iface-lora` (`prefix: rns_lora`) — SX1262 LoRa, carries the RadioLib ESP-IDF HAL (`esp_idf_hal.cpp/h`). `requires: [reticulous/rns]`.
    - `reticulous/lxmf` (`prefix: lxmf`) — LXMF messaging + its on-device LCD UI. `requires: [reticulous/rns]`.
    - `reticulous/nomad` (`prefix: nomad`) — Nomad Network page client + its on-device LCD UI. Sits on rnsd's byte-array API directly (not on LXMF). `requires: [reticulous/rns]`.
    - `reticulous/maps` (`prefix: maps`) — offline RGB565 map viewer as an LCD launcher program. No RNS dep — published under the `reticulous/` org but consumable by any LCD buildable straddle. Reads the GPS fix via ephemeral storage keys, so no compile-time GPS dep — the GNSS chip lives in `hw-tdeck` until a GPS service abstraction earns its own straddle. `requires: [spangap/spangap-core, spangap/spangap-lcd]`. Doubles as the docs' "first non-RNS feature straddle" walkthrough.

    **Buildable straddle (builds the device image):**
    - `reticulous/hw-tdeck` — owns the T-Deck Plus board HAL (`tdeck.cpp/h`), the GNSS receiver task (`gps.cpp/h` — board-specific until a GPS service abstraction earns its own straddle), partition layout, OTA pubkey, browser SPA shell, and `app_main` (which drives the explicit per-straddle `init()` sequence — see Phase 2 layering rework). `requires: [reticulous/rns, reticulous/iface-tcp, reticulous/iface-auto, reticulous/iface-espnow, reticulous/iface-lora, reticulous/lxmf, reticulous/nomad, reticulous/maps, spangap/spangap-core, spangap/spangap-net, spangap/spangap-web, spangap/spangap-lcd, spangap/wg, spangap/upnp, spangap/duckdns, spangap/acme, spangap/ota]`.
11. **Convert `seccam` to its straddle shape** — `seccam/seccam` initially. Further decomposition into protocol/web/lcd parallels can wait until the codebase grows into it.

### Phase 4 — Public

13. **Write user-facing docs.** Kit-experience getting-started: install Docker, `brew install spangap`, first device flashed, first source edit producing live HMR. Update [`../../CLAUDE.md`](../../CLAUDE.md) to describe the new architecture once it's settled.
14. **Announce.** Whatever channels make sense.

### Deferred indefinitely

15. **Runtime version-match safety check** — once a real version-skew incident motivates it.
16. **Third-party CI linter / GitHub Action** — when third parties are actually shipping straddles.

## Bootstrap, workspaces, and the inner loop

This section refines the CLI mechanics sketched under *The CLI* and *Onboarding* with the operational shape worked out during planning: how the CLI is installed, how its container is structured to support fast iteration, how a user's project relates to a container's bind-mount root, and how the inner loop (browser-half edit → HMR; firmware edit → ninja rebuild) actually happens without the container becoming an obstacle.

### The install fork problem

`npm install -g spangap` is frictionless only when Node is already there. For spangap's target user — kit buyer, hardware hobbyist, someone who's never built ESP-IDF — Node is itself a fork in the road (official installer / brew / nvm / fnm / asdf / volta), and the npm-global install path on macOS hits `EACCES` on `/usr/local/lib/node_modules` for first-timers. Linux distros ship ancient Node by default; `apt install nodejs` produces something that breaks `npm install -g`. Windows has working npm-global but you want a real installer there anyway. So the question isn't "npm or brew?" — it's how to make install one step regardless of what the user already has.

### CLI shape: a Docker-launching shim

The host install is a small shim — 30-ish lines of shell/PowerShell or a single-file Go binary — that does roughly:

```bash
exec docker exec -i spangap-<workspace-hash>-<image-tag> spangap-internal "$@"
# (auto-starts the container if it isn't running)
```

The "real" CLI — manifest parser, resolver, dispatcher generator, build orchestration — lives inside the container image. The host side is just a launcher. Distribution channels (brew formula, scoop manifest, `curl | sh`, unscoped npm `spangap`) all ship the same shim. Whichever the user reaches for, they get the same artifact.

Why not write the CLI in Node and skip the container? Because Node is itself the prerequisite users don't have, and the container is needed for the build anyway. Embedding the CLI inside the container collapses the host prerequisites to exactly one: Docker.

### The container is a daemon, not a one-shot

`docker run --rm` per invocation costs ~500-1000 ms cold on macOS and re-runs ESP-IDF's `idf.py export` (~1 s) every time. That's a non-starter for ninja-style iteration. The shim instead uses a long-running container, devcontainer-style:

- First command in a workspace: `docker run -d --name spangap-<hash>-<tag> …` starts a detached container.
- Every command after: `docker exec` (~50-100 ms).
- `spangap down` stops it. A stopped container costs nothing, so idle-stop isn't needed; leave it up.

Container name = `spangap-<hash(workspace-dir)>-<image-tag>`. Same workspace, different projects → same warm container. Different workspaces → different containers, isolated. Image bump → fresh sibling container, named-volume caches still match their tag. Image-hash check at the start of every invocation: if the running container's image is stale relative to what the workspace's pinned platform-straddle version resolves to, stop-and-recreate. Crash recovery is `docker kill && docker run -d …`; build state on the bind-mount survives.

### Where the build state lives

Two persistence tiers:

1. **Project tree and build outputs** — bind-mounted from host. The container sees `/work/…`; the host filesystem holds the bytes. `build/`, `node_modules/`, `staging/`, ninja's `build.ninja` deps DB all live here. ninja's incrementality works across container recreation because its state files are real files on the bind-mount; container lifetime doesn't touch them.

2. **Build-tool caches** — `ccache`, IDF managed-components cache, npm/Vite cache. These live in named Docker volumes mounted at fixed container paths. They survive image upgrade and container recreation.

The container is cattle; the workspace and caches are pets.

One ninja gotcha: bind-mount paths must be stable per workspace. If `build.ninja` was generated with `/work/calendar/…` inside the container and a later invocation runs ninja against `/Users/rop/code/calendar/…` (e.g., user dropped to `--no-docker`), ninja sees a different graph and rebuilds the world. Rule: once a workspace is built in Docker mode, it stays in Docker mode. The workspace marker records the choice; mixing is refused with a clear error.

### Project vs workspace

Two concepts, deliberately separate:

- **Project** = the thing being built. Found by walking up from CWD until a `straddle.yaml` appears.
- **Workspace** = the bind-mount root, what the container sees. Declared via a `spangap.workspace.yaml` marker file; never guessed from sibling layouts.

Resolution on every invocation:

1. Walk up from CWD to find `straddle.yaml` → project `P`.
2. Walk up from CWD (or from `P`) looking for `spangap.workspace.yaml`.
3. If found and it's an ancestor of `P` → workspace = that dir.
4. Otherwise → workspace = `P`'s own dir.
5. Container = `spangap-<hash(workspace)>-<image-tag>`.

This makes the single-clone case zero-config:

```
git clone https://github.com/spangap/calendar
cd calendar
spangap make            # workspace = ./calendar; just works
```

…while letting a user opt into a multi-straddle workspace by dropping a marker:

```
~/code/
  spangap.workspace.yaml      # `spangap workspace init` created this
  spangap/                    # clone of github.com/spangap/spangap
  calendar/                   # clone of github.com/spangap/calendar
  coolapp/                    # clone of github.com/joe/coolapp
  my-app/
    straddle.yaml
```

From `~/code/my-app/`, `spangap make` finds the marker upward, bind-mounts `~/code/`, and uses sibling straddles as local overrides automatically. The marker can be empty as a pure sentinel, or carry optional knobs:

```yaml
name: my-stuff               # cosmetic, shown in the signaling line
workspaces: [libs/*]         # globs for deeper layouts; default is immediate children
default_project: my-app      # for `spangap make` invoked at the workspace root
```

`SPANGAP_WORKSPACE=/path/to/dir` is the ad-hoc shell/CI override for cases where dropping a file is awkward.

### Local layout: short names only

The user's workspace contains plain short directory names — `calendar/`, `coolapp/` — matching what `git clone` produces by default. The full org/repo identity (`spangap/calendar`, `joe/coolapp`) lives only in each straddle's own `name:` field and in consuming straddles' `requires:` lines. The encoded staged form (`spangap__calendar/`) appears only inside `staging/components/`, never in the user's workspace.

The resolver matches on the manifest's `name:` field, not on directory name. Consequences:

- **Forking requires no source or layout change.** Clone the fork, edit consumers' `requires:` to point at it, done. Source includes (`#include "calendar/calendar.h"`), the on-disk dir name (`calendar/`), and the short prefix all stay the same across forks. The org appears in exactly one place per consumer — the `requires:` line — and nowhere in the forked straddle's own contents.
- **Same-prefix collision detection happens at workspace scan time.** Two locally-present straddles declaring `prefix: calendar` produce a clear error with both paths shown, rather than a confusing link failure later. (Two same-prefix straddles can never coexist in one build anyway — their public symbols and CLI/menu registrations would collide — so catching it early is just turning a runtime crash into a build-time message.)

In short: source uses the **short identity** (`#include "calendar/calendar.h"`, `import { X } from "calendar"`); the org-prefixed form survives only as a staging-dir name (`staging/components/spangap__calendar/`) and as the encoded npm name (`@spangap/spangap__calendar`), neither of which appears in human-authored code. The short identity isn't inferable from the github path, so authors declare it explicitly via the manifest's `prefix:` field (see *The manifest* above).

### Signaling: never silent

Every command starts with a one-line preamble. No hidden state, no surprise reuse:

```
$ spangap make
workspace: ~/code (my-stuff)        4 straddles
project:   my-app
container: spangap-3f8c1a-v1.2.0    (running, ESP-IDF 5.5)
```

Likely-confusion cases get explicit notes:

```
note: workspace at ~/code is running; this build is using ~/work/standalone instead
note: image v1.3.0 newer than container's v1.2.0 — recreating
```

The user always knows which container is being used, which workspace it's bound to, and whether the project is part of a multi-straddle workspace or standalone.

### Inner loop: `spangap dev`

The browser half should iterate without flashing the device. `spangap dev` runs `quasar dev` inside the warm container; the dev server listens on `:9000` inside, the shim port-forwards `:9000` to the host, the user's browser hits `http://localhost:9000`.

Files are real host files. The bind-mount is not a virtual filesystem and there is no sync step:

```
host:      /Users/rop/code/calendar/browser/src/App.vue
                       ↕      (same bytes, same inode events)
container: /work/calendar/browser/src/App.vue
```

The editor edits the host file. VirtioFS (Docker Desktop ≥ 4.6) or OrbStack relays the inotify event into the container. Vite re-bundles, HMR pushes to the browser tab. The experience is identical to native `quasar dev` on a machine that happens to have ESP-IDF installed.

Three non-obvious bits to get right:

- **Watcher fallback.** If VirtioFS isn't propagating events (rare on modern Docker Desktop, common on network drives), Vite falls back to `server.watch.usePolling: true` — slow and CPU-hungry, but works. `spangap doctor`'s findings drive the choice automatically.
- **UID mapping** on Linux hosts so container-created files (`npm install` outputs, `.vite/` cache) are host-owned, not root-owned. Docker Desktop on Mac handles this transparently; Linux invocations bake `--user $(id -u):$(id -g)`.
- **Device on LAN.** A real device on the LAN can't reach `localhost:9000`. Same as native `quasar dev`: `--host 0.0.0.0` and the device hits the host's LAN IP. The container model doesn't make this harder, doesn't solve it either.

### Escape hatches

For users who actively don't want the default model:

- `--no-docker` — run the build natively. Requires ESP-IDF + Node + Python on the host. CLI refuses if the workspace was previously built in Docker mode (ninja path stability).
- `--no-workspace` — ignore any workspace marker; treat the project as standalone.
- `--no-local` — stay in the workspace but ignore sibling overrides; fetch everything fresh from github.
- `spangap shell` — manual `docker exec -it … bash` into the warm container. Power users can `idf.py build` directly with ninja warm in muscle memory; Docker overhead becomes once-per-session, not once-per-command.

### What the host actually has to install

Documented happy path:

| Component | How |
|---|---|
| Docker Desktop or OrbStack | DMG install / `brew install --cask docker` / `brew install orbstack` |
| `spangap` shim | `brew install spangap` (Mac/Linux) · `scoop install spangap` (Windows) · `curl -fsSL https://spangap.io/install \| sh` (any) · `npm install -g spangap` (if Node already present) |

Two install steps, both one-liners. No Node, no Python, no ESP-IDF, no esptool on the host. The container brings all of those.

`spangap doctor` then verifies before the first `make`:

- Docker (or OrbStack) daemon reachable.
- On Mac, file-sharing is VirtioFS, not legacy `osxfs` / gRPC FUSE. This one knob is the difference between "feels native" and "feels broken."
- Platform-straddle image (`ghcr.io/spangap/build-env:<version>`) is pullable.
- USB serial device visible to the host (if `flash -d` is going to be used).
- ESP-IDF version matches the pinned version (only if `--no-docker`).

Each failure prints a clear "do this next" line rather than dumping diagnostics.
