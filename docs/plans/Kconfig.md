# Kconfig + selectable modules + one spangapInit()

Status: planning, not implemented. Captures the design discussion held while ironing out how spangap modules get selected, configured, and initialized — so that turning a feature off in one consumer can't fight another consumer that wants it on, and so the consumer's `app_main` doesn't grow boilerplate every time a module is added.

## Goals

1. **Per-consumer feature selection.** Each consumer can enable or disable spangap modules independently. Two consumers don't fight each other over which modules are on.
2. **Single platform init from the consumer's perspective.** `spangapInit()` from the consumer's `app_main` brings up all the platform modules they have enabled, in the right order. The consumer doesn't write or maintain the order.
3. **Lockstep firmware + browser.** Disabling a module on the firmware side automatically removes the matching panel/menu items from the SPA bundle. The two never get out of sync because they're not allowed to be — they always travel together.
4. **No magic.** Detection of which modules are present is deterministic at build time. No constructor-attribute or linker-section tricks for normal core modules. (Linker-section auto-discovery is the kind of thing that makes init order a guessing game and cross-module dependencies hard to follow. Avoided.)
5. **Right unit of granularity.** Most spangap modules (storage, log, cli, web, auth, tls, net, mdns, ntp, cron, wg, duckdns, acme, upnp, ota, …) are too small to deserve their own ESP-IDF component. They live as files inside `spangap-core` and are toggled with Kconfig. Only genuinely big or third-party-origin pieces become separate IDF components.

## Terminology

- **Spangap module** — a logical unit inside spangap: storage, log, cli, web, etc. Each typically owns one `.cpp` + `.h`, registers some CLI commands, owns a config-key prefix, and exposes an `xInit()`. **This is not the same as an ESP-IDF component.**
- **ESP-IDF component** — the packaging unit IDF builds and the registry distributes. `spangap-core` is one. `esp_wireguard` (vendored) is another. Most spangap modules live inside `spangap-core`; only a few become separate components.
- **Consumer** — an app project that depends on spangap (a camera, sensor-hub, network appliance, etc.).

## How module selection works

### ESP-IDF Kconfig is per-project, not per-component

`sdkconfig.defaults` lives in the project root. Components ship `Kconfig` files declaring options + sensible defaults; the project picks which to override. Adopt this:

- `spangap-core/Kconfig` declares `config SPANGAP_DUCKDNS  default y`, `config SPANGAP_WG  default y`, `config SPANGAP_OTA  default y`, etc. — one toggle per optional module.
- Each consumer's project root has its own `sdkconfig.defaults` where it overrides whichever defaults it doesn't want.
- Two consumers with different choices are completely independent — `sdkconfig.defaults` is never shared.

Hard requirements (e.g. WireGuard absolutely needs `LWIP_PPP_SUPPORT=y`) are expressed in Kconfig via `select`:

```kconfig
config SPANGAP_WG
    bool "Enable WireGuard tunnel module"
    default y
    select LWIP_PPP_SUPPORT
```

…so consumers can't half-enable a module and miss its substrate. Sizing knobs (max peers, buffer sizes, etc.) live in the same Kconfig for tuning.

### spangap.h: the umbrella header

`spangap-core/include/spangap.h` is the only header consumer code needs to include for the platform API.

```c
// spangap-core/include/spangap.h
#pragma once
#include "sdkconfig.h"

// always-present platform foundation
#include "pm.h"
#include "log.h"
#include "fs.h"
#include "storage.h"
#include "its.h"
#include "cli.h"
#include "cron.h"
#include "auth.h"
#include "net.h"
#include "web.h"
#include "tls.h"
#include "ntp.h"
#include "spangap_mdns.h"

// optionals — included only when their Kconfig is on
#if CONFIG_SPANGAP_WG
#include "wg.h"
#endif
#if CONFIG_SPANGAP_DUCKDNS
#include "duckdns.h"
#endif
#if CONFIG_SPANGAP_UPNP
#include "upnp.h"
#endif
#if CONFIG_SPANGAP_ACME
#include "acme.h"
#endif
#if CONFIG_SPANGAP_OTA
#include "ota.h"
#endif
```

The consumer's `app_main` becomes:

```c
#include "spangap.h"

extern "C" void app_main(void) {
    spangapInit();
    /* app-specific init */
    /* run /state/boot if you want a CLI-driven boot script */
}
```

No other spangap headers are referenced by name in consumer code. Adding a new optional module to spangap = one `#if`/`#endif` block in `spangap.h` and a one-line entry in `spangapInit()`. The consumer doesn't change anything.

### spangapInit(): one platform-curated startup function

Provided by spangap-core. The consumer doesn't implement it; they call it.

```c
// spangap-core/src/spangap_init.cpp
#include "spangap.h"

void spangapInit(void) {
    pmInit();
    logInit();
    fsInit();
    storageInit();
    itsInit();
    cliInit();
    cronInit();
    authInit();

    netInit();
    mdnsInit();

    webInit();
    tlsInit();
    ntpInit();

#if CONFIG_SPANGAP_WG
    wgInit();
#endif
#if CONFIG_SPANGAP_DUCKDNS
    duckdnsInit();
#endif
#if CONFIG_SPANGAP_UPNP
    upnpInit();
#endif
#if CONFIG_SPANGAP_ACME
    acmeInit();
#endif
#if CONFIG_SPANGAP_OTA
    otaInit();        /* registers CLI, installs storage defaults, etc. */
#endif
}
```

Spangap owns the order. It's tested as a unit. Consumers never touch it.

### "But I want to do something between webInit and ntpInit"

You don't. The recurring need for "do X partway through platform init" is almost always actually "do X when event Y happens" — and there are already four orthogonal mechanisms for that, all available *before* `spangapInit()` returns:

- **`netRegister(NET_EV_*, cb)`** — for "do X when WiFi/STA/upstream comes up or goes down."
- **`storageSubscribeChanges(prefix, cb)`** — for "do X when a config key changes."
- **`cron`** — for "do X at this time / interval."
- **`/state/boot` and `/state/net_up`** scripts — declarative "run these CLI commands at boot / when net is up." Editable at runtime via the in-browser editor; no rebuild.

If a real "I need to interleave with platform init" case ever surfaces, the platform either grows an event for it (e.g. `SPANGAP_EV_PRE_WEB`) or splits `spangapInit()` into named phases. Defer until a concrete need shows up.

### OTA: separate pubkey-set call

OTA is the one platform module that takes consumer-supplied data at init time (the verification public key). To keep `spangapInit()`'s signature parameter-less forever, OTA's pubkey is set via its own call:

```c
// app_main, after spangapInit() returned
#include "spangap.h"
#if CONFIG_SPANGAP_OTA
#include "ota_pubkey.h"
#endif

extern "C" void app_main(void) {
    spangapInit();
#if CONFIG_SPANGAP_OTA
    otaSetPubkey(OTA_PUBKEY_PEM, OTA_PUBKEY_PEM_LEN);
#endif
    appInit();
    cliRunFile("/state/boot");
}
```

(The `#if` on the consumer side could also live inside `spangap.h` — provide a no-op `otaSetPubkey()` stub when `CONFIG_SPANGAP_OTA=n` so the consumer can drop the wrapping `#if`. Detail to settle when implementing.)

Until `otaSetPubkey()` is called, `otaCheck()` / `otaUpgrade()` refuse to verify anything — fail-safe. That separation also makes it cleaner if/when a future OTA wants to support multiple keys, key rotation, or runtime-supplied keys.

OTA itself is also Kconfig-gated. A consumer that doesn't want OTA at all sets `CONFIG_SPANGAP_OTA=n` and the whole module disappears; `otaSetPubkey` either ceases to exist or becomes a no-op.

## CMake-level gating

For modules whose `.cpp` files contain meaningful code, we want the source out of the build entirely when disabled (no dead code in the binary, no compile time spent on it). Pattern:

```cmake
# spangap-core/CMakeLists.txt
file(GLOB CORE_SRCS "src/*.cpp" "src/*.c")

if(NOT CONFIG_SPANGAP_WG)
    list(FILTER CORE_SRCS EXCLUDE REGEX ".*wg\\.cpp$")
    # also exclude vendored esp_wireguard subtree if WG is the only thing using it
endif()
if(NOT CONFIG_SPANGAP_DUCKDNS)
    list(FILTER CORE_SRCS EXCLUDE REGEX ".*duckdns\\.cpp$")
endif()
if(NOT CONFIG_SPANGAP_UPNP)
    list(FILTER CORE_SRCS EXCLUDE REGEX ".*upnp\\.cpp$")
endif()
if(NOT CONFIG_SPANGAP_ACME)
    list(FILTER CORE_SRCS EXCLUDE REGEX ".*acme\\.cpp$")
endif()
if(NOT CONFIG_SPANGAP_OTA)
    list(FILTER CORE_SRCS EXCLUDE REGEX ".*ota\\.cpp$")
endif()

idf_component_register(
    SRCS ${CORE_SRCS}
    INCLUDE_DIRS "include"
    REQUIRES …)
```

Two equally-valid alternatives that affect how the disabled-module API surface looks:

- **(A) Source excluded; calls in `spangapInit()` `#if`'d.** What the sketch above shows — no symbol exists for `wgInit()` when WG is off, and `spangap_init.cpp` has matching `#if` around the call.
- **(B) Source included but stubbed.** `wg.cpp` itself wraps its body in `#if CONFIG_SPANGAP_WG / #else / #endif` with empty stubs in the `#else`. `spangap_init.cpp` calls unconditionally; the call lands on a `ret`. Slightly more code in source, zero code in binary (linker dead-strips), `spangap_init.cpp` stays cleaner with no `#if` clutter.

(B) generalizes better when modules expose more than just `Init` — e.g. a future ACME module's `acmeRequestRenewal()` becomes a no-op stub when ACME's compiled out, and app code can call it without `#ifdef`. (A) keeps source files smaller. Picking between them is a per-module call; the codebase can mix.

## Browser side: lockstep gating

The SPA is built per-consumer, paired with the firmware. Every Kconfig flip on the firmware side is reflected in the browser bundle — disabled modules' panels and menu items don't ship.

### Approach: build-time bridge from sdkconfig to Vite

The consumer's `web-interface/deploy.sh` (run from `idf.py build` via `add_custom_target`) reads `sdkconfig` after `idf.py reconfigure` writes it, derives `VITE_SPANGAP_DUCKDNS=1` etc., and exports them before invoking `quasar build`. `spangap-browser` modules become:

```ts
// spangap-browser/modules/duckdns.ts
import DuckDnsPanel from '../panels/DuckDnsPanel.vue'

export function registerDuckDns() {
  if (!import.meta.env.VITE_SPANGAP_DUCKDNS) return
  menuRegistry.register({ /* ... */ })
}
```

Vite tree-shakes the disabled panel out of the bundle entirely (the `import.meta.env` value is inlined as a constant at build time, so dead-code elimination removes the entire branch — including the `import` if nothing else references it).

A mechanical translator in `deploy.sh` does `CONFIG_SPANGAP_* → VITE_SPANGAP_*` so adding a new module needs zero work in the deploy script.

### Drawbacks (acknowledged, accepted)

- **Single-flavor bundle.** The SPA is built against one firmware's feature set. Two firmware variants need two SPA builds. Consumers that ship paired app+files OTA slots are fine.
- **Build-order rigidity.** Anyone running `quasar build` standalone (without going through `idf.py build` → `deploy.sh`) gets stale or missing env vars. Mitigated by `deploy.sh` failing loudly if `sdkconfig` is absent rather than silently defaulting.
- **Dev-loop friction.** Toggling a Kconfig knob requires `idf.py reconfigure` + a fresh quasar build before the SPA reflects it. Hot-reload still works for everything that doesn't change Kconfig.
- **OTA implication.** A firmware-only OTA push that flips a Kconfig (without re-pushing the paired files image) leaves the SPA pointing at a feature set that no longer matches. Consumers whose OTA pairs app + files are unaffected.
- **Two sources of truth, kept aligned by deploy.sh.** Acceptable cost.

The tight coupling is the point. SPA and firmware travel together by design, change together, and disabled modules vanish from both.

### Optional safety net: runtime check

In addition to the build-time tree-shake, `spangap-browser` modules can also check the device's storage tree for module presence on first connect (`if (!device.has('s.duckdns')) return`). Belt + suspenders: build-time stripping is the primary mechanism, runtime check covers any case where the env var bridge had a hiccup. Cheap to add, no downside.

## When to break out into a separate IDF component

Most spangap modules stay inside `spangap-core` and are toggled at the source-file level. A few real triggers for separating into their own IDF component:

1. **Third-party fork with its own license / attribution** — e.g. `esp_wireguard` is currently vendored under `spangap-core/src/esp_wireguard/`, but if the upstream `netif->state` fix never propagates, or if other apps want to consume the fork without spangap-core, it stays a separate IDF component.
2. **Independent versioning cadence** — a module that needs to ship/update on a different schedule than spangap-core proper.
3. **Heavy dependency tree** — a module that pulls in a big component (ML lib, alternate TLS stack, something large) where consumers who don't want it shouldn't pay for the dep declaration.
4. **External Kconfig menus** — a third-party library with its own `Kconfig` you want exposed to the consumer's `menuconfig` only when the module is in use.

Most spangap modules don't hit any of these, so they live as toggleable `.cpp` files inside `spangap-core`. The ones that *do* (e.g. `esp_wireguard`) become full IDF components with their own `idf_component.yml`, declared as deps of `spangap-core` (or directly by the consumer, if independently useful).

For the browser side, the analogous question — "does it need its own npm package?" — is answered by the same triggers.

## Summary of moving parts

| Layer | What | Where |
|---|---|---|
| Kconfig declarations | `config SPANGAP_X` toggles + `select` chains | `spangap-core/Kconfig` |
| Per-consumer overrides | `CONFIG_SPANGAP_X=n` etc. | each consumer's `sdkconfig.defaults` |
| Public umbrella header | `#include "spangap.h"` | `spangap-core/include/spangap.h` |
| Conditional sub-headers | `#if CONFIG_SPANGAP_X / #include "x.h" / #endif` | inside `spangap.h` |
| Single startup call | `void spangapInit(void)` | `spangap-core/src/spangap_init.cpp` |
| Module-level Kconfig gate at init site | `#if CONFIG_SPANGAP_X / xInit() / #endif` | inside `spangapInit()` |
| Source exclusion (option A) | `list(FILTER CORE_SRCS EXCLUDE …)` | `spangap-core/CMakeLists.txt` |
| Stub bodies (option B alternative) | `#if CONFIG_SPANGAP_X / real / #else / stub / #endif` | individual `.cpp` files |
| Hard substrate requirements | `select LWIP_PPP_SUPPORT` etc. | `spangap-core/Kconfig` |
| Consumer-supplied OTA pubkey | `otaSetPubkey(pem, len)` | called from consumer's `app_main` |
| Browser feature gates | `import.meta.env.VITE_SPANGAP_X` | `spangap-browser/modules/*.ts` |
| sdkconfig → env-var bridge | mechanical `CONFIG_SPANGAP_* → VITE_SPANGAP_*` | consumer's `web-interface/deploy.sh` |

## Open questions for implementation pass

1. **(A) source-exclude vs (B) stub-body** — pick per module, or pick one convention for the codebase. Probably (B) for modules that expose APIs beyond `Init`, (A) for pure-init modules.
2. **`otaSetPubkey()` stub when `CONFIG_SPANGAP_OTA=n`** — define it as a no-op so consumer code stays uniform, or omit and require `#if` at the call site? Mild preference for the no-op stub.
3. **Vendored `esp_wireguard`** — currently linked into spangap-core unconditionally. When `CONFIG_SPANGAP_WG=n`, its sources should drop out too. Probably means a CMake `if(CONFIG_SPANGAP_WG)` around the `WG_SRCS` list, plus dropping the `wg.cpp` source.
4. **`spangap-browser` module-glob auto-import** — Vite's `import.meta.glob('spangap-browser/modules/*.ts', { eager: true })` is one option to discover all modules without an explicit list in the consumer's boot file. Cleaner than maintaining a per-module `import` line, but less explicit. Decide.
5. **Hard-fail or soft-fail when a module's prereq is missing** — e.g. ACME without DNS-01 helpers. Probably hard-fail at CMake time via `select`/`depends on`, never at runtime.
6. **Where does the recommendation list of `sdkconfig.defaults` knobs live** — spangap-core ships a documentation snippet (in `spangap/docs/sdkconfig-required.md`) listing the IDF-level settings consumers need (`CONFIG_LWIP_PPP_SUPPORT=y`, `CONFIG_USJ_NO_AUTO_LS_ON_CONNECTION=y`, etc.) that aren't already enforced by `select`. Or, better, fold them all into `select` chains so they happen automatically. Settle when implementing.

## Plan for execution (deferred)

When this is actually implemented:

1. Author `spangap-core/Kconfig` with toggles + `select` chains.
2. Author `spangap-core/include/spangap.h` umbrella.
3. Author `spangap-core/src/spangap_init.cpp` with `spangapInit()`.
4. Update `spangap-core/CMakeLists.txt` for source-level Kconfig gating (decide A vs B per module).
5. Refactor each optional module as needed (option B if applicable).
6. Update consumer apps to drop the verbose init list and just call `spangapInit()` + `otaSetPubkey()` + `appInit()`.
7. Author the `sdkconfig → VITE_*` bridge in each consumer's `deploy.sh`; refactor `spangap-browser/modules/*.ts` to gate registration on `import.meta.env`.
8. Build, test, document. Update [`../../CLAUDE.md`](../../CLAUDE.md) with the new init shape.
