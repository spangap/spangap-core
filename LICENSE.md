# License

This repository, **spangap-core** (runtime, networking, web/auth/TLS, storage,
log/CLI, ITS, WebRTC plumbing for ESP32-S3 device apps), is released under the
**Apache License, Version 2.0**.

Full license text: <https://www.apache.org/licenses/LICENSE-2.0>

Copyright (c) 2026 by spangap project contributors.

## Third-party software

### Vendored in this repository

None. This repo contains no third-party source code.

### Build-time dependencies

Declared in `esp-idf/idf_component.yml`. These ship into any firmware image
that links spangap-core:

| Component | Source | License |
|---|---|---|
| ESP-IDF (platform) | espressif/esp-idf | Apache-2.0 |
| `espressif/mdns`   | components.espressif.com | Apache-2.0 |
| `joltwallet/littlefs` (wraps `littlefs-project/littlefs`) | components.espressif.com | BSD-3-Clause |

mbedTLS, FreeRTOS, lwIP and other libraries linked transitively via ESP-IDF
retain their upstream licenses.
