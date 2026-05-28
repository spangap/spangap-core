# spangap-core

ESP-IDF managed component: runtime, networking, web/auth/TLS, storage, log/CLI, inter-task streaming (ITS), WebRTC plumbing for ESP32-S3 device applications.

Half of the [spangap](../) dual-side platform; pairs with [spangap-browser](../browser).

## Install

In your project's `main/idf_component.yml`:

```yaml
dependencies:
  spangap/spangap-core: "^0.1.0"
```

This pulls `spangap-core` from components.espressif.com and its transitive dep `spangap/esp_wireguard`.

For local development against a sibling spangap checkout:

```yaml
dependencies:
  spangap/spangap-core:
    version: "^0.1.0"
    path: "../../path/to/spangap/core"
```

## Use

In `app_main()`:

```cpp
#include "log.h"
#include "fs.h"
#include "storage.h"
#include "its.h"
#include "pm.h"
#include "cli.h"
#include "net.h"
#include "web.h"
#include "tls.h"
#include "ntp.h"
#include "ota.h"
#include "ota_pubkey.h"   // your project's OTA verification key

extern "C" void app_main() {
    pmInit();
    logInit();
    fs_init();
    storageInit();
    itsInit();
    cliInit();
    netInit();
    webInit();
    tlsInit();
    ntpInit();
    otaInit(OTA_PUBKEY_PEM, OTA_PUBKEY_PEM_LEN);
    // ... your application init here
}
```

Each subsystem self-registers its CLI commands, storage defaults, cron entries (if any), and WebSocket / HTTP handlers as part of its init.

## Public headers

The component exposes its API via `include/`. See the [parent CLAUDE.md](../CLAUDE.md) for the conventions (logging macros, `safeStrncpy`, config-key namespaces, ITS architecture) and the per-subsystem deep dives in [docs/](../docs/).

## Development

`CLAUDE.md` (this dir) describes the scope of the core component for AI tooling. The platform-wide CLAUDE.md is at `../CLAUDE.md`.
