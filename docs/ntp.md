# NTP Time Sync

Sets system time from NTP. Include `ntp.h`.

## Usage

```cpp
ntpBegin();  // call after WiFi is connected (non-blocking, just calls esp_sntp_init)
ntpStop();   // stop SNTP (call before network down)
```

`ntpBegin()` configures and starts `esp_sntp` (non-blocking). The blocking wait for time sync is in `main.cpp`'s `waitForTime()`, which blocks the main task until valid time (>Jan 1 2026), then releases the boot deep sleep lock. A 30s one-shot FreeRTOS timer logs the synced time. Runs on the network task.

**Important**: `esp_sntp_setservername()` stores the pointer, not a copy. The server buffer is `static` to avoid dangling pointer.

## Configuration

| Config key | Default | Description |
|------------|---------|-------------|
| `s.ntp.server` | `pool.ntp.org` | NTP server hostname |
| `s.ntp.tz` | `GMT0` | POSIX timezone string |

Timezone uses POSIX TZ format, e.g.:
- `GMT0` — UTC
- `CET-1CEST,M3.5.0,M10.5.0/3` — Central European Time
- `EST5EDT,M3.2.0,M11.1.0` — US Eastern
- `PST8PDT,M3.2.0,M11.1.0` — US Pacific

DNS must be reachable for NTP to work.

## Timezone after factory reset

`ntpApplyTimezone()` ([ntp.cpp](../spangap-core/src/ntp.cpp)) reads `s.ntp.tz` (IANA name) and sets the POSIX `TZ` env var via `setenv` + `tzset()`. The POSIX string is resolved from `<stateDir>/timezones.json` — the IANA→POSIX map, a plain user-state file parsed transiently (cJSON parse → read one string → free) rather than held in the config tree — and the result is cached in `s.ntp.posix` to skip the file parse on later boots. After a factory reset both `s.ntp.tz` and the cached `s.ntp.posix` are unset, so `ntpApplyTimezone()` early-exits and `TZ` stays unset — `localtime_r()` then returns UTC. The browser pushes the user's IANA timezone via `s.ntp.tz` on first connect (the device does the POSIX lookup itself); a change to `s.ntp.tz` clears the cached `s.ntp.posix` so the new zone re-resolves. After that `tzset` runs and timestamps shift to local time.

So on a fresh device, all log lines printed before the first browser connection have **UTC** wall-clock timestamps. The boot order matters: `ntpApplyTimezone()` should be called once early in the consumer's `app_main()` (before `logInit`) and again from `ntpStart` when STA upstream comes up. Neither call changes `TZ` if `s.ntp.tz` is unset — the browser connection is the trigger.

This is intentional (no "default timezone" guess), but worth knowing when reading early-boot logs after a factory reset.

## WiFi lifecycle

`net.cpp` calls `ntpBegin()` on network up and `ntpStop()` on network down. On network down→up cycle, `ntpBegin()` is called again (re-inits SNTP with fresh config from storage).

## CLI

`date` — show current date/time
`date yyyymmddhhmmss` — set date/time manually (e.g. `date 20260315143000`)
