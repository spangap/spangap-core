# storage — internals

Maintainer reference for `storage.cpp` / `storage.h`. The
[operator guide](storage.md) covers usage; this is for changing the code
without breaking it.

## 0. Structured record stores (`storage_db.*` + routing in `storage.cpp`)

Large homogeneous collections (LXMF message history first) no longer live in the
cJSON tree. They move to packed fixed-schema record stores — `storage_db.cpp` is
the value-agnostic engine, `storage.cpp` owns the routing/notify/registry glue.
Design rationale: [`plans/storage-structured-db.md`](../../plans/storage-structured-db.md).

**Engine (`storage_db.h/.cpp`).** A store is one contiguous PSRAM arena
(`[16 B file header][record][record]…`) plus a `key→offset` index. A record is
`[u32 rec_len][u8 flags][fixed fields…][u16 keyLen,key][u16 len,text]…`. Field
kinds: `SDB_U8`/`SDB_U32`/`SDB_FIXSTR` are fixed-width and **mutate in place**
(the hot path — a stage transition is one memcpy at a known offset);
`SDB_TEXT` is length-prefixed and immutable (a changed text field rebuilds the
record: overwrite in place if the new size matches, else tombstone + append).
`flags` bit0 is a RAM-only tombstone; the next flush drops tombstoned records
(the flush **is** the compaction) and rebuilds the index. On disk the block is
gzip-wrapped (own binary codec — storage.cpp's is text/NUL-oriented); load =
gunzip + validate `SGDB` magic/schema-id/hdr_size + walk to rebuild the index.
Ephemeral stores (`cap_records > 0`) drop the oldest records past the cap.
Unit-tested host-side (create/mutate/rebuild/tombstone/compact/cap/round-trip).

**Growth & OOM reclaim.** `ensureCap` doubles the arena, but on a fragmented
heap the large contiguous `realloc` can fail even with ample total free — so it
retries at just the page-rounded size needed before giving up, and warns once
per failed episode with the free/largest-block figures (`grow_failed` gates the
spam). When an append can neither fit nor grow, `reclaimForAppend` runs without
any large allocation: if tombstones are scarce (live ≳ 90% of `cap`) it relocates
the live set into a fresh +20% block (a new allocation can land where an in-place
grow could not); otherwise `sdbCompactInPlace` streams live records through an
8 KB PSRAM scratch and drops tombstones in place, patching offsets as records
move (no index rebuild). This is a RAM-path compaction distinct from the
flush-time one above.

**Registration + registry.** `storageStructuredDB(name, keyPattern, schema, opts)`
registers a store; the owner keeps the `sdb_schema` alive (a static). The
declaration is also published to the ephemeral, browser-synced, **firmware-only**
`storage.db.<name>` registry (schema/pattern/file) so the browser can decode
records; `isStorageDb()` gates it out of inbound patches like `fw.*`. LXMF
registers `s.lxmf.id.$.msgs.$` → `lxmf/msgs/$1/$2.db.gz` in `LxmfService::onInit`.

**Routing.** `sdbRoute()` matches a dot-key against each registration's pattern
(`$` = wildcard bind); the tail after the matched pattern is 0 segs (instance),
1 (record), or 2 (`record.field`). A cheap `litPrefix` strncmp precedes the
`splitDots` allocation so ordinary config reads pay almost nothing. The strncmp
runs over `min(key_len, litPrefix_len)`: a key that IS the bare instance prefix
(a zero-wildcard store — `lxmf.announces` — iterated at exactly its pattern) is
one char shorter than `litPrefix`'s trailing dot and must still be a candidate;
over-matching is harmless, the full segment compare rejects any real mismatch.
Matching keys are served from the store, **not** cfgRoot:

- **Reads** (`storageGetStr/Int/Exists/ForEach`) try `sdbGetLocked` /
  `sdbExistsLocked` / `sdbForEachUnderLocked` first, under `CFG_LOCK`.
  `storageForEach` over a whole-identity prefix (`…id.N.msgs`) enumerates
  instances from the resident set + a disk scan.
- **Writes** are intercepted in `storageApplyOps` pass 2 (`sdbApplyOpLocked`)
  before they reach the cfgRoot patch. The store applies the op and appends the
  synthesized change to the same `changes` vector, so the post-unlock notify
  fan-out is identical to a cfgRoot write — **that** is what keeps the LCD
  `s.lxmf.id` subscription and the browser updating (the plan's crux seam). A
  routed write arms the save timer via `routedDirty`.

All store access is serialized by `CFG_LOCK` (the same recursive mutex guarding
cfgRoot): the resident block only mutates with the lock held, so a reader holding
it is safe from writes and from block relocation (the moving-block footgun).

**Flush.** `storageDbFlushDirty()` runs on the persist worker (in
`writeSettingsFile`): snapshot each dirty instance's raw block under `CFG_LOCK`
(`sdbSnapshotRaw`), then deflate + atomic-write lock-free — the `writeExternalFile`
discipline (never hold the lock across a deflate/flash write). Instance-file
unlinks are deferred to `g_sdbPendingDeletes`, drained here (fs I/O off the actor).

**Browser sync (three layers).** (1) The connect dump is cheap because bodies
aren't in cfgRoot. (2) On open, the browser sends `{"fetch":"<instance-prefix>"}`;
`dcShipStorePrefix` ships that instance's records as one merge-patch at the
instance path (where the browser's message code already reads) and records
`dcOpenPrefix`. (3) `dcAccumulateChange` mirrors a routed body change **only**
when it's under `dcOpenPrefix` (reading the value from the store, not cfgRoot);
other conversations are represented only by their directory entry.
*Note:* this ships records as JSON, not the plan's raw `.db.gz` cold-ship — a
correctness-first realization; the raw-bytes speedup is a future optimization.
(4) **Browser-mirrored stores** (`opts.browserMirror`) are small always-needed
collections — a directory or catalogue, not a body — so they behave like a
cfgRoot subtree used to: the browser fetches each once (same `{"fetch":…}` →
`dcShipStorePrefix`, but it does **not** claim `dcOpenPrefix`), and
`dcAccumulateChange` mirrors **every** change to them, not just while open. So a
mirrored store stays live in the browser without being the open instance. LXMF's
contacts + announces set this; message bodies do not.

**Conversation directory (LXMF-side, maintained).** `contacts.<peer>.{count,
last_ts,preview,unread,display_name,…}` is now its own **browser-mirrored**
record store (`s.lxmf.id.$.contacts` → `lxmf/contacts/$1.db.gz`), one record per
peer, bumped by `bumpConvDirectory` as messages are written; `unread` is a
maintained counter cleared by `convMarkRead` (and by the browser on open), not
derived by walking messages. The browser conversation list and the LCD list read
it — O(conversations). It left cfgRoot so it no longer rides the config
save/deflate, but stays browser-visible via the mirror. Announces
(`lxmf.announces`) are likewise a store now: a RAM-only (`persist=null`),
self-capped (`STORAGE_DB_DROP`, `s.lxmf.max_announces`), browser-mirrored
catalogue — one record per heard dest (`last`/`hops`/`cost`/`ratchet`/`name`).

**Migration (`storageDbMigrate`, one-way, crash-safe).** Run once from
`LxmfService::onInit` after registration, guarded by `storage/external.old`.
Walks cfgRoot per pattern, packs each record subtree into its store, detaches the
subtree from cfgRoot, collapses the legacy externals into `root.json`
(`externals.clear()` + `rootDirty`), `storageSave()` (durable), then renames
`storage/external → storage/external.old` (done-marker + recovery backup).
Subsumes `migrateLxmfMonolith`. A crash before the rename re-runs it next boot.

`storageDbMigrateStore(name)` migrates **one** store, independent of the global
done-marker: for a store added *after* the initial split already ran (its
`external.old` marker is present), so the global migrate would early-return and
strand the store's existing cfgRoot data. Guarded by its own
`storage/migrated-<name>` marker; packs the matching subtree, detaches it,
`storageSave()`, then writes the marker. LXMF calls it for `lxmf_contacts` so a
device that already migrated messages still packs its existing contacts.

**Known follow-ups (not yet done):** idle eviction of resident instances (they
stay resident once loaded — bounded by session breadth, not total history);
raw-`.db.gz` cold-ship (bodies are shipped as JSON on demand instead). *Done
since:* the conversation directory (contacts) and the announce catalogue both
moved out of cfgRoot into browser-mirrored stores (announces RAM-only + capped).
*Done since:* the `wire` RAM-outbox (packed outbound bytes live in lxmf's
`g_wireOutbox`, not the record — schema v2 dropped the field; a resend reuses the
cached wire instead of re-packing + re-stamping); drafts are already transient
(browser keeps them client-local, never a store record).

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
marks `rootDirty`. `externals` is kept **shortest-prefix-first** — a *load*-order
invariant (§5), not a routing one: `routePatchDirty` exact-matches the patch
path at each depth, so vector order never changes which external a write
dirties.

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
`requestSave` → `xTaskNotifyGive(storage_save)`. It is **armed once**: the first
dirtying write after a flush starts the countdown and later writes ride the same
window without resetting it (`startSaveTimer` returns early while `savePending`).
Resetting on every write starved the flush on a busy device — background traffic
(rnsd stats, announces, per-message directory bumps) wrote more often than
`flash_delay`, pushing the deadline out forever so nothing persisted despite
ample wall-clock time. `savePending` is cleared at the **start** of
`writeSettingsFile`, so a change during a flush re-arms for the next window. The worker runs
`writeSettingsFile` in a **crash-safe order**: dirty externals first (each
writes its own subtree atomically), then `root.json` if `rootDirty`, and only
*then* external deletes (`fs_remove` of both format siblings + unregister) —
data is never deleted before its replacement is durable. `root.json` is
`cfgRoot` minus the external subtrees (detached during the snapshot via
`withExternalsDetached`, then reattached — a `pendingDelete` external is
skipped there, so its content persists via `root.json`) minus ephemerals (only
`s` and `secrets` are serialized).

Both writers follow the dump-builder lock shape: only the `cJSON_Duplicate`
snapshot holds `CFG_LOCK`; `cJSON_PrintUnformatted` runs lock-free. Printing a
large conversation external under the lock blocked the actor's
`storageApplyOps` (port-44 drain, browser ping→pong) for the whole serialize —
long enough to time out every task's sync write and trip the browser's 2 s DC
liveness. Compact output also shrinks the flash write itself.

All writes go through `atomicWriteJsonGz` → `atomicWriteFile` (`<file>.new` +
`fs_rename`) — overwrite-correct on FAT because `fs_rename` removes an existing
dest and retries (see [fs](fs.md)). It writes in `STORAGE_FLUSH_CHUNK` (8 KB)
pieces with a tick's sleep between them: each flash program/erase op disables
the PSRAM cache, and one large `fs_write` keeps those windows back-to-back for
the whole file, starving every PSRAM-stack task (actor, rnsd, lxmf, ifaces) for
its duration. The inter-chunk tick is what keeps the actor responsive while a
large external flushes.

**Files are persisted gzip-compressed** (`<name>.json.gz`, ROM miniz
tdefl/tinfl + `esp_rom_crc32_le` for the footer): JSON compresses ~5–10×, and
since the fs worker's flash write windows are the system's stall source, a
smaller file shortens the blocking write far more than deflate costs on the
background save worker. Reads (`readJsonFile`) accept both `<base>.json.gz`
and plain `<base>.json` (legacy files, hand-placed files, factory seeds; a
corrupt `.gz` falls back to the plain sibling); writes only ever produce the
`.gz` and remove the plain sibling so a stale format can't shadow newer data.
The miniz one-shot entry points are deliberately avoided — they put an ~11 KB
decompressor on the caller stack or allocate from the ROM heap — state is
`gp_alloc`'d (PSRAM) and transient instead. Callers always name the base
`.json` path; only the read/write helpers know about the `.gz` sibling.
`storageNewTreeFile` refuses a prefix whose basename would exceed
`fs_dirent_t`'s 64-byte name (it would be re-registered *truncated* on the
next boot's rescan); the subtree then simply persists via `root.json`.

`storageSave()` is a synchronous `'W'` op carrying a binary semaphore the worker
gives after the flush; **never call it from the storage task** (it would wait on
its own actor — guarded). `{"save":1}` from the browser pokes the worker
directly.

`storageLoad` (run on `main_task`, before the storage task spawns): mkdirp
`<stateDir>/storage/external`, parse `root.json` (absent → `{}`), `scanExternals`
+ `loadExternals` (external files overwrite same-path content from `root.json`),
and on first boot deep-merge `/fixed/additional_state/settings.json`. Externals
load **shortest-prefix-first**: a parent external (`s.lxmf`) must attach before
a deeper one (`s.lxmf.id.0.msgs.<peer>`) so the deeper file's newer content
overwrites its slice of the parent — `attachAtPath` replaces the whole node at
the prefix. The storage task then **re-homes** the tree onto itself
(`cJSON_Duplicate` of `cfgRoot`) so the long-lived tree is attributed to
`storage`, not the self-deleting `main_task`.

**Legacy `s.lxmf` monolith split** (`migrateLxmfMonolith`, end of
`storageLoad`): a monolithic `s.lxmf` external predates lxmf's per-conversation
`storageNewTreeFile` registration and would otherwise persist forever —
`scanExternals` resurrects registrations from filenames, so nothing retires it
and every message write rewrites the whole file. The one-time split registers a
dirty external per non-empty conversation, marks the monolith `pendingDelete`
and sets `rootDirty` (contacts/identity fall through to `root.json` because
`withExternalsDetached` skips `pendingDelete` entries); `storageInit` kicks a
save if any migration dirt is pending. Crash-safe end to end: deletes flush
last, and until the monolith file is actually removed a reboot just re-runs the
split — with both generations on disk, the load order above makes the deeper
per-conversation files win. A conversation whose prefix would overflow the
64-byte dirent name is left unsplit (it persists via `root.json`).

## 6. Browser DataChannel protocol

Packet-mode, single client (the WebRTC router in spangap-web enforces
single-session + auth before any DC reaches here). On connect, the callback only
sets `dcDumpPending` and returns immediately — the dump must **not** be built or
sent in the callback, because the connect ack is withheld until the callback
returns and the browser can't drain the stream until acked; building in-callback
deadlocks the ack (client gives up after 3 s) and freezes the inbox drain.

- **Dump**: built **off-actor** by a one-shot `storage_dump` worker (core 0,
  spawned by `dcPumpDump` when `dcDumpPending`): `dcBuildDumpInto` pre-serializes
  the (secrets-stripped, `fw.*`-injected) tree into `≤ DC_DUMP_MAX` nested-JSON
  chunks bracketed by `{"__dump":"b"}` / `{"__dump":"e"}`; the actor adopts the
  finished queue on its next pass (a generation counter, bumped on connect and
  disconnect, discards a build whose session died mid-build). Off-actor because
  the build — `cJSON_Duplicate` + serialize over a tree bloated by saved
  announces / Nomad page bodies — runs ~1 s, and inline it made the actor deaf
  (no ping→pong, no port-44 drain) long enough to trip the browser's 2 s
  liveness check on every connect: a connect→dump→abort flap loop. Only the
  clone holds `CFG_LOCK`; serialization is lock-free. `dcPumpDump` then streams
  the chunks from the task loop, paced to DataChannel buffer space, never
  blocking. Chunks are nested objects (not dotted
  paths — announce keys are opaque hashes that may contain `.`); the browser
  deep-merges each.
- **Patches**: changes coalesce into `dcPendingPatch`; `dcFlushPatch` sends one
  packet per pass, retried on back-pressure, never dropped (a deletion is echoed
  as an explicit `null`). It gates on `itsSpacesAvailable` **before**
  serializing: against a back-pressured or mid-teardown browser it must not pay
  `cJSON_PrintUnformatted` of a potentially huge patch on every 10 ms pass. Patches are **held until the dump fully drains** so a
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
