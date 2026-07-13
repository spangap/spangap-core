# storage — the config store

`storage` is the device's single configuration and runtime-state store: one
in-RAM cJSON tree with hierarchical dot-notation keys (`s.net.hostname`,
`rnsd.up`, `secrets.wg.key`), readable and writable from any task, persisted to
JSON on the active state store, and mirrored live to the browser over a WebRTC
DataChannel. Every straddle uses it; there is no other place device settings or
cross-task status flags live.

Storage is an **actor**: reads go directly to the tree under a recursive mutex,
but all writes funnel through a single owning task that applies them as atomic
op-list messages (build patch → RFC 7396 deep-merge → notify subscribers → arm
the save timer). Writes are synchronous — the caller blocks until applied, so
read-your-writes holds. File I/O is delegated to [fs](fs.md); storage never
touches flash on its own poll loop.

## What it does and how straddles use it

A module owns a key prefix (`s.<mod>.*`), seeds its defaults once at init, reads
config when it needs it, and subscribes to changes it cares about. Status and
telemetry flow the other way: a module publishes ephemeral keys (`rnsd.up`,
`wg.up`) that the CLI, the browser, and other modules observe. The whole tree
syncs to the browser, so a settings UI is just a binding onto storage keys.

Three persistence namespaces, decided by key prefix:

| Prefix | Persisted | To browser | Lifetime |
|---|---|---|---|
| `s.*` | yes (`storage/root.json` or an external file) | yes | durable |
| `secrets.*` | yes | **never** | durable; private keys/passwords |
| bare (no prefix) | no | yes | in-RAM only, lost on reboot |

A minimal consumer (init-time seed + live subscription; the platform
auto-inits storage, so never call `storageInit` yourself):

```c
// Seed defaults the module owns, gated by its own version key.
if (storageGetInt("s.detect.version", 0) < 1) {
  storageDefaultTree("s.detect", R"({"motion":{"fps":-2,"pct":5}})");
  storageSet("s.detect.version", 1);
}

// React to live changes (and apply the current value once).
NOW_AND_ON_CHANGE("s.detect.motion", { applyMotionCfg(); });

// Publish ephemeral status for the CLI / browser to observe.
storageSet("detect.active", 1);
```

### Browser sync

The browser receives a full nested-JSON dump on connect, then coalesced
merge-patches as keys change, and writes patches back. That transport is the
`storage:1` DataChannel; the WebRTC plumbing that forwards it lives in
**spangap-web** (`webrtc_task`), not here — storage just owns the server end of
the channel. `secrets.*` and `fw.*` are filtered out of everything sent to the
browser, and browser writes to either are ignored.

## Public API

Exact signatures and ownership/lifetime contracts are in
[`include/storage.h`](../esp-idf/include/storage.h); this is the map.

| Function(s) | Purpose |
|---|---|
| `storageGetInt`, `storageGetStr` | Read a key (with default). |
| `storageSet` (int / `const char*` / `std::string`) | Write a key. |
| `storageUnset`, `storageDeleteTree` | Delete a key / a whole subtree (the latter also drops a matching external file). |
| `storageExists`, `storageGetType` | Presence / type probe. |
| `storageDefault`, `storageDefaultTree` | Seed only-missing defaults (silent — no change callbacks). |
| `storageSetTree` | Set an arbitrary cJSON node (array/object) at a key. |
| `storageBegin` / `storageEnd` | Bracket several writes into one atomic op-list message. |
| `storageCopy`, `storageCopyNoNotify` | Prefix-copy a subtree (optionally only over existing target keys). |
| `storageForEach`, `storageList`, `storageArrayCount` | Iterate / dump / count numbered entries. |
| `storageNewTreeFile` | Register a runtime external file for a prefix. |
| `storageSave` | Force an immediate flush, blocking until written. |
| `storageSubscribeChanges`, `storageUnsubscribe`, `NOW_AND_ON_CHANGE` | Prefix-scoped change subscriptions. |

For threading rules, the op-list wire format, the change fan-out, and the
browser dump/patch protocol, see [storage-internals.md](storage-internals.md).

## Namespace ownership

Storage holds the keys of the whole device; most prefixes are owned by other
straddles, which install their own defaults and are the authority for their
keys' meanings and values. Document and change those in their owning straddle,
not here.

| Prefix | Owner |
|---|---|
| `s.net.*`, `s.net.wifi.*`, `s.net.dns.*` | spangap-net |
| `s.ntp.*` | spangap-net (NTP/timezone) |
| `s.wg.*`, `secrets.wg.*` | wg |
| `s.upnp.*` | spangap-net (UPnP) |
| `s.duckdns.*` | spangap-net (DuckDNS) |
| `s.acme.*` | spangap-net (ACME) |
| `s.web.*` | spangap-web |
| `secrets.auth.*` | core auth (`auth.cpp`) — see the auth doc |
| `s.log.*` | core logging — see the logging doc |
| `s.cron.*` | core cron — see the cron doc |
| `s.cli.*` | core CLI — see the cli doc |
| `s.rnsd.*`, `s.lxmf.*`, … | the respective network straddles |
| `s.storage.*`, `s.sys.*` | **this straddle** (below) |

## Storage variables owned by spangap-core storage

### `s.storage.*` — the store's own settings

| Key | Default | Meaning |
|---|---|---|
| `s.storage.flash_delay` | `60` | Save-coalescing delay, seconds. After a `s.*`/`secrets.*` write the flush timer arms for this long; further writes inside the window ride the same flush. Clamped to a 1 s floor. |
| `s.storage.version` | `1` | Module default-install gate (see "self-registering defaults"). Internal; not an operator knob. |

### `s.sys.*` — the two persisted platform settings

| Key | Default | Meaning |
|---|---|---|
| `s.sys.project` | `CONFIG_SPANGAP_PROJECT_NAME` | Immutable project identity, written verbatim on first boot. On every later boot a mismatch between the stored value and the compiled `CONFIG_SPANGAP_PROJECT_NAME` factory-resets `/state` and reboots — flashing a different spangap project over the same chip starts clean. Not an operator knob. |
| `s.sys.time_wait_s` | `30` (effective) | How long `waitForTime()` blocks at boot for a valid clock. `0` skips the wait outright (offline node, no time source). No default is seeded; the `30` is the in-code fallback when the key is absent. |

### `fw.*` — read-only firmware identity

`fw.*` is **not** stored config: it is synthesized into the browser dump
straight from ROM string constants, never resident in the config tree, never
persisted, never patchable. `set fw.* …` errors with *"fw.* is read-only
firmware identity"*, and browser writes to `fw.*` are ignored.

| Key | Source | Meaning |
|---|---|---|
| `fw.stub` | `CONFIG_SPANGAP_FW_STUB` (straddle.yaml `stub:`) | Short lowercase id, e.g. `reticulous`. |
| `fw.name` | `CONFIG_SPANGAP_FW_NAME` (straddle.yaml `display_name:`) | User-facing proper name. |
| `fw.banner` | `CONFIG_SPANGAP_FW_BANNER` (straddle.yaml `banner:`) | One-line slogan / description. |

(The mutable hostname is `s.net.hostname`, seeded from `CONFIG_SPANGAP_FW_HOSTNAME`
and owned by spangap-net — not part of `fw.*`.)

### `sys.*` — ephemeral platform telemetry

In-RAM only, synced to the browser, lost on reboot. Published by the platform;
modules subscribe to react to them.

| Key | Set by | Meaning |
|---|---|---|
| `sys.boot_complete` | `spangapPostAppInit` | `1` once the boot script has run and all CLI commands are registered. Modules subscribe to defer activation until customisations are in. |
| `sys.build_time` | `publishBuildTimes` | Compact build summary string `a<app> f<fixed> w<webroot>` for the 32-byte WS notify payload. |
| `sys.build.straddle` / `.version` / `.args` | `publishBuildTimes` | The `spangap build` invocation identity (straddle name, version, flags). |
| `sys.buildtime.app` | `publishBuildTimes` | Firmware (app) build epoch. |
| `sys.buildtime.fixed` | `publishBuildTimes` | `/fixed` image source mtime. |
| `sys.buildtime.web` | `publishBuildTimes` | Webroot CRC32 (unset when no webroot is present). |
| `sys.going_down` | `pm.cpp` | Set to `1` ahead of sleep/shutdown so subscribers can flush. |
| `sys.time.valid` | spangap-net (NTP) | `1` once system time is sane (≥ 2025). |
| `sys.time.set` | browser | Browser pushes epoch seconds here; NTP adopts it if time is invalid, then clears it to `0`. |

Foreign ephemerals (`wg.up`, `dns.txtrecord`, `webrtc.up`, `battery.*`, …) are
owned and documented by their producing straddles.

## Self-registering defaults

Each module installs its own config block in its init, gated by an
`s.<mod>.version` key: on first boot the version is absent, the install runs and
bumps the version; later boots skip it, preserving user edits. The APIs are
`storageDefault(key,val)` (set if absent), `storageDefaultTree(prefix,json)`
(walk a JSON literal, install each missing leaf), and `cronDefault(schedule,cmd)`
(append a crontab line if absent). All three are **silent** — they fire no
change subscriptions, since first-boot seeding would otherwise flood subscriber
inboxes. The `s.<mod>.version` gate is purely a code mechanism; this project
runs no config-version migrations, so do not treat version-bumping as a
user-facing feature — adding a key and bumping the version simply makes the new
key appear at next boot beside untouched existing values.

## External storage files

A subtree can be persisted to its own file under
`<stateDir>/storage/external/<prefix>.json` instead of bloating `root.json`, so
a chatty subtree (one contact's message history) rewrites only its own small
file. (On disk the file is actually written gzip-compressed as
`<prefix>.json.gz` — as is `root.json` — but a plain hand-placed `.json` is
read fine and converted on the next flush.) Drop a file in
`data/factory_state/storage/external/` and it is picked up
at boot with no compile-time registration; or call `storageNewTreeFile("s.foo")`
at runtime (RAM-only — safe from `itsPoll`-serving tasks; the physical file is
created on the next flush when a key under the prefix dirties it). Deleting the
prefix (via `storageDeleteTree`) removes and unregisters the file on the next
flush. An external only changes *where the file is*: the subtree is still fully
resident in the in-RAM tree and still syncs to the browser, so it is not a
substitute for a true out-of-tree store for unbounded data.

## Timezone map (a loose factory file, not a storage blob)

The IANA→POSIX timezone map `timezones.json` is platform-owned but ships as a
*loose* factory file at the root of the state store (`<stateDir>/timezones.json`,
not under `storage/`), so the ~15 KB map never attaches to the config tree or
costs steady-state RAM. It is parsed transiently — cJSON parse, read one POSIX
string, free — only on a timezone change. The parsing and refresh logic
(`ntpApplyTimezone`, the build-time `make timezones` step) belong to
**spangap-net**; see the ntp/net docs for specifics.

## CLI

storage owns four verbs (run any on-device with `spangap cli "<command>"`):

```
set <key>=<value>      set a config variable; s.*/secrets.* auto-flush on the save timer
                       (`set <key> <value>` also works — a space is an equally valid separator)
show [<prefix>]        print config variables (exact key, subtree, or prefix match)
unset <key>            delete a key or subtree
save                   force an immediate flush to flash, blocking until written
```

`set`, `unset`, and `save` are **silent on success**. `set fw.* …` is rejected
(read-only identity). `set` of a `s.log.*` key also re-applies log levels.

Filesystem verbs (`ls`, `cat`, `cp`, `mv`, `df`, …) and the state-store
commands (`format flash`, `format sd`, `reset factory`) belong to [fs](fs.md);
`run`/`sleep` are CLI-framework commands owned by the cli doc.
