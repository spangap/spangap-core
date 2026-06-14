# Memory placement: PSRAM vs internal DRAM

The platform targets ESP32-S3 boards with external PSRAM (T-Deck: 8 MB octal;
Heltec V4: 2 MB quad), and is intended to also build for **no-PSRAM** targets
(a minimal LoRa-only RNS node on a bare S3 or a classic ESP32). Getting memory
placement right is the difference between "boots and runs for weeks" and
"sporadic, near-undebuggable corruption." This is the reasoning we settled on.

## The asymmetry that drives everything

On these boards the two heaps are wildly mismatched:

| Heap | Size | Typical free | Role |
|------|------|--------------|------|
| **Internal DRAM** | ~320 KB total | **~73 KB** | scarce; the bottleneck |
| **PSRAM** | 2–8 MB | ~7.8 MB | plentiful; the default sink |

Internal DRAM is the constrained resource — and a *subset* of it (DMA-capable
internal RAM) is scarcer still. So the governing principle is:

> **PSRAM is the default home for bulk data. Internal DRAM is reserved for the
> things that physically cannot live in PSRAM. Be explicit about the latter.**

## What MUST be in internal DRAM (never PSRAM)

These are correctness requirements, not preferences. Putting any of them in
PSRAM produces corruption that surfaces far from the cause:

1. **FreeRTOS sync objects accessed from a task/ISR critical section** — queues,
   stream buffers, message buffers, ring buffers. Their control block embeds an
   SMP spinlock taken via `taskENTER_CRITICAL` on every send/receive. Two
   reasons it can't be in PSRAM:
   - the spinlock acquire uses an `S32C1I` atomic that is **unreliable on
     external PSRAM** → `owner`/`count` desync → `assert spinlock_acquire
     spinlock.h:142 (lock->count == 0)`;
   - a flash op on the other core **disables the cache**, making the PSRAM
     control block unreadable mid-critical-section.

   Being "task-context only" (no ISR access) is *not* sufficient — these hit in
   plain task context. See the ITS inbox queue / link ring / stream-buffer pool
   (`its.cpp`) and the USB-JTAG driver ring buffers (`cli.cpp`).

2. **DMA buffers** — WiFi static RX (16 × ~1.6 KB, internal-DMA-only, allocated
   when the radio starts), the SD-on-SPI read bounce, LCD transfer buffers. Use
   `MALLOC_CAP_DMA`. Reserved via `CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL` (64 KB).

3. **Anything touched during a flash cache-disable window** — including the
   stack of a task that *itself* runs SPI-flash code (direct `fopen`/`fread` on
   LittleFS). `spawnTask(..., STACK_DRAM)` for those (default is `STACK_PSRAM`).

## What is fine in PSRAM (the default)

Plain data that is only ever touched in normal task context with the cache on:
the cJSON config tree (`cfgRoot`), `std::string`/`vector`/`map`, LVGL object/
style allocations, ITS/log payloads, and most task stacks. None of these are
ISR-touched, lock-protected, DMA, or cache-disable-sensitive.

## The mechanisms

- **`CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL`** — the size threshold for the C
  `malloc()` path: untagged allocations *smaller* than this go internal, the
  rest go PSRAM. It is **0** here (everything untagged → PSRAM), because internal
  DRAM is scarce and WiFi needs it. The control structures that must be internal
  are pinned explicitly (below), rather than relying on a size heuristic.
  - We measured the inverse (`=16384`, IDF's default): it pulled ~28 KB into
    internal DRAM — almost all of it PSRAM-safe UI/buffer data (LVGL, CLI) — and
    dropped DMA-lowest to ~19 KB *with WiFi off*. Not affordable. PSRAM-default +
    surgical pins beats internal-default here.
- **Explicit `heap_caps_malloc(size, cap)`** at the call site to declare intent:
  `MALLOC_CAP_INTERNAL` for control structures, `MALLOC_CAP_DMA` for DMA,
  `MALLOC_CAP_SPIRAM` for "definitely PSRAM regardless of threshold" (e.g. the
  cJSON allocator hook, big log/ITS buffers).
- **`spawnTask(..., stack_caps_t)`** — `STACK_PSRAM` (default) vs `STACK_DRAM`,
  gated on `CONFIG_SPIRAM` (all stacks internal on a no-PSRAM build).

## Direction: one allocator shim (`mem.h`)

Per-site `heap_caps_*` does not scale to the ~1,100 `std::` allocation sites
(they go through the global `operator new`). The chosen end-state centralises the
policy:

- `gp_alloc` / `gp_calloc` / `gp_realloc` — "could be PSRAM": prefer PSRAM, fall
  back to internal; internal-only when `!CONFIG_SPIRAM`.
- `dram_alloc` — "must be internal" (control structures).
- `dma_alloc` — "must be DMA-capable."
- The global `operator new`/`delete` (`mem_new.cpp`) route through `gp_alloc`, so
  the entire C++ heap follows the policy in one place, and **decoupled from
  `ALWAYSINTERNAL`** (which then only governs IDF's own C `malloc`). This is also
  exactly what the no-PSRAM build needs: same source, `gp_alloc` → internal.

`CONFIG_SPIRAM` is the single switch (a no-PSRAM board overrides it `=n` — its
"0 MB"). LVGL follows via `LV_USE_CUSTOM_MALLOC` → `lv_malloc_core` → `gp_alloc`.

## Decision guide

```
Is it ISR-touched / inside a critical section / a FreeRTOS sync object? → dram_alloc
Is it DMA target hardware?                                              → dma_alloc
Is it touched while a flash op has the cache disabled (incl. that
  task's own stack)?                                                    → internal (STACK_DRAM)
Otherwise (bulk data, std::, cJSON, UI):                                → gp_alloc (PSRAM)
```

## Hard-won lessons (the corruption hunt)

- **PSRAM-placement crashes are deterministic and surface far away.** A FreeRTOS
  queue/ring in PSRAM corrupted via the spinlock/cache path, but the *symptom*
  was a `LoadProhibited` walking the cJSON tree, or an `InstructionFetchError`
  calling a garbage ring-buffer function pointer. Fix the placement, not the
  victim.
- **It was not MSPI timing.** Dropping octal PSRAM 80→40 MHz and pinning DFS
  (`esp_pm` min==max) changed nothing — the crashes were byte-identical and
  deterministic. 40 MHz was reverted; it only cost bandwidth.
- **A silently-orphaned hook is deadly.** The ITS dead-task cleanup was named
  `vPortCleanUpTCB` (the legacy hook, gated by `ENABLE_STATIC_TASK_CLEAN_UP`,
  never set). IDF 5.5 renamed the live hook to `vTaskPreDeletionHook`
  (`CONFIG_FREERTOS_TASK_PRE_DELETION_HOOK`). The function compiled fine and was
  never called — `main_task`'s stale `s_tasks` handle leaked → `xTaskNotifyGive`
  into the freed TCB → UAF. No link error, just silence.
- **WiFi's memory cost is on connect, not scan.** Turning WiFi fully off freed
  almost no DRAM/DMA vs idle scanning/AP; the internal-DMA pressure shows when
  STA actually connects (lwIP/TLS). The headroom test that matters is
  WiFi-connected + SD + LCD, not WiFi-off.

## Debugging corruption

A temporary debug kit lives in `sdkconfig.defaults.spangap` (revert before ship):
`CONFIG_HEAP_POISONING_COMPREHENSIVE` (head/tail canaries + `0xFE` free-fill),
`CONFIG_SPANGAP_HEAP_INTEGRITY_POLL` (1 Hz scan on the log task, aborts at the
first corrupt block), `CONFIG_FREERTOS_CHECK_STACKOVERFLOW_PTRVAL`, and
`CONFIG_SPANGAP_WATCH_ADDR` (HW store-watchpoint to catch a UAF writer's
backtrace — see `log.cpp`). Workflow: poll reports `CORRUPT HEAP at 0xADDR` →
arm the watchpoint there → reflash → the faulting write names the culprit.
Caveat: a watchpoint on live-then-freed memory fires on the legit live write
first (it still identifies *what* lives there, which is half the answer).
