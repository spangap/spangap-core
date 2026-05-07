# Storage — Config

`storage.cpp/h` — config store + browser config WebSocket (ITS server). File I/O lives in `fs.cpp/h`.

In-memory cJSON tree with hierarchical dot-notation keys. Backed by `/state/settings.json` (nested JSON) plus per-prefix external files under `/state/storage/<mode>/`. Thread-safe: recursive mutex protects all tree access (reads and writes from any task). Three key namespaces:
- **`s.*`** — persisted, synced to browser.
- **`secrets.*`** — persisted, **never sent to browser** (WS dump and per-key updates filtered). Browser writes to `secrets.*` are silently ignored. Used for private keys.
- **Ephemeral** (no prefix) — in-memory only, synced to browser. Lost on reboot.

**Self-registering defaults** (the dominant pattern): each module installs its own config defaults and crontab entries in its `init()`, gated by an `s.<mod>.version` key. On first boot the keys aren't there → install runs → version is bumped. Subsequent boots see version up-to-date → install block skipped, user edits preserved. Add a key to a module's tree, bump version, the new key appears at next boot without disturbing existing values. APIs: `storageDefault(key, val)` (set if absent), `storageDefaultTree(prefix, json)` (walk a JSON literal, install each missing leaf), `cronDefault(schedule, command)` (append crontab line if no active-or-commented line has the same command). All three are silent — no change subscriptions fire during default install.

**External storage files** (`/state/storage/<mode>/<key.path>.json`): runtime registry. The first-level subdir under `storage/` is the **mode**, the filename's stem is the dot-path **prefix**. Today only mode `external` is implemented: file is mounted into `cfgRoot` at boot and saved to its own file when its sub-tree dirties (instead of going through `settings.json`). Drop a file in `data/factory_state/storage/external/` and `scanExternals` picks it up — no compile-time registration. The big `s.time.zones` IANA→POSIX map lives this way (saves rewriting ~15 KB on unrelated config changes). Future modes (e.g. `flash-only/`) can be added by handling more subdirs in `scanExternals()`.

**Boot sequence**: `fs_init()` → mount `/fixed` + `/state` → if `/state` is empty, `fs_factory_reset()` recursively copies `/fixed/factory_state/` to `/state/`, then `applyAdditionalState()` overlays `/fixed/additional_state/` (also recursive; `settings.json` deferred to storageLoad's deepMerge) → `storageLoad()` reads `settings.json` (may be absent → `{}`) and mounts each `storage/external/*.json` at its prefix → `cronWakeupHandler()` → `pmInit()` → all module inits (each runs its `if (version < N)` install block) → `cliRunFile("/state/boot")` → `recNotifyBootScriptDone()` → `cronPoll(true)` → `vTaskDelete`. Boot script runs after all CLI commands are registered. `/state/net_up` runs on a temp task each time **NET_EV_UPSTREAM_UP** fires (STA upstream connect, not AP-only).

**Config API**: `storageGetInt()`, `storageGetStr()`, `storageSet()`, `storageUnset()`, `storageExists()`, `storageDefault()`, `storageDefaultTree()`, `storageSetTree()` (cJSON node), `storageSave()` (force immediate flush), `storageCopy()` (prefix copy with optional `onlyIfTargetKeyExists`), `storageForEach()`, `storageList()`, `storageArrayCount()` (count consecutive `.0.`, `.1.`, ... entries for numbered arrays like `s.web.map.`).

**Auto-save**: `storageSet("s.xxx", val)` routes the patch via `routePatchDirty()` — if the changed path falls inside a registered external prefix, mark *that* file dirty; otherwise mark `rootDirty`. Coalescing save timer (configurable via `s.storage.flash_delay`, default 60s) flushes only the dirty files: each external writes its own sub-tree atomically (`<file>.new` + rename), and `settings.json` writes `cfgRoot` minus the externals (sub-trees temporarily detached during print). Browser sends `{"save":1}` on page unload to flush immediately.

**Array writes** (e.g. `set s.net.wifi.nets.3.pass=foo`): the patch tree builds nested objects (numeric segments become object keys). When that lands on an array in `cfgRoot`, `deepMerge` detects "object-with-numeric-keys patch into array" and merges element-wise: `null` deletes that index (with shift), object recurses into the existing element, primitive replaces, out-of-bounds extends. Same logic mirrored in browser-side `deepMerge` ([../web-interface/src/stores/device.ts](../web-interface/src/stores/device.ts)). Plain JSON arrays in patches still replace wholesale.

**Change notification**: `storageSet()` fires `storageSubscribeChanges` callbacks — targeted ITS aux messages to tasks that registered for matching key prefixes. **`storageDefault*` writes are silent** (defaults aren't real changes; firing subscriptions during first-boot install would flood subscriber inboxes).

**Browser sync (ITS server)**: owns the `storage:1` DataChannel (forwarded by `webrtc_task` from the shared PC, packet-mode, single client). On connect: sends full nested JSON dump as one packet (including externals). On changes: sends coalesced nested JSON merge-patches, one packet per flush pass, retried on back-pressure. Browser sends nested JSON patches back, one packet per `ws.send`. Protocol: `{"ping":1}`/`{"pong":1}` heartbeat, `{"save":1}` force flush. No auth/WS handshake here — `/webrtc` already gated the session.

**Factory layout**: `data/factory_state/` holds (a) loose files copied verbatim (`boot`, `crontab`, `net_up`), (b) `storage/<mode>/<prefix>.json` external blobs, and (c) optionally `settings.json` for keys that don't have a natural module owner (none currently — modules own everything). `data/additional_state/` is the per-build user overlay applied on first boot only: same layout (`settings.json` deep-merged into `cfgRoot`, plain files copied, `storage/<mode>/*.json` overlays the corresponding factory file). Both directories are flashed read-only; user-edited config lives in `/state/settings.json` after first boot.

The Quasar build also bundles a fresh `s.time.zones.json` via `scripts/update-zones.py` (ETag-conditional fetch from GitHub) — the file format is an external storage blob, not a settings.json fragment.

## Config Key Naming

Dot-notation hierarchy. Keys starting with `s.` are saved to JSON; others are ephemeral.

**Stored settings (`s.*`, `secrets.*`)** — persisted to `/state/settings.json` unless marked external. Each prefix is owned by one module which installs its defaults via `storageDefaultTree` gated on `s.<mod>.version`.

| Prefix | Owner / install site | Examples |
|--------|----------------------|----------|
| `s.camera.{img,exp,cor,hw}.*` | `cameraInit` ([camera.cpp](../main/camera.cpp)) | `s.camera.img.quality`, `s.camera.exp.aec`, `s.camera.hw.vts` |
| `s.audio.*` | `audioInit` | `s.audio.rate` |
| `s.stream.*` | `liveInit` (consumed by webrtc + rtsp + browser overlays) | `s.stream.max_fps`, `s.stream.audio.codec`, `s.stream.topleft.format` |
| `s.record.*` | `recInit` | `s.record.filename`, `s.record.auto`, `s.record.maxlen`, `s.record.add_duration` |
| `s.detect.*` | `detectInit` | `s.detect.motion.fps`, `s.detect.motion.pct`, `s.detect.audio.level` |
| `s.net.{hostname,*_port,mdns}` | `netInit` (TCP/UDP server config) | `s.net.hostname`, `s.net.http_port`, `s.net.mdns` |
| `s.net.wifi.*` | `netInit` (sub-tree of net) | `s.net.wifi.ap.ssid`, `s.net.wifi.ap.retry`, `s.net.wifi.nets[]` |
| `s.net.dns.*` | `netInit` (sub-tree of net) | `s.net.dns.fqdn` |
| `s.wg.*`, `secrets.wg.key` | `wgInit` | `s.wg.enable`, `s.wg.endpoint`, `s.wg.peer_pubkey` |
| `secrets.auth.*` | `authInit` | `secrets.auth.enable`, `secrets.auth.realms[]`, `secrets.auth.cookies[]` |
| `s.storage.*` | `storageInit` | `s.storage.flash_delay` (save coalescing, default 60s) |
| `s.upnp.*` | `upnpInit` | `s.upnp.enable`, `s.upnp.ext_port` |
| `s.duckdns.*` | `duckdnsInit` | `s.duckdns.domain`, `s.duckdns.token` |
| `s.acme.*` | `acmeInit` | `s.acme.enable`, `s.acme.url`, `s.acme.method`, `s.acme.webdir` |
| `s.ntp.*` | `ntpInit` | `s.ntp.server`, `s.ntp.tz` (IANA name), `s.ntp.posix` (cached POSIX TZ string) |
| `s.log.*` | `logInit` | `s.log.level`, `s.log.timestamp`, `s.log.dir`, `s.log.file.{name,level,interval}` |
| `s.cron.*` | `cronInit` | `s.cron.enable` |
| `s.web.{max_connections,https_only,http_allowed,mime}` | `webInit` | configurable knobs + MIME table |
| `s.web.map[]` | `main.cpp` (wiring, not module config) | URL→filesystem mappings, gated by `s.web.wiring_version` (overwrites on bump) |
| `s.sys.*` | `main.cpp` (no `sysInit`) | `s.sys.banner` |
| `s.cli.*` | `cliInit` | `s.cli.sticky`, `s.cli.start_dir` |
| `s.time.zones.*` | **External** — `/state/storage/external/s.time.zones.json` | IANA→POSIX map. Updated at build time from GitHub by `scripts/update-zones.py` |

**Ephemeral keys** — live working config, populated by `storageCopy()` on stream/record start:

| Prefix | Source | Description |
|--------|--------|-------------|
| `audio.*` | `s.audio.` + `s.stream.audio.`/`s.record.audio.` | Active audio config — consumers read from here |
| `camera.sensor` | Published by camera | Detected sensor name |
| `camera.width/height` | Published by camera | Current resolution dimensions |
| `camera.resolutions.*` | Published by camera | Available resolutions for this sensor |
| `detect.*` | Detection state | `detect.motion`, `detect.audio` |
| `dns.txtrecord` | Set by ACME for DNS-01 | DuckDNS subscribes, publishes TXT |
| `dns.txtrecord.capable` | Set by DuckDNS at init | `1` if DNS TXT API available |
| `wg.up` | WireGuard status | |
| `wg.keygen` | Set to 1 by browser to trigger key generation | WG generates key after 2s, resets to 0 |
| `webrtc.up` | WebRTC status | |
| `record.active` | Recording state | 1 while writing to SD, 0 when idle |
| `record.status` | Recording status | `REC`, `ERROR`, `FULL`, `WRITE_ERR`, `RETRY`, or empty |
| `sys.build_time` | Build version | |
| `sys.time.valid` | 1 when system time ≥ 2025 | Published by NTP |
| `sys.time.set` | Browser pushes epoch seconds | NTP accepts if time invalid, resets to 0 |

**Config copy on stream/record start** — `storageCopy(src, dst)`: audio only.
```
s.audio.          → audio.           (base audio settings)
s.stream.audio.   → audio.           (stream-specific overrides)       # webrtc / rtsp
s.record.audio.   → audio.           (record-specific overrides)       # rec_task
```
Video config is read directly from `s.camera.*` / `s.stream.max_fps` / `s.record.max_fps` by the camera module and its consumers — no ephemeral `camera.*` copy.

Defaults live with the owning module's `init()` (search the codebase for `storageDefaultTree(` to find them). Browser-side window/UI state lives in browser `localStorage` (`seccam.win.<id>`), not in device storage.

**fps convention**: positive = frames per second; negative = 1/N fps (e.g. `-3` = one frame every 3 seconds). Applies to `s.record.max_fps`, `s.stream.max_fps`, `s.detect.motion.fps`. Use `fpsToIntervalMs()` (`compat.h`) to convert.

## Patterns

### Same-value dedup + pending-flag for high-frequency writes

`storageSet()` skips the patch + change-notify entirely when the new value equals the currently committed value in `cfgRoot`. This is the general fix for notify-inbox floods from rapid browser writes (e.g. scrub bars firing at ~100/s).

For any rapid-write signal from the browser, use this pattern:

- Browser writes `value_key = X` (no subscriber on the value — it's pure state) **and** `trigger_key = 1`.
- Consumer subscribes **only to** `trigger_key`. On notify it reads `value_key`, processes, then sets `trigger_key = 0`.
- Dedup ensures repeated `trigger_key = 1` while the value is already `1` is silent. Only the `0 → 1` edge after consumption re-fires the subscriber.

Concrete example: `play.seek` (the target ms) + `play.seek_pending` (the trigger). Same shape works for any scrub / throttle / command signal where the browser may fire updates faster than the consumer can drain them.

## CLI

`set s.camera.img.quality=20` (auto-saves s.* keys), `show s.cam` (prefix match), `unset <key>`, `save` (force immediate flush), `run <file>`, `sleep <secs>`, `date [yyyymmddhhmmss]`, `date wait [timeout]`, `net` (WiFi status: SSID/IP/router/DNS/traffic), `net up|down|down!`, `usb up|down`, `detect start|stop`, `record start|stop`, `cam read [addr]` / `cam write <addr> <val>` (sensor register access, hex+decimal), `cert` / `cert self-signed` / `cert delete` / `cert acme [days]`, `wg` / `wg up|down` / `wg keygen`, `upnp`, `duckdns` / `duckdns update`, `web` (show file mappings + registered paths), `pm` / `pm wifi [none|min|max]`, `log [tag] [level]` / `log timestamp` / `log notimestamp`, `logfile [level] [path|off]`, `logrotate [days]`, `help`, `reboot` (flushes settings), `reset factory` (formats /state, copies factory defaults, reboots). Supports `#` comments and `;` for multiple commands on one line. **Silent on success**: `set`, `unset`, `save`, `detect` commands produce no output on success.
