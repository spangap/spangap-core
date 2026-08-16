# safe mode — state backup, restore, and factory reset

Three device operations that all need the same thing: a running system that is
**not doing anything else to the state store**. Safe mode is that system — a
normal boot that stops early, does one thing, and reboots.

- **Backup** — stream the entire active state store out as a
  `<fw>_<host>_<yyyymmddhhmmss>_<used>kB.tgz`, generated on the fly.
- **Restore** — take such a file back, inflate and untar it straight onto a
  freshly formatted store, reboot.
- **Factory reset** — overwrite the whole flash state region with random bytes
  and reboot, so the next boot builds a fresh store wherever *this* firmware
  places one.

None of the three can hold the archive anywhere: not in RAM, not on flash. Both
transfer directions therefore stream, and the design is shaped almost entirely by
that constraint plus its consequence — that a restore has a point of no return in
the middle of it.

Spans [spangap-core](../README.md) (the boot, the filesystem work, the CLI) and
[spangap-web](../../spangap-web/README.md) (the page and the one endpoint).

## Entering: the flag names the operation

A storage flag, and the **key** says which operation, so there is no mode menu
and no landing page. The device boots and does the thing.

```
set s.sys.backup=1          → stream the archive out
set s.sys.restore=1         → take an archive in
set s.sys.factory_reset=N   → 1 = flash, 2 = SD, 3 = both
```

Every message safe mode emits goes through the platform logger (`info`/`warn`/
`err`), including the ones from `readSafeModeFlags()` that fire before
`logInit()` — those reach the native ESP-IDF logger, so they carry no trailing
newline, exactly like the two lines at the top of `spangapInit()`. Nothing here
`printf`s to the console directly.

Setting any of them on a running system **saves and reboots at once** — the write
is the request. That is what makes the CLI, the browser's Settings → System
buttons, an rnsh session and a `/state/boot` line all one mechanism: no new
transport, no new credential, no endpoint.

There are two roads to that reboot, and the difference matters when one of them
misbehaves. The CLI verbs `backup`, `restore` and `reset factory [flash|sd|both]`
set the flag and restart **inline**, on the task already running the command. Any
other writer — the browser, rnsh, a boot script — cannot restart the device
itself, so a watcher on the cron task
(`spangapWatchSafeModeFlags`) notices the write and does it for them. The watcher
logs every `s.sys.*` change it is handed, which is what tells a silent button
apart from a write that never arrived.

**A flag is an edge, not a state.** The storage actor dedups a write whose value
already equals the committed one ([storage-internals](storage-internals.md#3-the-actor-op-list-framing-and-apply-pipeline)),
so setting a flag that is *already* `1` produces no notification and no reboot —
and since the reboot is what clears it, a flag left set by an attempt that did
not complete would swallow every later request, for good. Two things prevent
that: writers clear the flag before setting it, so there is always a `0→1` edge;
and `readSafeModeFlags()` clears all three at boot, so no boot ever leaves one
behind. The CLI verbs are immune either way — they reboot whether or not the
write registered as a change.

The entry gate **is** the authentication: setting the flag needs an authenticated
storage write or physical console access, and a device you cannot reach at all
wants a factory reset, not a login prompt.

`spangapInit()` reads the three keys immediately after `storageLoad()`, unsets
whichever are set, and flushes once — before anything else happens. A crash
anywhere inside safe mode therefore comes back into a **normal** boot. That one
flush is the only write safe mode makes outside the operation itself. If more
than one key is set it takes `factory_reset` > `restore` > `backup` and says so.

`spangapSafeMode()` reports the resulting `safe_mode_t` for the rest of the boot.

## What comes up

`serviceRegister()` carries a **band** (`service_band_t`, [service.h]
(../esp-idf/include/service.h)) that the generator already knows from
`init_order()`: core, net, web and lcd register `SERVICE_BAND_SAFE`, every
straddle `SERVICE_BAND_FULL`. Safe mode runs the SAFE band only. No per-service
virtual, no per-straddle opt-in, no default for a future straddle to get wrong —
one argument on the registration and one `if` in the walk.

| Phase | Safe mode |
|---|---|
| `serviceRunStart()` | unchanged — board HAL, bare hardware |
| `spangapInit()` | unchanged — fs mounts, state-store choice, `storageLoad`, log, CLI, pm, auth — except the eager `cronWakeupHandler()`, skipped |
| `spangapSettingsGenDefaults()` | skipped |
| `serviceRunInit()` | the SAFE band: storage, net, web, lcd. **cron** and **webrtc** additionally skip themselves; **net keeps the radio down in a factory reset** |
| `spangapSettingsGenRegister()` | skipped |
| `spangapPostAppInit()` | boot script and first cron poll skipped; `sys.boot_complete` still published |

Storage comes up **fully**, so safe mode gets real WiFi credentials, hostname,
TLS certificate and admin hash with no special-casing. This is a normal boot that
stops early, not a stripped-down parallel bring-up.

Three things opt out inside the band. **cron** does not start: firing scheduled
commands in a recovery mode is wrong, and one of them may be the thing being
restored away. **webrtc** does not start: its storage DataChannel is a config
write path into a store that is about to be replaced, and the SPA it exists to
serve is not what safe mode serves. **net keeps the radio down in a factory
reset** — see below.

**The screen comes up.** A board with a panel brings the shell up as usual and
spangap-lcd covers it with what this boot is doing
([safe_screen.cpp](../../spangap-lcd/esp-idf/src/lcd_ui/safe_screen.cpp)):
`WIPING FLASH` in the largest type the panel takes, over a progress bar, with
`Do not power off`; `BACKING UP` / `RESTORING` for the other two. The layer is
opaque and clickable on `lv_layer_top`, so the shell behind it is never
reachable. It reads one ephemeral key and draws — it touches no store, which is
what makes it safe in the one boot whose whole purpose is that nothing else
touches the store.

The bar advances in steps, not smoothly: an SPI-flash erase disables the flash
cache, so every task running from flash is stopped for the length of each erase
op and runs again between them. A bar that moves at all is the answer to "is it
stuck?", and a smooth one is not available at any price here.

### The radio stays down in a factory reset

A factory-reset boot exists to erase the store and restart. Everything a radio
would be for — joining the network this device is configured for, standing its AP
up, answering for a hostname — describes a device that is about to stop existing,
so `netInit` leaves `rtcWantUp` false for that mode. The stack still comes up (the
console, the log and the socket relay ride it); only the radio does not.

Backup and restore are the other way round and keep it: those modes are *reached*
over the network. This is also the one thing a factory reset trades away — the
served estimate page below has nobody to serve on a headless node — which is the
price of the panel report above and of not putting a device that is being erased
back on the air to do it.

## What is suppressed

**Storage flushing.** Once a restore or a factory reset begins, storage must not
flush again, or the stale in-RAM tree lands on top of the restored files, or into
a partition being erased. One one-way boolean, checked at the top of
`writeSettingsFile`:

```c
void storageStopFlushing();   /* one-way; cleared only by reboot */
```

It needs no enforcement in the `fs` worker and no caller exemption table,
*because in safe mode nothing else is writing to the store*. That is the whole
argument for the mode. (A backup does not need it — it only reads, and the atomic
`<file>.new` + rename discipline means a concurrent flush is invisible to it.)

**Web's mapping table.** In safe mode web does not **load** `s.web.map` and
short-circuits routing ahead of `findMapping`: every request resolves to the
safe-mode page or the one gated endpoint. That — not skipping the seeding, which
only ever writes on a first boot — is what makes `/state`, `/fixed` and `/sdcard`
unreachable over HTTP and WebDAV for the window, and it bounds the entire
reachable surface to one page and one endpoint.

## The page

One compiled-in HTML string per operation, served from the routing
short-circuit, so `/` and any stray path get it. **Not** a file in `/fixed`: a
recovery mode that depends on the webroot being intact has a hole in it, and a
broken webroot is one of the states you enter safe mode to repair.

One path is exempt: anything under the endpoint's own prefix that the endpoint
did not claim gets a `404`, never the page. The backup page navigates to the
download the moment it loads, so a page served *at* the download URL
re-navigates to itself, for ever — the tab spins with nothing to show. A `404`
there is the honest answer and a dead end.

Each page is **one modal card on a dimmed scrim**, the same shape as the
confirmation dialog the operator clicked through in the SPA and as the cover
`lib/safeMode` holds up over the reboot. The three are one sequence of dialogs
that happens to span a restart; a recovery page in a style of its own would read
as a fourth, unrelated program.

Every page has exactly two states — one message while the operation runs, one
when it is done — and each done state says what happened and what the device is
doing about it.

| Mode | Running | Done |
|---|---|---|
| `BACKUP` | "Stand by as your data is gathered and sent to your browser as `<name>`", then fetches and saves it | "You should have the file in your download folder now. Rebooting back to normal operation", then hands back |
| `RESTORE` | "Select or drop file to upload"; picking one reveals the red **DELETE state and restore from this file.** button, which uploads via `fetch('/backup/' + name, {method:'POST', body: file})` | "Restored N entries", a hostname warning, then hands back |
| `FACTORY_RESET` | nothing is served: the radio is down for this boot (above). A board with a panel reports there — `WIPING FLASH` over a real progress bar; a headless one reports on the console | the device restarts as a new one — no hand-back |

The backup page is served **with the archive's name already in it**, and the
name is computed once per boot: the operator is told the filename before there
is a `Content-Disposition` to read it off, and recomputing it would let the
timestamp field tick between the page and the download, printing one name while
saving another.

The restore page is the only one that waits for the user, and the only one with
buttons. Choosing a file is **inert** — it reveals the red button and touches
nothing — because the pick is reversible and the commit is not, and a drop that
started the erase by itself would make an accidental drag the last thing that
ever happened to this device's state. Until that button is pressed, the green
**cancel and reboot** leaves with the store untouched, via a `GET
/backup/cancel` that reboots device-side like every other exit. A `GET` cannot
collide with an upload (`POST`/`PUT`), so `cancel` is not a reserved filename.

The backup is **fetched, not navigated to**. A navigation to an attachment gives
the page no completion event, and completion is the whole point: it is what tells
the operator the archive is whole and what starts the hand-back. Reading the body
also turns a truncated chunked stream into a rejected read, where a navigation
would have saved a short file in silence.

**Handing back.** A done page shows its message for five seconds and then simply
navigates to `/`. It does **not** probe first. Probing looked tidier and was
worse: a restore can change the hostname *and* the TLS certificate, and a browser
`fetch` cannot tell "certificate the browser will not accept" from "device still
down" — both are an opaque rejected promise, so the page sat waiting out a device
that was already up and serving. Navigating hands the problem to the one
component that can actually show it: the browser renders a certificate
interstitial, a name that no longer resolves, or the app. A factory reset is the
exception with nothing to hand back to — the wipe took the WiFi credentials with
it, so the device comes up on its own access point and this browser's network is
no longer where it is.

A restore additionally warns that the hostname came out of the archive, so a
restore from another device moves this one to that device's name — and the
address this page is served on stops resolving to it.

Safe mode enforces the `admin` realm when `authEnabled()` and **falls open when
it is not** — exactly the fresh-or-broken-store case that must stay reachable.
When a password is set but the browser has no session cookie, the page is a
sign-in form that posts to the ordinary `/auth/login`.

## Exit

**Every** exit is a reboot, decided server-side — last chunk drained, or
extraction verified, or wipe finished, or a restore cancelled before it started. No client acknowledgement and no polling
handshake; the browser sees the connection go away, which is what it would see
anyway. The restart rides a one-shot timer armed as the handler returns, not
inline: web drains and disconnects *after* a handler returns, so an inline
`esp_restart()` would cut its own response off.

One fixed deadline, armed at boot: **10 minutes, then reboot**, regardless of
progress. A state store is a few hundred KB over WiFi; a transfer that has not
finished inside that is dead, and progress-extension plumbing would serve only
already-dead transfers. A power-management lock is held for the window so nothing
deep-sleeps mid-transfer.

**A factory reset ignores all of this and starts at boot regardless of any
client** — `spangapPostAppInit()` kicks it off, so it happens on a headless node,
and it happens with the radio deliberately down and nothing served. There is
nobody to wait for: the operator asked for it on the boot before this one, and
this boot's only job is to carry it out.

---

## Backup

```
GET /backup/state.tgz                       (BACKUP mode only; 404 otherwise)
  → 200
    Content-Type: application/gzip
    Content-Disposition: attachment; filename="reticulous_tdeck1_20260804101500_412kB.tgz"
    Transfer-Encoding: chunked
    <chunk><chunk>…0\r\n\r\n
  → esp_restart()
```

### Why chunked

`Content-Length` is unavailable: the body is a gzip of a tar generated while
walking the filesystem, so its length is not known until it has been produced,
and producing it in advance means storing it — the one thing we cannot do.

That leaves chunked or connection-close, and connection-close is the trap. Under
it a truncated transfer — device reboot, WiFi drop, TCP reset — is byte-for-byte
indistinguishable from a complete one: the client sees the socket close and
assumes it has everything. The operator keeps a truncated `.tgz` and finds out at
restore time, which is the worst possible moment. Under chunked the terminating
zero chunk never arrives, so `curl` exits non-zero and the browser marks the
download failed.

This is a *transport* completeness check, and it stacks with the gzip footer's
CRC32/ISIZE, which is a *content* correctness check. They catch different
failures — a stream can arrive complete and corrupt, or intact and truncated —
and the restore path checks both. No `Range`, no resume: the stream is generated,
not stored.

### What is in it

The walk is `fs_listdir` per directory (one round-trip a level, a PSRAM listing
array per level, depth-capped at 8), each file read in 16 KB bites through a
512-byte ustar header, everything passing through `tdefl` incrementally and out
as HTTP chunks with back-pressure.

`tdefl` is driven with an **explicit output buffer** — `tdefl_init(…, nullptr,
nullptr, …)` then `tdefl_compress` in a loop — never with its `put_buf` callback
and `tdefl_compress_buffer`. That is the shape this platform's ROM miniz is
known good for and the one storage.cpp's gzip has always used; the callback
shape yielded a compressor that swallowed input and emitted nothing, so the
client got the 10-byte gzip header and then silence.

Paths are stored **relative to the state dir**, so a `/state` backup restores
onto an SD store and vice versa. Excluded: `*.new` (in-flight atomic writes),
dotfiles, and `flashme.bin` (the updater's staged firmware image — megabytes,
meaningless in a state backup). Logs and recordings live under `/sdcard`, outside
the store, and fall out of the walk naturally.

### The filename is the manifest

```
reticulous_tdeck1_20260804101500_412kB.tgz
└ fw stub   └ host  └ localtime      └ allocation needed
```

There is no `MANIFEST` tar entry, and only one field is ever machine-read.

| Field | Used for |
|---|---|
| fw stub | Cross-project refusal — **the only parsed field**, and only on the way back in. |
| host | Which device this came from. Informational. |
| `yyyymmddhhmmss` | Localtime at backup; the literal `nodate` when `sys.time.valid` is 0, rather than a fabricated 1970 stamp. |
| `<x>kB` | What the content needs **allocated** at the far end, in kB. LittleFS reports allocated blocks directly; on SD each file is rounded up the same way a block allocator would, so one archive reports one number whichever medium it came off. (Summing raw bytes on SD against LittleFS's allocated figure on flash gave ~400 kB and ~640 kB for the same files.) Informational: there is no size precheck. |

Served as `Content-Disposition: attachment`, so a browser saves it under that
name by default (`curl -OJ` likewise; plain `curl -O` would keep the request path
and lose the metadata).

---

## Restore

```
POST /backup/reticulous_tdeck1_20260804101500_412kB.tgz     ← the page
PUT  /backup/reticulous_tdeck1_20260804101500_412kB.tgz     ← curl -T
    <raw streamed body>                     (RESTORE mode only; 404 otherwise)

  parse the fw stub from the trailing path segment — absent/unparseable warns and continues
    → fw stub mismatch                   → 409, nothing touched
    → no gzip magic on the first bytes   → 415, nothing touched
  storageStopFlushing()
  format the store                       ← point of no return
  write .restore-active
  inflate + untar directly onto it
  verify gzip CRC32 / ISIZE
  remove .restore-active
  → 200 {"ok":true,"entries":37,"bytes":412996}
  → esp_restart()
```

**The page checks the archive before the device commits to it.** A restore
formats first — it has to, there is nowhere to stage — so a damaged archive
costs the operator everything that was on the device and leaves them at factory
defaults. The browser is already holding the whole file and can inflate it end
to end, CRC included, via `DecompressionStream` at no cost to the device: the
one point in this design where truncation can be caught while it is still free.
A browser without `DecompressionStream` proceeds, and the device's own footer
check still backstops it — just later, and at the price the format already
exacted.

**Raw body, no multipart.** The page uploads with
`fetch(url, {method:'POST', body: fileObject})`, and `curl --data-binary @f.tgz`
already works that way. spangap-web has no multipart parser and this keeps it
that way — multipart exists only to serve a `<form>` submit we do not use, and
never writing the parser avoids a boundary-splitting bug class outright.

The upload body carries no filename of its own, so the name **is the last path
segment** of the URL: the page appends the `File` object's name, and
`curl -T <file> https://host/backup/` appends the local filename to a URL ending
in `/` by itself. Nobody types the name in either direction. Both verbs are
accepted on the prefix.

### Format, not erase

`fsFormatStateStore()` gives an empty, mounted, writable store at the current
computed location in one call, and extraction starts immediately. A raw erase
would leave nothing mountable and force a reboot between wipe and write — the
staging problem again in a different costume. On an SD-backed store it is a
recursive clear of `/sdcard/state`, not a card format.

Format-first is what buys the space guarantee: you need room for the expanded
state, not for the archive *plus* the state. Given no room to stage, that is as
good as the space problem gets.

### Failure model

There is no rollback to the previous state. Without space to stage, no scheme
has one — the only choice is which failure you get, and format-first makes it the
good one:

- The store is already empty when the risky part starts.
- A crash, stall, truncated upload, or CRC mismatch leaves a partial store.
- `.restore-active` is written immediately after the format and removed only
  after the CRC verifies. `fsSelectStateStore()` treats its presence as
  "suspect — format and treat as first boot", repopulating from
  `/fixed/factory_state` + `/fixed/additional_state`.

So **every** failure resolves to a clean factory store, never to a
plausible-looking corrupt one. This marker is the only crash-safety artifact in
the design; a factory reset needs none (see below).

The reader **withholds the last eight bytes** of the stream from the
decompressor and makes one final `tinfl_decompress` pass with
`TINFL_FLAG_HAS_MORE_INPUT` cleared once the upload has ended. Both halves are
load-bearing, and neither is tidiness:

- Under that flag miniz answers "need more input" rather than finishing, and
  closing a final deflate block can want a few more bits — so a complete, valid
  archive may never report `DONE`. Clearing it on the last call is the only way
  it will close.
- The footer is identified by **position**, not by asking tinfl what it did not
  consume. By the time it stops it has already drawn those bytes into its bit
  buffer and counted them consumed, so there is nothing to hand back. gzip's
  footer is the last eight bytes of the stream by definition; holding a rolling
  eight-byte window costs nothing and does not depend on the decompressor's
  internals at all.

Getting either wrong produces `archive truncated` on an archive that `gzip -t`
calls sound — and because it turns on where the last block ends within a byte,
it strikes some archives and not others of the same size.

Integrity is confirmed only *after* application, because the gzip footer arrives
last — inherent to streaming without staging, and the price of the constraint.
`Content-Length` is an early sanity check, not the truncation detector; the gzip
footer is. Chunked uploads work.

### The one check that is not a failure-lands-clean case

The fw stub earns its parser because its failure mode is the exception: a
cross-project restore **succeeds** and then self-destructs. `s.sys.project`
mismatch makes the next boot factory-reset the flash store (or, on an SD-backed
store, reset-loop), so the archive silently destroys what it just restored.
Mismatch → refuse; absent or unparseable → warn and proceed, since a stripped
filename cannot be told from a foreign one.

### Hardening

The archive is attacker-supplied input written directly to a filesystem, so the
reader — not the caller — rejects absolute paths, `..` components, over-long
ustar names, and every typeflag other than regular file and directory (links,
devices, FIFOs, GNU long-name extensions).

---

## Factory reset

**Write random bytes over the whole flash state region, then reboot.** No fast
path, no head-only variant. Starts at boot, needs no client.

### Why random, and not a format

Erasing to `0xFF` unlinks the store but leaves everything past the superblock
intact on flash — the Reticulum identity, WiFi passwords, WireGuard and TLS keys
all recoverable with a flash dump. A factory reset has to make a device safe to
hand on, so the whole extent is overwritten. Random rather than zeros because it
is the same cost and leaves no structure at all.

Formatting would rewrite LittleFS at whatever location *this* firmware computes
and leave a perfectly findable superblock behind — flashmon's
`det_state_partition()` would still detect it, still report it, and still warn
that the image writes into the state partition, permanently. Formatting also
cannot *move* the store: position is recomputed from the table top every boot, so
a store left low by an older, lower-floored firmware stays low through any number
of formats. Overwriting and rebooting lets `statePartitionEnsure()` place a fresh
one where the current firmware thinks it belongs.

### Extent

From the **end of the last firmware partition to the end of the physical chip** —
the table top excluding `state`, with `reserved` folded in. `reserved` is inert
filler by construction so overwriting it costs nothing, and it is the one region
where a stale, lower superblock can survive: a predecessor firmware with a lower
floor put its store inside what this firmware's table calls `reserved`, and
flashing the new image does not write there. Anchoring at the floor would leave
that one behind — which is exactly the case this exists to fix.

The extent cannot be phrased as "`reserved`'s start": gen-partitions emits no
`reserved` row at all when `fixed` reaches the floor, so the anchor is the end of
the last firmware partition, which equals `reserved`'s start whenever one exists.

When a board pins `state` in its own table there is no filler and no ambiguity:
overwrite that partition and nothing else. `nvs` sits below the floor and is
untouched — a factory reset must not discard WiFi PHY calibration.

### SD

`s.sys.factory_reset` selects the target: `1` flash, `2` SD, `3` both. The flash
region can hold a stale store even when the active one is on the card, which is
why "both" exists.

The SD side is a plain recursive delete of `/sdcard/state` — not a card format
(recordings and logs live outside the store) and **not** a random overwrite. An
SD controller does its own wear levelling, so writing over a file's logical
blocks says nothing about the physical ones; the overwrite would cost time and
buy no guarantee. The flash path has no such indirection, which is why the random
overwrite is real there and pointless here.

### Implementation constraints

- **Wipe low-to-high.** The superblock at block 0 dies first, so a crash mid-wipe
  leaves a store whose mount fails → `format_if_mount_failed` → empty → first
  boot. That is why a factory reset needs no marker and no recovery path.
- **The random source buffer must be internal DRAM**, and the whole operation
  runs on a DRAM-stack worker: a flash program disables the PSRAM cache, so
  reading the source out of PSRAM mid-write faults.
- Per block: `esp_flash_erase_region` then `esp_flash_write` of a DRAM-resident
  buffer refreshed from `esp_fill_random()`. The radio is down for this boot, so
  that RNG is running on its non-RF entropy — which does not matter here: what
  makes the wipe safe is that every byte is overwritten, not that the bytes are
  unpredictable. A perfectly guessable pattern recovers nothing of what was
  there. Do not reuse this buffer for anything that needs real randomness.
- Never round the region's start down — that clobbers the firmware table.
- Feed the task watchdog between blocks.

### Timing

`FS_WIPE_MS_PER_MB` in [`fs.h`](../esp-idf/include/fs.h) is what
spangap-web's `factoryPage()` estimate is computed from. It is a datasheet-typical
figure for the W25Q/GD25Q-class parts these boards carry — erase plus random
overwrite at roughly 4.5–5 s/MB, so about a minute for a 12 MB region.

That page is now unreachable during the operation it describes: the radio is down
for a factory-reset boot (above), so nothing connects to be served it. It is left
in place rather than deleted — it costs nothing, and it is what a build that
brings the radio back up would serve. **The reports that do reach an operator are
the panel's bar and the console's `factory reset: N%`, and both are real
progress** (`sys.wipe.percent`, published per block from bytes actually written),
not an estimate — so neither depends on that constant being right.

---

## Memory budget

| Path | Cost |
|---|---|
| Backup | `tdefl_compressor` ~160 KB PSRAM + 16 KB file read + 8 KB deflate output |
| Restore | `tinfl_decompressor` ~11 KB + 32 KB wrapping LZ dictionary + a 512 B tar header |
| Wipe | one 64 KB DRAM random buffer (falls back to 16 KB, then 4 KB) |

The restore's 32 KB dictionary must wrap: the output is not resident, which is
the whole point, so the LZ77 back-reference history has to be.

There is no uncompressed-`.tar` fallback for a board where the 160 KB will not
allocate. Safe mode is the boot where that is least likely to be true; reinstate
it if a real board fails.

LittleFS write throughput is ~3–7 s/MB in practice: raw programming is ~2 s/MB,
and this platform's 8 KB-chunk-with-a-tick-yield discipline adds ~1.3 s/MB of
pure sleep at a 100 Hz tick. That yield is **not optional** during a restore — we
are receiving over TCP while writing, and each program disables the PSRAM cache,
so long unyielded bursts stall the network task feeding us. A typical state store
is a few hundred KB, so extraction is 1–3 s and the upload dominates.

## Where the code is

| Where | What |
|---|---|
| [`service.h`](../esp-idf/include/service.h), `service.cpp` | the band on `serviceRegister`, the boundary check in `serviceRunInit` |
| `spangap-inside` | emits the band per registration and per trampoline |
| [`spangap.h`](../esp-idf/include/spangap.h), `spangap_init.cpp` | the flags, `spangapSafeMode()`, the skips, the wipe task |
| `storage.cpp` | `storageStopFlushing()`, and `storageSave()` before the persist worker exists |
| [`targz.h`](../esp-idf/include/targz.h), `targz.cpp` | the streaming ustar writer and the streaming gunzip/untar reader |
| [`fs.h`](../esp-idf/include/fs.h), `fs.cpp` | `.restore-active`, `fsFormatStateStore()`, `fsFactoryWipeExtent()`, `fsWipeFlashState()`, `fsClearSdState()` |
| `cli_cmd_sys.cpp` | `reset factory [flash\|sd\|both]` |
| spangap-web `web.cpp` | the mapping-table skip and the routing short-circuit |
| spangap-web `safe_mode.{h,cpp}` | the three pages, the gated endpoint, the deadline |
| spangap-web `browser/` | the Settings → System buttons, their warnings, and the reboot wait |

## Rejected

| Alternative | Why not |
|---|---|
| Hot backup/restore against the live system | Needs a flush hold, a write seal enforced in the `fs` worker, and an originating-task field on `fs_op_t` — and still cannot format-first. Strictly more code than the mode. |
| Stage the archive to flash, apply at boot | No space. This was the original design and the constraint killed it. |
| Extract to `<stateDir>.new/` and swap | Double the space, and LittleFS has no atomic directory swap. |
| Two-pass upload (verify, then apply) | Doubles the transfer and still leaves the apply pass unprotected. |
| RTC-RAM entry magic | The storage flag covers every reachable case; the unreachable case wants a factory reset, which a board's `onStart()` can trigger from a GPIO before storage exists. |
| One generic safe mode with a menu page | The flag can name the operation, which deletes the menu, the landing page, and most of the client handshake. |
| A one-time token for safe-mode auth | If you can enter safe mode you are already authenticated; the existing cookie check costs nothing and the endpoint gate bounds the exposure. |
| A `Service::inMaintenance()` virtual | Per-straddle opt-in with a default a future straddle gets wrong, where the registry already encodes the band. |
| Multipart upload parsing | Our page controls the upload; a raw body serves both it and curl. |
| A `MANIFEST` tar entry | The filename carries the same fields, where a human already reads them, with no writer, no parser and no first-entry ordering constraint. |
| Overwriting SD files before unlink | The card's wear levelling makes a logical overwrite meaningless. |
| Head-only factory reset | Leaves every secret recoverable from a flash dump. |
| An uncompressed `.tar` fallback | Dead code in the one boot with the most free PSRAM. |
| A size-fit precheck on the filename's `<x>kB` | `ENOSPC` mid-extraction lands in the same clean-factory path. |
| A factory-reset progress endpoint | A client-side bar over a served estimate says the same thing with no endpoint and no polling through a janky mid-wipe HTTP stack. |
| A progress-extended deadline | One fixed 10-minute timer covers every live transfer; extension plumbing serves only dead ones. |
| A factory-reset progress key polled by the browser | `sys.wipe.percent` exists for the panel, which is on the same chip as the writer. Feeding it back out to a client would rebuild the endpoint rejected above, through the same mid-wipe HTTP stack, for a page the radio is now down for anyway. |
