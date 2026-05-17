# Storage — Config

`storage.cpp/h` — config store + browser config WebSocket (ITS server). File I/O lives in `fs.cpp/h`.

In-memory cJSON tree with hierarchical dot-notation keys. Backed by `<stateDir>/storage/root.json` (nested JSON) plus per-prefix external files under `<stateDir>/storage/<mode>/`. Thread-safe: recursive mutex protects all tree access (reads and writes from any task). Three key namespaces:
- **`s.*`** — persisted, synced to browser.
- **`secrets.*`** — persisted, **never sent to browser** (WS dump and per-key updates filtered). Browser writes to `secrets.*` are silently ignored. Used for private keys.
- **Ephemeral** (no prefix) — in-memory only, synced to browser. Lost on reboot.

## State store: flash or SD

The persisted config tree (and everything else under `<stateDir>/`) lives in **one of two real, always-available locations**, decided once at boot:

- **`/state`** — the on-flash LittleFS partition. **Always mounted**, regardless of the choice below.
- **`/sdcard/state`** — a directory on the FAT-formatted microSD. Becomes the active store **iff `/sdcard/state` exists when `fsSelectStateStore()` runs** (after `fs_mount_sd()`, before `storageLoad()`).

There is **no path rewriting and no `/state`↔SD aliasing**. `fs.h` exposes the choice as plain strings — every consumer of the state store builds paths from these, never hard-coding `FS_STATE`:

- `const char* fsStateDir()` — `"/state"` or `"/sdcard/state"`, stable for the process.
- `std::string fsStatePath(const char* sub)` — `fsStateDir() + sub` (sub starts with `/`).
- `bool fsStateOnSd()` — derived (`fsStateDir() != "/state"`); for the few callers that need the predicate (e.g. the `reset factory` guard).

Operator flow (zero install base — no migration; old config is abandoned, not converted):

- **Move config to SD:** `format sd; mv /state /sdcard; reboot` — `/sdcard/state` now exists → next boot runs from SD.
- **Fresh SD system:** `format sd; mkdir /sdcard/state; reboot` — empty dir → first-boot factory-populates it.
- **`reset factory`** = format the flash `state` partition + reboot. **Refused when booted from SD** (it would wipe the inactive flash copy, not the running SD store) — it prints how to wipe an SD system instead.
- **`format flash` / `format sd`** — CLI primitives (synchronous: the command blocks on a DRAM-stack worker until done, so scripted one-liners like `format sd; mkdir /sdcard/state; reboot` run in order). `esp_littlefs_format("state")` is label-based and **never** rewritten — "format flash" always means the on-flash partition even when booted from SD.

The root config file is **`<stateDir>/storage/root.json`** (was `/state/settings.json` — renamed so all persisted config sits under `storage/`, beside `storage/external/`). Atomic writes (`<file>.new` + rename) are **FAT-safe**: `fs_rename` is overwrite-correct everywhere — on FAT (SD) it removes an existing destination and retries, since `f_rename` (unlike LittleFS/POSIX) won't overwrite. Without this, SD-backed persistence silently failed (stale file kept, `.new` orphaned).

**Self-registering defaults** (the dominant pattern): each module installs its own config defaults and crontab entries in its `init()`, gated by an `s.<mod>.version` key. On first boot the keys aren't there → install runs → version is bumped. Subsequent boots see version up-to-date → install block skipped, user edits preserved. Add a key to a module's tree, bump version, the new key appears at next boot without disturbing existing values. APIs: `storageDefault(key, val)` (set if absent), `storageDefaultTree(prefix, json)` (walk a JSON literal, install each missing leaf), `cronDefault(schedule, command)` (append crontab line if no active-or-commented line has the same command). All three are silent — no change subscriptions fire during default install.

**External storage files** (`/state/storage/<mode>/<key.path>.json`): runtime registry. The first-level subdir under `storage/` is the **mode**, the filename's stem is the dot-path **prefix**. Today only mode `external` is implemented: file is mounted into `cfgRoot` at boot and saved to its own file when its sub-tree dirties (instead of going through `root.json`). Drop a file in `data/factory_state/storage/external/` and `scanExternals` picks it up — no compile-time registration. The big `s.time.zones` IANA→POSIX map lives this way, and `s.lxmf` (potentially large message history) is externalised the same way — both so unrelated config changes don't rewrite them (and vice versa). NOTE: externalising only changes the *file*; the sub-tree is still fully resident in the in-RAM cJSON tree, so an external is not a substitute for a true out-of-tree store when the data can grow without bound. Future modes (e.g. `flash-only/`) can be added by handling more subdirs in `scanExternals()`.

**Boot sequence**: `fs_init()` → mount `/fixed` + the on-flash `/state` (always) and start the fs workers (it no longer probes SD or does first-boot) → `fs_mount_sd()` → **`fsSelectStateStore()`**: pick `/state` vs `/sdcard/state`, and if the chosen store has no non-dot entries, `fs_factory_reset()` recursively copies `/fixed/factory_state/` into it, then `applyAdditionalState()` overlays `/fixed/additional_state/` (recursive; `settings.json` deferred to storageLoad's deepMerge) → `storageLoad()` `fs_mkdirp(<stateDir>/storage/external)`, reads `<stateDir>/storage/root.json` (may be absent → `{}`) and mounts each `storage/external/*.json` at its prefix, then on first boot deep-merges `/fixed/additional_state/settings.json` → `cronWakeupHandler()` → `pmInit()` → all module inits (each runs its `if (version < N)` install block) → `cliRunFile("/state/boot")` → `recordNotifyBootScriptDone()` → `cronPoll(true)` → `vTaskDelete`. Boot script runs after all CLI commands are registered. `/state/net_up` runs on a temp task each time **NET_EV_UPSTREAM_UP** fires (STA upstream connect, not AP-only).

**Config API**: `storageGetInt()`, `storageGetStr()`, `storageSet()`, `storageUnset()`, `storageExists()`, `storageDefault()`, `storageDefaultTree()`, `storageSetTree()` (cJSON node), `storageSave()` (force immediate flush), `storageCopy()` (prefix copy with optional `onlyIfTargetKeyExists`), `storageForEach()`, `storageList()`, `storageArrayCount()` (count consecutive `.0.`, `.1.`, ... entries for numbered arrays like `s.web.map.`).

**Auto-save**: `storageSet("s.xxx", val)` routes the patch via `routePatchDirty()` — if the changed path falls inside a registered external prefix, mark *that* file dirty; otherwise mark `rootDirty`. Coalescing save timer (configurable via `s.storage.flash_delay`, default 60s) flushes only the dirty files: each external writes its own sub-tree atomically (`<file>.new` + rename — FAT-safe, see above), and `root.json` writes `cfgRoot` minus the externals (sub-trees temporarily detached during print). Browser sends `{"save":1}` on page unload to flush immediately.

**Array writes** (e.g. `set s.net.wifi.nets.3.pass=foo`): the patch tree builds nested objects (numeric segments become object keys). When that lands on an array in `cfgRoot`, `deepMerge` detects "object-with-numeric-keys patch into array" and merges element-wise: `null` deletes that index (with shift), object recurses into the existing element, primitive replaces, out-of-bounds extends. Same logic mirrored in browser-side `deepMerge` ([../browser/src/stores/device.ts](../browser/src/stores/device.ts)). Plain JSON arrays in patches still replace wholesale.

**Change notification**: `storageSet()` fires `storageSubscribeChanges` callbacks — targeted ITS aux messages to tasks that registered for matching key prefixes. **`storageDefault*` writes are silent** (defaults aren't real changes; firing subscriptions during first-boot install would flood subscriber inboxes).

**Browser sync (ITS server)**: owns the `storage:1` DataChannel (forwarded by `webrtc_task` from the shared PC, packet-mode, single client). On connect: sends full nested JSON dump as one packet (including externals). On changes: sends coalesced nested JSON merge-patches, one packet per flush pass, retried on back-pressure. Browser sends nested JSON patches back, one packet per `ws.send`. Protocol: `{"ping":1}`/`{"pong":1}` heartbeat, `{"save":1}` force flush. No auth/WS handshake here — `/webrtc` already gated the session.

**Factory layout (consumer-supplied)**: each consumer app ships a `data/factory_state/` directory holding (a) loose files copied verbatim (`boot`, `crontab`, `net_up`), (b) `storage/<mode>/<prefix>.json` external blobs, and (c) optionally `settings.json` for keys that don't have a natural module owner (it is deep-merged on first boot, **not** copied verbatim — the live file is `storage/root.json`). A sibling `data/additional_state/` is the per-build user overlay applied on first boot only: same layout (`settings.json` deep-merged into `cfgRoot`, plain files copied, `storage/<mode>/*.json` overlays the corresponding factory file). Both directories are flashed read-only via the consumer's LittleFS image build; user-edited config lives in `<stateDir>/storage/root.json` after first boot.

Diptych ships one such blob itself: `s.time.zones.json`, an IANA→POSIX timezone map. The consumer's CMake calls `diptych-core/scripts/update-zones.py` during the build, which fetches the latest map from a GitHub-hosted source (ETag-cached so it only re-downloads when upstream changes) and drops the JSON into the merged factory-state dir. From there it gets baked into the LittleFS image like any other factory file.

## Config Key Naming

Dot-notation hierarchy. Keys starting with `s.` are saved to JSON; others are ephemeral.

**Stored settings (`s.*`, `secrets.*`)** — persisted to `<stateDir>/storage/root.json` unless marked external. Each prefix is owned by one module which installs its defaults via `storageDefaultTree` gated on `s.<mod>.version`.

### Diptych-side prefixes

| Prefix | Owner / install site | Examples |
|--------|----------------------|----------|
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
| `s.web.map[]` | consumer wiring (typically in `app_main`) | URL→filesystem mappings, gated by `s.web.wiring_version` (overwrites on bump) |
| `s.sys.*` | consumer (no `sysInit`) | `s.sys.banner`, `s.sys.ota.url` |
| `s.cli.*` | `cliInit` | `s.cli.sticky`, `s.cli.start_dir` |

### Consumer-side prefixes (examples)

Apps add their own prefixes for domain config. A camera-and-audio app, for example, might own `s.camera.*`, `s.audio.*`, `s.stream.*`, `s.record.*`, `s.detect.*`.

**Ephemeral keys** — runtime state, no `s.` prefix, never persisted:

| Prefix | Source | Description |
|--------|--------|-------------|
| `dns.txtrecord` | Set by ACME for DNS-01 | DuckDNS subscribes, publishes TXT |
| `dns.txtrecord.capable` | Set by DuckDNS at init | `1` if DNS TXT API available |
| `wg.up` | WireGuard status | |
| `wg.keygen` | Set to 1 by browser to trigger key generation | WG generates key after 2s, resets to 0 |
| `webrtc.up` | WebRTC status | |
| `sys.build_time` | Build version | |
| `sys.time.valid` | 1 when system time ≥ 2025 | Published by NTP |
| `sys.time.set` | Browser pushes epoch seconds | NTP accepts if time invalid, resets to 0 |

Consumer apps publish their own ephemerals (e.g. status flags, detection state); they show up alongside diptych's in `show` output and over the `storage:1` DC.

**Config copy** — `storageCopy(src, dst)` is the way to layer base + override config trees into a single read-only ephemeral tree at activation time. Useful when one config family (e.g. audio settings) has multiple modes that overlay on a common base. Apps that need this set up the copies themselves; diptych provides only the primitive.

Defaults live with the owning module's `init()` (search the codebase for `storageDefaultTree(` to find them). Browser-side window/UI state lives in browser `localStorage` (`diptych.win.<id>`), not in device storage.

**fps convention** (`compat.h`'s `fpsToIntervalMs()`): positive = frames per second; negative = 1/N fps (e.g. `-3` = one frame every 3 seconds). Used by any consumer that has an fps-style config knob.

## Patterns

### Same-value dedup + pending-flag for high-frequency writes

`storageSet()` skips the patch + change-notify entirely when the new value equals the currently committed value in `cfgRoot`. This is the general fix for notify-inbox floods from rapid browser writes (e.g. scrub bars firing at ~100/s).

For any rapid-write signal from the browser, use this pattern:

- Browser writes `value_key = X` (no subscriber on the value — it's pure state) **and** `trigger_key = 1`.
- Consumer subscribes **only to** `trigger_key`. On notify it reads `value_key`, processes, then sets `trigger_key = 0`.
- Dedup ensures repeated `trigger_key = 1` while the value is already `1` is silent. Only the `0 → 1` edge after consumption re-fires the subscriber.

Same shape works for any scrub / throttle / command signal where the browser may fire updates faster than the consumer can drain them. One might, for example, expose `play.seek` (target ms) + `play.seek_pending` (trigger) for a video scrubber.

## CLI

`set s.foo.bar=20` (auto-saves `s.*` keys), `show s.foo` (prefix match), `unset <key>`, `save` (force immediate flush), `run <file>`, `sleep <secs>`, `date [yyyymmddhhmmss]`, `date wait [timeout]`, `net` (WiFi status: SSID/IP/router/DNS/traffic), `net up|down|down!`, `usb up|down`, `cert` / `cert self-signed` / `cert delete` / `cert acme [days]`, `wg` / `wg up|down` / `wg keygen`, `upnp`, `duckdns` / `duckdns update`, `web` (show file mappings + registered paths), `pm` / `pm wifi [none|min|max]`, `log [tag] [level]` / `log timestamp` / `log notimestamp`, `logfile [level] [path|off]`, `logrotate [days]`, `help`, `reboot` (flushes settings), `reset factory` (formats the flash `state` partition + reboots; on next boot it factory-repopulates — **refused when booted from SD**), `format flash` (unmount/format/remount the flash `state` partition), `format sd` (reformat the SD card in place, kept mounted). Supports `#` comments and `;` for multiple commands on one line. Consumer apps register their own commands via `cliRegisterCmd`. **Silent on success**: `set`, `unset`, `save` produce no output on success.
