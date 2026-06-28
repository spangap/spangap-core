# auth — credential primitive

`auth` is the device's credential store: cookie-based authentication with
realm-specific passwords, owned by spangap-core. It is a pure-core primitive —
no transport, no HTTP. It holds the realms, hashes passwords, mints and checks
session cookies, and rate-limits failed logins. All state lives in
`secrets.auth.*` config keys, persisted to the state store and **never** sent to
the browser. There are no in-memory caches; every read and write goes through
[storage](storage.md).

The HTTP *enforcement* — the `POST /auth/login|passwd|logout` REST endpoints,
the WebSocket/WebRTC cookie checks, and the per-request realm gating on web file
mappings — lives in **[spangap-web](../../spangap-web)**, built on top of this C
API (`authWebInit`). This document covers the primitive: the model, the C API,
the storage surface, and the on-device CLI. For the request-handling side, see
spangap-web.

## Model

- **Realms** are named password scopes (e.g. `admin`). Each realm has its own
  password. A device ships with a single `admin` realm whose password is unset.
- **Passwords are unique across realms.** `authLogin` resolves the realm from
  the password alone (it scans every realm and matches by password unless a
  specific realm is requested), so two realms cannot share a password —
  `authPasswd` refuses a new password that already belongs to another realm.
- **Password hashing**: SHA-256 over a 16-byte random salt concatenated with the
  password, stored as `salt_hex:hash_hex` (a 32-hex salt, a colon, a 64-hex
  digest).
- **Sessions** are 128-bit random tokens (32 hex characters) with a **60-day**
  expiry. Up to **16** concurrent sessions are kept; when full, the session with
  the earliest expiry is evicted to make room.
- **Rate limiting**: a single global exponential backoff on failed logins —
  1 s after the first failure, doubling per failure, capped at **5 minutes**
  (300 s). A successful login resets the counter.
- **Enforcement** is active when `secrets.auth.enable == 1` (`authEnabled()`).
  The flag is seeded to `1` at init; consumers of this primitive (spangap-web,
  sshd) decide what to do when it is off.

A realm's password hash carries three states: empty string `""` = **unset** (no
password — login against it always fails), `"--"` = **locked** (administratively
disabled, also unusable for login), and any `salt:hash` value = **set**.

## How other straddles use it

`auth` has no socket or port of its own. Callers link against `auth.h` and call
the C API directly:

- **[spangap-web](../../spangap-web)** wires the HTTP face in `authWebInit`
  (invoked from `webInit`): login/logout/passwd endpoints, cookie validation on
  WS/WebRTC upgrades, and realm gating on `s.web.map[]` entries.
- **sshd** ([../../sshd](../../sshd)) authenticates shell logins with `authLogin`
  against the `admin` realm (the SSH username is ignored).

A minimal consumer checks a cookie and acts on the realm it resolves to:

```c
std::string realm = authCheck(cookieFromRequest);   // "" if invalid/expired
if (realm.empty()) reject();
else grant(realm);                                   // e.g. "admin"
```

`auth` is initialised automatically at boot, before the straddles that depend on
it (sshd, web) come up — consumers never call `authInit` themselves.

## Public C API

Declared in [`include/auth.h`](../esp-idf/include/auth.h).

| Symbol | Purpose |
|---|---|
| `authEnabled()` | True when `secrets.auth.enable == 1`. |
| `authPasswd(realm, oldPw, newPw)` | Set or change a realm password (enforces the old-password check; see below). |
| `authLogin(password, tryRealm, &outRealm, &outCookie)` | Try a login. Empty `tryRealm` scans all realms; otherwise only that one. On success fills the matched realm name and a fresh session token. |
| `authCheck(cookie)` | Returns the realm a valid, unexpired cookie belongs to, or `""`. |
| `authLogout(cookie)` | Deletes a session cookie; returns true if it existed. |
| `authInit()` | Platform-invoked at boot: seeds defaults, sweeps expired cookies, registers the CLI commands. Not for consumers. |
| `auth_err_t` | Result enum: `AUTH_OK` (0), `AUTH_NO_SUCH_REALM` (1), `AUTH_WRONG_PASSWORD` (2), `AUTH_SAME_AS_OTHER_REALM` (3), `AUTH_RATE_LIMITED` (4). |

`authPasswd` semantics, by current realm state:

- **Probe unset**: `old=""` + `new=""` → `AUTH_OK` if the password is unset (or
  locked), letting a UI ask "does this realm need a password?" without changing
  anything.
- **Set initial**: `old=""` + `new=<pw>` when the hash is `""` or `"--"`.
- **Change**: `old=<current>` + `new=<new>` (the old password must verify).
- An **empty** new password is refused once a password is set
  (`AUTH_WRONG_PASSWORD`) — you cannot clear a password through `authPasswd`.
- A new password that matches another realm's returns `AUTH_SAME_AS_OTHER_REALM`.

## Owned storage keys

Everything `auth` owns is under `secrets.auth.*` — persisted to the state store
and **never** mirrored to the browser.

| Key | Default | Meaning |
|---|---|---|
| `secrets.auth.version` | `1` | Schema version; gates the one-time default seed. |
| `secrets.auth.enable` | `1` (seeded; reads as `0` if absent) | Master enforcement switch — `authEnabled()`. |
| `secrets.auth.realms.<N>.name` | `admin` (realm 0) | Realm name. |
| `secrets.auth.realms.<N>.hash` | `""` (realm 0) | Password hash `salt_hex:hash_hex`, or `""` (unset) / `"--"` (locked). |
| `secrets.auth.cookies.<N>.cookie` | — | 32-hex session token. |
| `secrets.auth.cookies.<N>.realm` | — | Realm this session belongs to. |
| `secrets.auth.cookies.<N>.expires` | — | Expiry as a Unix timestamp (seconds). |

The factory seed (written once when `version` is behind) is one `admin` realm
with an empty (unset) hash, an empty cookie array, and `enable = 1`.

## CLI

`auth` registers two commands. See the full on-device command line in
[cli.md](cli.md).

```
auth                          show enforcement state + realm/cookie counts
auth realms                   list realms with set / locked / unset state
auth passwd <realm> <newpw>   set (overwrite) a realm password, admin-only

passwd                        set the admin password (prompts twice, echoed as stars)
```

`auth passwd` is the administrative override: it sets a realm's password
unconditionally, bypassing the old-password check that `authPasswd` enforces. It
still refuses an empty password and still refuses a password that matches
another realm. `passwd` is the convenient front door — it prompts for the new
password twice and commits to the `admin` realm only when both entries match.

See [auth-internals.md](auth-internals.md) for the hash format, the session
table and eviction, the backoff state, and maintainer pitfalls.
