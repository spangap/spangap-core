# storage — internals

Maintainer reference for `storage.cpp` / `storage.h`. The
[operator guide](storage.md) covers usage; this is for changing the code
without breaking it.

## 1. Inventory: what storage owns

**Tasks**

- **`storage`** — the actor. PSRAM stack (8 KB), prio 1, core 1. Owns `cfgRoot`
  (after re-homing it onto this task), the subscription table, the single
  browser DataChannel handle, and the op-apply pipeline. Its only blocking call
  is `itsPoll`; it must never do fs I/O on its loop.
- **`storage_save`** — the persist worker. 8 KB stack, prio 1, core 1. Owns the
  blocking flash writes (`writeSettingsFile`). Not an ITS task; it blocks on
  `ulTaskNotifyTake` and flushes when poked. Spawned *before* the storage task
  so it is ready for the first save poke.

**ITS ports** (constants in `storage.h`):

| Port | Name | Role |
|---|---|---|
| 1 | `STORAGE_CONFIG_PORT` | The `storage:1` browser DataChannel (packet-mode, single client). `webrtc_task` in **spangap-web** forwards it from the shared PeerConnection. |
| 44 | `STORAGE_OP_PORT` | Config-write op-lists arrive here from foreign tasks. |
| 42 | `STORAGE_CHANGE_PORT` | Change-dispatch aux installed on every subscribing task. |
| 43 | `STORAGE_SAVE_PORT` | Reserved (saves now run on the `storage_save` worker, not a port). |

**Kconfig knobs** (defaults verified in `Kconfig`):

| Symbol | Default | Meaning |
|---|---|---|
| `CONFIG_SPANGAP_STORAGE_OP_TIMEOUT_MS` | `5000` | How long a sync write waits for the actor to apply its op message before warning. |
| `CONFIG_SPANGAP_STORAGE_NOTIFY_TIMEOUT_MS` | `10` | How long the actor blocks enqueuing one CHANGED message to a remote subscriber before dropping it. |
| `CONFIG_SPANGAP_STORAGE_NOTIFY_INBOX` | `256` | Depth of the storage task's PSRAM inbox (op messages + the `""`-sub self-sends). Sized to absorb a ~1 Hz multi-producer stats burst (>100 notifies in one drain window). |
| `CONFIG_SPANGAP_STORAGE_OP_MSG_MAX` | `196608` (192 KB) | Per-message size guard: the largest single op a foreign task can send (a big `storageSetTree`/`storageCopy` subtree, or the largest published value — Nomad page bodies run to ~128 KB). |

**Compile-time constants**: `STORAGE_NOTIFY_VAL_MAX = 512` (a cross-task CHANGED
message carries at most 512 B of the value — see §4), `STORAGE_MAX_SUBS = 128`,
`STORAGE_MAX_ACCUM = 24` (per-task bracket accumulators), `DC_DUMP_MAX = 14000`
(browser dump chunk body budget), `DC_DUMP_DEPTH = 32`. The browser DataChannel
port opens with toCap 64 KB / fromCap 256 KB / maxMsg 256 KB.

**Browser-dump synthesis**: `dcBuildDump` clones `cfgRoot`, **detaches and
deletes `secrets`**, then **synthesizes `fw.*`** (`fw.stub`/`fw.name`/`fw.banner`)
from `CONFIG_SPANGAP_FW_*` straight into the clone — so `fw.*` reaches the
browser without ever existing in `cfgRoot`. `isFw()` and `isSecret()` gate both
directions (`mergeIncomingPatch` skips both on inbound).

## 2. The tree and the recursive mutex

`cfgRoot` is one cJSON tree; cJSON's allocator is hooked to `gp_alloc` (PSRAM on
PSRAM targets) at load. `cfgMux` is a recursive mutex guarding readers against
the single actor-writer, plus the `externals` bookkeeping. Reads
(`storageGetInt`/`getStr`/`Exists`/`ForEach`/…) take the lock directly and
resolve against the committed tree — there is no pending-write overlay, so a
read never sees an op that has not yet been applied.

Path navigation (`navigatePath` / `navigateOrCreate`) parses dot-paths with a
**96-byte segment buffer** — large enough for the 64-char SHA-256 hex segments
lxmf uses in message keys. A longer segment warns and returns null (the write
silently does nothing); if you ever store longer segments, bump `seg[]` *and*
the matching `leaf[]` buffers throughout the file together. Numeric segments
index into arrays; everything else indexes objects.

`deepMerge` is RFC 7396 with one extension: a numeric-keyed object patch merging
into an existing **array** is applied element-wise by `deepMergeIntoArray` —
`null` deletes index `i` (descending order so earlier indices stay valid, with
shift), an object recurses into the existing element, a primitive replaces, an
out-of-bounds index extends (padding with null). This is what makes
`set s.net.wifi.nets.3.pass=foo` patch one array element instead of replacing
the whole array. The browser's own `deepMerge` mirrors this logic. Plain JSON
arrays in a patch still replace wholesale.

`routePatchDirty` walks the committed patch and sets dirty flags: a subtree
whose path exactly equals a registered external prefix marks **that external**
dirty (and stops descending); any primitive/array leaf under `s.`/`secrets.`
marks `rootDirty`. `externals` is kept longest-prefix-first so overlapping
prefixes route correctly.

## 3. The actor: op-list framing and apply pipeline

All writes serialize into an op-list (a `std::string` buffer) and reach the
actor one of two ways:

- **Fast path** — if the caller *is* the storage task, or storage has not
  spawned yet, `storageSubmit` applies the ops inline (no message).
- **Sync message** — otherwise the buffer is copied to a `gp_alloc` block and
  sent to `STORAGE_OP_PORT` with `ITS_WAIT_PICKUP` (or `ITS_WAIT_DELIVERY`); the
  caller blocks until the actor has applied it. Read-your-writes holds because
  the call returns only after apply.

Op-list wire format (one heap block, single leading flags byte; `bit0 = SILENT`):

```
'S' SET     key\0 vtype value     vtype 'I': int32 LE
'D' DELETE  key\0                        'S': u32 len + bytes
'd' DEFAULT key\0 vtype value            'J': u32 len + printed JSON subtree
'+' SUB     scope\0 cb(void*)
'-' UNSUB   scope\0 cb(void*)     cb NULL = all of sender's subs on scope
'W' SAVE    sem(SemaphoreHandle_t)
```

`storageApplyOps` runs in two passes: **pass 1** validates and parses the whole
list with no side effects (a malformed list is rejected whole, and any queued
SAVE sems are released so `storageSave` callers don't hang); **pass 2** applies
under `cfgMux` — builds a patch tree, dedups, `deepMerge`s it into `cfgRoot`,
collects changes, routes dirty flags + arms the save timer, then mutates the
subscription table (SUB/UNSUB), all atomically. Notifications fire **after** the
unlock so callbacks see committed state and direct-called ones may re-take the
recursive mutex.

**Dedup is load-bearing.** A SET whose value equals the committed value is
skipped entirely (no patch, no notify). This is the defence against notify-inbox
floods from rapid browser writes (a scrub bar at ~100/s). The intended pattern
for a high-frequency signal: the browser writes `value_key = X` (no subscriber)
**and** `trigger_key = 1`; the consumer subscribes only to `trigger_key`, reads
`value_key` on notify, processes, then sets `trigger_key = 0`. Repeated
`trigger_key = 1` while already `1` is deduped to silence; only the `0→1` edge
re-fires.

**Brackets.** `storageBegin`/`storageEnd` accumulate ops per-task (table of 24
`op_accum_t`) and submit one atomic message at the outer `End`. Reads inside an
open bracket see committed state, **not** the bracket's pending writes — read
before opening the bracket. A full accumulator table degrades gracefully:
writes auto-commit individually (atomicity lost, never data lost).

## 4. Change notification fan-out

`notifyChange` iterates the subscription table for each changed key:

- The storage task's own subs (the `""` browser-sync sub) are invoked
  **directly** — no self-send, and the full value is passed (the DC handler
  re-reads by key anyway).
- A remote subscriber gets a variable-length `gp_alloc`'d CHANGED message
  `{cb(void*), key\0, val\0}` sent to its `STORAGE_CHANGE_PORT` aux with a
  bounded enqueue (`CONFIG_SPANGAP_STORAGE_NOTIFY_TIMEOUT_MS`, 10 ms). On
  timeout the buffer is freed and a `notify drop: <key>=<val> → [task]` warns —
  **the write still committed; only its notification was lost.** The carried
  value is truncated to `STORAGE_NOTIFY_VAL_MAX` (512 B): notifications are
  signals, not value transport, so a handler that needs the full value re-reads
  by key. `logSafe()` sanitizes the logged key/value (folds C0/DEL to `.`,
  length-capped) because storage values can be attacker-influenced bytes (e.g. a
  fetched NomadNet page) that could otherwise drive the operator's terminal.

The subscription table is **owned by the actor** — only ever mutated on the
storage task via SUB/UNSUB ops (or at boot before the task spawns), so the
old unguarded-`subCount++` race is gone by construction. `subAdd` is idempotent
on `(task, scope, cb)`, so re-subscribing a captureless lambda from a repeating
event does not leak rows until the table fills. Task death is handled by
`storageOnTaskDeath` (called from ITS's global `vTaskPreDeletionHook` in a
scheduler critical section — no locks/alloc/logging there): it nulls the owner
handle of any sub on the dead task so `notifyChange` stops delivering into a
freed TCB. The slot stays allocated-but-inert until a later matching `subRemove`
compacts it.

## 5. Persistence

The save timer (`esp_timer`, `s.storage.flash_delay` seconds, 1 s floor) fires
`requestSave` → `xTaskNotifyGive(storage_save)`. The worker runs
`writeSettingsFile`: it processes external deletes (`fs_remove` + unregister)
and dirty externals (each writes its own subtree atomically), then rewrites
`root.json` if `rootDirty`. `root.json` is `cfgRoot` minus the external subtrees
(detached during print via `withExternalsDetached`, then reattached) minus
ephemerals (only `s` and `secrets` are serialized). All writes go through
`atomicWriteJson` (`<file>.new` + `fs_rename`) — overwrite-correct on FAT
because `fs_rename` removes an existing dest and retries (see [fs](fs.md)).

`storageSave()` is a synchronous `'W'` op carrying a binary semaphore the worker
gives after the flush; **never call it from the storage task** (it would wait on
its own actor — guarded). `{"save":1}` from the browser pokes the worker
directly.

`storageLoad` (run on `main_task`, before the storage task spawns): mkdirp
`<stateDir>/storage/external`, parse `root.json` (absent → `{}`), `scanExternals`
+ `loadExternals` (external files overwrite same-path content from `root.json`),
and on first boot deep-merge `/fixed/additional_state/settings.json`. The storage
task then **re-homes** the tree onto itself (`cJSON_Duplicate` of `cfgRoot`) so
the long-lived tree is attributed to `storage`, not the self-deleting
`main_task`.

## 6. Browser DataChannel protocol

Packet-mode, single client (the WebRTC router in spangap-web enforces
single-session + auth before any DC reaches here). On connect, the callback only
sets `dcDumpPending` and returns immediately — the dump must **not** be built or
sent in the callback, because the connect ack is withheld until the callback
returns and the browser can't drain the stream until acked; building in-callback
deadlocks the ack (client gives up after 3 s) and freezes the inbox drain.

- **Dump**: `dcBuildDump` pre-serializes the (secrets-stripped, `fw.*`-injected)
  tree into `≤ DC_DUMP_MAX` nested-JSON chunks bracketed by `{"__dump":"b"}` /
  `{"__dump":"e"}`; `dcPumpDump` streams them from the task loop, paced to
  DataChannel buffer space, never blocking. Chunks are nested objects (not dotted
  paths — announce keys are opaque hashes that may contain `.`); the browser
  deep-merges each.
- **Patches**: changes coalesce into `dcPendingPatch`; `dcFlushPatch` sends one
  packet per pass, retried on back-pressure, never dropped (a deletion is echoed
  as an explicit `null`). Patches are **held until the dump fully drains** so a
  post-snapshot change can't be overwritten by a late dump chunk.
- **Inbound**: `dcPollConfig` takes ownership of each JSON body (`itsRecvRef`,
  zero-copy), parses by length. `{"ping":1}`→`{"pong":1}`, `{"save":1}`→poke the
  worker, otherwise `mergeIncomingPatch` (null = silent delete, values =
  `storageSet`; `secrets.*` and `fw.*` skipped).

## 7. Pitfalls

- **`storageDefault*` are silent.** They install seeds without firing change
  subscriptions. A handler that must run on the seeded value won't — use
  `NOW_AND_ON_CHANGE`, which subscribes *and* applies the current value once.
- **No config-version migrations.** `storageDefault` is idempotent — the actor
  writes the key only if it is currently unset — so seeding a module's defaults
  unconditionally on init is sufficient and re-running init is a no-op. Do **not**
  add schema-version scaffolding (a `s.<mod>.*_version` key or a `*_VERSION`
  define guarding a seed block, bumped to re-seed migrated devices): there are no
  deployed devices holding an older schema to migrate, so the version gate is pure
  ceremony. To add a new default, call `storageDefault(...)` directly in the
  module's init — no version check, no companion `storageSet(..._version, N)`.
- **Secrets never leave the device.** `isSecret()` gates the dump, per-key
  patches, and inbound merges. Keep any new "must not reach the browser" key
  under `secrets.*`; do not add a second filtering path.
- **PSRAM inbox, internal sync objects.** The storage inbox is a PSRAM-backed
  ITS queue (op messages are small pointers/blocks; large values must fit
  `CONFIG_SPANGAP_STORAGE_OP_MSG_MAX`). FreeRTOS sync objects (queues,
  stream-buffers, mutexes) must **not** live in PSRAM — they trip the `S32C1I`
  spinlock assert. See the memory doc for the placement policy.
- **Never `storageSave()` or do fs I/O on the storage task.** The save worker
  exists precisely so the actor's `itsPoll` loop never blocks on flash; a single
  `proxyOp` would park the inbox drain and trigger a `notify drop → [storage]`
  storm.
- **Bump segment buffers together.** `navigatePath`, `navigateOrCreate`,
  `navigateLeaf`, the CLI `set`/`show` key buffers, and `leaf[96]` sites all
  assume ≤ 95-char segments. Changing one in isolation reintroduces the silent
  rejected-write class of bug.
- **Never hold `CFG_LOCK` across a blocking CLI/transport write.** The
  collect-then-emit shape of `storageList` and `cmdShow` is load-bearing, not a
  style choice. They snapshot the walked tree into a heap `std::string` under the
  lock (`walkTreeCollect`), release, then push it line-by-line with the lock
  dropped (`emitLines`, one `write()` per line so a packet-mode DC never sees a
  body over its cap). The CLI's own output drains over a depth-1 DataChannel whose
  consumer is the **LCD task**, and that task takes `CFG_LOCK` on a timer (the
  status-bar clock's `updateClock`, plus wifi/battery change-subscriptions calling
  `storageGetStr`/`storageGetInt`). If `show`/`storageList` held `CFG_LOCK` across
  the back-pressured send, a large output would keep the lock held long enough to
  span a status tick: the LCD task blocks on `CFG_LOCK` → stops draining the DC →
  the CLI send never completes → both wedge → watchdog reboot. Small output drains
  before any tick contends, so it never trips — which is exactly why `show s` (and
  any large collection) crashed reliably while small `show`s looked fine. Fixed
  2026-07-01. Any new command that walks `cfgRoot` and writes to the CLI must use
  the same collect-under-lock, emit-after-unlock shape — never `write()` while
  holding `CFG_LOCK`.
- **A `LoadProhibited` in cJSON / `navigatePath` / `storageGetInt` during flash
  reads is MSPI timing, not heap corruption** — marginal 80 MHz octal-PSRAM
  timing. Don't poison-hunt it.
