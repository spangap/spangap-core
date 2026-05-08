# Authentication (`auth.cpp/h`)

Cookie-based authentication with realm-specific passwords. All state in `secrets.auth.*` (persisted, never sent to browser).

- **Passwords**: SHA-256 with 16-byte random salt. Stored as `salt_hex:hash_hex`.
- **Sessions**: 128-bit random tokens (32 hex chars), 60-day expiry. Max 16 concurrent sessions (oldest evicted).
- **Rate limiting**: exponential backoff (1 s to 5 min) on repeated login failures.
- **Realms**: named password scopes (e.g. "admin"). Factory default: one "admin" realm with empty hash (unset). Password determines realm on login. `secrets.auth.enable=1` activates auth enforcement.

## API (`auth.h`)

- `authEnabled()`
- `authPasswd(realm, old, new)`
- `authLogin(password, tryRealm, outRealm, outCookie)`
- `authCheck(cookie)`
- `authLogout(cookie)`

## REST endpoints (handled by web task in `handleAuth()`)

- `POST /auth/login` — `{password, realm?}` → `{result, realm?, cookie?}`
- `POST /auth/passwd` — `{realm, old, new}` → `{result}`. Probe unset: `old=""` + `new=""` → OK if unset.
- `POST /auth/logout` — reads session cookie from headers, deletes it. Kicks all web connections from the same client IP.

## WebSocket / WebRTC auth

`/webrtc` and `/rtsp` do WS upgrade first, then check the session cookie. Reject with WS close code 4401.

`/webrtc` additionally evaluates `?force=1` (after auth) — reject with **4409** if another session is active and the client didn't opt in; evict the current holder with **4008** and accept if they did. `storage` / `log` / `cli` are all DC-only now — they inherit auth from `/webrtc`.

## Web file mappings

The `map.auth` field on each `s.web.map[]` entry specifies required realm(s). Unauthed requests get redirected to the root index.

## WebRTC single-session

One `RTCPeerConnection` at a time. If a second browser connects the signaling WS, the first is evicted. The `/webrtc` endpoint has `maxHandles=1` and `onBusy` always returns false so the new client takes the slot. Old client sees the DC close and stops reconnecting.

## Browser flow

`MainLayout` checks `isAdminUnset()` → `/setup` if no password, `checkAuth()` → `/login` if auth enabled but no session. `/setup` and `/login` have reverse guards (redirect to `/` if already set up / logged in). Forms include `autocomplete` hints for password managers. DOM removal before `router.push()` triggers Chrome's password save prompt (requires valid TLS cert — Chrome blocks on self-signed).
