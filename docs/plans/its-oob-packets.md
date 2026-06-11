# ITS — out-of-band packets (heap-backed payloads, unified)

**Status:** plan. No code yet. Changes [ITS](../../esp-idf/include/its.h) (`its.cpp/h`).
Motivated by, and a prerequisite for, the [Lua plan](lua.md) and the Nomad-page-render bug
documented below.

## Problem

ITS packet-mode layers discrete packets on top of a per-connection FreeRTOS **stream buffer** (a byte
ring drawn from the PSRAM pool). A byte ring is the wrong substrate for packets, and it costs us twice:

1. **Ring capacity becomes a hard packet-size cap.** A packet body must fit `fromSize`/`toSize` minus
   the header. The storage config DC runs a 16384-byte ring (`WEB_ITS_FROM_SIZE`), so a single live
   merge-patch is capped — `DC_PATCH_MAX = 15500` in `storage.cpp`, **dropped with a warn above that**
   (only the connect-time *dump* is chunked).
2. **Every ring is permanently sized for its worst-case packet**, × `maxHandles`, reserved from the
   pool whether or not that traffic ever flows. seccam's A/V ports reserve **256 KB** each; storage
   reserves 48 KB / 16 KB. Mostly idle, always held.

### The bug this fixes (concrete)

Nomad publishes a fetched page body to the ephemeral config tree (`nomad.page.body`) so the SPA
renders it over the storage DC. `nomad.cpp` caps the publish at `s.nomad.max_page_publish`
(**default 32768**) — but 32768 is **more than double** the 15500 live-patch ceiling. So any Micron
page roughly **15–32 KB renders empty**: it passes Nomad's own cap, then the merge-patch exceeds
`DC_PATCH_MAX` and is dropped one layer down, visible only as a serial-log warn (with
`nomad.page.truncated` still showing `0`, because Nomad believes it sent the body). That band is
common — hence "Nomad so often doesn't return anything." It doesn't self-heal on reload either: the
dump path skips any single leaf value larger than the ring (`storage.cpp` ~1680).

## Core idea — one way: every packet is a heap block + a descriptor in the ring

There is **no in-band path**. A packet-mode payload is always a plain heap block; the ring carries
only a fixed **8-byte descriptor** — `{ uint32 len, void* ptr }`. The payload travels out-of-band in
the same address space (ITS is **point-to-point, single-address-space** inter-task transport, so the
pointer is valid and read exactly once).

Blocks are **plain `heap_caps_malloc` (PSRAM), freed with `free()`, always.** No size classes, no
slab pool, no inline fast-path, no pool-vs-heap tag. The whole design reduces to one invariant:

> **Every payload is a malloc'd block, referenced by one descriptor, freed exactly once.**

This is deliberately the simplest possible scheme, and the justification is empirical: **storage
already churns the heap far harder than this** — `cJSON_Create`/`Duplicate`/`Print` spray many tiny
heterogeneous nodes and free them on every patch and every one of the >100 notifications/sec, and it
works. Uniform, short-lived packet blocks are noise next to that, and on PSRAM they don't touch the
DMA-critical internal pool. A small packet (even 20 bytes) becomes a heap block too; that micro-cost
is the price of having exactly one path, and it's well within the existing budget. (If profiling ever
shows real heap-lock contention or fragmentation, a block pool drops in *behind* the ITS boundary
without touching a caller or the wire format — a reversible decision, so the simple choice wins now.
See Rejected alternatives.)

### The ring is the free-set

The descriptors in the ring *are* the set of in-flight blocks. `itsRecv` removes-and-frees as the
reader consumes. There is no parallel list. **Teardown/reset/close drain-and-free:** walk the ring,
`free()` every descriptor's `ptr`, then delete the ring. Because there is no inline branch, this is
unconditional — every ring entry is a freeable block. The one discipline: **every path that discards
ring contents must route through the drain-freeing helper, never a raw `xStreamBufferReset`/delete**
(`itsReset`, the disconnect-side delete, `itsServerPortClose`). A raw reset would leak every block in
flight. This is the same lifetime-sensitive area as the historical connect-timeout slot leak — be
deliberate.

## Two limits, cleanly separated

Each direction has two independent caps; a send needs room in both:

- **Descriptor ring → max in-flight packet *count*.** The ring holds fixed 8-byte descriptors, so its
  capacity is purely "how many packets may be outstanding," a small knob (e.g. a 512-byte ring = 64
  in-flight packets). Internal to ITS; callers don't size it.
- **Logical byte counter → max in-flight *bytes*.** One `size_t` per direction: `+= len` on send,
  `-= len` on receive. This is the backpressure window and the value the flow-control API reports.

Both gates are simple now because the descriptor is fixed-size — none of the variable-body
reconciliation the inline design needed. A send blocks/fails if the ring is full (too many packets
outstanding) *or* the byte counter would exceed the cap.

### "Feels endlessly large, capped by the bytes it may take"

The byte counter makes a connection feel limitless: keep sending, blocks are allocated on demand, and
the cap is "memory I'm willing to let it reach," not "memory I permanently donate." Idle cost is ≈ 0
(no blocks allocated, only the tiny descriptor ring); cost materializes only under traffic and is
freed the instant the receiver reads. So caps can be **generous and lazy** — a 64 KB cap that idles
at near-zero, where today a 64 KB ring reserves 64 KB forever.

The flip side: **the cap is the backpressure.** Uncapped means a producer faster than its consumer
grows the heap without bound → OOM. On a few-hundred-KB-PSRAM device, default to a generous-but-finite
cap (`CONFIG_SPANGAP_ITS_DEFAULT_LOGICAL_CAP`) so a runaway producer meets backpressure, not a crash.
A per-packet `CONFIG_SPANGAP_ITS_MAX_BLOCK` guard keeps one pathological allocation from eating the
whole window.

### `itsSpacesAvailable` is purely logical

It returns the logical window (`cap − counter`) — nothing else. It must **not** try to report
"largest body acceptable now," because that would depend on the largest contiguous PSRAM block:
global, fragmentation-dependent, racy, useless per-connection. The ring-count limit and a transient
malloc failure surface instead as `itsSend(timeout=0) → 0`, which the existing retry-don't-drop
contract already absorbs (`dcFlushPatch` retries and only clears its pending patch when `sent == len`).
So callers are unchanged, and `len` may rise to the logical cap. For the blocking path
(`timeout > 0`), the free-notify wakes when the counter drops under the cap (or a descriptor slot
frees); distinguish a transient ring-full/malloc-retry from a real cap-block.

## Send — copy (default) vs ownership transfer

- **`itsSend(handle, data, len, timeout)`** — copy/retain, unchanged signature. ITS `malloc`s a block
  and copies into it; the caller keeps its buffer (stack, static, borrowed, anything). **The only path
  where ITS allocates** and can hit its own OOM (→ `0`/retry). Every existing call site keeps working
  with no edit — payloads simply travel OOB now.
- **`itsSendOwned(handle, ptr, len, timeout)`** — ownership transfer, zero-copy: ITS takes the
  caller's heap block as-is, writes the descriptor, and on success owns it (frees on the receiver's
  read or on teardown drain). **Ownership transfers iff the call returns success;** on a back-pressure
  `0` the caller still owns the block — exactly what retry-don't-drop wants. Requires a plain
  `heap_caps`/`malloc` block ITS may `free()`. **No ITS malloc on this path → no in-ITS OOM there**:
  the allocation already happened in the caller's buffer-build step, where failure is local.

## Receive — transparent (default) vs by-reference

- **`itsRecv(handle, buf, maxLen, timeout)`** — copies the block into the caller's buffer and frees it
  internally. OOB is invisible to every existing reader.
- **`itsRecvRef(handle, void** out, size_t* len, timeout)`** — hands the pointer and transfers
  ownership; the caller frees. For forwarders (`webrtc_task`) that immediately re-emit, this avoids the
  recv-side copy.

### End-to-end zero-copy on the storage→browser patch path

`dcFlushPatch` is the center of the Nomad fix and the ideal customer: it already
`cJSON_PrintUnformatted`s into a throwaway `text`, null-checks it, and `cJSON_free`s it. With
`itsSendOwned` it hands `text` straight over — **no memcpy, no `cJSON_free`** (ITS frees it on the
receiver's read), the only "error" the `cJSON_Print → NULL` it already checks. Paired with
`itsRecvRef` in `webrtc_task`, a patch goes producer → ring (8 bytes) → SCTP with **not a single
payload copy**. `DC_PATCH_MAX` rises to the logical cap and the Nomad body flows.

## Memory: the win

Per-connection rings collapse from "worst-case payload bytes" to "max-in-flight-count × 8 bytes" — a
few hundred bytes instead of 16 KB / 48 KB / 256 KB. Payload memory is demand-allocated from the
shared heap, bounded per connection by its logical cap, and freed on read. So **idle memory ≈ the sum
of tiny descriptor rings**, and peak is demand-driven and bounded — versus today's permanently-reserved
worst-case rings across every handle. The big reservations (seccam 256 KB, storage 48 KB/16 KB) are
where the reclaim lands.

## Downstream cleanup this enables

- **The live-patch drop goes away.** `dcFlushPatch`'s `len > DC_PATCH_MAX → drop+warn` becomes a
  non-event; `DC_PATCH_MAX` rises to the logical cap. The direct Nomad fix.
- **Per-connection ring memory drops hard** (above).
- **Dump chunking coarsens, then vanishes.** With OOB alone the dump is still bounded by SCTP's 64 KB
  `max-message-size`, so `DC_DUMP_MAX` only relaxes from 14000 (sized for the 16 KB ring) to ~60 KB.
  With the arbitrary-size SCTP send below, the 64 KB ceiling lifts and the dump becomes **one logical
  packet** — `dcBuildDump` / `DC_DUMP_MAX` / the `{"__dump":"b/e"}` bracketing all retire. The full
  "do away with storage chunking" outcome.

## Storage change-notify over packet streams (the bigger prize)

Storage's change dispatch currently rides the shared per-task **aux inbox** (FreeRTOS queue, 320 B/msg
cap, depth `CONFIG_SPANGAP_STORAGE_NOTIFY_INBOX = 256`). That is *the* structure that floods and drops
under the >100/sec stats fan-in. Once payloads are cheap descriptors, move bulk notifications onto a
per-subscriber **packet stream**: each subscriber drains a deep, cheap ring of 8-byte descriptors
instead of contending for one shared 320 B-limited inbox. The flood-and-drop problem and its tuning
knob largely dissolve. This is a follow-on refactor — rework subscribe/dispatch into per-subscriber
streams; keep aux for connect/disconnect/forward signaling only — but it is arguably the headline win,
more than the Nomad fix, and it is *enabled* by unified OOB.

## Arbitrary-sized packets (SCTP send-side)

OOB removes the *ITS* cap; the next ceiling is **SCTP's `max-message-size: 65536`** (advertised in
`webrtc_task`, mirrored by the receive reassembly cap `MAX_MESSAGE`). 64 KB covers Nomad pages and
most Lua output, so this is a **later, optional phase** — but it's cheap and it's what lets the dump
collapse to a single packet.

### In-place fragment-bitmap retransmit

Today `sctpSend` copies each message into the rexmit pool as one slot and gates the *whole* message on
`peerRwnd`, capping message size at the pool/window. Replace that with per-packet fragment accounting
that never copies the payload:

- Per in-flight packet keep `{ptr, len, baseTSN, bitmap}`. Fragment size `F` is fixed, so bit *i* ↔
  fragment *i* ↔ byte offset *i·F* ↔ TSN `baseTSN + i`. **The packet is never copied** — it stays in
  place and is the retransmit source (read fragment *i* from `ptr + i·F`).
- **2 bits per fragment** — `sent` and `acked`; outstanding = `sent & ~acked`. A 1 MB packet is ~840
  fragments ≈ 210 bytes of bitmap. Negligible, and less memory than today's whole-message pool copy.
- SACK gap-ack blocks are TSN ranges → set `acked` bits at `tsn − baseTSN`. Retransmit walks
  `sent & ~acked` past the gaps. Windowing falls out: emit while outstanding < window, free the packet
  when every `acked` bit sets.

Two invariants keep bit↔TSN exact:
- **Reserve a contiguous TSN range per packet at fragmentation time** (`nFrags = ceil(len/F)`, claim
  `[myTsn, myTsn+nFrags)`, advance). Interleaving other packets/streams for windowing is then fine.
- **Fixed `F` ⇒ stable PMTU.** Pin `F` to a conservative DTLS/UDP-safe size; never make it dynamic.

### The block makes one journey, never copied, freed once

```
producer mallocs (PSRAM)
  → itsSendOwned    ITS owns (descriptor in ring)
  → itsRecvRef      webrtc_task owns (block dequeued; ITS no longer references it)
  → sctpSendOwned   SCTP rexmit owns; block IS the in-place retransmit source
  → last SACK       SCTP frees it. Once.
```

The no-copy property is coupled to this phase: it needs a `sctpSendOwned` mirroring `itsSendOwned`.
With today's pool-copying `sctpSend` the copy doesn't vanish, it moves from the ITS boundary to the
pool (and `webrtc_task` frees the block right after the call). Contract details mirror the ITS rules:
**reject → caller retains** (feeds the existing stash/`pendingBuf`, now a pointer-hold = another copy
gone); **teardown/stream-reset → drain-and-free** the un-ACKed blocks. The wire is *not* copy-free —
each fragment is DTLS-encrypted into a transient buffer per (re)transmit, re-run from the in-place
plaintext on retransmit — but that's not a preservation copy, which is the property that matters.

### Memory: the ESP32 is the only ledger

The browser has gigabytes; a few hundred KB of dump in Chrome's reassembly buffer is nothing, and a
config dump has no useful partial state, so "progressive delivery" buys nothing. The only constrained
party is the device, asymmetrically:
- **Outbound (device→browser):** send arbitrary. Set the SDP `a=max-message-size` to `0` (unlimited,
  RFC 8841 — usrsctp/Chrome honor it). Bound **total outstanding outbound bytes** so a stalled browser
  can't pin device RAM — the same cap-as-backpressure idea, one layer down.
- **Inbound (→device):** **keep a reassembly cap** (`CONFIG_SPANGAP_SCTP_INBOUND_MAX_MSG`, replacing
  the hard-coded 64 KB `MAX_MESSAGE`). This is the only place "arbitrary size" is dangerous — a peer
  could make the *ESP32* malloc unboundedly.

## Rejected alternatives

- **Keep an inline fast-path for small packets.** We worried the hot small-packet path — RNS, capped
  at the 500 B protocol MTU (`rns .../ports.h`), one packet per ITS packet per hop — shouldn't pay a
  malloc, which argued for an inline path with a threshold above 500 B. Rejected: storage's cJSON
  churn is the existence proof the heap handles this rate, so the inline path bought complexity (a
  threshold, a dual-gate physical/logical reconciliation, two wire formats) against no measured
  problem. One way is simpler and the cost is in budget.
- **Slab/block pool with size classes.** Solves fragmentation and heap-lock contention — neither of
  which we have evidence for (cJSON again). Size classes plus the unavoidable big-packet exception is
  two mechanisms where plain `malloc` is one and handles arbitrary sizes. And it's reversible: a pool
  can drop in behind the ITS boundary later without touching callers or the wire format, so there's no
  reason to pay for it now.
- **DataChannel reassembly shim (vs in-place SCTP bitmap).** Splitting large packets into N ≤64 KB
  SCTP messages with a browser-side reassembly shim avoids touching the SCTP core and an SDP bump, but
  the in-place bitmap is *cheaper* on the device (no copy, less state), needs no framing on either
  end, and uses native one-message-one-`onmessage` delivery. The shim would only win for progressive
  delivery, which — browser memory being a non-issue — we don't need.

## Kconfig

- `CONFIG_SPANGAP_ITS_DEFAULT_LOGICAL_CAP` — per-direction byte cap when a port doesn't specify one
  (generous-but-finite OOM backstop).
- `CONFIG_SPANGAP_ITS_MAX_BLOCK` — per-packet allocation guard.
- Descriptor-ring default depth (max in-flight packet count) — internal, not caller-facing.

SCTP arbitrary-size phase (webrtc):
- `CONFIG_SPANGAP_SCTP_OUTSTANDING_CAP` — max total outstanding (unACKed) outbound bytes.
- `CONFIG_SPANGAP_SCTP_INBOUND_MAX_MSG` — inbound reassembly cap (replaces hard-coded `MAX_MESSAGE`).
- SDP `a=max-message-size` → `0` (unlimited) outbound.

## API changes (its.h)

- `itsSend(handle, data, len, timeout)` — **unchanged signature**; now copies into a heap block (not
  the ring). No existing call site changes.
- `itsSendOwned(handle, ptr, len, timeout)` — new: ownership-transfer send (zero-copy); transfers on
  success only, caller retains on `0`. Plain heap blocks ITS may `free()` only.
- `itsRecvRef(handle, void** out, size_t* len, timeout)` — new: zero-copy receive; caller frees.
- `itsServerPortOpen(... toSize, fromSize)` — **meaning shifts** from "bytes reserved" to "max logical
  bytes in flight" (the per-direction cap). Argument list unchanged; the descriptor ring is internal.
- `itsSpacesAvailable` — returns the logical window. `itsBytesAvailable`, `itsSetTriggerLevel`,
  `itsSetFreeNotify`, `itsWaitForSpace` operate on the logical counter.
- SCTP phase: `sctpSend` reworked to in-place fragment-bitmap (above); add `sctpSendOwned`.

## Phased rollout

1. **Descriptor transport + plain blocks.** Convert packet-mode `itsSend` to malloc-block +
   8-byte-descriptor-in-ring; `itsRecv` copies-and-frees; logical byte counter + count-limited
   descriptor ring; drain-and-free teardown routed through `itsReset`/disconnect/port-close. Behavior-
   preserving for callers; the risk is concentrated in teardown lifetime. **De-risk by converting one
   port first (storage DC), validating under load, then the rest** — per-port, not a temporary hybrid
   format.
2. **Zero-copy.** Add `itsSendOwned` + `itsRecvRef`; convert `dcFlushPatch` and `webrtc_task`. Raise
   `DC_PATCH_MAX` to the logical cap; correct `s.nomad.max_page_publish`. Verify a 15–32 KB Nomad page
   renders.
3. **Sweep caps.** Audit `itsServerPortOpen` call sites; set per-direction logical caps + descriptor
   depths; confirm the PSRAM pool / heap high-water drops (seccam 256 KB, storage 48/16 KB).
4. **(Optional) SCTP arbitrary-size.** In-place fragment-bitmap retransmit; SDP `max-message-size: 0`
   outbound; outstanding-bytes cap; inbound reassembly cap. Then collapse the storage dump to a single
   packet — retire `dcBuildDump` / `DC_DUMP_MAX` / `__dump` bracketing.
5. **(Optional) Storage notify over packet streams.** Move change-dispatch off the shared aux inbox
   onto per-subscriber descriptor streams; retire the notify-inbox pressure and its Kconfig knob.
