# Framed RPC on the console port

A host tool that wants to *know* things about a device — what image is running,
whether a password is set, which networks are in range — has historically had to
read them out of the boot log with regexes. That is fragile: the interesting
lines scroll past once and never repeat, the log is suppressed the moment
anything is typed at the console (which switches it into a CLI session), and
every fact depends on a log string nobody thinks of as an API.

Framed RPC is the replacement. It is a small framed side-channel multiplexed
onto the existing console port, interleaved with log and CLI traffic, over which
a host runs an ordinary CLI command and gets exactly that command's output back.

This document is the contract. Both ends implement it: the device in
[`cli.cpp`](../esp-idf/src/cli.cpp) (the serial task), and flashmon in
`flashmon/flashmon/flashmon.js` (search `RPC_MAGIC`).

## Wire format

One frame layout, both directions:

```
host → device   <magic:4> <id:1> <len:2> <command bytes>
device → host   <magic:4> <id:1> <len:2> <reply bytes>
```

| Field | Value |
|---|---|
| magic | `F5 53 47 01` — `0xF5`, `'S'`, `'G'`, version `0x01` |
| id | one byte, opaque to the device, echoed into the reply |
| len | payload length, 2 bytes big-endian, payload only |

The magic leads with `0xF5`, which cannot appear in valid UTF-8, so every byte
of ordinary console traffic fails the match on byte one. The length prefix drops
any constraint on which byte values may appear in a payload.

Frames are never echoed, never enter the line editor, and never flip the console
from log into CLI mode. A frame arriving while a person has a CLI session open
leaves their line in progress untouched, because the command runs on its own
session. The host swallows frames out of the byte stream and displays the rest
unchanged, so log and interactive CLI keep working exactly as they did.

There is no integrity check beyond the magic, deliberately. The peer on the
serial console can already type `reset factory`; there is nothing to defend
against here, only line noise to recover from.

## Semantics

- **Empty reply is an answer.** A command that prints nothing still answers,
  with a zero-length frame — otherwise "no output" and "device didn't answer"
  are indistinguishable.
- **Failure is an answer too.** A command that fails replies with whatever it
  printed. The frame carries no status of its own.
- **Truncation is not signalled.** A reply that would exceed the buffer cap is
  cut at the **last complete line**. A mid-line cut turns `state=ap` into
  `state=a`, which parses as a valid but wrong value; dropping the partial tail
  leaves only the missing-key case every reader already treats as unknown.
- **No progress for ~1 s abandons an inbound frame** and resyncs on the magic.
  What that guards against is a corrupted length — a flaky cable — leaving the
  device allocated and waiting for bytes that never arrive.
- **A command is at most 4096 bytes.** A longer one is not run; the reply is
  the single line `rpc: command over 4096 bytes`. A payload the device cannot
  buffer at all (over 8 KB on a board without PSRAM) is swallowed unanswered,
  as a corrupted length would be.
- **The exec is bounded** at a few seconds. On the deadline the device drops the
  CLI session and replies with whatever had been printed, rather than wedging
  the relay and the console with it.

### The query id

Only ever one frame is in flight; nothing here needs concurrency. The id exists
because a host-side timeout otherwise returns a *wrong* answer rather than no
answer: if the host gives up and the device replies late, that reply is in the
stream when the next query goes out and is taken as its answer.

The id identifies **what was asked**, not when. So a host retrying an unanswered
read reuses the same id, and a late reply to the first attempt is a perfectly
good answer to the second. Duplicate replies for one id are either reprocessed
harmlessly or ignored; a reply whose id does not match the outstanding query is
dropped.

The device never interprets the id — all the policy is on the host, which
derives the id from the command string and so keeps no state either.

Mutating commands need no exception: a reply is only ever produced by an
execution, so a reply carrying this id proves some send of exactly this command
ran. Which execution answered is immaterial, for writes as for reads.

## Capability marker

Firmware built before this existed will never answer a frame, and a frame sent
blind at such firmware is *typed at the console* — the first byte opens a CLI
session and the rest land in its line editor. So the device advertises: it
prints one line the moment the sniffer arms, very early in boot:

```
serial: framed rpc v1
```

A host that sees that line uses frames from then on, at no cost and with no
guessing. That is the fast path, and for a host that watched the device boot —
flash, then watch it come up — it is the only path ever taken.

The marker is also the line under which console input starts counting: the
serial task drains whatever its receive path was already holding immediately
before arming, because on USB-Serial-JTAG that is bytes from before this
firmware was running (see
[usb-console](usb-console.md#input-that-predates-the-console)). A frame sent in
answer to the marker is never in that window.

**The marker alone is not enough**, because it is printed once and very early.
A host that attaches to an already-running device never sees it, and that is the
ordinary case: opening a monitor deliberately does not reset the device. Gating
purely on the marker means such a session silently falls back forever, which is
the failure this mechanism exists to remove, merely moved.

So a host that needs frames and has no marker **probes once**, and the probe is
safe because it is *recoverable*:

- on firmware that speaks frames, the probe is swallowed and answered — it costs
  nothing and is invisible;
- on firmware that does not, the bytes are typed at the console, so the host
  follows with **Ctrl-C** (`0x03`), which that firmware treats as "abort this
  line and go back to the log". The price of guessing wrong is one CLI banner
  and a `Press Ctrl-]` notice in the stream, once per session.

Two details make that recovery reliable. The probe carries no `\n` or `\r`, so
nothing the line editor accumulated is ever executed; and the **query id is
constrained to `0x20..0xBF`**, so it cannot itself be a CR/LF that executes the
garbage, a `0x03` that aborts early, or a `0xC0` that opens a serial-handler
session. The device never interprets the id — it copies it back — so
constraining it costs nothing.

A host that has neither seen the marker nor had a probe answered sends no
further frames for that session.

This is itself a load-bearing log line, of exactly the kind the mechanism exists
to remove — accepted deliberately: one line, one bit plus a version, emitted
before anything can be in flight, and documented as an API on both sides.

That is the whole of the compatibility story: marker, else one recoverable
probe, else nothing. There is no capability negotiation and no second way to
obtain any of these facts — a device that answers neither is simply not known,
which is a state every reader already handles (see the missing-key rule in
[onboarding-output.md](onboarding-output.md)). Guessing from log text is what
this mechanism exists to stop, so there is no fallback that does it.

## Device implementation

The sniffer sits in `handleChar` in the serial task, ahead of both the line
editor and the log/CLI switch, so a frame arriving mid-line does not disturb the
editor and one arriving in log mode does not flip modes.

It is a state machine rather than a buffer compare, and its state lives outside
`handleChar`: `portRead` delivers 128-byte chunks so a preamble can straddle
two, and `handleChar` is called from two places (the idle blocking-read path and
the active drain path), so state kept inside it would break frames depending on
which path happened to receive them.

Three rules:

- **A failed match replays, not drops.** Bytes withheld while the magic was
  partially matched belong to the normal path, so a disagreeing byte sends the
  held bytes through the rest of `handleChar`. The `0xF5` lead makes false
  starts rare, but pasted garbage exists.
- **Mid-frame swallows everything, including `0xC0`.** The frame state machine
  runs ahead of the `0xC0` serial-handler attach check, so an id or length byte
  of `0xC0` must not open a handler session. Idle, the `0xC0` check keeps its
  place.
- **Frames are dead while a handler owns port 0** — bytes go to `hdlPump` and
  never reach `handleChar`. Deliberate: the handler mechanism exists for
  Reticulum clients, and a port claimed for one is for-sure not flashmon.

Transport-agnostic: it sits above both USB-Serial-JTAG and CDC, unlike the
`0xC0` attach, which is gated on `!consoleOnCdc` because it substitutes for a
DTR signal only CDC has.

### The console write lock

Reply frames must not interleave with the log. In log mode the log task echoes
lines straight to stdout from its own task (`logVprintf`), while the serial task
writes frames through the driver — two tasks, two write paths. A log line
landing mid-frame is unrecoverable for the host, which counts the log bytes as
payload; resync-on-magic cannot help once the length has been read.

The direct echo is not the thing to fix — it is what lets logs reach the wire
when the serial task is wedged or not yet up, and what lets the idle serial task
park indefinitely on the driver's RX ring instead of running a notify-and-poll
loop. Instead a console-write mutex is taken around the echo's `fwrite` and
around each whole reply frame. The cost is that the echo can stall for the
duration of one frame write, bounded by the host draining the port.

It is recursive: anything a frame write touches may log, and a log call
re-entering the lock on the task that already holds it would deadlock the
console outright. Interleaving one line into one frame is the lesser failure.

### Execution

The relay reuses the CLI's existing one-shot exec path, so there are no core
changes: a second ITS client connection to `CLI_PORT_TCP` with
`cli_connect_t{CLI_LINE, from_usb_serial=0, CLI_NO_COLOR, no_prompt=1,
login=0}`, fed `"<cmd>;\n"` — the trailing `;` being the CLI's "run this and
close the session" signal — read until the session closes, then framed back out.
This is what ssh `exec` already does.

The session's input stream (512 bytes) is smaller than the longest command, so
the line is fed in as the CLI task drains it: each non-blocking `itsSend` takes
what fits, a one-shot `itsSetFreeNotify` wakes the serial task's `itsPoll` once
the CLI task has read some, and the output is collected in the same loop. The
CLI task, for its part, reads a session 128 bytes at a time and does not park
while any session still has input queued, because ITS notifies once per send,
not once per byte left unread. The command bound is the CLI's: a LINE-mode
session accumulates at most `CLI_LINE_MAX` (4096) bytes plus the trailing `;`,
so the relay refuses anything longer instead of handing over a cut line, which
would run as a different command and lose the `;` that closes the session.

It runs **synchronously** on the serial task. That is what makes a retry
arriving mid-exec need no handling: the second frame waits in the driver's
buffer until the first exec finishes, both get answered, and the host's
duplicate-reply rule absorbs the extra. An async implementation would have to
choose a policy there.

Session accounting had to be made, not found:

- The serial task's `itsClientInit` cap is 3, and it is hard. All three can be
  live at once: a console CLI session, a handler session on port 1, and the RPC
  exec.
- The `CLI_PORT_TCP` pool is 8 (with `CLI_MAX_CLIENTS` 10, 8 TCP + 2 DC), shared
  with the console session and every ssh shell and exec, so a full house of ssh
  sessions cannot starve the RPC of a slot.

### Buffers

The 2-byte length allows 64 KB. Frame buffers come from PSRAM at that cap; 64 KB
of internal DRAM is far too much on a device where every task stack comes out of
it. A board without PSRAM gets an 8 KB internal buffer instead of making PSRAM a
dependency of the transport — every reply a host actually reads fits in a
screenful, and truncation has defined behaviour above.

## What a host asks for

The commands worth knowing about are the ones that answer in `key=value` lines
— see [`-O` onboarding output](onboarding-output.md) — plus `show`, which is
already machine-shaped and takes a prefix, so `show sys.build` and
`show sys.flash` each return a whole subtree in one round trip.

## Later

When commands become callable functions, the RPC handler calls one directly
instead of dialling `cli` — no ITS round trip, no slot, no prompt-suppression
flags. The wire format does not change.

## See also

- [onboarding-output.md](onboarding-output.md) — the `-O` contract the queries
  read.
- [cli.md](cli.md) / [cli-internals.md](cli-internals.md) — the command line the
  relay executes into, and the serial task it runs on.
- [usb-console.md](usb-console.md) — the transports the frames ride on and the
  serial-port handler that suspends them.
