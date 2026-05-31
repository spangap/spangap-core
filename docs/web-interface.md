# spangap-browser SPA conventions

`spangap-browser` is the npm package side of spangap. It exports the SPA shell pieces — settings UI, FloatingWindow, log/CLI windows, WebRTC session, auth, default panels, login/setup pages — that consuming Quasar / Vue 3 apps assemble into their own SPA. Built and bundled by the consumer (typically via Quasar's `vite` build), gzipped, and served from the device's LittleFS `/fixed/webroot/`.

The **build pipeline** is the consumer's concern: typically a `web-interface/deploy.sh` that runs `quasar build` → gzips each output → drops the `*.gz` files into `data/webroot/` → a project-level `CMakeLists.txt` builds the LittleFS image and flashes it. Apps also commonly emit a `data/webroot/build_version` file with a UTC deploy timestamp for browser auto-reload detection.

## Shared WebRTC Session — `spangap-browser/lib/webrtc-session`

Singleton owning the signaling WebSocket (`/webrtc`) and a single `RTCPeerConnection` that every consumer shares.

- **Signaling**: WS forwarded by web to `webrtc_task`; browser sends offer, device answers. Connect is idempotent — already-connected / connecting calls are no-ops so multiple consumers (player + device store + terminal) can't race to tear down the same PC.
- **Channel builders**: consumers call `session.registerChannel((pc) => { pc.createDataChannel(...); ... })`. Builders fire **before** `createOffer()` on every fresh PC so the initial SDP always carries an `m=application` line — without this Chrome rejects the answer with "order of m-lines doesn't match".
- **States**: `idle` / `connecting` / `connected` / `busy` (4409) / `kicked` (4008) / `auth` (4401) / `error`. `busy` and `kicked` disable auto-reconnect; the user clicks "Take over" (next connect uses `?force=1`) or "Resume" (retry without force). Other close codes auto-reconnect with backoff.

App-side video / audio players build their own DC on the shared session — spangap-browser doesn't ship a video player.

## Config Sync — `spangap-browser/stores/device` ↔ device `storage` module

Pinia store syncs bidirectionally with the device's storage tree. Nested reactive JS object mirrors the JSON structure.

- **DC on `storage:1`**: `device.ts` registers a channel builder with the shared session; on every fresh PC the builder calls `pc.createDataChannel('storage:1', {ordered:true})`. Device sends a full dump on open, then coalesced nested-JSON merge-patches (one packet per flush pass). Browser sends nested JSON patches back. `{"save":1}` forces immediate flush; `{"ping":1}` / `{"pong":1}` heartbeat. Single-client (the whole tab shares one PC).
- **Auto-reload**: `sys.build_time` config value = app version/date + data deploy timestamp. Browser remembers first value, reloads page on change (detects firmware updates).
- **Clean disconnect**: `beforeunload` handler sends `{"save":1}` and closes the DC on page unload.
- **Reconnect resilience**: session handles reconnect; visibility-change detects phone wake and nudges `session.connect()` after 2 s if the DC is dead. Heartbeat ping every 10 s, 30 s staleness check nudges reconnect via the session.

## Menu bar — `spangap-browser/components/MenuBar.vue`

macOS-style header bar. The first item is the app name (the consumer drives this from `s.sys.progname` or any other config source) + `Settings` dropdown + "Log out" (visible when session cookie present). Submenus open on hover. Auto-close after 1500 ms when mouse leaves. Menus are white bg / black text, rounded.

**Compact mode** (`$q.screen.lt.md` — viewports < 1024 px): the entire bar collapses to a hamburger button on the left whose dropdown nests every menu group inside `q-expansion-item`s; About + Log out sit under a `progName`-labeled group.

The right-side area is reserved for app-defined status pills. One might, for example, render MOTION / SOUND / REC indicators driven by app-published ephemerals — spangap-browser exposes a slot for these but doesn't render anything itself.

## Modular settings panels

Each feature module self-registers its menus via `spangap-browser/stores/menu` (Pinia). Modules in `src/modules/*.ts`, panels in `src/panels/*.vue` (within spangap-browser; consumer apps add their own under their own `src/modules/` and `src/modules/panels/`). `Setting*` components (`SettingSlider`, `SettingToggle`, `SettingSelect`, `SettingText`, `PanelHeading`) are typically registered globally in the consumer's `boot/modules.ts` boot file.

`SettingSelect` accepts a `disable` prop forwarded to `q-select`.

**Panel drawer**: opens left of video on menu item click. Sticky title header, scroll indicators (floating arrows), dismiss overlay on click outside. **Portrait phone layout** (`$q.screen.lt.sm && height > width`): the side drawer is replaced by a stacked panel rendered below the video in the same `usable-area` flex column (50/50 split). Apps that show a video canvas should always apply `videoStyle`; the stacked CSS rule overrides position/size with `!important` when `panelOpen`.

**Default panels** shipped by spangap-browser: System (hostname, NTP server, timezone), Network (WiFi + UPnP + WireGuard + DuckDNS + ACME submenus), About, Developer. Consumer apps add their own (a camera app might add CameraPanel, RecordingPanel, etc.).

**Components use** `device.get("s.…")` for reading and `device.set("s.…", value)` for writing. Uses Quasar SVG icon set (no font downloads).

**Timezone**: browser auto-pushes IANA timezone + POSIX string on first connect if unset. Timezone DB (`s.time.zones`) is a platform-owned external storage blob shipped from `spangap-core/data` via the factory-image merge (refresh with spangap-core's `make timezones`).

**Client time push**: browser sends `sys.time.set` (epoch seconds) if `sys.time.valid=0`, giving the device time even without NTP.

## Log Stream — `spangap-browser/stores/log`, `components/LogWindow.vue`, DataChannel `log:1`

Pre-connects the `log:1` DC at app startup (channel builder registered from `MainLayout.onMounted` via `startLogStream()`) so the device-side log task starts streaming immediately — the LogWindow doesn't have to wait for a fresh PC handshake when it's opened.

- **Bounded byte buffer**: 256 KB ring (line-aligned trim). `subscribeLog(cb)` lets the LogWindow stream new chunks; `getLogBuffer()` provides paste-back when it opens.
- **Pre-open queue**: `sendLogLine()` queues up to 128 lines while the DC is opening; `dc.onopen` flushes them. Console output from very early in the page lifecycle still reaches the device.
- **Console hook**: `installConsoleHooks()` wraps `console.{log,info,warn,error,debug}` plus `window.error` and `unhandledrejection` so each call is also forwarded to the device log. The wrapper still calls the original `console[name]` first (devtools unaffected). Lines are pre-colored with ANSI escapes (grey timestamp + level color matching `s.log.colors.*`) so xterm renders colors and the device's `containsAnsi()` pass-through path delivers the same colored line to other ANSI consumers.
- **Format**: `<Mon DD HH:MM:SS.mmm> L Browser: <msg>` where L is `E`/`W`/`I`/`D`/`I` for `error`/`warn`/`info`/`debug`/`log`. The device's level-char detection accepts the tag-less `Browser:` form (no `[tag]` required).
- **Local echo**: each console call is also written to the local buffer + subscribers, so the user's own console output appears in their LogWindow without a device round-trip (the device deliberately does not echo lines back to the source slot).
- **LogWindow**: pure xterm display, no DC of its own. On open it pastes `getLogBuffer()` then `subscribeLog`s; on close it disposes the terminal but the DC stays connected. CLI window uses `TerminalWindow.vue` (which owns its own `cli:1` DC).

## Floating subwindows — `spangap-browser/components/FloatingWindow.vue`

Generic draggable / resizable / dockable shell used by `LogWindow` + `TerminalWindow`. State (geom, dock side+size, visibility) per-window in `localStorage` under `spangap.win.<id>`.

- **Pointer events** drive drag + resize (titlebar `pointerdown` → `setPointerCapture` + window-level `pointermove` / `pointerup` / `pointercancel`). `touch-action: none` on titlebar + handles so phones don't hijack the gesture as scroll. One pointer at a time (`isPrimary` + `pointerId` match), so multi-touch doesn't desync.
- **Resize handles** are invisible but generously sized (~20 px edges, 24–28 px corners). `.fw` is `overflow: visible` so each handle's outside-of-border half is clickable; `.fw-titlebar` has `z-index: 11` so close + drag still win over handles inside the title-bar area, while bottom corners get the largest hit zone.
- **Dock state persistence**: `saveState` snapshots `docks[id].side` / `docks[id].size` synchronously at call time and writes 500 ms later. The synchronous snapshot is load-bearing: a sibling visibility watcher calls `undockWindow` on hide, and an async-read save would otherwise persist the post-undock `null` and lose the user's dock choice across reload.
- **Default dock**: optional `defaultDock` prop applied only when no `localStorage` entry exists for the window. LogWindow + TerminalWindow set `{ side: 'top', size: 50 }` plus a full-width floating geom and a `-3` zoom (font ≈ 8 px) when `matchMedia('(max-width: 599px)')` matches at setup, so phones get a sensible first-run layout without disturbing existing users.
