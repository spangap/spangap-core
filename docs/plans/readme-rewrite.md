# Plan: rewrite `spangap/README.md`

Status: outline + content brief. README is **not** rewritten yet — this
document is the agreed skeleton and the precise points each section must hit.
Rewrite the README only after this is signed off.

Decisions locked: heading voice = **problem-as-pain**; structure = **three
acts** (description → problems with *no* solutions → components that map back
to the problems).

## Why the current README needs a rewrite, not edits

The text is a competent first draft but the framing is off and several
descriptions are close-but-wrong:

1. **"The problems it solves" doesn't contain problems.** Its subheadings are
   slogans ("One place for code to wait") or solution names, not statements of
   a pain a device developer actually has. A reader scanning the headings
   learns nothing about *what hurts without spangap*.
2. **Only two problems are claimed**, then "The approach" turns into a flat
   feature dump (web stack, logging, cron, OTA, security, hardware). Most of
   those features exist to solve a real, nameable problem that never gets
   stated. The doc undersells itself.
3. **Imprecise mechanism descriptions.** Examples:
   - ITS is described as "stream connections **plus out-of-band aux
     messages**". That foregrounds aux as a co-equal concept. The real story:
     ITS moves **byte streams and discrete packets between tasks**; aux is the
     minor fire-and-forget case. Forwarding — the genuinely novel part — is
     not mentioned in the problems section at all.
   - "the web server consumes HTTP headers, then hands the live connection to
     the task that owns the path" is in "The approach" but the *point* of
     forwarding (the middle task does zero copying / no proxy buffer) is never
     made.
   - The DRAM-vs-PSRAM problem — arguably the single biggest source of
     embedded crashes spangap quietly removes — is invisible. It appears only
     as a one-line gotcha, never as a problem spangap solves.
4. **No load-bearing ordering.** Sections don't build on each other; ITS,
   storage, WebRTC are presented as peers when storage and WebRTC are both
   *consumers* of ITS.

## Approach — three acts

Separate problems from solutions cleanly, then let the components section earn
its keep by pointing back:

1. **What it is** — two paragraphs, no mechanism.
2. **The problems it solves** — pure problem statements, problem-as-pain
   headings, *no spangap solution text at all*. The reader should finish this
   section nodding "yes, all of those have bitten me" without yet knowing how
   spangap helps. Each problem gets a short tag/anchor so act 3 can reference
   it.
3. **The components** — each component described precisely, and each one
   explicitly says *which problem(s) from act 2 it removes*. Depth lives in
   `docs/` / component READMEs; this section links out rather than inlining
   deep dives. Components ordered by dependency (ITS first — the others are
   its clients).

This keeps the README itself short: act 2 is tight, act 3 is description +
"solves P1/P3" + a link, not a tutorial.

## Skeleton outline

```
# spangap

## What it is
    Para 1: dual-side ESP32-S3 platform; app writes only domain code and
            inherits networking, web, storage, config, log/CLI, OTA, remote
            access.
    Para 2: the "spangap" idea — every feature has a device half and a
            browser half, designed and shipped together; two published
            artifacts (core component + browser npm).

## The problems it solves         [problems only — zero solution text]
    ### Wiring tasks together takes over the codebase            (P1)
    ### Some tasks aren't allowed to touch the filesystem        (P2)
    ### The device and the browser drift out of sync             (P3)
    ### The browser app gets rebuilt from scratch every time     (P4)
    ### A device behind NAT is unreachable and has no real TLS   (P5)
    ### You still have to operate the thing after it ships       (P6)

## The components                 [each maps back to P1..P6 + links out]
    ### ITS — inter-task streaming            → P1 (and substrate for the rest)
    ### Unified filesystem (fs workers)       → P2
    ### Storage / config tree                 → P3
    ### spangap-browser (npm)                 → P4
    ### Remote access + WebRTC                → P5  (P3/P6 ride the same path)
    ### Log / CLI / cron / OTA + security     → P6

## How it's shipped
    two halves published separately, examples, scripts. Point at the module
    map in CLAUDE.md — do not re-list modules.

## Hardware

---
docs/ + CLAUDE.md pointer (kept).
```

(Problem-heading wording is provisional final-pass polish; intent is fixed.)

## Act 2 — the six problems, stated WITHOUT solutions

Each ~3–5 lines. No "spangap does X" — only the pain.

- **P1 — Wiring tasks together takes over the codebase.** Every concern is its
  own FreeRTOS task (WiFi, HTTP, TLS, a radio, storage, a sensor loop).
  Connecting them is bespoke: a queue here, a task notification there, a poll
  loop somewhere else — each with its own timeout, backpressure rule, and race
  to get wrong. The glue outweighs the feature and most of it is busy-waiting.
- **P2 — Some tasks aren't allowed to touch the filesystem.** The ESP32-S3 has
  ~512 KB internal DRAM but 8 MB PSRAM, and they are not interchangeable. Any
  SPI-flash op disables the PSRAM cache, so a PSRAM-stack task that reads a
  LittleFS file crashes. DMA/WiFi/lwIP need *internal* DRAM specifically; SD
  DMA needs internal buffers and breaks under concurrent writers. Normally the
  app author must track every allocation's memory class and every task's stack
  placement by hand.
- **P3 — The device and the browser drift out of sync.** Settings sprawl into
  a pile of bespoke REST endpoints; firmware and UI disagree; "add one
  setting" means touching both sides plus a save path, and getting the
  private-vs-public split right every time.
- **P4 — The browser app gets rebuilt from scratch every time.** Every
  connected device needs the same furniture: a settings UI bound to live
  config, a log viewer, a CLI console, a live media session, an auth flow, a
  menu to hang panels on. It is re-implemented per project.
- **P5 — A device behind NAT is unreachable and has no real TLS.** A box on a
  home LAN can't be reached from outside and has no publicly trusted
  certificate. Punching out, getting a name, and getting a real cert is its
  own sub-project; doing it over many short-lived browser connections is worse.
- **P6 — You still have to operate the thing after it ships.** Seeing what
  it's doing (and *which task* did it), poking it live, scheduling
  maintenance that survives deep sleep, and updating it safely without
  bricking it — each normally hand-rolled, each a place to get security wrong.

## Act 3 — components, each precise and each pointing back

Per component: what it is (precise), the problem(s) it kills, a docs link.
Apply the recurring corrections (below) here.

- **ITS — inter-task streaming** → **P1**, and the substrate every other
  component rides on (say this explicitly).
  - Socket-style point-to-point connections — *TCP between FreeRTOS tasks*.
    Server/client/both; numbered **ports** flow from `itsConnect` to the
    server's `onConnect`, so one task hosts many endpoints (a TCP port, a URL
    path, a DataChannel) over one mechanism.
  - A connection carries either a **byte stream** or **discrete packets** (one
    send = one delivered message). State it as "streams and packets between
    tasks." Aux gets **one clause**: a fire-and-forget message for things that
    aren't a connection (subscribe/notify, register-with-net handshakes) — not
    co-billing.
  - `itsPoll(timeout)` is the single universal blocking primitive: one place a
    task waits, woken there for everything — connection, packet, stream bytes,
    deadline. No second poll loop, idle CPU → 0. (The old "one place for code
    to wait" slogan lands here as the *payoff*.)
  - **Forwarding — make this prominent.** A live connection is handed from one
    server task to another with already-consumed protocol bytes injected back,
    so the new owner re-parses a fresh-looking request and **the middle task
    does zero copying — no proxy buffer, no pump loop.** Real example:
    `net` accepts the socket → `web` parses HTTP headers and decides the route
    → forwards to the owning task (`webrtc`/`storage`/…); after handoff `web`
    is out of the data path.
  - Link: [docs/its.md](../its.md).
- **Unified filesystem — fs/fs_strm workers** → **P2**.
  - All file I/O routed through dedicated **DRAM-stack worker tasks** over ITS;
    any task (PSRAM stack included) calls `fs_*` safely, and the workers
    serialize SDMMC DMA. Note the platform-wide malloc policy (PSRAM-prefer,
    reserved DRAM pool, explicit DMA caps) and that ITS inboxes/buffers live in
    PSRAM made safe by the no-ITS-from-ISR rule. Net effect: app code never
    reasons about cache state or memory class.
  - Link: [docs/unified-fs-api.md](../unified-fs-api.md).
- **Storage / config tree** → **P3**.
  - One in-memory cJSON tree is the contract. `s.*` persisted **and** synced
    to browser; `secrets.*` persisted but **never** sent; unprefixed =
    ephemeral. Modules subscribe to key prefixes and react. Self-registering,
    version-gated defaults so adding a key never disturbs a saved value. Sync
    is one packet-mode `storage:1` channel both ways — itself just an ITS
    server (ties back to ITS).
  - Link: [docs/storage.md](../storage.md).
- **spangap-browser (npm)** → **P4**.
  - Ships the furniture once: config-bound controls, FloatingWindow, the
    WebRTC session manager, storage sync, log/CLI rendering, auth flow, menu
    registry. An app registers its own panels and writes nothing else.
  - Link: [docs/web-interface.md](../web-interface.md).
- **Remote access + WebRTC** → **P5** (and P3/P6 ride the same transport).
  - Remote-access stack: UPnP + DuckDNS + ACME for a real cert and
    reachability behind NAT; WireGuard as the alternative path in.
  - WebRTC-for-everything: one `RTCPeerConnection`/DTLS per tab carries every
    data path. The WebRTC task is **content-free** — terminates DTLS/ICE/SCTP
    and routes a DataChannel `<task>:<n>` to ITS port `n`, both directions.
    Adding a channel = open an ITS port + name a DataChannel. Per-channel
    reliability. (Another ITS consumer.)
  - Links: [docs/remote-access.md](../remote-access.md),
    [docs/webrtc.md](../webrtc.md).
- **Log / CLI / cron / OTA (+ security)** → **P6**.
  - Log task hooks ESP-IDF `vprintf`, tags every line with its originating
    task, DRAM ring buffer, fans out to serial/browser/file. Full
    line-editing CLI with history, registrable commands, boot scripts.
    Deep-sleep-surviving minute-resolution cron. Signed-manifest OTA where the
    platform owns verification and the app supplies its own public key.
  - Security as a closing note: ChaCha20-Poly1305 to dodge the ESP32-S3
    AES-GCM DMA bug; cookie sessions with realms, rate limiting,
    force-takeover; `secrets.*` never crosses to the browser.
  - Links: [docs/logging.md](../logging.md), [docs/cron.md](../cron.md),
    [docs/ota.md](../ota.md), [docs/auth.md](../auth.md),
    [docs/tls.md](../tls.md).

## Recurring corrections to apply everywhere

- Never bill "aux messages" as a headline ITS concept. ITS = streams + packets
  between tasks; aux is the small fire-and-forget exception.
- Always state the *payoff* of forwarding (zero-copy / middle task leaves the
  data path), not just the mechanism. Use net→web→webrtc as the example.
- Storage sync, WebRTC routing, log/CLI transport are **ITS clients/servers** —
  say so, so the doc reads as one architecture, not a feature list.
- Headings name problems (act 2) or components (act 3), never slogans.
- Act 2 contains **no** solution text. Act 3 components **always** name the
  problem they solve.

## Open question (resolve before rewriting)

1. In act 3, link out to existing `docs/*.md` only, or also create dedicated
   per-component README stubs the user mentioned ("some of which may or may
   not link to component READMEs")? Plan currently links to existing
   `docs/*.md`; creating new component READMEs is a separate task if wanted.
```
