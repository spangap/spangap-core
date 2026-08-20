# cron — minute-resolution scheduler with deep sleep

`cron` runs commands on a schedule, at one-minute resolution, and doubles as the
device's deep-sleep driver. Entries are storage keys; whenever one is added,
changed or removed, cron computes the first minute any entry must run and stores
it, so the steady state is a clock compare — not a re-scan every minute. When
that minute arrives it feeds the due commands into the CLI and computes the next
one. On a battery device it also owns the path into and out of deep sleep: when
nothing needs the CPU it puts the device to sleep toward the next scheduled
minute, and a fast wake handler decides on boot whether that minute has arrived
or whether to drop straight back to sleep.

It is part of spangap-core, so it starts automatically whenever the straddle is
in the build — there is nothing to call.

## Entries

One entry = one storage key:

```
s.cron.tab.<name> = "min hour dom month dow flags command"
```

The value is a single line in standard unix cron format (no user field), edited
like any other config key (web settings tree, `set` on the CLI). The `<name>` is
free-form; modules use their own name (`upnp`, `duckdns`, `acme`, `logrotate`).

The five time fields use standard cron field syntax — `*` (any), a plain number,
`,` lists, `-` ranges, and `/` steps (including `*/5` and `1-30/2`). There are no
named months/days and no `@`-style shortcuts. `dow` is `0`–`7` with both `0` and
`7` meaning Sunday. All five fields must match (dom and dow are ANDed).
Everything after the flags field is the command, passed verbatim to the CLI.

The **flags** column sits between the time fields and the command and gates *when
a matched entry actually runs*:

| Flag | Meaning |
|---|---|
| `-` | None — run on every wake, including a brief deep-sleep timer wake. |
| `A` | Awake-only — skip when this minute is being serviced by a deep-sleep wake (run only when the device was already up). |
| `N` | Upstream-network-only — run only when the STA is connected to a real upstream network (an AP-only link does not count). Implies `A`. |

Cron reads the network condition for `N` off the storage bus (`wifi.sta.state`,
published by [spangap-net](../../spangap-net)); core itself has no network
dependency, so on a build with no networking the key is simply absent and
`N`-flagged jobs never run.

A due command is handed to the CLI through an internal stream, drained and
executed by the CLI task — so cron jobs are serialised with interactive input
and run with the same command surface as a typed command.

### Module-owned entries

A module that wants a periodic job owns its `s.cron.tab.<name>` key outright: it
installs the entry (via `storageDefault`, so a user's schedule tweak survives)
while its feature is enabled or configured, and deletes it when the feature is
switched off. The entries currently installed this way:

```
s.cron.tab.upnp      = "*/15 * * * * N upnp update"      while s.upnp.enable
s.cron.tab.duckdns   = "*/15 * * * * N duckdns update"   while domain+token configured
s.cron.tab.acme      = "0 3 * * * N acme renew 30"       while s.acme.enable + fqdn
s.cron.tab.logrotate = "0 0 * * * A logrotate 7"         always (log's version gate;
                                                          delete it and it stays gone)
```

See the "Add a cron entry from a module" recipe in
[spangap/INTERNALS.md](../../spangap/INTERNALS.md) for the pattern (a
storage-task-hosted subscription on the feature's enable key).

### Deep sleep

cron is the only thing that puts the device into deep sleep, and it never does so
on a timer of its own. It holds a `NO_DEEP_SLEEP` power-management lock named
`cron` by default, which blocks deep sleep for the whole device. That lock is
released **only when `s.cron.enable=1` and at least one `s.cron.tab.*` entry
exists** — i.e. only when there is actually a schedule that can wake the device
again. When the last power lock is released, [power management](power-management.md)
sets `sys.going_down`; cron observes that and sleeps toward the next scheduled
wake minute (`A`/`N` entries don't count — they are skipped on a deep-sleep wake,
so waking for one would be pure waste). Sleep timers run off an RC oscillator
with single-digit-percent skew, so cron sleeps 85% of the remaining time per hop
and iterates until the minute arrives — it can land short, never overshoot. See
[cron-internals.md](cron-internals.md) for the full handshake.

The practical consequence: a device with no entries (or cron disabled) never deep
sleeps — it would have no way to wake itself on schedule. Intermittent-power
duty-cycling is therefore expressed entirely as cron entries.

## Example

Duty-cycle WiFi for battery life — up two minutes out of every ten, deep sleep
the rest of the time:

```
set s.cron.tab.netup   "*/10 * * * * - net up"
set s.cron.tab.netdown "2/10 * * * * - net down"
```

With `s.cron.enable=1` and these entries present, the device brings WiFi up at
:00, :10, :20…, takes it down at :02, :12, :22…, and deep sleeps in the gaps,
waking on the timer for each scheduled minute.

## Public surface

Cross-straddle entry points — full signatures in
[`cron.h`](../esp-idf/include/cron.h):

| Symbol | For |
|---|---|
| `cronPoll()` | Run entries due since the last serviced minute, then recompute (internal wiring; the cron task and end-of-boot call it). |
| `cronReschedule()` | Recompute the stored next-run minutes and the deep-sleep lock (internal wiring; the s.cron subscription and end-of-boot call it). |

cron registers **no CLI command** of its own — entries are config keys, edited as
config keys. Modules install entries with plain storage calls; there is no
cron-specific installer API.

## Storage keys

cron owns these keys (defaults verified against source):

| Key | Default | Meaning |
|---|---|---|
| `s.cron.enable` | `1` | Master switch. When `0`, no jobs run and the deep-sleep lock stays held. Default-on so module-installed entries run on a fresh device. |
| `s.cron.version` | `1` | Config version for cron's own defaults. |
| `s.cron.tab.<name>` | — | One entry each, owned by whoever created it (a module or the user). |

The `N`-flag input `wifi.sta.state` is read but **owned by**
[spangap-net](../../spangap-net), not cron.

## See also

- [cron-internals.md](cron-internals.md) — task model, next-minute scheduling,
  the deep-sleep handshake with pm, the skew-margin wake iteration, and pitfalls.
- [power-management.md](power-management.md) — the lock model cron's deep-sleep
  gate plugs into.
