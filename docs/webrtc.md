# WebRTC for everything

Config sync (storage), log tail, CLI, and any app-defined data path
(live video / audio / file playback / etc.) all go over WebRTC DataChannels.
A single `RTCPeerConnection` per page carries every data path at once;
`/webrtc` is the only per-session WS that touches the device (and only for
SDP signaling).

## Why

TLS on the device is single-threaded in the net task; every WS upgrade
serialises a full handshake. Once DTLS+SCTP is up over UDP, opening and
closing additional DataChannels is essentially free — a few SCTP control
messages.

That buys us:

- **Per-channel reliability** — pick `ordered` / `maxRetransmits` / `maxPacketLifeTime` per DC. Video can be unreliable unordered, audio partial-reliable, control fully reliable, all on the same PC.
- **Browser-side decoding** — packet content is opaque to the device. Whatever framing the app picks, the browser does all decoding, rendering, and post-processing.
- **Clean "seek = reset"** — closing and reopening a DC on the same PC is the natural way to restart a session with new parameters.
- **Everything on one channel** — storage/log/cli use the same PC, so there's
  one DTLS handshake per tab, one heartbeat story, one reconnect story,
  one auth gate at the `/webrtc` signaling WS.

Single-session by design: the device runs one DTLS + one SCTP association
at a time. See [Single-session BUSY/takeover](#single-session-busytakeover)
below for how the browser surfaces that.

## Architecture

```
┌───────── browser ─────────┐                ┌───────── device ──────────┐
│ Page                      │                │ webrtc-task               │
│  ├ /webrtc signaling WS   │ <- TLS/TCP ->  │  signaling, DTLS, SCTP,   │
│  └ RTCPeerConnection      │                │  DCEP. Content-free —     │
│      └ DC "<task>:<port>" │ <- DTLS/UDP -> │  pure router between DCs  │
│                           │                │  and packet-mode ITS.     │
│                           │                │                           │
│                           │                │ consumer task — opens     │
│                           │                │ a packet-mode ITS port    │
│                           │                │ and produces/consumes     │
│                           │                │ packets                   │
└───────────────────────────┘                └───────────────────────────┘
```

webrtc-task is content-free. It has one job: bridge each WebRTC
DataChannel to a packet-mode ITS connection, and stay out of the way
afterwards.

## The DC ↔ ITS router

DCEP label format is `"<taskname>:<port-number>"`. webrtc-task splits on
`':'`, parses the integer port, and `itsConnect(taskname, port,
protocolBytes)`. Content-level metadata (timestamps, filenames, seek
ranges) travels in the DCEP `protocol` field — never in the label.

Runtime bridging (both directions):

- SCTP DATA on streamId → `itsSend(handle, payload, len)` on the paired
  ITS handle. One DC message = one ITS packet. Fragments are reassembled
  in `webrtc_sctp.cpp::processDataFragment` before being handed to
  `sctp.onData`; one full DCEP message lands with its boundaries intact.
- Packet read from an ITS handle → scheduler dispatches SCTP DATA on the
  paired streamId with the DC's negotiated reliability.
- SCTP RE-CONFIG (stream reset) on streamId → `itsDisconnect` on the
  paired ITS handle, and vice versa.

That's the entire router. Zero content bookkeeping beyond the
`streamId ↔ handle` table (see `dcMap` in `webrtc_task.cpp`).

### What DCEP carries

Per RFC 8832:

| Field       | Size            | Source                                        |
|-------------|-----------------|-----------------------------------------------|
| channelType | 1B              | `ordered` + `maxRetransmits`/`maxPacketLifeTime` |
| priority    | 2B              | browser's `options.priority` (hint for the scheduler) |
| reliability | 4B              | parameter to channelType                      |
| label       | up to ~1.3 KB   | `createDataChannel`'s first arg: `"<task>:<port>"` |
| protocol    | up to ~1.3 KB   | `options.protocol` — JSON with content params |

Our SCTP doesn't reassemble fragmented DCEP control messages, so label +
protocol must fit in one unfragmented DATA chunk. Labels are tiny;
protocol in practice carries short JSON.

Multiple DCs with the same label are fine — label is routing metadata.
StreamId is the unique channel identity.

### Send scheduler

In `webrtc_task.cpp::schedulerPass`. Strict priority with round-robin
among equal-priority channels; DCEP priority from the browser is
respected. Capped at a small number of messages per pass so the main loop
stays responsive and inbound SACKs get drained at roughly Chrome's
delayed-ACK cadence.

On `sendto` returning `EAGAIN`, the BIO retries with short yields rather
than propagating WANT_WRITE up on the first hiccup. Partial SCTP records
are never emitted — either the whole fragment goes out or we retry.

### Retransmit pool

1 MB PSRAM pool, 256 slot entries, shared across reliable channels. One
entry per whole DCEP message (not per fragment), keyed by first-TSN.

- SACK parsing: cumulative ACK frees covered entries; **Gap Ack Blocks**
  also free entries whose TSN range was entirely received out-of-order
  past an earlier loss. Gap-block freeing is critical when cumTsn gets
  stuck behind an abandoned TSN — new in-flight data still drains.
- SACK-triggered retransmit resends only fragments the peer's latest gap
  blocks show missing.
- **RTO retransmit**: if no SACK arrives for 1500 ms and the pool has
  outstanding data, force-resend missing fragments. Handles silent UDP
  loss where the peer never reports the gap at all.
- On stream reset (our outgoing RE-CONFIG or an inbound one), the pool's
  entries for that stream are purged, and a FORWARD-TSN chunk is sent to
  the peer so its cumTsn can advance past our abandoned TSNs.
- The DCEP ACK we emit on a new DC's OPEN is inserted in the pool — if
  that single control packet were lost without retransmit, the new DC
  would never progress past its first TSN.

### ITS packet mode

All packet-mode ITS ports used here are internal: both endpoints are
device tasks we control. Each `itsSend`/`itsRecv` is a discrete packet
with a 4-byte header. The sender blocks until the whole packet fits; the
receiver reads exactly one packet per call. This is how a DC's message
boundary is preserved through the router even though ITS streams are
otherwise byte-oriented.

## Shipping app data — example shape

Say you have a video stream you want the browser to consume. The integration is:

1. Pick a task name and port number — e.g. task `mystream`, port 1. Browser DCs labeled `"mystream:1"` will be auto-connected to that port.
2. In `mystream`'s init, open a packet-mode ITS server port:
   ```cpp
   itsServerPortOpen(MY_PORT, /*packetBased=*/true, /*maxHandles=*/2,
                     /*toSize=*/2048, /*fromSize=*/256 * 1024);
   ```
   `fromSize` is the per-DC send buffer; size it for the largest packet you'll emit (one frame, one audio block, etc.).
3. On `onConnect`, start producing packets and `itsSend` each one. If you carry multiple payload kinds on the same DC, prepend your own per-packet header so the receiver can demux.
4. On `onDisconnect`, stop the producer.

Per-packet framing inside the payload is entirely up to the app — webrtc-task never inspects it. Reliability is set by the browser at `createDataChannel()` time (`ordered`, `maxRetransmits`, `maxPacketLifeTime`); the device side only sees that `itsSend` enqueued one packet.

Subscription lifecycle (refcounting, PM-lock acquisition on first subscribe, release on last unsubscribe) is the app's concern. Apps that want a newest-wins single-slot delivery primitive between a producer and consumer task can use seccam's `frameslot.h` as a reference — a small enough pattern that copying it into an app is fine.

## Content-task DCs — storage / log / cli

Three more packet-mode ITS ports, reached by the browser under the same
label convention:

| DC label      | Server task     | Port constant         | Direction  | Payload format                |
|---------------|-----------------|-----------------------|------------|-------------------------------|
| `storage:1`   | `storage`       | `STORAGE_CONFIG_PORT=1` | bi-dir   | JSON merge-patches + ping/save |
| `log:1`       | `log`           | `LOG_PORT_DC=1`       | device → browser | ANSI log lines (tail paste + live) |
| `cli:1`       | `cli`           | `CLI_PORT_DC=1`       | bi-dir     | Raw terminal bytes              |

The browser's xterm/CLI client coalesces keystrokes 50 ms before
`dc.send()`, so a burst of typing arrives as one DC message instead of
one-packet-per-key.

`log:1` accepts a `{"backlog":N}` DCEP protocol string to override the
default tail paste size (`s.log.file.paste`, default 48 KB).

Each task also keeps a stream-mode TCP port (`LOG_PORT_TCP=8080`,
`CLI_PORT_TCP=8081`) for `nc`-style access and, for CLI, the device
serial task. These default to disabled (`s.net.log_port = s.net.cli_port
= 0`).

## Browser — `spangap-browser/src/lib/webrtc-session.ts`

One `RTCPeerConnection` per page, owned by the shared `webrtc-session` singleton (exported by `spangap-browser`). Every consumer — Pinia device store, terminal windows, app-side video player — calls `session.registerChannel(builder)` where the builder synchronously creates its DC on each fresh PC **before** `createOffer()`. That guarantees the offer always carries an `m=application` line so Chrome's SDP m-line order check passes on the
first exchange.

Channel labels and protocol payloads are app-defined; the DCEP `protocol` field carries arbitrary JSON the app uses to negotiate per-channel options at OPEN time. Closing and reopening a DC on the same PC is cheap (DCEP RESET + OPEN, no DTLS handshake), which makes it the natural mechanism for "seek" or "switch source" without renegotiating the PC.

App-side AV decoding (sample-accurate WebAudio chain scheduling, decode-paced video queue with audio-position pacing, stall recovery) is the app's job; the browser plumbing here just opens the DC and consumes packets.

## Single-session BUSY/takeover

The `/webrtc` signaling WS accepts one active session at a time. The
server evaluates auth first, then checks an explicit opt-in to evict:

| Situation                                                 | WS close code |
|-----------------------------------------------------------|---------------|
| No session cookie / invalid cookie                        | 4401          |
| Another session is active, client didn't send `?force=1`  | 4409          |
| Another session is active, client sent `?force=1`         | victim: 4008, new client accepted |

The browser session translates those into `busy`/`kicked`/`auth` states.
`MainLayout.vue` renders an overlay with a single button: "Take over" in
`busy` (next connect sends `?force=1`), "Resume" in `kicked` (retry
without force — may land back in `busy` if the new holder is still
there). No automatic takeover even with a valid cookie; the user has to
click.

Normal network blips (close codes other than 4401/4409/4008) fall back
to the session's auto-reconnect with exponential backoff.

## Failure mode

DTLS dies → SCTP dies → all DCs die → browser reconnects WS, reopens DCs
it needs. Bigger blast radius than per-WS-endpoint, but far cleaner — no
independent reconnect logic per endpoint.

TCP `log` and `cli` ports stay as a `nc`-accessible backstop when
enabled.

