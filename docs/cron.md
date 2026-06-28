# cron — minute-resolution scheduler with deep sleep

`cron` runs commands on a schedule, at one-minute resolution, and doubles as the
device's deep-sleep driver. It reads a crontab file, and every minute it matches
each line against the wall clock and feeds any due command into the CLI. On a
battery device it also owns the path into and out of deep sleep: when nothing
needs the CPU it puts the device to sleep until the next scheduled minute, and a
fast wake handler decides on boot whether there is any work this minute or
whether to drop straight back to sleep.

It is part of spangap-core, so it starts automatically whenever the straddle is
in the build — there is nothing to call.

## What it does

The crontab lives at `<stateDir>/crontab` — the active state store plus
`/crontab`, normally `/state/crontab` on flash (or `/sdcard/state/crontab` when
the state store is on SD). It is a plain text file, one entry per line, edited
like any other state file (the web file browser, or the `fs` CLI). Blank lines
and lines beginning with `#` are ignored.

Each entry is seven whitespace-separated columns:

```
min  hour  dom  month  dow  flags  command
```

The five time fields use standard cron field syntax — `*` (any), a plain number,
`,` lists, `-` ranges, and `/` steps (including `*/5` and `1-30/2`). There are no
named months/days and no `@`-style shortcuts. `dow` is `0`–`7` with both `0` and
`7` meaning Sunday. Everything after the flags field is the command, passed
verbatim to the CLI.

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

A matched command is handed to the CLI through an internal stream, drained and
executed by the CLI task — so cron jobs are serialised with interactive input
and run with the same command surface as a typed command.

### Deep sleep

cron is the only thing that puts the device into deep sleep, and it never does so
on a timer of its own. It holds a `NO_DEEP_SLEEP` power-management lock named
`cron` by default, which blocks deep sleep for the whole device. That lock is
released **only when `s.cron.enable=1` and the crontab has at least one active
entry** — i.e. only when there is actually a schedule that can wake the device
again. When the last power lock is released, [power management](power-management.md)
sets `sys.going_down`; cron observes that and sleeps until the next minute
boundary. See [cron-internals.md](cron-internals.md) for the full handshake.

The practical consequence: a device with an empty or disabled crontab never deep
sleeps (it would have no way to wake itself on schedule). Intermittent-power
duty-cycling is therefore expressed entirely as crontab entries.

## Example

Duty-cycle WiFi for battery life — up two minutes out of every ten, deep sleep
the rest of the time:

```
*/10 *    *    *    *    -    net up
2/10 *    *    *    *    -    net down
```

With `s.cron.enable=1` and these entries present, the device brings WiFi up at
:00, :10, :20…, takes it down at :02, :12, :22…, and deep sleeps in the gaps,
waking on the timer for each scheduled minute.

## Sibling-installed defaults

Other straddles install their own periodic jobs by calling `cronDefault(schedule,
command)` from their init, once per version bump. `cronDefault` appends a line
only if no existing line — active *or* commented — already carries that exact
command, so it is idempotent across reboots and **respects user edits**: delete
or comment the line and it stays gone until that straddle bumps its own version
key. These are owned by the installing straddle, not by cron; examples of what
siblings install:

```
*/15 *    *    *    *    N    duckdns update     # duckdns
*/15 *    *    *    *    N    upnp update        # upnp
0    3    *    *    *    N    acme renew 30      # acme
0    0    *    *    *    A    logrotate 7        # log
```

The factory crontab ships with only the column header and a flags legend — no
entries — so a bare build schedules nothing until a sibling installs a default or
a user adds a line.

## Public surface

Cross-straddle entry point — full signatures in
[`cron.h`](../esp-idf/include/cron.h):

| Symbol | For |
|---|---|
| `cronDefault(schedule, command)` | Sibling straddles installing a default periodic job from their init. Idempotent, edit-respecting. |
| `cronPoll(execute)` | Evaluate the crontab against the current minute (internal wiring; `execute=false` is the dry-run used by the wake fast path). |

cron registers **no CLI command** of its own — the crontab is a file, edited as a
file.

## Storage keys

cron owns these keys (defaults verified against source):

| Key | Default | Meaning |
|---|---|---|
| `s.cron.enable` | `1` | Master switch. When `0`, no jobs run and the deep-sleep lock stays held. Default-on so sibling-installed entries run on a fresh device. |
| `s.cron.version` | `1` | Config version for cron's own defaults; bumped only when cron changes its installed defaults. |

The `N`-flag input `wifi.sta.state` is read but **owned by**
[spangap-net](../../spangap-net), not cron.

## See also

- [cron-internals.md](cron-internals.md) — task model, the deep-sleep handshake
  with pm, the RTC fast-wake path, column layout, and pitfalls.
- [power-management.md](power-management.md) — the lock model cron's deep-sleep
  gate plugs into.
