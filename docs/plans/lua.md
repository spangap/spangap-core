# Lua — scriptable device logic

**Status:** plan. No code yet. This sits on top of [storage](../storage.md) and reuses its
notification bus as the job queue, so read that first.

## Goal

Give the device an embedded Lua interpreter that can:

- handle **incoming LXMF** messages,
- render **served Nomad pages** (later) and **served HTML** with inline `<?lua ?>` scripts,

and — the ergonomic centrepiece — let scripts read and write the config store as **ordinary
Lua variables** instead of `get`/`set` calls:

```lua
-- instead of  storage_set("s.net.hostname", "foo")
s.net.hostname = "foo"
print(s.net.http_port + 1)          -- reads the live store
secrets.api.key = request.token     -- request is the inbound message / params
```

## Non-goals

- A general OS-level scripting environment. Lua sees the config store, the inbound request, and
  whatever functions we choose to expose — nothing more.
- Long-running / resident scripts, coroutines that outlive a request, background timers. A Lua
  job is **script in → output out**, promptly. Recurring work stays in C straddles + cron.
- Persisting jobs across reboot. The job queue is ephemeral by construction.

## Decisions already locked

| # | Decision | Rationale |
|---|----------|-----------|
| 1 | Absent/branch reads return a **truthy proxy table**; existence is checked with `exists(k)`. | "Things just work" — deep writes (`s.a.b.c = 5`) auto-vivify without pre-creating parents. The cost (a missing key is truthy) is paid by one explicit `exists()`. |
| 2 | No explicit transaction needed for coalescing — the store already coalesces. `storageBegin/End` is used **only** to assemble a job atomically. | Avoids a commit/WS-patch storm only where it matters (job hand-off). |
| 3 | One **serial** Lua task, no VM mutex. Jobs run one at a time, dispatched off the storage bus. | Long/slow scripts block other Lua calls — acceptable for the stated scope, and it removes all VM-reentrancy hazards. |
| 4 | `nil` = unset; **bool stored as int `0`/`1`**. | Matches existing store conventions everywhere. |
| 5 | Job **data** lives under `lua.jobs.<id>.*`; the **trigger** is a *separate* prefix `lua.run.<id>`. | Subscriptions are prefix-matched and fire per-leaf. A separate trigger prefix means the Lua task gets exactly **one** notification per job and never sees (or floods its inbox on) the request-subtree churn. |
| 6 | **Lua owns the whole job lifecycle.** The requester deletes `.output`/`.error` to acknowledge; if it hasn't within a reap timeout (~3 s, tunable), Lua logs an error and reaps the job itself. | Requesters that vanish (fire-and-forget LXMF, disconnected browser) can't leak RAM. |

## Architecture: the storage bus *is* the job queue

No new IPC. A requester writes a job into the (ephemeral) config tree and flips a trigger; the Lua
task subscribes to the trigger prefix and processes triggers serially. The browser already mirrors
all ephemerals over `storage:1`, so browser-originated jobs and their output round-trip for free.

```
requester (browser / LXMF task / HTTP handler)
  │  storageBegin()
  │    set  lua.jobs.<id>.script       = "<lua source>"     (or .script_var = "s.app.handler")
  │    set  lua.jobs.<id>.request.*    = <inbound data tree>
  │    set  lua.run.<id>               = 1                    ← trigger, separate prefix
  │  storageEnd()                                             ← one coalesced commit
  ▼
lua task  (subscribed ONLY to "lua.run")
  1. notify on lua.run.<id>            → enqueue id (cheap; dedup on value)
  2. delete lua.run.<id>               → consumed, won't re-fire
  3. snapshot lua.jobs.<id>.request    → native Lua table (frozen copy)
  4. resolve script  (.script literal, or read key named by .script_var)
  5. run with instruction+wallclock watchdog, _ENV proxied to the store
  6. write lua.jobs.<id>.output / .error
  7. delete inputs (.script, .script_var, .request)         ← Lua owns cleanup
  ▼
requester reads .output/.error, then deletes them   (= ack)
reap watchdog: if not acked within CONFIG_SPANGAP_LUA_REAP_MS, log + storageDeleteTree(lua.jobs.<id>)
```

### Storage key layout

All ephemeral (no `s.` prefix) — never persisted, lost on reboot, mirrored to the browser.

| Key | Writer | Meaning |
|-----|--------|---------|
| `lua.jobs.<id>.script` | requester | Lua source, inline. |
| `lua.jobs.<id>.script_var` | requester | *Alternative* to `.script`: a storage key naming where the source lives (e.g. a handler stored at `s.app.lxmf_handler`). Lets a job reference shared/installed scripts without copying them. |
| `lua.jobs.<id>.request.*` | requester | Inbound payload subtree (LXMF fields, HTTP params). Snapshotted into the `request` Lua table. |
| `lua.jobs.<id>.output` | lua task | Script output (string; or a tree if the script returns a table). |
| `lua.jobs.<id>.error` | lua task | Set instead of/with `.output` on failure (compile error, runtime error, watchdog kill). |
| `lua.run.<id>` | requester (set), lua task (delete) | Trigger. Separate prefix so the Lua task subscribes narrowly. |

`<id>`: browser uses `crypto.randomUUID()`; device tasks use `<taskname>.<counter>`. No coordination
needed — the namespaces can't collide.

### Job state machine

```
            set lua.run.<id>=1
   (none) ─────────────────────▶ QUEUED
                                   │ lua task picks up: delete trigger, snapshot request
                                   ▼
                                RUNNING ── watchdog kill / error ──▶ (writes .error)
                                   │ returns
                                   ▼
                                  DONE  (inputs deleted; .output/.error present)
                                   │
              requester deletes .output/.error (ack)   reap timeout elapses
                                   │                          │ log error
                                   ▼                          ▼
                                REAPED  ◀───────────  reap watchdog deletes lua.jobs.<id>
```

The reap deadline is tracked per DONE job. The Lua task is serial, so it folds reaping into its
dispatch loop: it waits on its ITS aux inbox with a timeout equal to the nearest reap deadline,
waking either on a new trigger or to sweep an overdue job.

## The global proxy — variables backed by the store

Install a metatable on the script's environment (`_ENV` / `_G`). `__index`/`__newindex` only fire
on *misses*, so as long as we never `rawset` a backed name into the table, every read/write to those
names routes through the store.

```lua
-- C-installed bootstrap, conceptually:
setmetatable(_ENV, {
  __index    = function(_, k) return storage_resolve(k) end,   -- scalar | proxy | (truthy) proxy
  __newindex = function(_, k, v) storage_assign(k, v) end,
})
```

`storage_resolve(path)` inspects the cJSON node at `path`:

| Node at `path` | Returns |
|----------------|---------|
| primitive (int/str) | the Lua scalar (number / string). `0` and `""` are returned as-is — both truthy in Lua, so no footgun. |
| object (branch) | a **sub-proxy** rooted at `path`, with the same `__index`/`__newindex`. |
| absent | a **truthy empty sub-proxy** rooted at `path` (decision #1 — enables auto-vivifying deep writes). |

`storage_assign(path, v)`:

| `v` | Action |
|-----|--------|
| `nil` | `storageUnset(path)` |
| boolean | `storageSet(path, b and 1 or 0)` (decision #4) |
| number | `storageSet(path, int)` — see number note below |
| string | `storageSet(path, str)` |
| table | convert Lua table → cJSON, `storageSetTree(path, node)` |

Sub-proxies carry their root path; indexing them appends a segment. `s` is just the sub-proxy
rooted at `"s"`, `secrets` at `"secrets"`, and a bare name like `foo` is the ephemeral key `foo`.
The `s.` / `secrets.` / ephemeral namespaces fall out of the path, no extra convention.

### Reads, writes, and the one wart

```lua
print(s.net.http_port)          -- 80         (primitive leaf → scalar)
s.net.http_port = 8080          -- storageSet("s.net.http_port", 8080)
s.app.new.deep.key = 5          -- auto-vivifies: storageSet("s.app.new.deep.key", 5)
s.app.flag = true               -- stored as 1
s.app.flag = nil                -- storageUnset
for k, v in pairs(s.net) do ... end   -- via __pairs / iter() over the subtree

-- the wart (accepted):
if s.app.maybe then ... end      -- ALWAYS true (absent → truthy proxy)
if exists("s.app.maybe") then    -- correct existence test
```

### Type mapping

| Lua | Store | Notes |
|-----|-------|-------|
| number (integer) | `CFG_INT` | |
| number (float) | `CFG_FLT` | **First-class float type** — a `spangap-core` storage change, not just a Lua concern: new `CFG_FLT` in `cfg_type_t`, `storageGetFloat`/`storageSet(key, double)` overloads, `storageGetType` tweak, and the browser-sync path must preserve the int-vs-float tag. cJSON already holds numbers as doubles, so on-disk JSON is unaffected — the distinction is a spangap-side type tag. |
| string | `CFG_STR` | |
| boolean | `CFG_INT` `0`/`1` | decision #4 |
| `nil` | unset | |
| table | cJSON subtree | object or array per Lua keys; `storageSetTree`. |

### `request` is a snapshot, globals are live

`request` is copied into a **plain native Lua table** at step 3 — *not* a proxy into
`lua.jobs.<id>.request`. The job is then self-contained: no race if the key is deleted mid-run, no
surprise mutation, and `pairs(request)` is ordinary iteration. The `s.`/`secrets.`/ephemeral globals
stay **live** (they are the config store).

## Concurrency & safety

- **Single serial Lua task**, dispatched off `lua.run`. No VM mutex. Storage reads/writes from the
  task still take the store's own recursive mutex internally — unchanged, already thread-safe.
- **Execution watchdog** (mandatory): `lua_sethook(L, hook, LUA_MASKCOUNT, N)` instruction cap plus a
  wall-clock limit. Inline-HTML and LXMF scripts are partially-untrusted input; one `while true do end`
  would otherwise wedge the entire subsystem permanently. On trip: abort, write `.error`, advance.
- **Reap watchdog**: per decision #6, completed jobs whose `.output`/`.error` aren't acked within the
  reap timeout are logged and reaped by the Lua task.
- **No synchronous sub-jobs.** A script *may* enqueue another job (write `lua.jobs.<new>.*` +
  `lua.run.<new>`) — nice for async fan-out — but it must **not** block waiting on that job's output:
  serial single task ⇒ the sub-job can't run until the current one returns ⇒ deadlock. Async-only;
  enforce/document.
- **Notify-inbox sizing.** The Lua task subscribes only to `lua.run`, so it gets one notification per
  job regardless of request size — the separate-prefix design (decision #5) keeps it off the
  request-subtree churn. Its inbox can stay small.
- **Browser visibility.** Job script + request + output are ephemeral and therefore **mirrored to any
  connected browser** over `storage:1`, including LXMF-triggered jobs. Fine (and useful for
  debugging) for now. If we later want LXMF job content off the browser channel, note there is no
  "ephemeral + unsynced" namespace today (`secrets.` is unsynced but *persisted*) — that's a future
  addition, not part of this plan.

## Lua API surface (initial)

Beyond the global proxy, expose a minimal function set:

- `exists(key)` → bool — `storageExists`.
- `unset(key)` — explicit `storageUnset` (same as `= nil`).
- `iter(node)` / `__pairs` — iterate a subtree (`storageForEach` over the prefix).
- `log(...)` — to the device log.
- `output(s)` — set this job's `.output`. **A single output stream, a plain string** (also implicit
  from a script `return s`). A script never needs more than one stream: anything else it wants to
  *emit* is a side effect with its own function, not "output."
- `lxmf_send(dest, body, ...)` — send an LXMF message. Sending is a side effect, not output, so it
  gets a function rather than overloading `.output` as a message channel. Same shape for any other
  emission we add.
- Entry-point helpers TBD per source (HTTP response headers/body for inline pages).

The standard library is sandboxed: no `os`, `io`, `package`, `debug`, `load`/`loadstring`,
`require`. Keep `string`, `table`, `math`, and the exposed device functions.

## Entry points

| Source | How a job is created |
|--------|----------------------|
| Incoming LXMF | LXMF straddle, on a message addressed to a Lua-handler destination, writes `request.*` from the message fields + sets `.script_var` to the installed handler key. |
| Served HTML (inline `<?lua ?>`) | Web handler extracts each block into `.script`, request params into `request.*`, blocks on `.output`. (Inline = templating; needs an output-capture convention.) |
| Served Nomad pages | Later. Same job shape. |
| Browser (dev/testing) | Writes the job keys directly over `storage:1`; reads `.output` from the mirror. |

## Kconfig tunables

Per house convention (no magic constants — see Kconfig plan):

- `CONFIG_SPANGAP_LUA_REAP_MS` — DONE-job reap timeout (default 3000).
- `CONFIG_SPANGAP_LUA_MAX_INSTRUCTIONS` — watchdog instruction cap per job.
- `CONFIG_SPANGAP_LUA_MAX_WALLCLOCK_MS` — watchdog wall-clock cap per job.
- `CONFIG_SPANGAP_LUA_TASK_STACK` / `..._HEAP` — VM memory budget (PSRAM-backed allocator).

## Transport dependency — output size ceiling

A job's `.output` reaches the browser as an ephemeral storage value over the `storage:1` DC, exactly
like a Nomad page body. **Today that path silently caps at ~15.5 KB** (the storage live-patch limit,
itself bounded by the 16 KB ITS ring) — values above it are dropped, not chunked. So a Lua-rendered
Nomad/HTML string larger than ~15 KB would not reach the browser intact. This is the same bug that
makes mid-size Nomad pages render blank, and it is fixed at the transport layer by
**[its-oob-packets.md](its-oob-packets.md)** (heap-backed large packets), which raises the ceiling to
SCTP's 64 KB `max-message-size` — and an optional later phase in that plan (in-place fragment-bitmap
SCTP send) lifts even that, making outbound packets effectively arbitrary-sized. **The OOB-packets
change is a prerequisite for Lua output (and Nomad) of any real size.**

## Open questions

1. **Lua version.** 5.4 (integers, no `__pairs`) vs 5.3 (`__pairs`). 5.4 preferred — it pairs cleanly
   with first-class int/float in the store; provide `iter()` instead of relying on `__pairs`.
2. **Per-source resource budgets.** Should LXMF-triggered jobs get a tighter instruction cap than
   browser/dev jobs (untrusted-er input)? Possible per-job `request.__limits`, watchdog reads it.
   Leaning: ship one global cap first (phase 2), add per-source tightening only if a real abuse case
   appears — don't pre-build it.

**Resolved:** floats → first-class `CFG_FLT` storage type (see Type mapping). Output → a single
`.output` string + a `lxmf_send` function for message emission (see API surface), so no tree/string
ambiguity.

## Implementation phases

1. **VM + proxy.** Embed Lua, PSRAM allocator, sandboxed stdlib, `_ENV` metatable →
   `storage_resolve`/`storage_assign`, `exists`/`unset`/`iter`/`log`. Unit-test against the store
   directly (no job bus yet).
2. **Job bus.** `lua.jobs`/`lua.run` layout, the serial dispatch loop (subscribe → consume trigger →
   snapshot request → run → write output → delete inputs), execution watchdog.
3. **Reaper.** DONE-deadline tracking, timed inbox wait, ack-or-reap.
4. **Entry points.** Inline-HTML templating first (easiest to test in-browser), then LXMF handler
   wiring, then Nomad pages.
5. **Kconfig + docs.** Expose tunables, document the API and the `exists()`/truthy-proxy contract for
   script authors.
