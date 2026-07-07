# ITS — internals

Maintainer reference for the ITS layer. The [operator guide](its.md) covers the
API and concepts; this document is for changing the implementation without
breaking it. Source: [`src/its.cpp`](../esp-idf/src/its.cpp), header
[`include/its.h`](../esp-idf/include/its.h).

## 1. What the layer is

ITS is **spangap's own primitive** — there is no upstream to track. The whole
thing is built from FreeRTOS queues, stream buffers, task notifications, and
binary semaphores, with three global tables and one per-task record. Everything
below is ours.

**Global tables** (all fixed-capacity, never shrink):

| Table | Constant | Home | Meaning |
|-------|----------|------|---------|
| Connection table (`connTable`) | `ITS_MAX_CONNS = 128` | `.bss` | Active connections; handle = slot index, round-robin allocation. |
| Stream-buffer pool (`itsPool`) | `ITS_MAX_POOL = 128` | `.bss` descriptors, PSRAM rings | Per-direction byte rings for stream/legacy connections, keyed by size. |
| Packet-link descriptor rings (`itsLinks`) | `ITS_MAX_LINKS = 256` | PSRAM (lazy) | Two per packet connection (one per direction); each is a ring of message descriptors. |
| Task table (`s_tasks`) | `ITS_MAX_TASKS = 48` | PSRAM (lazy) | One `its_task_t` per ITS-registered task. |

`s_tasks` (~23 KB) and `itsLinks` are `heap_caps_calloc(..., MALLOC_CAP_SPIRAM)`,
allocated together on the first `taskFindOrCreate` call — a single serialized
chokepoint before any connection exists, so a packet connect finds `itsLinks`
ready without a lazy-alloc race. They live in PSRAM rather than `.bss` on
purpose: these tables are task-context-only (never touched from an ISR or a
cache-disabled window — the module's standing invariant), and on a
display-plus-radio board the scarce resource is internal DRAM / the DMA-capable
pool, not PSRAM. On the reticulous T-Deck build internal DRAM runs at ~91% (free
~19 KB, low-water ~4.75 KB, DMA-pool low-water 26 B) while PSRAM sits near 10%.
Moving these two tables off `.bss` reclaimed ~25 KB of internal DRAM; before the
move, the ~10 KB of static arrays the ITS-as-mailbox work added tipped the pool
over — WiFi AP-fallback crashed in `ieee80211_hostap_attach` ("alloc eb … fail")
and a boot-time SD read crashed in `setup_dma_priv_buffer` ("Failed to allocate
priv RX buffer"), both internal-DMA-pool exhaustion. `connTable` and `itsPool`'s
descriptor array are still `.bss`; move them the same way (lazy, count-guarded)
if more headroom is needed.

**Adding static `.bss` to spangap-core is dangerous.** Any new fixed array in
this layer competes for the internal DMA pool. Measure `.dram0.bss`
(`xtensa-esp32s3-elf-size -A build/reticulous.elf`) after such a change and
prefer PSRAM for task-context-only data. Reclaiming *PSRAM* instead (e.g. the
packet-link work freed 18×4 KB of rnsd stream-pool buffers and 256 KB of seccam)
does nothing for the internal-DRAM budget.

**Per-task record** (`its_task_t`): the inbox queue, the server-port table
(`ITS_MAX_PORTS = 8`), the aux-port table (also 8), the pickup semaphore, and the
in-flight-payload accounting. Allocated on first `itsServerInit` /
`itsClientInit` / `itsOnAux`.

**Public payload cap**: `ITS_MAX_MSG_DATA = 320`. Every framed connect/aux struct
shipped through the inbox must `static_assert(sizeof(T) <= ITS_MAX_MSG_DATA)`.
This is the floor under the per-task inbox guard (`CONFIG_SPANGAP_ITS_INBOX_MSG_MAX`,
default 4096): the guard is raised to 320 if a task asks for less, so a
small-guard task never regresses below the framed-struct minimum.

**Port kinds** (`its_port_kind_t`): `ITS_STREAM` (byte ring), `ITS_PACKET`
(descriptor ring + borrowed heap payloads), and `ITS_PACKET_LEGACY` (framed
packets carried inside a stream ring — the transitional shape the bool-overload
of `itsServerPortOpen` maps `true` to). New packet ports use `ITS_PACKET`.

## 2. Threading and ownership

ITS is **task-context only**. None of `itsSend` / `itsRecv` / `itsSendAux` /
`itsConnect` / `itsPoll` may run in an ISR. The whole layer (pool, descriptor
rings, connection table, per-task records) is touched only from registered tasks.

The inbox is a FreeRTOS queue of **single pointers** (`its_msg*`, ~4 B per slot),
created with `MALLOC_CAP_INTERNAL` — **internal RAM, not PSRAM**. The queue's
control block embeds an SMP spinlock taken by `taskENTER_CRITICAL` inside
`xQueueSend`/`xQueueReceive`, and that spinlock cannot live in PSRAM:

1. the acquire uses an `S32C1I` atomic, unreliable on external PSRAM → owner/count
   desync → `spinlock_acquire … count==0` assert; and
2. a flash op on the other core disables the cache, making a PSRAM queue struct
   unreadable while this core is inside the critical section.

Because slots are tiny pointers, the queue is cheap in internal RAM. The message
**headers** and **payloads** they point at are separate heap blocks in PSRAM,
borrowed per message and freed on read (the transport frees the header on every
path; it frees the payload unless the receiver adopts it via the owned/ref
zero-copy calls).

`itsPoll(deadline)` is the single blocking primitive: it drains at most one inbox
message, then dispatches per-connection `onRecv` for any connection with unread
bytes, and otherwise blocks on `ulTaskNotifyTake`. A task wakes on an inbox
message OR on a connection's incoming buffer reaching its trigger level (§6) OR
on an ISR's `vTaskNotifyGiveFromISR`.

**Pickup semaphore is per-task** (`its_task_t.pickupSem`, one binary semaphore),
not a global pool. A task is single-threaded, so it can have only one outstanding
`ITS_WAIT_PICKUP` send at a time; one semaphore per task suffices and avoids the
SMP races a shared claim-from-pool design had (non-atomic check-then-claim, and a
giver firing before the waiter reached its wait).

### Why plain heap blocks — rejected alternatives

The borrowed-block design was chosen over several alternatives; the reasons
still bind, so don't reintroduce them casually:

- **A pooled / size-class payload allocator** — rejected for now: plain heap
  allocation handles arbitrary sizes and the heap takes the load fine. Heap
  churn from per-message payloads was judged a non-issue — storage's JSON
  handling already churns the heap far harder (hundreds of small allocations
  per second) without trouble. The acquire wrappers (`payAlloc`/`payAdopt` in
  `its.cpp`) are deliberately the seam a pooled allocator could later drop in
  behind without touching any caller.
- **Carrying cJSON pointers as payloads** — rejected: a cJSON tree is many
  blocks needing a type-specific destructor, which breaks the "any payload is
  one `free()`" rule the blind teardown drain depends on. Structured data is
  serialized into the block.
- **A tagged allocator (prefix headers on payload blocks)** — rejected: it
  would catch "took ownership and forgot to free", but kills adoption of
  foreign blocks — `itsSendOwned` adopting `cJSON_PrintUnformatted` output
  directly is the flagship zero-copy path. The ownership-boundary counters
  (`itsStatus`) catch everything else.

### Task death and the append-only task table

`s_tasks` is append-only: `taskFindOrCreate` adds a permanent slot the first time
a task touches ITS — including a transient task that registers only to use the
connectionless fs/storage pickup proxy (`pickupArm`) and then dies. The slot is
never removed, so its `TaskHandle_t` would dangle after the task exits. A later
send routed to that slot does `xTaskNotifyGive(slot->task)`, writing a
notification word (`ucNotifyState = 2`) into the freed TCB — a use-after-free in
internal DRAM. The canonical victim is `main_task`: it drives `proxyOp` during
boot (`tlsInit` → `fs_stat` → `itsSendAux`) and self-deletes when `app_main`
returns; the corruption surfaced downstream as a NULL-owner
`xTaskRemoveFromEventList` assert from the USB-Serial-JTAG RX ISR (confirmed with
a hardware watchpoint that caught `main_task`'s TCB as the victim).

The fix (in tree) is `vTaskPreDeletionHook`: IDF's Xtensa port calls it for every
deleted task from `prvDeleteTCB`, inside a scheduler critical section, when
`CONFIG_FREERTOS_TASK_PRE_DELETION_HOOK=y` (set in `sdkconfig.defaults.spangap`).
ITS's hook NULLs the dead task's `s_tasks[i].task` so `taskFind` can never match
it again; the slot, its semaphores and inbox stay allocated (honoring the
append-only invariant, just inert). A cached `its_task_t*` whose `->task` is now
NULL degrades to `xTaskNotifyGive(NULL)`, which the kernel maps to the *current*
task — a harmless spurious notify, not a UAF. It is a single aligned pointer
write, so no lock is needed (and none is allowed: critical-section context
forbids FreeRTOS calls, malloc, or logging). The one hook also drives storage's
subscription-table cleanup through a weak `storageOnTaskDeath`.

**The bug was the hook name, and it failed silently.** The cleanup logic was
always correct but orphaned: it was named `vPortCleanUpTCB`, the legacy hook IDF
only calls under `CONFIG_FREERTOS_ENABLE_STATIC_TASK_CLEAN_UP` (never set). IDF
5.5 renamed the live hook to `vTaskPreDeletionHook` (`port.c`
`vPortTCBPreDeleteHook`), so the old function never ran — no link error, just a
dead `extern "C"` symbol. Lesson: an `extern "C"` hook with the wrong name links
cleanly and is never called. Applied in tree; not re-verified on hardware since
the rename.

Out of scope: a task that dies while still owning *live* connections leaves stale
`clientTask`/`serverTask` handles that the link-backpressure notifies (`remoteOf`)
use directly. The hook does not cover that — no such task self-deletes today.

## 3. Framing

### Stream connections
A pair of FreeRTOS stream buffers from the pool (`to` = client→server, `from` =
server→client). `itsSend`/`itsRecv` are thin wrappers over
`xStreamBufferSend`/`Receive`. Either direction may be size 0 (aux-only server).

### Packet connections (`ITS_PACKET`)
Each direction is a descriptor ring (`depth` slots, default
`CONFIG_SPANGAP_ITS_PKT_DEPTH = 16`) plus per-message heap payloads borrowed and
freed on read. The per-direction **byte window** (`toCap`/`fromCap`) is a lazy
backpressure cap (idle cost ≈ 0), distinct from `maxMsg` (default
`CONFIG_SPANGAP_ITS_MSG_MAX = 65536`), the hard per-message size guard reported by
`itsRecvBufSize`/`itsSendBufSize`. `itsSendOwned`/`itsRecvRef` move payloads
zero-copy by adopting the caller's heap block.

### Legacy packets (`ITS_PACKET_LEGACY`)
Discrete packets framed inside a stream ring with a 4-byte header
`[reserved=0][len_hi][len_mid][len_lo]` (24-bit big-endian body length). `itsSend`
writes one whole packet atomically (single writer per direction); `itsRecv` copies
exactly one body and discards the header. `itsSpacesAvailable` subtracts the 4-byte
overhead in this mode.

### Inbox messages
`its_msg` header carries: sender task handle, message kind (CONNECT / DISCONNECT /
AUX / FORWARD / CONNECT_CANCEL), port, length, handle, a flag field
(`ITS_F_PICKUP`), and the client's connect nonce. The payload (if any) is a
separate borrowed block.

## 4. Lifecycle

- **Connect** — the client sends a CONNECT inbox message carrying its connect
  payload and a nonce. The server's `onConnect` returns its serverRef (`>=0`
  accept, `<0` reject); buffers are allocated at accept. A client whose ack wait
  times out leaves a `CONNECT_CANCEL` keyed on the same nonce to reap the slot the
  server may later allocate. This closes a real leak class: on timeout the client
  returns -1 and walks away, but its CONNECT is still in the server's inbox, so
  when a slow or briefly saturated server finally drains it, `onConnect` allocates
  a conn slot, pool/link buffers, and whatever the handler took (e.g. a CLI
  session slot) for a client that is already gone. Left unreaped this is the "out
  of CLI sessions" wedge — a server that is momentarily saturated, or not pumping
  `itsPoll` at all (blocked at boot), refuses every subsequent connect. The
  trigger is any window where the server task isn't draining its inbox. **Caveat:**
  the CANCEL is best-effort — it is enqueued with a 50 ms budget and dropped if
  the inbox is full, so a server wedged inbox-full can still leak the slot; the
  reap depends on the CANCEL landing behind its CONNECT, which only FIFO ordering
  from the same sender guarantees. Deferred design that would close this gap: a
  per-task connect-resolution mutex the server takes before `onConnect`, letting
  it drop an abandoned connect *before* allocating and letting a client that lost
  the timeout race recover the ack instead of returning -1 — no CANCEL message
  needed. Not implemented; the nonce+CANCEL path above is what ships.
- **Forward** — `itsServerForward` transfers a connection's ownership to another
  server task on a named port; the stream buffers stay, the new owner sees a fresh
  `onConnect`. `itsInject` pushes already-consumed protocol bytes back into a
  direction first (`asServer=false` writes client→server, `asServer=true` writes
  server→client), so the new owner re-parses a whole-looking request. This is how
  the web task hands an HTTP connection to the target service.
- **Disconnect** — works from either side; `itsDisconnect(-1)` closes every
  connection the calling task owns. The connection record is freed before the
  remote's disconnect callback runs.

### Disconnect-payload trick
When a server kicks a client, the connection record (and the client's
per-connection disconnect callback that lives in it) is freed before the client
wakes to process its inbox. So the client-side callback pointer is captured and
shipped as raw bytes in the `ITS_K_DISC_FROM_CLIENT` disconnect message; the
client's dispatcher calls it from the message, not from the dead record.
Function pointers are valid across the shared address space. Client-initiated
disconnects need no such trick — the server's disconnect callback lives in the
per-port table on the server task, which is not going anywhere.

## 5. ISR-safety rule

ITS calls never come from an ISR — both because the inbox spinlock work is not
ISR-bounded, and because a flash op disabling the cache would make any
PSRAM-resident ITS structure unreadable mid-operation. The correct ISR→task
pattern on spangap:

1. ISR writes a heap-resident flag (plain global / `volatile` / counter).
2. ISR calls `vTaskNotifyGiveFromISR(taskHandle, &hp)` + `portYIELD_FROM_ISR(hp)`.
3. The target task's loop blocks in `itsPoll(timeout)` (which uses
   `ulTaskNotifyTake`, so it also wakes on the ISR's notify) and reads the flag.

The LoRa DIO1 ISR and lwIP's UDP recv callback both follow this — they touch no
ITS call, and drain the hardware inside the task's `itsPoll` wake.

## 6. Flow control

`itsSetTriggerLevel(handle, N)` sets the high-water mark on the caller's **incoming**
stream: the remote's `itsSend` only wakes the caller once the buffer fill reaches
`N` bytes (default 1 = wake on every send; 0 is treated as 1). The receiver
declares its own wake threshold — the side that knows what "enough data" means
owns the policy, rather than the sender guessing or busy-nudging. It is also
pushed into the underlying `xStreamBufferSetTriggerLevel` so any blocking
`xStreamBufferReceive` sees consistent semantics. The fs streaming-write server
uses it to coalesce many small `itsSend`s into one `fwrite`.

`itsSetFreeNotify(handle, N)` is the low-water complement on the caller's
**outgoing** stream: a one-shot wake (task notification + a dedicated per-buffer
semaphore) when at least `N` bytes of send space free up. `itsWaitForSpace` blocks
on that semaphore without consuming task notifications, so it is safe to call from
inside a handler on a task whose main loop blocks in `itsPoll`. Packet-mode
`itsSend(timeout>0)` uses the blocking path internally to wait for a whole packet
to fit.

## 7. Limits

| Constant | Value | Meaning |
|----------|-------|---------|
| `ITS_MAX_MSG_DATA` | 320 | Floor under the inbox payload guard; framed-struct `static_assert` ceiling. |
| `ITS_MAX_PORTS` | 8 | Server ports + aux ports per task (separate tables). |
| `ITS_MAX_CONNS` | 128 | Global active-connection table. |
| `ITS_MAX_POOL` | 128 | Global stream-buffer pool (lazy growth). |
| `ITS_MAX_LINKS` | 256 | Packet-link descriptor rings (two per packet connection). |
| `ITS_MAX_TASKS` | 48 | ITS-registered tasks. |

`CONFIG_SPANGAP_ITS_INBOX_DEPTH` (32), `CONFIG_SPANGAP_ITS_INBOX_MSG_MAX` (4096),
`CONFIG_SPANGAP_ITS_PKT_DEPTH` (16), and `CONFIG_SPANGAP_ITS_MSG_MAX` (65536) are
the Kconfig-tunable defaults (see [its.md](its.md#configuration)).

## 8. Pitfalls

- **No ITS from an ISR.** §5. The symptom of breaking this is a sporadic crash
  that only manifests during a concurrent flash write (OTA, LittleFS commit).
- **FreeRTOS sync objects (queues, stream buffers, semaphores) stay in internal
  RAM, never PSRAM.** The `S32C1I` spinlock embedded in their control blocks trips
  on external PSRAM. Large *data* (payloads, recv buffers, the pool's byte rings)
  goes in PSRAM; the sync object guarding it does not.
- **The pool never shrinks.** `poolGet(size)` reuses a free entry of exactly
  `size` or allocates a new one; `poolFree` marks it unused (and resets its
  trigger level to 1) but never destroys it. This trades a bounded high-water
  memory cost for zero PSRAM fragmentation. A port that opens many connections of
  one-off odd sizes inflates the pool permanently — size connections to a small
  set of sizes, or pass `no_pool = true` to `itsServerInit` so that server's
  connection buffers bypass the pool entirely: each is created fresh on connect
  and deleted on disconnect (by the disconnect's receiving end), returning the
  memory to the heap and keeping the server's buffers exactly attributed under
  per-task heap tracking instead of inherited cross-task.
- **The receiver, not the sender, sets the wake threshold.** Use
  `itsSetTriggerLevel` on the receiving side; never reintroduce a "silent send +
  manual nudge" pattern on the sender (it spins the receiver awake on every byte).
- **Anchor everything to a port.** Aux sends to an unregistered port are rejected
  loudly. Keep that — it surfaces wiring mistakes at startup instead of as silent
  cross-task corruption later. There are no global catch-all queues; a port
  dispatches only on its registering task (so plain `static`s in a recv callback
  are safe; `thread_local` is not — use plain `static`).
- **A packet body must fit `maxMsg`,** and in legacy mode `4 + len` must fit the
  ring capacity. `itsRecv` into an undersized buffer drains and discards the
  packet and returns 0.

## 9. Keeping ITS portable

ITS is meant to be liftable out of spangap — a generic task-to-task transport
another project could drop in. So `its.cpp` must not reference spangap
application concerns or Kconfig symbols that only exist in a spangap build.

- **No spangap Kconfig in ITS primitives.** Don't bake application ordering (e.g.
  rnsd's boot sequence) into ITS. Prefer making behavior fall out of the generic
  parameters ITS already has: `itsConnect`'s single caller `timeout` governs the
  *whole* wait — task-exists, server-ready, and handshake — rather than a separate
  readiness flag or knob.
- **Genuine tunables get a self-contained overridable default:** `#ifndef ITS_FOO`
  / `#define ITS_FOO <default>` / `#endif`, so ITS builds standalone and an
  embedder overrides with `-DITS_FOO=...`.
- **Don't over-configure.** A pure implementation detail (e.g. a re-check poll
  cadence) is a named constant, not a config knob.

The `CONFIG_SPANGAP_ITS_*` symbols (`INBOX_DEPTH`, `INBOX_MSG_MAX`, `PKT_DEPTH`,
`MSG_MAX`) predate this rule and partly violate it: each is already wrapped in an
`#ifndef` guard with a standalone default, so ITS still compiles outside spangap,
but the name carries the spangap namespace. Renaming them to neutral
`ITS_`-prefixed symbols is a candidate cleanup, not yet done.

## 10. Deferred work

Known, sanctioned follow-ups — each was deliberately left out of a
verified-working build rather than forgotten:

- **Delete `ITS_PACKET_LEGACY` and the bool `packetBased` overload of
  `itsServerPortOpen`.** Both are transitional: no port uses legacy mode any
  more (every remaining bool-overload caller passes `false` → `ITS_STREAM`),
  so the legacy framing arms in `itsSend`/`itsRecv` are dead code. Removing
  them is behavior-neutral but means migrating the ~10 stream-port call sites
  still on the bool overload (fs.cpp ×2, cli.cpp, log.cpp here; web.cpp and
  webrtc_task.cpp in spangap-web; sshd, net, nomad, lxmf, iface-tcp) to the
  kind-taking signature.
- **webrtc router `pendingBuf` → `itsRecvRef`.** The router pulls
  data-channel-bound packets with a copying `itsRecv` into its shared
  buffer, then copies again
  into a per-channel stash when the SCTP rexmit pool is full. Pulling with
  `itsRecvRef` would adopt the payload block and drop one copy. A
  micro-optimization, not a bug.
- **In-place SCTP retransmit / single-message config dump.** Tracking an
  outbound message's fragments in place (a bitmap) instead of copying each
  whole message into the rexmit pool, and collapsing the browser config dump
  into one message, are sketched but unbuilt: the pool already holds large
  messages, PSRAM is abundant, and the dump streams fine in chunks.
</content>
