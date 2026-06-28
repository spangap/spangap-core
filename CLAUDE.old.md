# spangap — ESP-IDF + browser device platform

> **Status — 2026-05-23:** the `spangap-core/` (firmware) + `browser/` (npm) split is in place and is the canonical layout — the old seccam-duplicate `main/` / `web-interface/` staging dirs are gone. Firmware lives in `spangap-core/src` + `spangap-core/include`, the browser package in `browser/src`.

## What is spangap

Dual-side device platform for ESP32-S3:
- **`spangap-core/`** — IDF managed component, published as `spangap/spangap-core` on components.espressif.com: runtime + networking + web stack + storage + log/CLI + ITS + config + WebRTC plumbing
- **`browser/`** — npm package, published as `spangap-browser` (unscoped): paired browser pieces — settings UI, FloatingWindow, WebRTC session manager, storage sync, log/CLI rendering, auth flow, menu registry
- **`examples/`** *(planned, not yet present)* — dual-sided reference apps (starter, sensor-hub)
- **`scripts/`** — operator tools that work against any platform consumer (spangap itself, seccam, reticulous): [`flasher`](scripts/flasher) (esptool-direct reflash + monitor; `-d` daemon polls `build/flashme`, keeps an always-live monitor — passive `--no-reset` while idle so a VM can read the device terminal without reflashing — and wipes `build/flasher.log` per flash), and [`reallyclean.sh`](scripts/reallyclean.sh) (deep-clean a tree to source-only state; wired into `idf.py reallyclean` via `idf_ext.py` in consumer projects).

Apps consume one or both halves and write only domain code on top.

## Where is everything?

Fast map from a thing to its file. Paths from this dir (`spangap/`). Firmware lives in `spangap-core/`, browser in `browser/`.

**Firmware (`spangap-core/`)** — public API in `include/<mod>.h`, implementation in `src/<mod>.cpp`, one pair per module. The per-module rundown is in the **Module map** section below; the non-obvious spots:

- CLI engine + line editor + history → [`src/cli.cpp`](spangap-core/src/cli.cpp); built-in commands → [`src/cli_cmd_fs.cpp`](spangap-core/src/cli_cmd_fs.cpp), [`src/cli_cmd_sys.cpp`](spangap-core/src/cli_cmd_sys.cpp)
- On-device LVGL UI → [`src/lcd_ui/`](spangap-core/src/lcd_ui/) (launcher, settings, statusbar, icons); public API [`include/lcd.h`](spangap-core/include/lcd.h), board HAL [`include/lcd_board.h`](spangap-core/include/lcd_board.h)
- Vendored WireGuard crypto → [`src/esp_wireguard/`](spangap-core/src/esp_wireguard/)
- Build/admin scripts (partitions, OTA keygen/release, icon raster, timezones, size report, build epoch) → [`spangap-core/scripts/`](spangap-core/scripts/)`*.py`
- Factory defaults flashed to `/fixed` (boot, crontab, net_up) → [`spangap-core/data/factory_state/`](spangap-core/data/factory_state/)

**Browser (`browser/`, npm `spangap-browser`)** — TS/Vue under `browser/src/`. Consumer apps import these through a `spangap-browser` symlink, so this is the home of every *shared* UI piece:

| Looking for | Path |
|---|---|
| Web-UI CLI / terminal console | [`browser/src/components/TerminalWindow.vue`](browser/src/components/TerminalWindow.vue) |
| Log viewer window | [`browser/src/components/LogWindow.vue`](browser/src/components/LogWindow.vue) |
| Draggable/resizable window frame | [`browser/src/components/FloatingWindow.vue`](browser/src/components/FloatingWindow.vue) |
| File/text editor window | [`browser/src/components/EditorWindow.vue`](browser/src/components/EditorWindow.vue) |
| Config-bound controls | `browser/src/components/Setting{Toggle,Slider,Select,Text}.vue` |
| Settings container / section heading | [`SettingsPanel.vue`](browser/src/components/SettingsPanel.vue), [`PanelHeading.vue`](browser/src/components/PanelHeading.vue) |
| Top menu bar / usable area | [`MenuBar.vue`](browser/src/components/MenuBar.vue), [`UsableArea.vue`](browser/src/components/UsableArea.vue) |
| WebRTC session + DataChannel registry | [`browser/src/lib/webrtc-session.ts`](browser/src/lib/webrtc-session.ts) |
| Auth / login flow | [`browser/src/lib/auth.ts`](browser/src/lib/auth.ts), [`pages/LoginPage.vue`](browser/src/pages/LoginPage.vue) |
| Auto-reconnect, device URL | [`lib/reconnect.ts`](browser/src/lib/reconnect.ts), [`lib/device-url.ts`](browser/src/lib/device-url.ts) |
| Pinia stores (device, log, menu registry) | `browser/src/stores/{device,log,menu}.ts` |
| Built-in settings panels (Network, System, Developer, About, ACME, WireGuard, UPnP, DuckDNS, WiFi scan) | [`browser/src/panels/`](browser/src/panels/)`*.vue` |
| Panel auto-registration | [`browser/src/modules/`](browser/src/modules/)`*.ts` |
| Public export surface | [`browser/src/index.ts`](browser/src/index.ts) |

**Operator tools (`scripts/`)** — [`flasher`](scripts/flasher) (reflash + monitor daemon), [`spangap-cli`](scripts/spangap-cli)/[`cli`](scripts/cli) (remote CLI), [`reallyclean.sh`](scripts/reallyclean.sh).

**Docs** — platform deep dives in [`docs/`](docs/); indexed in the **Subsystem deep dives** section below.

## User Preferences

- Do NOT use shell commands (sed/cat/awk) to edit files — use Edit/Write tools
- Discuss before coding when user asks a question
- Keep things concise, no unnecessary changes
- Use modern C++ (`std::string`, `std::string_view`) — avoid C-style `char[]`/`strstr` parsing
- Allow all web searching and fetching without prompting
- Do NOT use PlatformIO

## Coding Conventions

- Use `info()` / `warn()` / `err()` / `dbg()` / `verb()` macros, not `ESP_LOGx` directly — the log task adds `[taskname]` automatically. Code on unregistered tasks must prefix manually.
- Use `safeStrncpy(dst, src, n)` not `strncpy` — always NUL-terminates, logs on truncation.
- Prefer modern C++: `std::string`, `std::string_view`. Avoid C-style `char[]` / `strstr` parsing.
- Config key namespaces: `s.*` = persisted + synced to browser; `secrets.*` = persisted, NEVER sent to browser; no prefix = ephemeral (lost on reboot).
- `storageDefault*` writes are silent — they don't fire change subscriptions. Use `storageSet` when subscribers need to react.
- The canonical `itsPoll` loop: `for (;;) { while (itsPoll(0)) {}; /* non-ITS work */; itsPoll(blockTime); }`
- Xtensa `uint32_t` is `long unsigned int` — cast with `(unsigned)` for `%u` in printf.
- Do NOT include task name in log messages — `[taskname]` is prepended automatically.
- CLI commands are silent on success (`set`, `unset`, `save`, `detect` produce no output on success).

## Architecture

Single communication layer: **ITS** (point-to-point stream connections, aux messages, FreeRTOS Queue-based inbox with `itsPoll(timeout)` as the universal blocking primitive).

### ITS — Inter-Task Streaming (`its.h/cpp`)
Generic point-to-point connection layer. Tasks are servers (accept connections), clients (initiate), or both. Like TCP for FreeRTOS tasks.
- **Server**: `itsServerInit(maxHandles, toSize, fromSize)`. Pre-allocates stream buffers per handle in PSRAM.
- **Client**: `itsClientInit(maxConns, onDisconnect)`.
- **Connect**: `itsConnect("taskName", itsPort, data, len, timeout, ref)` → server's `onConnect(handle, itsPort, data, len)` returns serverRef (>= 0 = accept, < 0 = reject).
- **itsPort**: 16-bit endpoint identifier flowing from connect → onConnect so the server knows which registered endpoint (TCP port, URL path) the connection is for.
- **Forward**: `itsServerForward(handle, "targetTask", itsPort, data, len)` — transfer a server handle to another server task.
- **Inject**: `itsServerInject(handle, data, len)` — write data into a server handle's receive buffer (used before forward to put consumed HTTP headers back).
- **Data**: `itsSend(handle, data, len, timeout)` / `itsRecv(handle, buf, maxLen, timeout)` — stream semantics.
- **Aux messages**: `itsSendAux("taskName", data, len, timeout, port)` — task-to-task signaling outside connections.
- **Inbox**: FreeRTOS Queue (thread-safe, multi-producer). `itsPoll(timeout)` reads one message, dispatches to callback.

Deep dive: [docs/its.md](docs/its.md).

### Module map (`spangap-core/`)

Public-API headers (live in `spangap-core/include/`); implementations + private headers (live in `spangap-core/src/`):

- `compat.h` — `millis()`/`delay()` shims, `safeStrncpy`, `fpsToIntervalMs`, `utcOffsetMinutes`, `fmtElapsed`/`fmtSize`/`fmtBps`/`fmtWallClock`
- `fs.cpp/h` — unified file I/O via DRAM-stack workers (LittleFS + FAT/SD)
- `storage.cpp/h` — config tree (cJSON) + browser sync via `storage:1` DC
- `its.cpp/h` — Inter-Task Streaming
- `pm.cpp/h` — power management locks, USB pullup, deep-sleep stats. CLI: `pm`, `top`, `usb`
- `log.cpp/h` — log task, ESP-IDF `vprintf` hook, DRAM ring buffer, fan-out + optional log file
- `cli.cpp/h`, `cli_cmd_fs.cpp`, `cli_cmd_sys.cpp` — CLI registry, line editor, history, `cliRunFile` for boot scripts
- `cron.cpp/h` — minute-resolution scheduler with deep-sleep support
- `auth.cpp/h` — cookie sessions, realms, rate limiting, force-takeover
- `web.cpp/h` — HTTP/HTTPS server, REST, WebDAV (WIP), WS helpers
- `tls.cpp/h` — mbedTLS server, EC P-256 cert (self-signed or ACME), hot reload
- `net.cpp/h` — WiFi + TCP/UDP call center + event dispatch (`netRegister(NET_EV_*, cb)`); `NET_EV_UP`/`DOWN` for any WiFi; `NET_EV_UPSTREAM_UP`/`DOWN` only on STA-connected-to-internet
- `wg.cpp/h` — WireGuard tunnel
- `spangap_mdns.cpp/h` — mDNS/Bonjour wrapper around ESP-IDF `mdns` (named to avoid collision with IDF's own `mdns.h`)
- `ntp.cpp/h` — non-blocking NTP via `esp_sntp`; IANA timezone → POSIX cache; `date` CLI
- `upnp.cpp/h`, `duckdns.cpp/h`, `acme.cpp/h` — remote access stack (NAT, DDNS, TLS cert)
- `ota.cpp/h` — signed-manifest OTA flow. **Public key supplied by consumer at init time** (`otaInit(pubkey_pem, len)`) — the platform supplies the verification logic, the app supplies its keys.
- `webrtc_task.cpp/h`, `webrtc_sctp.cpp/h` — generic WebRTC: DTLS/ICE-lite/SCTP signaling + content-free DC↔ITS router. DC labels of the form `<task>:<n>` route to ITS port `n` on `<task>` automatically.
- `lcd.h` / `lcd_board.h`, `src/lcd_ui/*` — on-device LVGL launcher: owns the display + input + the one LVGL context, paints program tiles + status bar + a built-in Settings menu + built-in Log/CLI programs (on-device terminals in the Spleen 5×8 font; the lcd task acts as an ITS client to `log:1`/`cli:1`). Gated on `CONFIG_SPANGAP_LCD`; the consumer supplies the board HAL (`lcd_board.h`). Deep dive: [docs/lcd.md](docs/lcd.md).
- `heap_track_stub.c` — IDF 5.5 heap-task-tracking `--wrap` stubs

## Common Recipes

### Add a config key with defaults
```cpp
#define MYMOD_VERSION 2  // bump when adding new keys
if (storageGetInt("s.mymod.version", 0) < MYMOD_VERSION) {
    storageDefault("s.mymod.new_key", default_value);
    storageDefaultTree("s.mymod", "{\"key\":42}");
    storageSet("s.mymod.version", MYMOD_VERSION);
}
```

### Add a CLI command
```cpp
cliRegisterCmd("mycmd", [](const char* args) {
    /* `help` → one short line for the `help` listing. */
    if (strcmp(args, "help") == 0) { cliPrintf("%-*s my thing; sub to do X\n", CLI_HELP_COL, "mycmd [sub]"); return; }
    /* `-h`/`--help` → fuller help. (Collapse with the line above via
     *  cliWantsHelp(args) when brief == detailed.) */
    if (cliWantsHelp(args)) { cliPrintf("%-*s do X to the thing\n", CLI_HELP_COL, "mycmd sub"); return; }
    if (strcmp(args, "sub") == 0) { /* do X */ return; }
    /* No args → status (no "status" verb). Output is flush-left. */
    cliPrintf("mycmd: idle\n");
});
```

### Add an ITS server port
```cpp
itsServerInit();
itsServerPortOpen(MY_PORT, MAX_CLIENTS, toBufSize, fromBufSize);
itsServerOnConnect(MY_PORT, onConnect);
itsServerOnRecv(MY_PORT, onRecv);
itsServerOnDisconnect(MY_PORT, onDisconnect);

// Register with net for TCP port, or with web for URL prefix:
net_port_msg_t reg = { .port = MY_PORT, .taskName = "mytask" };
itsSendAux("net", NET_PORT_REG_PORT, &reg, sizeof(reg), pdMS_TO_TICKS(500));
// or
web_path_msg_t wreg = { .itsPort = MY_PORT };
safeStrncpy(wreg.path, "mypath", sizeof(wreg.path));
itsSendAux("web", WEB_PATH_REG_PORT, &wreg, sizeof(wreg), pdMS_TO_TICKS(500));
```

### Add a browser DataChannel (WebRTC)
**Device**: open packet-mode ITS server port; webrtc-task auto-routes DC labels to ITS ports.
```cpp
itsServerPortOpen(MY_DC_PORT, 1, 2048, 4096);
itsServerOnConnect(MY_DC_PORT, onDcConnect);
itsServerOnRecv(MY_DC_PORT, onDcRecv);
// Browser creates DC with label "mytask:1" → webrtc-task parses port → itsConnect here.
```
**Browser**: register a channel builder with the spangap session.
```typescript
webrtcSession.registerChannel((pc) => {
  const dc = pc.createDataChannel('mytask:1', { ordered: true, protocol: JSON.stringify({...}) });
  dc.onmessage = (e) => { /* ... */ };
  return dc;
});
```

### Add a UI settings panel
1. Create `web-interface/src/modules/panels/MyPanel.vue` (in the consuming app)
2. Register: `menuRegistry.register({ group: 'My Group', id: 'my-panel', label: 'My Panel', component: () => import('./panels/MyPanel.vue') })`
3. Use spangap's `SettingToggle` / `SettingSlider` / `SettingSelect` / `SettingText` components for config-bound controls.
4. For the **on-device** equivalent (`CONFIG_SPANGAP_LCD` builds), register a pane with `lcdRegisterSettings("Group/Item", "Item", fn)` and build it with the `lcdSetting*` helpers — gate the call behind `#if CONFIG_SPANGAP_LCD`. See [docs/lcd.md](docs/lcd.md).

### Add a cron entry from a module
```cpp
cronDefault("*/15 *    *    *    *    N    mycmd sub", "mycmd sub");
// Flags: - = always, A = awake only, N = STA upstream only.
```

### Add a net event callback
```cpp
netRegister(NET_EV_UPSTREAM_UP, [](const char* data) { info("upstream up\n"); });
// Events: NET_EV_UP, NET_EV_DOWN, NET_EV_UPSTREAM_UP, NET_EV_UPSTREAM_DOWN, NET_EV_CFG_CHANGED, NET_EV_POLL
```

## Subsystem deep dives

(Platform deep dives live at [`docs/`](docs/).)

- **Storage / Config** — [docs/storage.md](docs/storage.md). cJSON tree; `s.*` (synced), `secrets.*` (private), ephemeral. Self-registering defaults gated on `s.<mod>.version`.
- **Unified file I/O** — [docs/unified-fs-api.md](docs/unified-fs-api.md). All I/O via DRAM-stack workers; serializes SDMMC DMA; safe from PSRAM-stack callers.
- **Cron** — [docs/cron.md](docs/cron.md).
- **Power Management** — [docs/power-management.md](docs/power-management.md).
- **Logging / CLI / serial transport** — [docs/logging.md](docs/logging.md).
- **Web server (HTTP/HTTPS, REST, WebDAV, WS helpers)** — [docs/web.md](docs/web.md).
- **Browser SPA conventions** — [docs/web-interface.md](docs/web-interface.md).
- **Authentication** — [docs/auth.md](docs/auth.md).
- **TLS / HTTPS** — [docs/tls.md](docs/tls.md).
- **Remote access (UPnP / DuckDNS / ACME)** — [docs/remote-access.md](docs/remote-access.md).
- **WireGuard VPN** — [docs/wireguard.md](docs/wireguard.md).
- **NTP / time** — [docs/ntp.md](docs/ntp.md).
- **OTA** — [docs/ota.md](docs/ota.md).
- **WebRTC plumbing** — [docs/webrtc-for-everything.md](docs/webrtc-for-everything.md).
- **ITS architecture (deep dive)** — [docs/its.md](docs/its.md).
- **On-device UI (LCD launcher / LVGL)** — [docs/lcd.md](docs/lcd.md). Gated on `CONFIG_SPANGAP_LCD`; `lcdRegister` programs, `lcdRegisterSettings` panes, board HAL (`lcd_board.h`), icon pipeline.

Cross-cutting:
- [docs/getting-started.md](docs/getting-started.md) — first-time host setup + build dependencies (ESP-IDF, Node, LCD icon rasterizer: libcairo + Pillow + cairosvg).
- [docs/development.md](docs/development.md) — toolchain, build, flash, VM+host workflow, remote CLI (`spangap-cli`), on-device debug surface (`top`/`pm`/`log`), panic-frame decoding.
- [docs/key-fixes.md](docs/key-fixes.md) — hard-won bug fixes by subsystem.
- [docs/idf-tweaks.md](docs/idf-tweaks.md) — local IDF workarounds (linker `--wrap`, etc.).

(Camera, audio, detect, recording, RTSP deep dives belong to seccam, not spangap.)

## Gotchas

- **PSRAM-stack tasks can't do FS-layer flash I/O.** Route all file I/O through `fs.cpp` workers.
- **`itsPoll` with short timeout in `while()` → infinite spin.** Use `itsPoll(timeout); while (itsPoll(0)) {}` only when you genuinely need a periodic backstop.
- **`vTaskDelay(1)` is mandatory in webrtc_task's main loop** — without it, the IDLE0 watchdog fires on core 0.
- **Never `printf` from PSRAM-stack tasks** — use `info()`.
- **lwIP `O_NONBLOCK`/`MSG_DONTWAIT` can still briefly block.** Use `select()` with zero timeout before `accept()`/`recv()`.
- **ESP-IDF 5.x `netif->state` conflict with WireGuard** — required workaround: `CONFIG_LWIP_PPP_SUPPORT=y`.
- **`CONFIG_ESP_WIFI_NVS_ENABLED=n`** — prevents WiFi blob from auto-reconnecting outside `net.cpp` and avoids mid-session NVS writes.
- **SDMMC DMA requires internal DRAM buffers** — fs worker serializes all SD access.
- **ESP32-S3 AES-GCM hardware DMA bug** (espressif/esp-idf#12689) — spangap uses ChaCha20-Poly1305 for TLS/DTLS.
- **IDF 5.5 `HEAP_TASK_TRACKING` global mutex** — `spangap-core/src/heap_track_stub.c` provides `--wrap` no-op stubs; required for stable cJSON-heavy workloads.

## ESP-IDF Specifics

- **PSRAM malloc**: `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=0` — `malloc()` prefers PSRAM. DMA/WiFi/lwIP use `MALLOC_CAP_INTERNAL`/`MALLOC_CAP_DMA` explicitly. `CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=32768` reserves DRAM pool.
- Console: USB Serial JTAG (`CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y`); light sleep kills it.
- Task watchdog: IDLE1 not monitored (`CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU1=n`) — core 1 hosts heavy workers.
- `SO_SNDBUF` setsockopt is silently ignored — `TCP_SND_BUF` is compile-time only. Set `CONFIG_LWIP_TCP_SND_BUF_DEFAULT=16384`. TCP streaming requires `TCP_NODELAY`.

## Partition layout (default; apps may override)

- `nvs` (20KB @ 0x9000): IDF internal use only (WiFi cal, PHY data). App code never reads/writes NVS.
- `app0` (~6.25MB @ 0x10000): firmware
- `fixed` (~1.44MB @ 0x670000, **readonly**): LittleFS, flashed every build, mounted at `/fixed`. Contains `webroot/` (web UI), `factory_state/` (boot, crontab, net_up, `storage/external/<prefix>.json` blobs), and `additional_state/` (first-boot overlay).
- `state` (128KB @ 0x7E0000): LittleFS, read-write, mounted at `/state`, **always**. Contains `boot`, `net_up`, `crontab`, TLS certs, ACME key, and `storage/` (`storage/root.json` = the config tree, `storage/external/<prefix>.json` blobs). Auto-formatted on first boot; `reset factory`/`format flash` formats it + copies from `/fixed/factory_state/`.
- **Active state store may be SD instead of flash.** If `/sdcard/state` exists at boot, it (not `/state`) becomes the active store; `/state` stays mounted but unused. Code never hard-codes `/state` — it uses `fsStateDir()` / `fsStatePath()` (see [docs/storage.md](docs/storage.md), [docs/unified-fs-api.md](docs/unified-fs-api.md)). The config file is `<stateDir>/storage/root.json` (renamed from the old top-level `settings.json`).

## Hardware

ESP32-S3 family, dual-core, 8MB PSRAM (OPI), 8MB flash, WiFi. Other Espressif chips are not currently targeted.
