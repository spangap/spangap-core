# auth — internals

Maintainer reference for the `auth` credential primitive
([`src/auth.cpp`](../esp-idf/src/auth.cpp), [`include/auth.h`](../esp-idf/include/auth.h)).
The [operator guide](auth.md) covers the model and API; this document is for
changing the code without breaking it.

## 1. What auth owns and adds

Everything is stored in `secrets.auth.*` config keys through the
[storage](storage.md) API — there is no module state outside storage except the
rate-limit counters (§4). `authInit` is called once at boot from the platform
init chain, before sshd and web.

`authInit` does three things:

1. **Seeds defaults**, gated by `secrets.auth.version` (`AUTH_VERSION = 1`). The
   seed tree is `{"enable":1, "realms":[{"name":"admin","hash":""}], "cookies":[]}`,
   written with `storageDefaultTree("secrets.auth", …)`, then `version` is set.
2. **Sweeps expired cookies** — but only when the wall clock is valid
   (`gettimeofday().tv_sec >= 1700000000`). It walks the cookie array backward
   and removes any entry whose `expires` is a real timestamp (`> 1700000000`)
   and in the past. The clock guard exists because cookies seeded before NTP
   would otherwise all look expired against a 1970 clock.
3. **Registers the CLI commands** `auth` and `passwd` via `cliRegisterCmd`.

### Hash & verify

- `hashPassword(pw, salt, saltLen)` = SHA-256 of `salt || pw` (mbedTLS), rendered
  as `salt_hex(32) ":" hash_hex(64)`.
- `hashPasswordNew(pw)` draws a 16-byte salt from four `esp_random()` words.
- `verifyPassword(pw, stored)` parses the salt out of `stored` (rejects unless
  the colon sits at offset 32), recomputes, and compares the **whole**
  `salt:hash` string. There is no constant-time compare — `std::string ==`.

### Realm array

`secrets.auth.realms.<N>.{name,hash}`, counted by
`storageArrayCount("secrets.auth.realms.")`. `realmFind(name)` is a linear scan;
`realmGet(idx,…)` returns false when the name slot is empty. State is read off
the hash string: `""` = unset, `"--"` = locked, anything else = set. Both
`authPasswd` and `authLogin` skip unset and locked realms when matching a
password.

### Session (cookie) table

`secrets.auth.cookies.<N>.{cookie,realm,expires}`, counted by
`storageArrayCount("secrets.auth.cookies.")`.

- **Token**: `authLogin` fills four `esp_random()` words (16 bytes = 128 bits)
  and hex-encodes them to a 32-char token. Expiry = `now + COOKIE_EXPIRY_S`
  (`60 * 24 * 3600`, 60 days).
- **`cookieAdd`** appends at index `count`. If `count >= MAX_COOKIES` (16) it
  first scans for the entry with the smallest `expires` and `cookieRemove`s it —
  oldest-expiry eviction, not insertion order.
- **`cookieRemove(idx)`** compacts the array: it shifts every later entry
  (`cookie`/`expires`/`realm`) down by one and unsets the now-duplicate tail
  slot. Both the shift and the append run inside a single `storageBegin()` /
  `storageEnd()` batch.
- **`authCheck`** linear-scans for a matching token, lazily removes it if expired
  (again guarded by `now > 1700000000 && exp > 1700000000`), and otherwise
  returns the stored realm.

### Threading / ownership

`auth` has no task and no lock of its own. Concurrency safety comes entirely
from storage, which serializes writes through its owning task and guards reads
under a recursive mutex. The compound operations (`cookieAdd`, `cookieRemove`,
`authPasswd`) wrap their multi-key writes in `storageBegin`/`storageEnd` so the
patch applies atomically, but there is no cross-call locking: two simultaneous
`authLogin`s could in principle interleave array appends. In practice the device
authenticates one client at a time (the model the global backoff also assumes),
so this is not contended.

## 2. authPasswd state machine

`authPasswd(realm, old, new)`:

1. `realmFind(realm)` → `AUTH_NO_SUCH_REALM` if absent.
2. Read the hash; classify unset / locked / set.
   - **unset or locked**: a non-empty `old` → `AUTH_WRONG_PASSWORD`; an empty
     `new` → `AUTH_OK` (no change — the probe path); else proceed to set.
   - **set**: `verifyPassword(old)` must pass, and `new` must be non-empty, else
     `AUTH_WRONG_PASSWORD`.
3. Uniqueness scan: for every *other* realm with a real hash (skip `""` and
   `"--"`), `verifyPassword(new, otherHash)` → `AUTH_SAME_AS_OTHER_REALM` on a
   match.
4. Write `name` + the new `hashPasswordNew(new)` in one storage batch.

`authForceSetPassword` (CLI-only, static) is the admin override: it skips step 2
entirely (no old-password check) but keeps the empty-password refusal and the
uniqueness scan. `auth passwd <realm> <pw>` and the interactive `passwd` (fixed
to the `admin` realm) both route through it.

## 3. CLI surface

- `auth` (no args) → `enabled`, realm count, cookie count `/ MAX_COOKIES`.
- `auth realms` → each realm with `set` / `locked` / `unset`.
- `auth passwd <realm> <newpw>` → `authForceSetPassword`.
- `passwd` → reads two lines with `cliReadLine(…, CLI_ECHO_STARS)`, requires a
  match, sets `admin`. Both password buffers are `memset` to zero before return.

The help spellings follow the standard CLI convention (`help` one-liner, `-h`
fuller, `""` status).

## 4. Rate-limit backoff

Two file-static counters, `lastFailMs` and `failCount` — **global**, not
per-realm or per-client. `isRateLimited()` computes the current window as
`min(2^(failCount-1), MAX_BACKOFF_S)` seconds (1 s, 2 s, 4 s, … capped at
`MAX_BACKOFF_S = 300`) and returns true while `millis() - lastFailMs` is inside
it. `authLogin` checks it first (returns `AUTH_RATE_LIMITED`), bumps `failCount`
and restamps `lastFailMs` on failure, and zeroes `failCount` on success. The
state is RAM-only — a reboot clears it. This is deliberately a single shared
counter: the device's auth model is one client at a time.

## 5. Pitfalls

- **Passwords must be unique across realms.** The whole login-by-password design
  depends on it — `authLogin` matches a password against every realm, so two
  realms sharing a password would make the resolved realm ambiguous.
  `authPasswd` and `authForceSetPassword` both enforce uniqueness; never add a
  write path that bypasses the scan.
- **`secrets.auth.*` never leaves the device.** It lives under the `secrets.`
  prefix precisely so storage's browser mirror excludes it. Don't move any auth
  key out of `secrets.` and don't log hashes or tokens.
- **An empty password cannot be set once a password exists.** Clearing is not a
  supported operation through the API — use the `"--"` locked sentinel to
  disable a realm instead.
- **The clock guards are load-bearing.** Cookie sweep and `authCheck` expiry
  both gate on `tv_sec > 1700000000`; without the guard, a device that boots
  before NTP would expire every valid session against a 1970 clock.
- **No constant-time compare.** Hash verification is a plain string equality;
  if timing side-channels ever matter here, that is the line to change.
