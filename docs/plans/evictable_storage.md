# Evictable / paged storage — design exploration

> **Status: RIPENING. Not approved, not scheduled, not for now.**
> This is a captured thinking-out-loud session, not an implementation plan.
> It records the problem, the constraints we verified in the code, a candidate
> architecture, and — most importantly — the open forks still to decide.
> Circle back when it's time; do not start building from this as-is.

## 1. Why this exists (the problem)

LXMF message storage grows without bound. The Phase-F soak surfaced it as a
platform gap: inbound bodies are persisted into the RAM-backed config tree with
no retention cap, `/state` fills, and that cascades (storage writes fail →
ITS/CLI wedge → rnsd path-table watchdog panic).

The naive fix — "put messages in files like attachments" — does not work for
**messages**, only for attachments. Attachments can be an opaque blob fetched
on demand. A message has to be *visible to the browser*, and the only channel
the browser has is the synced config tree. Anything the browser must render has
to be *in the tree*. So the real question is: how do we keep thousands of
messages reachable by the browser, the way they are today, without holding them
all in RAM / flash forever.

Goal: storage handles big things; **everything keeps working the way it did**;
the change is invisible to storage consumers.

## 2. How it actually works today (verified constraints)

These are the load-bearing facts the design has to respect. All verified by
reading the code, not assumed.

- **Message layout is a flat, hash-keyed map — no per-contact grouping.**
  `s.lxmf.id.<n>.msgs.<message_id_hex64>.<field>`
  (`reticulous/main/lxmf.cpp:164-169` `msgPath`; inbound persist
  `lxmf.cpp:1491-1517`; outbound `lxmf.cpp:1227-1296`). Grouping by peer/thread
  is done by filtering the whole set in JS on the browser. There is zero
  on-device locality for "the conversation with peer X".
- **`s.lxmf` is ONE external file — `s.lxmf.json`** — for all identities, all
  peers, all messages. Any change anywhere under `s.lxmf` rewrites the entire
  file (`storage.cpp` `routePatchDirty` ~640, `writeExternalFile` ~425-432,
  write phase ~459-470). One inbound message rewrites the whole store.
- **Externals are boot-scan only.** `scanExternals()` runs once in
  `storageLoad()` (`storage.cpp:744-770`, called ~803). No runtime API to
  register/create a new external without a reboot. Nested externals *are*
  supported (longest-prefix-first sort) and dirty-route independently — the
  only missing primitive is runtime registration.
- **Browser sync = full `cfgRoot` dump on connect, then coalesced merge-patches.**
  `dcSendFullDump` (`storage.cpp:1353-1370`, on connect ~1498); patch flush
  capped ~60 KB (~1399). No subtree scoping, no per-prefix subscription. There
  is no message API, no pagination, no query — the browser gets everything and
  sorts in JS (CLI mirrors this: `lxmf.cpp:2366-2412`).
- **Persistence and sync are orthogonal axes.** Sync = "all of `cfgRoot` minus
  `secrets.*`". Persistence = "which subtrees get written to files (`s.*`)".
  A subtree can be browser-synced but not persisted, or vice versa.
- **The dominant value API already returns copies.** `storageGetStr` returns a
  `std::string` by value (`storage.h:58-62`), so eviction can't dangle a
  pointer under a caller on the common path. Only the tree-pointer surface
  (`storageSetTree`, exposed `cJSON*`) needs eviction-safety care.
- **`/state` is 128 KB LittleFS** (smaller than earlier notes said); SD is the
  realistic home for message volume. FAT cluster = 16 KB (tiny per-peer files
  waste a cluster each; fine on SD, fatal on `/state`). Streaming file I/O
  exists (`fs.h`: `fs_open_stream_read` 512 KB buf, `fs_open_stream` 16 KB) and
  is the attachment-style lazy-blob path.

## 3. Core idea: a sharded, paged, evictable subtree

Generalize the existing "external" from *one detached file* to a **directory of
lazily-paged files**:

- A prefix is marked as a *directory shard* (e.g. `s.lxmf.msgs.id.0`). Each
  immediate child (a peer) is its own file with its own cJSON object and its
  own resident lifetime.
- On access, `navigatePath` detects the shard boundary, resolves the file,
  pages it into a *separate* object (not `cfgRoot`).
- When cold, flush-if-dirty and free. The working set collapses to the
  conversations actually in use.
- The **same paging primitive applies to single-file externals** too
  (`s.time.zones`): paged in on first access, evicted when cold. One
  resident-set manager, two shard granularities (whole-external vs per-child).

General rule that falls out: **evictable ⟹ not guaranteed present in the
connect-dump ⟹ consumers must be able to pull it, never assume it's pushed.**

## 4. Layered model (keeps "works like it did" true)

Paging alone breaks the browser: if peer files aren't resident in `cfgRoot`,
they're not in the dump or the patch stream, and a fresh browser connect can't
be honored without paging *everything* in (which is the very thing we're
removing). The resolution is two layers:

- **Layer 1 — conversation index. Ephemeral, RAM-only, always resident,
  browser-synced, derived.** Per peer: last ts, snippet, unread count, a
  `resident` flag. ~100 B/peer → tens of KB for hundreds of peers; affordable
  to hold forever. It is **not persisted** — it is a projection of the per-peer
  files. Crash → it's just gone → rebuilt; it can never be corrupt or diverge,
  and it never costs a disk write. It still rides the browser sync (sync ≠
  persistence), so the inbox renders on any (re)connect exactly as today, with
  **no new protocol**. Maintained by storage as a derived change-subscriber
  (the codebase already has the change-subscription mechanism), so consumers
  just `get` the index path and never know it's derived.
- **Layer 2 — per-peer message detail. Persisted, paged in/out.** Pulled on
  open, pushed as a patch, evicted when cold. Bodies (`content`/`wire`) are
  blobs, lazy-fetched like attachments.

Boot consequence (hard, not optional): a transparent `get` of the index path
after a cold *device* boot must return populated data, or the inbox is silently
empty until each peer is touched. So the index must be **eagerly rebuilt at
boot** from small per-peer summary headers (read N tiny headers, not N
conversations) — lazy build is a correctness regression, not just latency.

## 5. The transparency invariant

Everything is driven by normal `set`/`get`. Paging, eviction, and index
maintenance live *below the get/set line*. No `ensureResident`, no special
verb in the consumer API.

- **Interface transparency: fully achievable.** Set into a paged subtree pages
  it in (or creates it) and mutates that object; the index update is an
  internal write-side trigger; eviction is background. Callers see normal
  get/set. The copy-returning value API makes this safe for free.
- **Latency transparency: impossible — and that's the one honest limit.** A
  cold `get` must page the file from disk *inside the call*. Correct shape:
  take lock → see not-resident → **release lock** → disk I/O → re-acquire →
  graft → return. The *caller* blocks; the system does not wedge. Therefore
  any caller of a get into a pageable subtree must be safe to block (off any
  lock-held / hard-timing path). That is a latency contract, not an API change.
- **Browser is unified, not special-cased.** Its get is async by nature
  (remote, over the datachannel). A cold get becomes: read index → see
  `resident:0` → write a `cmd.page_in` sentinel (same idiom as
  `cmd.send`/`cmd.delete`, which self-clear) → device pages in, grafts, flips
  `resident:1` (or `resident:err`) → patch flushes → awaited read resolves.
  Same semantics as the device-side blocking get; only the *waiting mechanism*
  differs. Failure **must** surface (`resident:err`) or the browser await
  hangs forever.

## 6. Eviction & lifetime — event-driven, never a timeout

A timeout/idle-sweep is the wrong reach (and against the standing no-polling
principle). Decompose what the timeout was doing:

- **Flush** (persist dirty): piggyback the cadence the storage task already
  uses to coalesce/flush DC patches. Dirty peer file written on the same beat
  its patches go out. No timer.
- **Free** (reclaim RAM): LRU under a resident-peer cap (paging in the N+1th
  evicts the coldest) and/or on the browser closing a conversation (open = a
  pin; close = unpin). Eviction happens on the access that overflows or the
  close signal — deterministic, no clock.
- **Pin/refcount** so an in-use subtree (CLI walk, in-flight send writing
  `stage`, an open browser view) is never yanked. Eviction only reclaims
  unpinned, already-flushed objects.
- An explicit consumer "I'm done with this subtree" signal would only ever be
  **polite** — advisory, never required, never a correctness or fragmentation
  dependency. At most an eviction-priority hint (the consumer knows "won't
  revisit"; LRU can only infer it). It does **not** fix fragmentation (see §7).

## 7. Heap fragmentation

cJSON is the fragmentation engine: a conversation is thousands of heterogeneous
small mallocs; paging in/out churns the heap with variable-sized blocks —
exactly the PSRAM failure mode where a large alloc fails with free heap
available. The polite "done" signal changes *when* you free, not the
allocate/free *pattern*, and fragmentation comes from the pattern. So the
structural fixes, in increasing order of leverage:

- **Arena-per-paged-subtree.** Know the file length → one right-sized arena
  malloc → parse the subtree into it via cJSON custom hooks
  (`cJSON_InitHooks`, bump-pointer) → on evict free the *one* arena block,
  per-node free is a no-op. Global heap only ever sees N arena blocks
  (N = working-set cap). O(1) eviction, fragmentation-free, fully transparent,
  **no advisory protocol** — it dissolves the need for the polite signal.
- **Minimize resident structured DOM.** Index as fixed-size records (flat
  array — zero fragmentation, trivial evict); bodies stay blobs; materialize a
  cJSON projection only transiently for the browser sync, into an arena freed
  the moment the patch flushes. Less structured RAM beats managing it.

## 8. The deeper option: drop cJSON/JSON as the on-disk representation

Not married to cJSON or JSON. Representation ≠ interface (same orthogonality as
sync ≠ persistence): the format lives below the get/set line, consumers and the
browser still see path get/set and JSON merge-patches.

Candidate: a custom format mapping the in-memory hierarchy — a per-level linked
list of binary nodes with **file-offset** links, plus a separate change/overlay
table searched first, periodically compacted into `file.new` and atomically
swapped.

- **Why a linked list is right here — for the actual pattern, not "search".**
  O(1) prepend + fast reverse-chronological iteration (follow head N hops) =
  exactly the inbox access pattern. It is *not* fast for point lookup by
  message-id (O(n) pointer-chase). Choose it for reverse-chron iteration; pair
  it with the RAM index for by-id.
- **Its cost is seek amplification.** Offset-chasing scattered through a file
  is random I/O — pathological on SD/FAT (16 KB clusters) unless the run is
  contiguous. So **compaction's real job is locality restoration**, not GC:
  rebuild `file.new` writing each peer depth-first contiguous so a conversation
  reads as one sequential gulp. The overlay absorbs scattered writes cheaply
  (append-only, sequential, torn-write-safe).
- **The overlay is an LSM tier, and its in-RAM searchable form *is* the
  ephemeral index** from §4 — two independent lines of reasoning converging on
  the same structure (a coherence signal).
- **Three invariants:** base file immutable between compactions; overlay
  append-only and torn-write-safe (length-prefixed + per-record checksum,
  replay until first bad record); RAM index derived from both, never
  authoritative. Base never mutated in place ⇒ only the overlay needs replay.
- **Compaction must be per-peer-shard**, not global, or it resurrects the
  whole-store write-amplification we set out to kill.
- Zero install base removes the schema-evolution/migration burden — pick offset
  width / endianness / alignment once. Only runtime defense owed: bounds-check
  every offset deref (a bad offset = wild read) + overlay checksum-replay.

## 9. Open forks — decide these when it ripens

1. **Per-peer log files vs. single content-addressed blob store.** Per-peer =
   locality for retention/pruning, needs a peer→file map. Content-addressed =
   dead simple, dedups, no per-conversation locality for pruning.
2. **Runtime external registration.** Per-peer-as-file requires creating +
   registering a new external without reboot — the single missing platform
   primitive. Without it, per-peer subtree is correct data-modelling but does
   *not* fix write-amplification or the browser dump.
3. **Boot rebuild strategy.** Per-peer summary header inside each file (keeps
   "files are the only truth" pure; rebuild reads N headers) vs. a persisted
   manifest treated explicitly as a stale-tolerant cache (faster cold boot, but
   a second thing that can lie).
4. **On-disk structure.** Navigable-on-disk (in-file offset pointers — good for
   index rebuild & partial residency, costs format complexity + random I/O +
   corruption surface) vs. dumb append-only length-prefixed records + mandatory
   RAM index as the sole access path (simpler, sequential, but RAM index
   mandatory and boot rebuild required).
5. **Fragmentation commitment.** Arena-per-subtree (keep cJSON, transparent,
   but transiently parse whole conversations) vs. fixed-size records + blob
   bodies (minimal structured RAM, representation diverges from interface).
6. **The scaling ceiling.** Full-dump-on-connect of the *index* is fine for
   thousands of conversations. Tens of thousands needs a real
   pagination/query API — a genuine break from "works like it did". Note where
   that ceiling is before committing.

## 10. Principles distilled

- Sync ≠ persistence. Representation ≠ interface. (Both let us hide the change
  below the get/set line.)
- The files are the truth; the index is derived. Never make the index
  authoritative, transactional, or persisted-as-truth.
- Interface transparency is achievable; latency transparency is not — a
  pageable get is a potential disk stall, and that's an honest contract.
- Event-driven, not timeout: flush on the coalesce beat, free on LRU/close.
- An explicit "done" signal is polite at best — never a correctness or
  fragmentation dependency.

## 11. Code anchors (so future-us resumes fast)

- LXMF message paths / persist: `reticulous/main/lxmf.cpp:164-169` (`msgPath`),
  inbound `:1491-1517`, outbound `:1227-1296`, delete sentinel `:1942-1950`,
  CLI list `:2366-2412`.
- Storage externals: `spangap/spangap-core/src/storage.cpp` — `scanExternals`
  `:744-770`, `loadExternals`/`attachAtPath` `:732-783`, dirty routing
  `routePatchDirty` ~`:636-662`, `writeExternalFile` `:425-432`, write phase
  `:459-470`, `storageLoad` ~`:783-835`.
- Browser sync: `storage.cpp` `dcSendFullDump` `:1353-1370`, connect `~:1498`,
  patch accumulate/flush `:1372-1401`, incoming merge `:1435-1471`. Browser
  side: `spangap/browser/src/stores/device.ts:244-299`.
- Storage API surface: `spangap/spangap-core/include/storage.h:58-122`
  (get/set/default/tree/unset/deleteTree, `storageForEach`,
  `storageArrayCount`). No append, no prefix wildcard.
- File I/O: `spangap/spangap-core/include/fs.h:114-221`; state-store selection
  `fs.h:70-75`; SD FAT 16 KB cluster `fs.cpp:707`.
- Existing docs: `spangap/docs/storage.md`, `unified-fs-api.md`, `its.md`,
  `webrtc.md`; `reticulous/docs/internals/lxmf.md` (schema `:573-591`, the
  unbounded-growth gap statement `:476-491`).
