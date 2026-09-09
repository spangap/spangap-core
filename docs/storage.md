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
| `storageForEach`, `storageList`, `storageArrayCount` | Iterate / dump / count numbered entries. A numbered list is an array or an object keyed `"0"`, `"1"`, … — which one depends on whether a default tree seeded it as `[]` before the first indexed write — and these count either. Seed the `[]` where the shape has a reader outside this API: the browser mirrors the tree verbatim, so a list built only from indexed writes arrives there as an object. |
| `storageNewTreeFile` | Register a runtime external file for a prefix. |
| `storageRegisterProvider` | Claim a key namespace and answer reads from your own module instead of the config tree. For structures too large, too volatile, or too lock-sensitive to mirror: the reader asks for one key and you answer it, rather than publishing everything against the chance someone asks. Dispatch happens ahead of the config mutex, so a provider read never queues behind a write. Provider namespaces live outside the config tree, so they are never saved and never appear in a dump. |
| `storagePersistBlob` | Hand a raw snapshot to the persist worker for an atomic write. For the task boundary, not convenience: a flash write stalls its task for the length of the program windows, so snapshot on your own task (where your structure is consistent) and hand over the bytes. A newer snapshot for the same path supersedes an unwritten earlier one, so a debouncing producer cannot build a backlog. |
| `storageSave` | Force an immediate flush, blocking until written. Before `storageInit()` has spawned the persist worker it flushes inline on the caller, so the early boot foundations can persist too. |
| `storageStopFlushing` | Stop persisting for the rest of this boot, one-way. For a [safe-mode](safe-mode.md) restore or factory reset, whose on-disk store is about to be replaced or erased and must not receive the stale in-RAM tree on top. |
| `storageSubscribeChanges`, `storageUnsubscribe`, `storageUnsubscribeCb`, `NOW_AND_ON_CHANGE`, `NOW_AND_ON_CHANGE_DIRECT` | Prefix-scoped change subscriptions. The callback is delivered to the registering task — which therefore must outlive boot (never subscribe from an `onInit`: app_main deletes itself). A module with no long-lived task passes `onStorageTask=true` (or uses `NOW_AND_ON_CHANGE_DIRECT`) instead: the callback then runs inline at change dispatch on the storage machinery — keep it quick, lock-free, and cycle-free (its own writes apply inline). A task may hold several subscriptions on one scope, one per module watching it; `storageUnsubscribe(scope)` drops all of them, so a module sharing a task with others (anything on the lcd task) drops its own callback with `storageUnsubscribeCb(scope, cb)` instead. |
| `uiTelemetryWanted` | Whether published stat keys have a plausible reader (LCD build, or WiFi up so a browser can pull them) — periodic publishers gate on it to skip churn on a headless, WiFi-down node. See [power-management](power-management.md#idle-discipline--park-dont-poll). |

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
| `sys.build.datetime` | `publishBuildTimes` | The catalogue run stamp `YYYYMMDDhhmmss` this image was published under. Empty when the image did not come from a catalogue run — a distinct state, not a missing value, since there is then nothing to compare it against. |
| `sys.build.dist` | `publishBuildTimes` | Which distribution this image is: the catalogue entry's name, free-format. Separate from `datetime` because the two answer different questions — *which* image this is, versus whether something newer exists. Empty outside a catalogue build. |
| `sys.build.catalogue` | `publishBuildTimes` | Which catalogue published this image — the directory name `spangap make-builds` ran in (`stable`, `dev`, a personal one). The channel, one level above `dist`: two catalogues share a stamp series but not a meaning, so a comparison against `datetime` only means something within one of them. Empty outside a catalogue build. |
| `sys.hw` | `publishBuildTimes` | Which board this **is**, as `hw-<straddle>` — the staged board straddle's own `detect_hw()` reading the hardware at the top of this boot. A running device has already proved this equals `sys.build.hw`: `confirmBoard()` halts the chip when they disagree, so the two cannot be seen apart. The device also *announces* it — `spangapLogBuildIdentity()` prints `build: hw <board>` at boot and again whenever a console attaches — so a tool learns the board without querying anything, and without resetting the device to probe the chip. Empty for the generic image, which stages no board straddle. |
| `sys.build.hw` | `publishBuildTimes` | The board straddle the image was built for, as `<org>/hw-<board>`, extracted from the invocation at build time. Empty for a board-less (generic) build. |
| `sys.buildtime.app` | `publishBuildTimes` | Firmware (app) build epoch. |
| `sys.buildtime.fixed` | `publishBuildTimes` | `/fixed` image source mtime. |
| `sys.buildtime.web` | `publishBuildTimes` | Webroot CRC32 (unset when no webroot is present). |
| `sys.flash.size` | `publishFlashGeometry` | Real chip size in bytes (SFDP). Equals `floor` when SFDP failed — not a confident chip size, so read it together with `state_size`. |
| `sys.flash.floor` | `publishFlashGeometry` | Top of the on-flash partition table: the minimum chip size this image needs. |
| `sys.flash.state_start` / `.state_size` | `publishFlashGeometry` | Where `/state` begins (the floor, 4K-aligned) and how big it is. `state_size` is `0` when no `/state` could be registered. |
| `sys.going_down` | `pm.cpp` | Set to `1` ahead of sleep/shutdown so subscribers can flush. |
| `sys.human_detected` / `.human_last_s` | `humanDetected()` | A person is at the controls: sticky `1` for the boot, plus the uptime seconds of the latest interaction. Also written by the browser on first interaction in the tab. See [init](init.md). |
| `sys.usb.serial_ports` | `usb_ports.cpp` | How many serial ports the console presents right now — `1` on USB-Serial-JTAG, `2` on `usb cdc`. A serial-port claimant subscribes to re-apply a port-1 claim across a transport switch ([usb-console](usb-console.md)). |
| `sys.time.valid` | spangap-net (NTP) | `1` once system time is sane (≥ 2025). |
| `sys.time.set` | browser | Browser pushes epoch seconds here; NTP adopts it if time is invalid, then clears it to `0`. |

Foreign ephemerals (`wg.up`, `dns.txtrecord`, `webrtc.up`, `battery.*`, …) are
owned and documented by their producing straddles.

## Self-registering defaults

Each module installs its own config block in its init, gated by an
`s.<mod>.version` key: on first boot the version is absent, the install runs and
bumps the version; later boots skip it, preserving user edits. The APIs are
`storageDefault(key,val)` (set if absent) and `storageDefaultTree(prefix,json)`
(walk a JSON literal, install each missing leaf). Both are **silent** — they fire no
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

## Timezone map (compiled in, not a storage blob)

The IANA→POSIX timezone map is platform-owned and compiled into the firmware:
two strcmp-sorted rodata arrays (`include/timezones.h`, generated
`src/timezones_gen.c`, refreshed by the release-time `make timezones` step).
It never attaches to the config tree, costs no RAM, and is looked up with a
binary search (`tzLookup`). The application logic (`ntpApplyTimezone`, the
`ntp.tz.set` command key) belongs to **spangap-net**; see the ntp docs.

## CLI

storage owns five verbs (run any on-device with `spangap cli "<command>"`):

```
set <key>[=<value>]    set a config variable; s.*/secrets.* auto-flush on the save timer
                       (`set <key> <value>` also works — a space is an equally valid
                       separator; a bare `set <key>` with no value sets 1)
reset <key>            set a config variable to 0 (shorthand for `set <key>=0`)
show [<prefix>]        print config variables (exact key, subtree, or prefix match)
unset <key>            delete a key or subtree
save                   force an immediate flush to flash, blocking until written
```

The key runs up to the first `=` or space, and spaces on either side of the
separator are ignored, so `set k=v`, `set k = v`, `set k v` and `set k =v` are
the same write. The value is everything after, and may itself contain `=` or
spaces. `set`, `reset`, `unset`, and `save` are **silent on success**.
`set fw.* …` is rejected (read-only identity). `set`/`reset` of a `s.log.*` key
also re-applies log levels. `reset factory` is a different, longer verb owned by
[fs](fs.md) — longest-prefix dispatch keeps the two apart, so `reset` never
writes a key called `factory`.

Filesystem verbs (`ls`, `cat`, `cp`, `mv`, `df`, …) and the state-store
commands (`format flash`, `format sd`, `reset factory`) belong to [fs](fs.md);
`run`/`sleep` are CLI-framework commands owned by the cli doc.
