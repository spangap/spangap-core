# spangap

## What it is

spangap is a device platform for the ESP32-S3 that ships in two halves: a
firmware component and a paired browser package. An app built on spangap
writes only its domain code — a sensor loop, a radio stack, a camera — and
inherits inter-task communication, networking, a web stack, storage,
configuration, logging, a CLI, OTA, and remote access for free.

The name is the dual-side idea: every feature has a device side and a browser
side, designed together and shipped together. The two halves are published
separately — an ESP-IDF managed component (`spangap/spangap-core`) and an npm
package (`spangap-browser`) — and an app consumes one or both and layers only
domain code on top.

## The problems it solves

### Wiring tasks together takes over the codebase

On an embedded device every concern is its own FreeRTOS task — WiFi, the web
server, TLS, a radio, storage, a sensor loop. Connecting them is normally
bespoke: a queue here, a task notification there, a poll loop somewhere else,
each with its own timeout, its own backpressure rule, its own race to get
wrong. The glue outweighs the feature, and most of it is busy-waiting.

### Some tasks aren't allowed to touch the filesystem

The ESP32-S3 has roughly 512 KB of internal DRAM but 8 MB of PSRAM, and they
are not interchangeable. Any SPI-flash operation disables the PSRAM cache, so
a task running on a PSRAM stack that reads a LittleFS file crashes. DMA, WiFi,
and lwIP need *internal* DRAM specifically; SD-card DMA needs internal buffers
and breaks under concurrent writers. Left to itself, an app has to track every
allocation's memory class and every task's stack placement by hand.

### The device and the browser drift out of sync

Settings sprawl into a pile of bespoke REST endpoints. Firmware and UI
disagree about shape and defaults. Adding one setting means touching both
sides plus a save path, and getting the private-vs-public split right every
single time.

### The browser app gets rebuilt from scratch every time

Every connected device needs the same furniture: a settings UI bound to live
config, a log viewer, a CLI console, a live media session, an auth flow, and a
menu to hang panels on. It gets re-implemented, badly, once per project.

### A device behind NAT is unreachable and has no real TLS

A box on a home LAN can't be reached from outside and has no publicly trusted
certificate. Punching out, getting a name, and obtaining a real cert is its
own sub-project — and doing it across many short-lived browser connections is
worse.

### You still have to operate the thing after it ships

Seeing what it's doing — and *which task* did it — poking it live, scheduling
maintenance that survives deep sleep, and updating it without bricking it: each
of these is normally hand-rolled, and each is a place to get security wrong.

## The components

Each component below removes one of the problems above. They are listed in
dependency order: **ITS** is the substrate; storage, the browser sync, the
WebRTC router, and the log/CLI transport are all just ITS clients and servers.

### ITS — inter-task streaming → *wiring*

Socket-style point-to-point connections between FreeRTOS tasks — TCP between
tasks. A task is a server, a client, or both. Servers open numbered **ports**;
the port flows from `itsConnect` through to the server's `onConnect`, so one
task hosts many endpoints — a TCP port, a URL path, a DataChannel — over one
mechanism. A connection carries either a **byte stream** or **discrete
packets** (one send = one delivered message); a fire-and-forget aux message
covers the things that aren't a connection at all, like subscribe/notify or a
register-with-net handshake.

`itsPoll(timeout)` is the single universal blocking primitive. A task has
exactly one place it waits and wakes there for everything: an incoming
connection, a packet, stream bytes, a computed deadline. There is no second
poll loop to keep serviced and no drain to babysit; idle CPU goes to zero.

A live connection can be **forwarded** from one server task to another, with
already-consumed protocol bytes injected back so the new owner re-parses a
fresh-looking request. The middle task does zero copying — no proxy buffer, no
pump-bytes-back-and-forth loop. The real chain: `net` accepts the TCP socket,
`web` parses the HTTP headers and decides the route, then forwards the live
connection to the task that owns it (`webrtc`, `storage`, an app task); after
the handoff `web` is out of the data path entirely.

Deep dive: [docs/its.md](docs/its.md).

### Unified filesystem — `fs` / `fs_strm` workers → *filesystem*

All file I/O is routed through dedicated DRAM-stack worker tasks over ITS. Any
task — PSRAM stack included — does file I/O safely by calling the `fs_*` API,
and the workers serialize SDMMC DMA so the recorder, log, and CLI can't
collide. The malloc policy is set once platform-wide (PSRAM-preferred, a DRAM
pool reserved, DMA/WiFi/lwIP allocations using explicit internal caps), and ITS
inboxes and stream buffers live in PSRAM, kept safe by the rule that ITS is
never called from an ISR. Net effect: app code never reasons about cache state
or memory class.

Deep dive: [docs/unified-fs-api.md](docs/unified-fs-api.md).

### Storage — the config tree → *drift*

One in-memory cJSON tree is the contract between device and browser. Two
prefixes are special: `s.*` is persisted **and** synced to the browser, and
`secrets.*` is persisted but **never** sent to the browser. Every other key is
ephemeral — kept in memory, mirrored to the browser, and lost on reboot. Modules subscribe to the key prefixes they care about and
react; the two sides agree through the tree, not through a pile of endpoints.
Defaults are self-registering and version-gated, so adding a key never
disturbs a user's saved value. Sync is one packet-mode `storage:1` channel in
both directions — itself just an ITS server.

Deep dive: [docs/storage.md](docs/storage.md).

### spangap-browser — the npm half → *rebuilt UI*

The browser furniture, shipped once: config-bound controls, the
FloatingWindow component, the WebRTC session manager, storage sync, log/CLI
rendering, the auth flow, and the menu registry. An app registers its own
panels and writes nothing else.

It is a **Vue 3 + Quasar** package, not a framework. Quasar was chosen for
the batteries — a coherent component set, responsive breakpoints
(`$q.screen`), and a `vite` build that emits a static SPA — without pulling in
a server runtime; everything the device needs is plain gzipped files. State is
Pinia (the config tree and the menu registry are stores), routing is
`vue-router`, and the log/CLI terminal is `@xterm/xterm`. These are
`peerDependencies`: spangap-browser ships `.vue` components and `.ts` modules
as source and the consuming app builds them with its own Quasar/`vite`
pipeline, so there is one toolchain and one bundle, not two. The built SPA is
served read-only from the device's LittleFS at `/fixed/webroot/`.

Conventions: [docs/web-interface.md](docs/web-interface.md).

### Remote access + WebRTC → *unreachable behind NAT*

The remote-access stack — UPnP port mapping, DuckDNS, and ACME — lets a device
behind NAT obtain a real (not self-signed) TLS certificate and be reachable
from anywhere; a WireGuard tunnel is the alternative path in.

WebRTC carries everything. One `RTCPeerConnection` and one DTLS handshake per
browser tab carry every data path at once — config, log, CLI, and any
app-defined media or file stream. The WebRTC task is content-free: it
terminates DTLS/ICE/SCTP and routes a DataChannel labelled `<task>:<n>` to ITS
port `n` on that task, in both directions. Adding a browser↔device channel is
opening an ITS port on one side and naming a DataChannel on the other — no
signalling code, no payload knowledge in the WebRTC task, per-channel
reliability.

Deep dives: [docs/remote-access.md](docs/remote-access.md),
[docs/webrtc.md](docs/webrtc.md).

### Log / CLI / cron / OTA → *operating it*

A dedicated log task hooks ESP-IDF's `vprintf`, tags every line with its
originating task, keeps a DRAM ring buffer, and fans out to serial, the
browser, and an optional log file. The CLI is a full line editor with history,
registrable commands, and boot-script execution. A minute-resolution cron
scheduler survives deep sleep. OTA uses a signed-manifest flow where the
platform supplies the verification logic and the app supplies its own public
key at init time.

Security runs through all of it: TLS/DTLS uses ChaCha20-Poly1305, not
AES-GCM, to route around the ESP32-S3 AES-GCM hardware DMA bug; auth is cookie
sessions with realms, rate limiting, and force-takeover; and secrets live
under `secrets.*` and never cross to the browser.

Deep dives: [docs/logging.md](docs/logging.md), [docs/cron.md](docs/cron.md),
[docs/ota.md](docs/ota.md), [docs/auth.md](docs/auth.md),
[docs/tls.md](docs/tls.md).

## How it's shipped

- **`core/`** — an ESP-IDF managed component, published as
  `spangap/spangap-core`. Runtime, networking, web stack, storage, log/CLI,
  ITS, config, WebRTC plumbing.
- **`browser/`** — an npm package, `spangap-browser` (unscoped). The paired
  browser pieces.
- **`scripts/`** — operator tools (`flasher`, `reallyclean.sh`) that work
  against any consumer: spangap itself, seccam, reticulous.

The per-module map lives in [CLAUDE.md](CLAUDE.md).

## Hardware

The ESP32-S3 is the only currently targeted chip: dual-core, WiFi, with PSRAM
(the platform assumes PSRAM exists, but flash and PSRAM sizes are not pinned —
the partition layout is the only size-dependent piece and apps may override
it). No other Espressif chip is supported today, but nothing in the
architecture is S3-specific: ITS, storage, the web/WebRTC stack, and the
DRAM/PSRAM placement discipline are all portable. Bringing up the classic
ESP32 or the ESP32-C5 should be a contained effort, mostly chip-init and
peripheral glue rather than a redesign — see
[docs/plans/future-ESP32-C5-and-classic.md](docs/plans/future-ESP32-C5-and-classic.md).

## Claude Code

The overwhelming majority of thsi project has been written by Claude Code. This was all done in about a month. It's obvious to me that a project like would have been an order of magnitude more work for a single person to pull off without LLMs.

That said, I have read and iterated on every .h file and made almost every architecural decision in the project myself. I've careflly read every .h file and at least glanced over the cpp implementations. The browser code I'be mostly left to Claude so far.

The CLAUDE.md files with the project have been kept human-readable and should help further development, whether by humans or AI tools.

### security implications

While I have had separate context hunt of security and made sure the design is at least planned to be securable, this code, as it is presently, should not be used if your life depends on things being unhackable.

---

Per-subsystem deep dives live under [`docs/`](docs/) and are indexed from
[`CLAUDE.md`](CLAUDE.md); start there before working in a subsystem.
