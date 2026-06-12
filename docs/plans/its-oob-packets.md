# ITS as a mailbox, and storage as an actor

**Status:** plan. No code yet. Supersedes the earlier, narrower "out-of-band packets"
framing — same motivation, bigger and cleaner idea. Changes [ITS](../../esp-idf/include/its.h)
(`its.cpp/h`) and `storage.cpp`. Prerequisite for, and motivated by, the [Lua plan](lua.md)
and the Nomad page-render bug below.

**Revision 2:** the architecture below is unchanged; this revision adds the *implementation
contract* — the decisions, API semantics, wire formats, caller anchors, and per-step
acceptance criteria the first implementer needs. Where the contract pins something down,
it wins over looser wording elsewhere in this document.

## The one idea

ITS today carries discrete messages — a config patch, a change notification, a video frame —
inside fixed-size byte rings, one per connection, drawn from a memory pool. That makes the ring
size both a hard cap on message size and a permanent worst-case reservation, and it is the root
of three live problems: the Nomad page bug, dropped notifications, and large idle memory
reservations. The fix is to stop pretending discrete messages are streams: ITS becomes a message
system, where a task sends a self-contained message to an address and the receiver frees it, and
byte rings stay only for traffic that is genuinely a stream of bytes. Each message is a plain heap
block with its length carried beside the pointer, so memory is borrowed under load and returned on
read instead of reserved forever. The same shape serves three transports — byte streams,
per-connection packet links, and a per-task mailbox — differing only in where messages queue and
how they are addressed. On top of this, storage becomes the single owner of its data: tasks change
it by sending messages while reading stays direct under a mutex, which removes the shared write lock
and the transaction machinery. We avoided allocating message memory before for fear of heap churn,
but storage's existing JSON handling already churns the heap far harder and is fine, so that fear
was unfounded.

## Why — the problems today

Discrete messages (a config patch, a change notification, a video frame) currently ride
inside a fixed-size byte ring, one ring per connection, drawn from a memory pool. That
costs us three ways:

1. **The ring size is a hard cap on message size.** The browser config ring is 16 KB, so
   a single live config patch is capped (`DC_PATCH_MAX = 15500`, dropped with a warning
   above that). **The concrete bug:** Nomad publishes a fetched page (cap 32768) to the
   config tree for the browser to render. Any page roughly **15–32 KB** passes Nomad's cap
   but then exceeds the patch ceiling one layer down and is silently dropped — so the page
   renders empty. It doesn't recover on reload either. This is "Nomad so often returns
   nothing."
2. **Notifications drop under load.** Change notifications ride a shared fixed-depth queue.
   The ~1 Hz stats burst (>100 keys at once from many tasks) overflows it; the overflow is
   dropped, and the browser shows stale values.
3. **Memory is reserved for the worst case, forever.** Every ring is permanently sized for
   its biggest possible message, times the number of connections, whether or not that
   traffic ever flows — seccam reserves 256 KB per A/V port, storage 48 KB / 16 KB. Mostly
   idle, always held.

We avoided just allocating message memory on demand because we feared heap churn. That fear
was wrong: storage's JSON handling already churns the heap far harder than this (hundreds of
small allocations per second) and is fine. Uniform short-lived message blocks are noise next
to that.

## Three kinds of transport

After the change there are three transports, and you pick by what the traffic actually is:

1. **Byte streams** — the existing byte ring, unchanged. For continuous byte flows with no
   message boundaries: terminal sessions, raw socket-like traffic.
2. **Packet links** — a per-connection flow of whole messages. The connection holds a small
   queue of *records*, each a `{pointer, length}` to a heap block. For per-connection traffic
   that needs its own ordering and its own backpressure: the browser data channels, camera
   frames.
3. **The mailbox** — one pointer per message in the task's shared inbox. For
   connect / disconnect / handoff signaling, and for connectionless messages addressed to a
   `(task, port)`. Notifications and storage operations travel here too.

Underneath, all three discrete transports are the same shape: a small record points at a heap
block, and the receiver frees the block. The only differences are **where the records queue**
(inside one connection, or in the task's shared mailbox) and **how routing is known**
(implicit in the connection, or written in a small header).

## How the memory works

- **Every payload is a plain heap block** (from PSRAM), allocated when sent and freed once —
  on receipt, or on teardown. No pool, no size classes, no central registry of blocks.
- **A payload must be freeable with a single `free()`.** It never carries ownership of other
  allocations (no embedded cJSON trees, no pointer graphs). This is what lets the teardown
  drain free *any* queued payload without knowing its type. Anything structured is serialized
  into the block (it's one address space, so function pointers and task handles may travel as
  plain values — they are not owned).
- **All payload ownership passes through two ITS choke points** — an acquire wrapper
  (`itsSend` allocating, `itsSendOwned` adopting) and a release wrapper (delivery-free,
  `itsRecvRef` handoff, teardown drain). The wrappers maintain global live counters
  (blocks, bytes, high-water) so a leak is a number, not a hunch. See *Observability*.
  Note the counters track the **ownership boundary**, not malloc itself: `itsSendOwned`
  must adopt blocks from any allocator (storage hands over `cJSON_PrintUnformatted` output),
  and after `itsRecvRef` the consumer frees with plain `free()`.
- **The length travels next to the pointer** — in the record, or in the message header.
  `free()` does not need it: the allocator already stores each block's size just before the
  block, so freeing only needs the pointer. We keep our own length anyway, because we need it
  to copy the payload out and to count bytes in flight (and because a future pooled allocator
  would round the real size up, so only our own length is trustworthy).
- **Header vs payload is a firm boundary:**
  - The **header** is fixed-size, owned by the transport, and never looks inside the payload.
    It carries only what's needed to route, count, and free a message: a *kind*, some flags,
    the addresses, and the `{payload pointer, length}`. The mailbox-drain loop can route and
    free *any* message from the header alone.
  - The **payload** is one separate block, or none. What's inside it is the application's
    business; its own first byte can be an application-level type that the transport ignores.
    So there are two type tags at two layers — the transport's *kind* in the header, the
    application's op-type in the payload — and neither layer parses the other's.
- **One rule governs lifetime:** the transport always frees the header; the payload is freed
  by whoever holds it last. By default that's the transport, right after delivery — unless the
  receiver asks to *take* it (a zero-copy handoff, e.g. receive a message and forward it on
  without copying). Teardown walks the queue and frees both.
- **Two limits per direction:** a *count* limit (how many messages may be outstanding — this
  bounds the records) and a *byte* limit (how many payload bytes may be outstanding — this is
  the backpressure window). Idle cost is near zero; memory grows only under traffic and is
  returned the moment the receiver reads. So a limit can be generous and lazy — a 64 KB cap
  that idles at nothing, instead of a 64 KB ring reserved forever.

## Connections live in the port, not in the message

A port is declared once as either a byte stream or a packet link, with its limits. A connect
message carries only **routing** (who is connecting, to which port) plus an optional
**handshake** payload — which may be large and travels as a pointer, so a big connect is not
copied. The transport looks up the port, sees its kind, and builds the matching machinery.

Nothing about "stream vs packet" is ever written into a message — it is a property of the
**port**. The connection itself is a real object the transport owns. That ownership is what
lets later messages route straight to the right connection and feel that connection's own
backpressure. (If connections were instead just an idea expressed in payloads, every flow on a
port would fall back into one shared queue and we'd be back to head-of-line blocking and no
per-connection backpressure — so connections stay transport-owned.)

## Storage becomes an actor

- **Storage alone owns the config tree.** Other tasks change it by sending messages. *Reading*
  stays direct and fast, guarded by a mutex so a reader never races the single writer. (Making
  reads go through messages too would turn every config read into a round-trip — too slow; the
  hot read path stays direct.)
- **Writes are synchronous.** A public write call sends the op message and waits for storage to
  apply it (the existing `ITS_WAIT_PICKUP` machinery), so **read-your-writes is preserved** —
  every `storageSet(k,v); … storageGetInt(k)` sequence in the tree keeps working. When the
  *caller is the storage task itself* (browser patches arrive there), or the storage task has
  not been spawned yet (boot-time module inits), the call applies directly — mandatory, since a
  task cannot wait on its own inbox. See decision **D1**.
- **A write message is a list of leaf changes:** set key to value, or delete key (a null value
  means delete). Applying the whole list is **atomic for free**, because storage handles one
  message at a time — the message boundary *is* the transaction. So "set many keys at once" is
  just one message carrying many leaves. This deletes the write-side lock role and the public
  transaction *machinery* — but **not** the merge engine: the actor builds a patch tree from the
  op list and reuses today's `deepMerge` / dirty-routing / subscription-walk pipeline (decision
  **D3**), and `storageBegin/End` survive as thin sugar that batch ops locally (decision **D4**).
- **No "merge" or "replace" operations are needed.** Merging is simply what applying a list of
  leaves does. Replacing a subtree is "delete those keys and set the new ones" in the same
  atomic message. An array is set as one opaque leaf. The only special write is **"set default
  if unset"**, which stays because it is conditional.
- **After applying a message, storage notifies.** Each change goes to each matching subscriber
  as a message carrying `{key, value, that subscription's callback}`; the subscriber's one
  generic handler just runs it. Notification sends are **bounded, never blocking forever**
  (decision **D2**) — the warn-and-drop backstop stays, but pointer-slot mailboxes make depth
  nearly free, so it becomes unreachable in practice. Because messages are cheap and arbitrarily
  sized, the 128-byte key/value truncation disappears.
- **The browser is unchanged at its edge.** It still speaks its existing JSON patches over its
  data channel. Browser patches already arrive *on* the storage task, so with the D1 fast path
  there is no translation layer at all — `mergeIncomingPatch` just calls the internal apply.
- The config mutex **survives with a narrower job**: readers-vs-the-single-writer on `cfgRoot`,
  plus the `externals` bookkeeping (which the save worker also touches). It stays recursive —
  `cmdShow` and direct-called subscription callbacks re-enter it.

## What this fixes

- **The Nomad page renders.** A patch is a heap block, not bounded by a ring; the patch ceiling
  rises to the port's message-size guard and Nomad's cap is corrected.
- **Notifications stop dropping** under any realistic load, and the shared notify-queue depth
  knob retires. (The bounded-timeout drop path remains as a backstop — see D2.)
- **Reserved per-connection memory collapses** to near-zero idle. Payload memory is borrowed
  under load and returned on read. The big reservations (seccam 256 KB, storage 48/16 KB) are
  where the reclaim lands.
- **Connect and handshake payloads stop being truncated** (today ~320 B / inbox item size,
  `its.cpp` `maxPayload` clamps).
- **The subscription table stops racing.** Today `storageSubscribeChanges` does an unguarded
  `subCount++` from arbitrary tasks (storage.cpp:588). SUB/UNSUB as ops make the table
  storage-task-owned — race-free by construction.
- **The inbound-patch size cap disappears.** `dcPollConfig`'s static 8 KB receive buffer
  (storage.cpp:1746) silently caps browser→device patches today; `itsRecvRef` removes the
  buffer entirely.

## Decisions locked

The architecture sections above leave several semantic questions open. They are decided here;
don't relitigate them in the diff.

- **D1 — storage writes are synchronous, with a direct fast path.** Public write APIs send one
  op message with `ITS_WAIT_PICKUP` and block until applied (timeout
  `CONFIG_SPANGAP_STORAGE_OP_TIMEOUT_MS`, then warn + give up — same "can't realistically
  happen" class as today's mutex never being released). Fast path: if
  `xTaskGetCurrentTaskHandle() == storageHandle` **or** `storageHandle == nullptr` (boot, before
  `storageInit` spawns the task), apply directly under the mutex. Without the fast path the
  browser-patch path deadlocks (it runs on the storage task); without sync writes, hundreds of
  existing set-then-get sequences break silently.
- **D2 — notification sends are bounded.** CHANGED messages use a short timeout
  (`CONFIG_SPANGAP_STORAGE_NOTIFY_TIMEOUT_MS`, default 10 ms as today) and keep the
  `notify drop` warn as a backstop. Never block storage indefinitely on a subscriber: task A
  blocked in a sync write while storage blocks on A's full mailbox is a permanent deadlock.
  The real fix for drops is that a pointer-slot mailbox makes depth ~4 B/slot, so defaults rise
  (D11) and the backstop becomes unreachable.
- **D3 — the actor reuses the patch engine.** Applying an op list = build a cJSON patch tree
  from the ops, then run the existing `deepMerge` + change-walk + `routePatchDirty` +
  save-timer pipeline. That code is battle-tested (element-wise array merge, externals
  routing) and gives all-or-nothing under OOM for free — the whole patch is built before
  `cfgRoot` is touched. What dies: `txPatch`-under-global-lock, `txDepth`, `silentDepth`
  (becomes a message flag), the notify queue knob. What survives: `deepMerge`,
  `deepMergeIntoArray`, the subscription walk (now collecting into a list), `routePatchDirty`,
  externals handling, the save timer.
- **D4 — `storageBegin/End` survive as sugar.** They bracket a *task-local* op-list
  accumulator (nestable depth counter, per-task slot); `storageEnd` at depth 0 sends the
  accumulated list as one message. Callers keep their API (9 files use it: sshd, ssh_client,
  net, auth, lxmf, gps, nomad, iface-tcp, rnsd). One behavior change: **read-your-writes
  *inside* an open bracket is gone** (`resolveKey` no longer consults a patch). Audit each
  bracket for a get-of-a-key-set-earlier-in-the-same-bracket; restructure any found (read
  first, or split the bracket). Most brackets are pure write batches and need nothing.
- **D5 — both packet implementations coexist during step 2.** The transport choice lives
  inside `itsSend/itsRecv`, so "convert one port first" requires a per-port flag:
  `ITS_PACKET_LEGACY` (today's framed-bytes-in-ring) vs `ITS_PACKET` (descriptor ring + heap
  payloads). The legacy kind and the old `bool packetBased` overload are **deleted at the end
  of step 2** — they are scaffolding, not a permanent mode.
- **D6 — CHANGED is per-key, and storage's own subscriptions are direct calls.** One message
  per (changed key × matching remote subscription) — simple and correct first; batching per
  subscriber×callback is deferred until measured. Subscriptions registered *by the storage
  task* (the `""` browser-sync sub) are invoked directly during the notify phase, no message:
  this avoids self-sends entirely and, since `dcAccumulateChange` ignores `val` and re-reads
  by key, avoids copying a 32 KB Nomad page into a payload just to throw it away.
- **D7 — `itsRecvBufSize`/`itsSendBufSize` mean "largest single message" (the size guard)** on
  packet links, not the byte cap. The webrtc router sizes its shared receive buffer from
  `itsRecvBufSize` (webrtc_task.cpp:755, "grow to this port's largest packet"); if that
  returned the logical cap, the router would permanently allocate the whole backpressure
  window (256 KB for seccam) and silently un-do the headline reclaim.
- **D8 — disconnect direction is explicit in the kind.** Today it's sniffed from the payload
  length (`hdr->len == 1` vs `1+sizeof(cb)`, its.cpp:525–533 — a comment there documents the
  sshd dual-role bug this once caused). The new header has `ITS_K_DISC_FROM_CLIENT` /
  `ITS_K_DISC_FROM_SERVER` kinds and dedicated `ref`/`cb` header fields, so disconnects are
  metadata-only messages (payload NULL, no tiny allocations) and never ambiguous.
- **D9 — the byte counter is atomic and decrements on dequeue.** One `std::atomic<size_t>`
  per direction (both ends touch it, dual-core). `+= len` on successful enqueue, `-= len` when
  the receiver dequeues — for **both** `itsRecv` and `itsRecvRef`. A block held by the app
  after `itsRecvRef` is deliberately outside the window: backpressure measures transport
  occupancy, not application memory.
- **D10 — the stream-buffer pool and `no_pool` remain for byte streams only.** Packet links
  never touch the pool (payloads are per-message heap blocks). `no_pool`'s reason-to-exist
  (attribution + reclaim of transient buffers) dissolves for packet links; rnsd is its only
  user (`rnsd.cpp:4324`) and its ports go packet-link in step 2 — after which `no_pool` either
  retires or stays as a streams-only nicety; decide when converting rnsd, don't pre-decide.
- **D11 — knobs are Kconfig symbols** (project rule: no magic constants), defaults below under
  *Settings*. `CONFIG_SPANGAP_STORAGE_NOTIFY_INBOX` (Kconfig:202) retires in step 3.
- **D12 — `storageUnset` and `storageDeleteTree` unify** on one DELETE op with
  `storageDeleteTree` semantics (null-in-patch + `markExternalsDeletedUnder`). Today the only
  difference is the externals marking; unsetting a key that *is* an external prefix now drops
  the file too, which is the less surprising behavior anyway.

## Optional later: lift the browser's 64 KB message limit

Independent of everything above, the browser link still caps one message at 64 KB. A later,
optional phase changes the retransmit bookkeeping to track a message's fragments *in place*
(no copy) instead of copying each whole message into a pool, and raises the limit. Then the
full config dump becomes a single message and the dump-chunking code retires. Cheap, separate,
not required for the fixes above. **Until then, every packet-link port routed to the browser
must keep its per-message size guard ≤ 64 KB.**

## Settings (the knobs)

All new knobs are `Kconfig` symbols in `esp-idf/Kconfig` with a README row each (project
convention):

- `CONFIG_SPANGAP_ITS_INBOX_DEPTH` (default 32) — default mailbox depth in messages. Slots are
  pointers now (~4 B each), so the default rises from 8.
- `CONFIG_SPANGAP_ITS_INBOX_MSG_MAX` (default 4096) — default per-message payload guard for
  mailboxes. `itsServerInit`'s `inboxMaxMsgLen` argument becomes a per-task override of this
  guard (it no longer sizes queue items).
- `CONFIG_SPANGAP_ITS_PKT_DEPTH` (default 16) — default packet-link descriptor depth per
  direction.
- `CONFIG_SPANGAP_ITS_MSG_MAX` (default 65536) — default packet-link per-message size guard.
- `CONFIG_SPANGAP_STORAGE_OP_TIMEOUT_MS` (default 5000) — sync-write wait bound (D1).
- `CONFIG_SPANGAP_STORAGE_NOTIFY_TIMEOUT_MS` (default 10) — CHANGED send bound (D2).
- **Retired:** `CONFIG_SPANGAP_STORAGE_NOTIFY_INBOX` (step 3). No version-gate migration —
  zero users, per project policy.

Per-port byte caps, depths, and size guards are arguments to `itsServerPortOpen` (see the
contract); the Kconfig symbols are only the defaults for the `0` cases.

## Order of work

1. **Mailbox = pointers.** Make each mailbox slot a pointer to a heap message with a fixed
   header. Small and self-contained; immediately stops notify drops and removes the
   connect/handshake size limits. Establish the header layout, the ownership rule, and the
   live counters here.
2. **Packet links.** Give packet connections their own small queue of `{pointer, length}`
   records with heap payloads, a copy-receive and a take-ownership receive. Fixes the Nomad
   patch and reclaims per-connection memory. De-risk by converting **one** port first (the
   browser config port), proving it under load, then the rest; delete the legacy packet mode
   at the end.
3. **Storage actor.** Move writes to atomic leaf-list messages; keep reads direct under the
   mutex; change-notify as messages. Delete the transaction internals, the write-lock role,
   and the notify-queue knob.
4. **(Optional) Lift the 64 KB browser limit** and collapse the config dump to one message.

Each step has explicit acceptance criteria in the contract below. Build with
`spangap build reticulous/reticulous --with reticulous/hw-tdeck` from the workspace root and
verify on hardware via `spangap log -f` and the `its` status CLI (cli_cmd_sys.cpp:129).

## The discipline that matters most

Every place that throws away queued messages — reset, disconnect, port close, task teardown —
must **walk the queue and free each payload**, never just drop the records. Write the ownership
rule once at the top of the file and make every send, receive, and teardown path an obvious
instance of it. Most of ITS's past lifetime bugs were exactly this rule going unstated.

And make violations visible: the acquire/release wrappers keep live counters, and the
acceptance criterion after every soak test is **outstanding ≈ 0 at idle**.

## Rejected / deferred

- **A memory pool or size classes.** No evidence we need it; plain allocation handles arbitrary
  sizes and the heap already takes this load. A pool can drop in *behind* the message boundary
  later without touching any caller — the acquire wrapper is the seam — so the simple choice
  wins now.
- **Inlining small payloads into the header, or a freelist of headers.** Real optimizations, but
  premature. The flag bit and the pointer indirection leave the door open; don't build them yet.
- **Connections as a payload-level idea.** Breaks per-connection routing and backpressure (every
  flow on a port collapses into one queue), so the connection stays a transport-owned object.
- **Carrying cJSON pointers as payloads.** Tempting for storage ops (no serialization), but a
  cJSON tree is many blocks needing a type-specific destructor — it breaks the "any payload is
  one `free()`" rule that makes blind teardown drains safe. Ops are serialized.
- **A tagged allocator (prefix headers on payload blocks).** Would catch "took ownership and
  forgot to free", but kills adoption of foreign blocks (cJSON print output) — the flagship
  zero-copy path. The ownership-boundary counters catch everything else. A debug-only
  registry (fixed table of `{ptr, len, owner, age}` dumpable from the CLI) may be added behind
  a `#ifdef` if a leak hunt ever needs it.
- **Batched CHANGED messages (per subscriber×callback per commit).** Deferred until per-key
  messages are measured to matter (D6).

---

# Implementation contract (the nitty-gritty)

The concrete shapes, written down so the first implementer doesn't re-derive them. File/line
anchors are as of this writing; re-locate with grep if drifted.

## The message header (mailbox) and the descriptor (packet link)

The mailbox becomes a FreeRTOS queue of single pointers; each points at a fixed header the
transport owns and never looks past:

```c
enum its_kind_t : uint8_t {
    ITS_K_CONNECT,            /* payload = handshake, or NULL          */
    ITS_K_DISC_FROM_CLIENT,   /* metadata only (D8)                    */
    ITS_K_DISC_FROM_SERVER,   /* metadata only; cb = client's disc cb  */
    ITS_K_FORWARD,            /* payload = forward data                */
    ITS_K_AUX,                /* payload = aux data                    */
};

#define ITS_F_PICKUP 0x01     /* give sender's pickupSem after dispatch (replaces pickupIdx) */

struct its_msg {              /* one heap allocation; the mailbox slot points at it */
    uint8_t             kind;
    uint8_t             flags;
    uint16_t            port;
    int16_t             handle;   /* connection handle, or -1                       */
    int8_t              ref;      /* disconnects: peer's ref for the callback; else -1 */
    uint8_t             rsvd;
    TaskHandle_t        sender;   /* for replies / accept-ack / pickup              */
    its_disconnect_cb_t cb;       /* ITS_K_DISC_FROM_SERVER only; else NULL         */
    uint32_t            len;      /* payload CONTENT length (not allocation size)   */
    void*               payload;  /* separate heap block, or NULL                   */
};
```

Notes against today's code:

- `processInboxMsg` (its.cpp:452) dispatches on `kind` — the `hdr->len == 1` direction
  sniffing at its.cpp:525–533 dies (D8).
- Disconnect kicks carry `ref` + `cb` in the header — no payload allocation for the common
  teardown message.
- The `alloca(inboxItemSize)` pads die (`inboxSend` its.cpp:389, `itsPoll` its.cpp:661,
  connect/aux/forward builders). `inboxSend` allocates the header (and copies the payload into
  a fresh block, for the copying variants) and enqueues one pointer.
- `ITS_MAX_MSG_DATA` (its.h:55) stops being a hard transport limit and becomes the *default
  mailbox size-guard floor*; rnsd's `static_assert(sizeof(...) <= ITS_MAX_MSG_DATA)` walls
  (rns/include/ports.h) stay true and unmodified. rnsd.cpp:2555's explicit size check keeps
  working; lifting it is rnsd's business, later.
- web.cpp:853 (`itsServerInit(sizeof(web_file_req_t*), …)`) already passes a pointer *as* the
  aux payload; it keeps working unchanged (its 4-byte payload now rides in a tiny heap block).

A packet link doesn't need routing fields — the connection knows them — so its queue element
is just the descriptor:

```c
struct its_desc {           /* 8 bytes; lives inside the per-connection descriptor ring */
    uint32_t len;           /* CONTENT length */
    void*    ptr;           /* heap block     */
};
```

The per-connection descriptor ring is a FreeRTOS stream buffer carrying these 8-byte records
(reusing its locking and notify; always sent/received in exact 8-byte units, single writer and
single reader per direction — state this invariant in a comment). Ring size = depth × 8.
Each direction also carries `{atomic outstanding-bytes counter, byte cap, maxMsg guard}`.

Payloads are `heap_caps_malloc(MALLOC_CAP_SPIRAM)`, freed with `free()`. The allocation may be
larger than `len`; `len` is authoritative for copying and accounting.

## The ownership rule — write this first, in one place

> The transport always frees the header. The payload is freed by whoever holds it last: by
> default the transport, immediately after delivery — unless the receiver takes it. Ownership
> transfers on **success only**: a backpressure/timeout return of 0 leaves an owned payload
> with the caller. Every path that discards a queue drains it and frees each payload. Every
> ownership entry and exit goes through the counting wrappers.

## Send / receive variants

```c
size_t itsSend     (int h, const void* data, size_t len, TickType_t to); // copy: ITS mallocs + copies in
size_t itsSendOwned(int h, void* ptr,        size_t len, TickType_t to); // adopt the caller's block
size_t itsRecv     (int h, void* buf, size_t maxLen,     TickType_t to); // copy out, then free
bool   itsRecvRef  (int h, void** out, size_t* len,      TickType_t to); // hand the pointer over, caller frees

/* mailbox equivalent of itsSendOwned, for storage's pre-built op lists and CHANGED payloads: */
bool   itsSendAuxOwnedByTaskHandle(TaskHandle_t task, uint16_t port, void* ptr, size_t len,
                                   TickType_t timeout, its_wait_t wait = ITS_WAIT_DELIVERY);
```

- `itsSend` is the only path where ITS allocates, so the only one that can hit its own
  out-of-memory → returns 0, caller retries (don't-drop). Signature unchanged; every existing
  caller keeps working.
- `itsSendOwned` does no allocation; on a `0` return the caller still owns `ptr`. This is the
  zero-copy producer — storage hands its printed JSON straight over. Adoption enters the
  counters. Packet links only (streams return 0 and log).
- `itsRecv` unchanged for callers; packet mode: pops one descriptor, copies, frees. If
  `len > maxLen`: log error, free, return 0 (same contract as today's drain-and-drop).
- `itsRecvRef` is the zero-copy consumer; packet links and **nothing else** (streams return
  false). Exits the counters; the consumer uses plain `free()`.

## Flow control, re-expressed on the counter

- One atomic `size_t` per direction: `+= len` on enqueue, `-= len` on dequeue (D9).
- A send blocks (or returns 0 at timeout) if the descriptor ring is full (count) **or** the
  counter would exceed the cap (bytes). The blocking wait must wake on either a descriptor
  slot freeing or the counter dropping — reuse the `spaceFreedSem` + `freeNotify` pattern
  (its.cpp:1279, `wakeSenderIfReady`), re-pointed at "slot freed OR window opened".
- A transient full/OOM surfaces as `itsSend(timeout=0) → 0`, which the existing
  retry-don't-drop contract already absorbs (`dcFlushPatch` clears its pending patch only when
  `sent == len`).

## What every ITS query means after the change

`itsSend/itsRecv` semantics are preserved; the *query* functions are where callers silently
depend on ring behavior. Sweep every caller against this table during step 2 (anchors are the
known-sensitive ones):

| API | byte stream | packet link (`ITS_PACKET`) |
|---|---|---|
| `itsBytesAvailable` | ring bytes (unchanged) | payload bytes outstanding, recv side. **webrtc_task.cpp:273 checks `> 4`** (legacy whole-packet heuristic) → becomes `> 0`. iface drain loops (auto.cpp:435, lora.cpp:593/750, espnow.cpp:302) read `> 0` / `== 0` — fine as-is. |
| `itsSpacesAvailable` | ring free (unchanged) | **0 if no descriptor slot free**, else `cap − outstanding`. The slot clause matters: callers do check-then-send(0) (play.cpp:484, `dcFlushPatch`, `dcPumpDump`) and must not see "space" they can't use. |
| `itsRecvBufSize` / `itsSendBufSize` | ring capacity (unchanged) | the **maxMsg size guard** (D7). Anchor: webrtc_task.cpp:755 router buffer sizing. |
| `itsIsEmpty` | unchanged | no descriptors queued |
| `itsIsFull` | unchanged | a `timeout=0` send would fail (slot or window) |
| `itsSendIsEmpty` / `itsSendDrain` | unchanged | peer has dequeued everything (count 0 ∧ outstanding 0) |
| `itsSetTriggerLevel` | unchanged (only fs.cpp:482/638 uses it) | no-op, return false — packet links wake per message |
| `itsSetFreeNotify` / `itsWaitForSpace` | unchanged | wait until a slot is free ∧ window ≥ freeBytes |
| `itsInject` | unchanged (all callers are web.cpp streams: 721/1189/1203/2009) | unsupported: log + return 0 |
| `itsReset` | reset rings (unchanged) | drain-and-free both directions via the teardown helper |
| `itsRecv` cb arg `bytesAvail` | bytes (unchanged) | outstanding payload bytes — callers must not parse it as a message count (most loop `itsRecv` until 0 and ignore it; verify the few that don't) |

`dispatchRecvCallbacks` (its.cpp:607): pending = descriptor count > 0 (replaces the
`avail > 4` packet heuristic); "made progress" = **a descriptor was popped** (count decreased)
or the conn went inactive — the existing no-spin rule, restated on counts.

## Port declaration and the migration flag

```c
enum its_port_kind_t : uint8_t {
    ITS_STREAM = 0,        /* byte ring, unchanged                                  */
    ITS_PACKET_LEGACY,     /* today's framed packets in a ring — DELETED end of step 2 */
    ITS_PACKET,            /* packet link: descriptor ring + heap payloads          */
};

bool itsServerPortOpen(uint16_t port, its_port_kind_t kind, int maxHandles,
                       size_t toCap, size_t fromCap,
                       size_t depth = 0,    /* 0 = CONFIG_SPANGAP_ITS_PKT_DEPTH  */
                       size_t maxMsg = 0);  /* 0 = CONFIG_SPANGAP_ITS_MSG_MAX    */
```

For `ITS_STREAM`, `toCap/fromCap` keep today's meaning (reserved ring bytes). For
`ITS_PACKET` they are the lazy per-direction byte windows (idle ≈ 0). The existing
`bool packetBased` overload stays during step 2 mapping `false→ITS_STREAM`,
`true→ITS_PACKET_LEGACY`, and is deleted with the legacy kind once the last port converts —
end state is one signature, all ~20 call sites migrated.

`itsServerInit(inboxMaxMsgLen, …)`: `inboxMaxMsgLen` becomes the per-task mailbox payload
guard (0 = `CONFIG_SPANGAP_ITS_INBOX_MSG_MAX`); `inboxDepth` 0 =
`CONFIG_SPANGAP_ITS_INBOX_DEPTH`. The queue item is always `sizeof(its_msg*)`.

## Teardown — one drain-and-free helper

A single helper walks a descriptor ring or mailbox, frees every payload (through the release
wrapper) and header, then deletes the structure. It is the only thing allowed to discard
packet-link or mailbox contents, and every path routes through it: `itsReset`, both arms of
`itsDisconnect`, `itsServerPortClose`, task teardown. Never a raw
`xStreamBufferReset`/delete on a packet link — that leaks every block in flight. Keep the
concurrency care already in `connFree` (its.cpp:221): read-and-null the ring/queue handles
under the conn spinlock before freeing, so two racing closes can't double-free. (Tasks never
actually die today — `s_tasks` never shrinks — so mailbox-teardown drain is future-proofing;
the live paths are all connection-level.)

## Connect path

The port is declared stream or packet at `itsServerPortOpen`, with its limits. A CONNECT
message carries routing in the header and the handshake as a payload pointer, so a large
connect — JSON args, an initial request — is not copied. The transport reads the port's kind,
builds the matching machinery, runs accept/reject, assigns the handle, then hands the
handshake to `onConnect` by reference; it is freed when `onConnect` returns, or taken. The
current ~320-byte connect-arg truncation disappears.

**Known adjacent bug — decide explicitly:** a client whose `itsConnect` times out leaks a
server slot when the busy server later processes the stale CONNECT and allocates anyway
(see repo memory; the rnsd boot trigger was fixed, the nonce/cancel fix deferred). The
connect/ack path is being rewritten in step 1 — fold the fix in: the client stamps a
per-attempt nonce into the CONNECT header (use `rsvd`/a widened field), the server's
ack-give checks the client is still waiting on *that* nonce, and on a stale ack the server
side tears the just-built conn down. If it turns out to be more than ~30 lines, punt — but
then write "preserved as-is, still leaks on timeout" into the commit message so it isn't
half-fixed by accident.

## Observability

- Acquire/release wrappers maintain `{live blocks, live bytes, high-water blocks/bytes}`
  atomics for headers and payloads (separately). All entry/exit goes through them.
- `itsStatus` (printed by the CLI, cli_cmd_sys.cpp:129) adds: the global counters; per
  connection — kind, per-direction outstanding count/bytes, caps, high-water.
- `itsTaskMem` (its.cpp:837) adds outstanding payload bytes attributed to the *receiving*
  task (its mailbox + recv-direction links), so `top` stays honest. Note the raw heap-task
  tracker will attribute in-flight blocks to the sender that allocated them — accepted;
  `itsTaskMem` is the corrective lens.
- **The acceptance criterion everywhere: idle outstanding ≈ 0.** After any soak or
  reconnect-storm test, non-zero residue = an ownership bug; the high-water marks tell you
  where to look.

## Storage actor — concrete protocol

Ops travel as one mailbox message to the storage task on a new aux port
`STORAGE_OP_PORT = 44` (43 is reserved, 42 is CHANGE, 1 is the DC). Payload:

```
[u8 flags][op]*          flags: bit0 SILENT (suppress subscriptions — replaces silentDepth)

op:
  'S' SET      key\0  vtype value          vtype 'I': int32 LE
  'D' DELETE   key\0                             'S': u32 len + bytes (string)
  'd' DEFAULT  key\0  vtype value                'J': u32 len + printed JSON
  '+' SUB      scope\0  cb(void*)                     (array/object subtree — storageSetTree)
  '-' UNSUB    scope\0  cb(void*)          cb NULL = all of sender's subs on scope
  'W' SAVE     sem(SemaphoreHandle_t)      sem may be NULL
```

Keys and scopes are NUL-terminated, **unbounded** — the 128-byte key cap, the `scope[40]`
field (storage.cpp:551), and the `storage_change_msg_t` 128/128 truncation (storage.cpp:560)
all die. Validate the whole list (bounds-check every field against `len`) **before** applying
anything; a malformed list is rejected whole. Sender identity for SUB/UNSUB ownership comes
from `its_msg.sender`.

### Disposition of every public storage.h function

| Function | Becomes |
|---|---|
| `storageExists/GetInt/GetStr/GetType/ArrayCount/ForEach/List` | direct read under mutex (unchanged; `resolveKey` loses its txPatch arm) |
| `storageSet` (all overloads) | one SET op, sync (D1) |
| `storageUnset` / `storageDeleteTree` | one DELETE op (unified, D12) |
| `storageSetTree` | SET op with 'J' value (serialize, then `cJSON_Delete` the caller's tree — ownership contract unchanged for callers) |
| `storageDefault` (both) | DEFAULT op. Return value = a *pre-check* `!storageExists(key)` — benign TOCTOU on the return only; the conditional itself is resolved race-free in the actor. (Only `storageDefaultTree` consumes the return today.) |
| `storageDefaultTree` | sugar: one message of DEFAULT (+ 'J' DEFAULT for absent arrays) ops, SILENT flag set (as today via silentDepth) |
| `storageBegin/End` | sugar: task-local op-list accumulator (D4) |
| `storageCopy` | read source under mutex → compose SET ops → one message. (Loses today's source-stability during the copy; callers are init-time, acceptable — note it at the definition.) |
| `storageCopyNoNotify` | same, with SILENT flag (it is a write and must go through the actor — today it merges into `cfgRoot` from arbitrary tasks) |
| `storageSubscribeChanges` / `storageUnsubscribe` | SUB / UNSUB ops, sync — so the subscription is live on return (NOW_AND_ON_CHANGE immediately reads; order must hold) |
| `storageSave` | SAVE op carrying a semaphore; see below |
| `storageNewTreeFile` | stays a direct mutex-guarded call — `externals` has multiple writers across tasks regardless (save worker erases entries), so the mutex keeps that job |
| `storageLoad` | unchanged (boot, pre-task) |

### The apply pipeline (runs on the storage task; also the D1 direct path)

One function, `storageApplyOps(payload, len)`, used by the aux handler and by the fast path:

1. Validate the op list fully.
2. Take the mutex. Build a patch tree from SET/DELETE/DEFAULT: DEFAULT resolves against
   `cfgRoot` *now* (skip if present); SET dedups against the committed value (the skip at
   storage.cpp:931 moves here — it is load-bearing against notify floods); DELETE adds null
   **and** calls `markExternalsDeletedUnder`.
3. `deepMerge(cfgRoot, patch)` — unchanged engine, element-wise array merges included.
4. Walk the patch collecting `{key, printed value}` changes (today's
   `firePatchSubscriptions` walk, collecting instead of sending); run `routePatchDirty` +
   `startSaveTimer` as today.
5. Release the mutex.
6. Notify (unless SILENT): for each change × matching sub — storage-task subs are called
   directly (D6); remote subs get one CHANGED message, bounded timeout, warn backstop (D2).
7. Process SUB/UNSUB (table is storage-task-owned now — delete the `subs` race) and SAVE.

Notifying *after* the unlock is a deliberate ordering change (today fires under the lock);
callbacks describe committed state and direct-called ones may re-lock — keep the mutex
recursive.

CHANGED payload: `{cb (void*), key\0, val\0}` packed, heap, arbitrary length;
`storageChangeDispatch` adapts to the variable-length form. The `(key, val)` pointers passed
to subscriber callbacks are valid for the duration of the callback only — same contract as
today.

SAVE: the actor appends the op's semaphore (if any) to a pending list (under the mutex) and
pokes the save worker; the worker gives every pending semaphore after `writeSettingsFile`
completes. `storageSave()` blocks on its semaphore (generous timeout, ~30 s — FAT can be
slow). It must not be called from the storage task (nothing does today; assert it).

### Boot ordering and the browser boundary

- Fast path triggers when `storageHandle == nullptr` (module inits before `storageInit`) or
  the caller *is* the storage task. After `storageInit` spawns the task, all foreign writes
  are messages. Sweep for writers in esp_timer/event-loop contexts (none known —
  `saveTimerCb` only pokes) since they would now block up to the op timeout.
- `mergeIncomingPatch` / `dcHandleMessage` run on the storage task → their `storageSet`/null
  deletes hit the fast path automatically. Express the browser's silent null-deletes as
  SILENT DELETE ops so the whole inbound path is ops + direct apply — no second delete
  mechanism.
- Convert `dcPollConfig` (storage.cpp:1741) to `itsRecvRef` in step 2: the static 8 KB
  buffer and its silent cap die; parse with `cJSON_ParseWithLength(text, len)` (no NUL
  guarantee), then `free()`.
- `dcFlushPatch`: `DC_PATCH_MAX` (storage.cpp:1625) stops being a constant — derive it from
  the port's `maxMsg`. The drop-oversized-patch arm stays as the final backstop but should
  now be unreachable below the guard.

## Step by step, with anchors and acceptance

### 1. Mailbox = pointers

Replace the fixed inbox slot with a pointer to `its_msg` (queue item = one pointer; PSRAM
queue stays, ISR rule unchanged). `inboxSend` (its.cpp:382) mallocs header + payload copy and
enqueues the pointer; `processInboxMsg` (its.cpp:452) dispatches on `kind`, frees both via the
wrappers. Split DISCONNECT into the two kinds (D8). Replace `pickupIdx` with `ITS_F_PICKUP`.
Drop every `alloca` pad. Connect/aux/forward payloads become arbitrary-size heap blocks
(remove the `maxPayload` truncation clamps at its.cpp:951/1044/1218). Add
`itsSendAuxOwnedByTaskHandle`. Add the counters + `itsStatus`/`itsTaskMem` extensions. Add the
drain-and-free helper and route `itsServerPortClose`/teardown through it. Optional: the
connect-nonce fix (see *Connect path*). Kconfig: the two INBOX knobs.

Acceptance:
- [x] tdeck image builds and boots; `its` CLI shows the new counters (`Messages: hdr … payload …`).
- [x] No `notify drop` warns through boot + network bring-up burst (verified; live blocks/bytes
      idle ≈ the in-flight message only).
- [ ] SSH session under heavy output closes promptly on Ctrl-D (the disconnect-kick path,
      both directions — sshd is the dual-role task that bit us before). *(user spot-check pending)*
- [ ] Browser connect/disconnect storm (rapid reloads ×20): connection table returns to
      baseline, live blocks/bytes return to ~0. *(user spot-check pending)*
- [x] Web/browser works (web UI loads).

### 2. Packet links

Add `its_port_kind_t` + the new `itsServerPortOpen`; implement the descriptor ring,
counters, `itsSendOwned`/`itsRecvRef`, blocking sends, and the query-API table above.
Convert in this order, soaking each:

1. **storage:1** (storage.cpp:1821): `toCap` 49152, `fromCap` 65536, `maxMsg` ≤ 64 KB
   (browser SCTP pool limit until step 4). Convert `dcPollConfig` to `itsRecvRef`; derive
   `DC_PATCH_MAX` from `maxMsg`; raise/verify `s.nomad.max_page_publish` ≤ the new ceiling
   (nomad.cpp:217/777).
2. **log DC** (log.cpp:725) and **cli DC** (cli.cpp:1211) — small, low-risk.
3. **rnsd ×5** (rnsd.cpp:4257/4335/4346/4360/4374) and **lxmf** (lxmf.cpp:3202) — exercises
   the iface drain loops and the connect-leak scenario; decide `no_pool`'s fate here (D10).
4. **seccam live/play ×6** (live.cpp:227–229, play.cpp:696–698) — the big reclaim; set
   `maxMsg` to the real largest frame, which also shrinks the router's shared recv buffer
   via D7.

All packet ports converted to `ITS_PACKET`; the webrtc router `> 4` → `> 0` (:273) and
recv-buffer sizing (:755 → `maxMsg`) are done. **Deferred (dead-code cleanup):** removing
`ITS_PACKET_LEGACY`, the bool overload, and the now-unreachable packet-framing code in
`itsSend`/`itsRecv` — no port uses legacy mode any more (all stream ports still use the bool
overload → `ITS_STREAM`, so deleting it also means migrating those ~10 call sites). Left as a
separate, behavior-neutral follow-up to avoid destabilizing a verified-working build. The
router `pendingBuf` → `itsRecvRef` micro-opt is also deferred (one extra copy, not a bug).

Acceptance:
- [x] **A 15–32 KB Nomad page renders** — verified on hardware ("EVERYTHING LOADS").
- [x] PSRAM reclaim measured: rnsd stream-pool buffers `4 kB (18/18)` → `(7/9)` (~44 KB freed);
      device internal-DRAM low-water also went 4.75 KB → 21 KB and DMA-pool 26 B → 13.8 KB after
      moving ITS metadata tables to PSRAM.
- [ ] Seccam live view + playback — N/A (seccam is not in the reticulous build; ports converted
      in source only).
- [ ] rnsd/lxmf soak over lora/tcp/espnow ifaces; repeated link setup/teardown. *(boots + runs
      clean; extended soak is user spot-check)*
- [ ] DC reconnect storm on storage:1: dump streams, patches resume. *(user spot-check pending)*
- [x] Browser→device patch > 8 KB applies (`dcPollConfig` → `itsRecvRef`, no buffer cap).

### 3. Storage actor

Implement `storageApplyOps` + the op port + the sugar per the disposition table. Move dedup
and DEFAULT into the actor; SILENT flag replaces `silentDepth`; subs table becomes
actor-owned; CHANGED becomes variable-length heap messages; SAVE gains the semaphore reply.
Delete: `txPatch`/`txDepth` globals, the write-lock role (mutex keeps readers + externals),
`storage_change_msg_t`, `CONFIG_SPANGAP_STORAGE_NOTIFY_INBOX` (Kconfig:202). Audit the 9
`storageBegin/End` files for in-bracket read-your-writes (D4). Cross-check the
[Lua plan](lua.md): it needs begin/end batching and prefix subscriptions — both survive.

**Implemented.** `storageApplyOps` + `STORAGE_OP_PORT=44` + the sugar; dedup/DEFAULT moved
into the actor; SILENT flag replaces `silentDepth`; subs table actor-owned; CHANGED is a
variable-length heap message; SAVE carries a semaphore. Deleted: `txPatch`/`txDepth`/
`silentDepth`, `commitPatch`, the write-lock role, `storage_change_msg_t`. **D4 audit
result:** all 5 flagged `storageBegin/End` brackets are actually SAFE — the array shifts read
*higher* (not-yet-written) indices and net:679 reads a different key than it writes, so no
in-bracket read-your-write dependency exists; no restructuring needed. (`NOTIFY_INBOX` kept as
the actor's inbox-depth knob rather than retired — it still sizes the op/notify backlog.)

Acceptance (implemented; flashed — user hardware verification pending):
- [ ] set-then-get round-trips from the CLI task and from inside a subscriber callback.
- [ ] Keys/scopes > 128 chars subscribe and notify untruncated (lxmf's 64-hex segments).
- [ ] Stats burst: no drops, no truncation, browser values live.
- [ ] `save` CLI returns only after the flush; set + save + immediate reboot retains.
- [ ] Browser dump + patches + deletions echo exactly as before (single-session UI diff).
- [ ] Boot is clean: module-init `storageDefault` storms work; first browser connect dumps.

### 4. (Optional) Browser 64 KB

**Implemented (conservative variant).** The rexmit pool is already 1 MB / 256 slots, so
outbound messages up to ~1 MB are supported without the in-place rewrite. Lifted the actual
ceiling: SDP `a=max-message-size` 65536 → **262144** (webrtc_task.cpp), inbound reassembly
`MAX_MESSAGE` 65536 → 262144 (webrtc_sctp.cpp), and storage:1 `maxMsg` → 262144 (`DC_PATCH_MAX`
follows via `itsSendBufSize`). **Deferred:** the in-place fragment-bitmap retransmit (a memory
optimization — the pool already holds large messages, and PSRAM is abundant) and collapsing
the dump to one message (it streams fine in ≤14 KB chunks; one big message is riskier for no
functional gain).

## Gotchas we already named

- `dispatchRecvCallbacks`: "made progress" must mean "a descriptor was popped," not
  "descriptors are present" — a callback that backpressures without popping isn't activity,
  or `itsPoll` spins (the existing no-spin comment at its.cpp:628 explains the WDT history).
- `itsSpacesAvailable` on a packet link must return 0 when no descriptor slot is free, even
  if the byte window is open — check-then-send(0) callers depend on it.
- Ownership-on-success matches retry-don't-drop: an `itsSendOwned → 0` feeds today's stash
  paths (`dcFlushPatch`'s pending patch, the router's `pendingBuf`) — now a pointer hold.
- ISR rule unchanged: no task may send from an ISR (PSRAM is unreachable during
  cache-disabled windows); the mailbox stays PSRAM-backed and task-context only.
- Roomy allocations are fine and expected: the counter and the copy use `len`; the block may
  be larger.
- Single writer per direction stays an invariant on packet links (it's what makes the
  descriptor ring and the counter race-free); assert it in debug builds if cheap.
- Don't break the D1 deadlock triangle: storage must never *block indefinitely* sending to a
  subscriber (D2), and no public write API may be called in a way that waits on the storage
  task *from* the storage task (the fast path covers every known case; assert otherwise).
- Heap-task attribution: in-flight payloads show under the sending task until freed —
  `itsTaskMem`'s new outstanding-bytes field is the corrective lens for `top`.

## Docs to update with the code

- `its.h` banner comment (it describes rings-for-everything today) and `docs/its.md`
  (including the ISR-safety section it anchors).
- `docs/storage.md` + the `storage.h` banner (transactions, locking story, subscription
  contract).
- `esp-idf/Kconfig` + the README knob rows (project convention) for every symbol in
  *Settings*; remove the retired one.
- This plan: tick the acceptance boxes per step as they land; strike decisions that get
  revised, with one line saying why.
