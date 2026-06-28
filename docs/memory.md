# Memory placement — PSRAM vs internal DRAM

On the ESP32-S3 boards this platform targets, the two heaps are wildly
mismatched. Internal DRAM is small and contended; external PSRAM is large and
idle. Placing an allocation on the wrong heap is the difference between "boots
and runs for weeks" and "sporadic, near-undebuggable corruption that surfaces
far from its cause."

| Heap | Size | Typical free | Role |
|------|------|--------------|------|
| **Internal DRAM** | ~320 KB total | **~73 KB** | scarce; the bottleneck |
| **PSRAM** | 2–8 MB (T-Deck 8 MB octal, Heltec V4 2 MB quad) | ~7.8 MB | plentiful; the default sink |

A *subset* of internal DRAM — DMA-capable internal RAM — is scarcer still. The
governing principle:

> **PSRAM is the default home for bulk data. Internal DRAM is reserved for the
> things that physically cannot live in PSRAM. Be explicit about the latter.**

`CONFIG_SPIRAM` is the single PSRAM switch. PSRAM boards set it in their own
straddle; a no-PSRAM board (a minimal LoRa-only node on a bare S3) leaves it
off, and every helper below degrades to plain internal allocation from the same
source.

## The allocator API (`mem.h`)

Two intents, one place to change them. `mem.h` is header-only `static inline`
wrappers over `heap_caps_*`.

| Call | Intent | Behaviour |
|------|--------|-----------|
| `gp_alloc(n)` | **could be PSRAM** | `MALLOC_CAP_SPIRAM`, falling back to `MALLOC_CAP_INTERNAL\|MALLOC_CAP_8BIT` so a full PSRAM never hard-fails. Plain internal when `!CONFIG_SPIRAM`. |
| `gp_calloc(count, size)` | could be PSRAM | zeroed `gp_alloc`. |
| `gp_realloc(p, n)` | could be PSRAM | `heap_caps_realloc(p, n, MALLOC_CAP_DEFAULT)` — keeps the block on whichever heap it already lives. |
| `gp_alloc_aligned(align, n)` | could be PSRAM | aligned `gp_alloc`. |
| `dram_alloc(n)` | **must be internal** | `MALLOC_CAP_INTERNAL\|MALLOC_CAP_8BIT`, never PSRAM, on any target. |
| `dma_alloc(n)` | **must be DMA-capable** | `MALLOC_CAP_DMA` (implies internal). |
| `gp_free(p)` | universal | `heap_caps_free` — frees a block from any of the above. |

**The global `operator new`/`delete` route through `gp_alloc`** (`mem_new.cpp`).
These are *replaceable* functions per the C++ standard, so a strong definition
wins over libstdc++'s without linker tricks. The consequence is that the entire
C++ heap — every `new`, every `std::string`/`vector`/`map` — follows this policy
in one place, with no per-site changes. C code that wants the policy calls
`gp_alloc` directly.

Because `operator new` calls `gp_alloc` → `heap_caps_malloc` directly, the C++
heap is **decoupled from `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL`** (which then only
governs IDF's own C `malloc()` path). LVGL follows the same policy through
`LV_USE_CUSTOM_MALLOC` → `lv_malloc_core` → `gp_alloc`.

`PSRAM_BSS` is the static-storage counterpart. A `static T x[N];` is `.bss`
reserved by the linker — no allocator runs, so it always lands in internal DRAM.
`PSRAM_BSS` (= `EXT_RAM_BSS_ATTR`) places such an array in external RAM instead —
the `.bss` equivalent of "could be PSRAM". It requires
`CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY` and degrades to internal `.bss`
when PSRAM (or that option) is absent. It carries `dram_alloc`'s opposite
constraint: **never** annotate data touched from an ISR, under a spinlock, or
during a flash cache-disable window.

## What MUST be in internal DRAM (never PSRAM)

These are correctness requirements, not preferences. Any of them in PSRAM
produces corruption that surfaces far from the cause.

1. **FreeRTOS sync objects accessed from a task or ISR critical section** —
   queues, stream buffers, message buffers, ring buffers. Their control block
   embeds an SMP spinlock taken via `taskENTER_CRITICAL` on every send/receive.
   Two independent failures:
   - the spinlock acquire uses an `S32C1I` atomic that is **unreliable on
     external PSRAM** → `owner`/`count` desync → `assert spinlock_acquire
     spinlock.h (lock->count == 0)`;
   - a flash op on the other core **disables the cache**, making the PSRAM
     control block unreadable mid-critical-section.

   Being "task-context only" (no ISR access) is **not** sufficient — these fault
   in plain task context. Allocate them with `dram_alloc`.

2. **DMA buffers** — WiFi static RX (16 × ~1.6 KB, internal-DMA-only, allocated
   when the radio connects), the SD-on-SPI read bounce, LCD transfer buffers.
   Use `dma_alloc` / `MALLOC_CAP_DMA`. Headroom is reserved via
   `CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL`.

3. **Anything touched during a flash cache-disable window** — including the
   *stack* of a task that itself runs SPI-flash code (direct `fopen`/`fread` on
   LittleFS). Spawn those with `STACK_DRAM` (below).

## What is fine in PSRAM (the default)

Plain data touched only in normal task context with the cache on: the cJSON
config tree, `std::string`/`vector`/`map`, LVGL object/style allocations, ITS
and log payloads, and most task stacks. None of these are ISR-touched,
lock-protected, DMA, or cache-disable-sensitive — so they take the `gp_alloc`
default and stay off scarce internal DRAM.

## Task stacks — `spawnTask`

`spawnTask(fn, name, stackBytes, arg, prio, core, stack_caps_t = STACK_PSRAM)`
(`compat.h`) wraps `xTaskCreatePinnedToCoreWithCaps`, allocating the TCB + stack
from the heap. The `stack_caps_t` argument is `STACK_PSRAM` (default) or
`STACK_DRAM`, gated on `CONFIG_SPIRAM` — on a no-PSRAM build every stack is
internal regardless. Use `STACK_DRAM` for a task that runs SPI-flash code paths
(direct `fopen`/`fread` on LittleFS), whose stack is therefore touched while the
PSRAM cache is disabled. Pair every `spawnTask` with `killSelf()` (or
`vTaskDeleteWithCaps`) at teardown so the heap-allocated TCB + stack are freed.

## Decision guide

```
Is it ISR-touched / inside a critical section / a FreeRTOS sync object? → dram_alloc
Is it a DMA target?                                                     → dma_alloc
Is it touched while a flash op has the cache disabled (incl. that
  task's own stack)?                                                    → internal (STACK_DRAM)
Otherwise (bulk data, std::, cJSON, UI):                                → gp_alloc (PSRAM)
```

## Kconfig knobs

Set in `sdkconfig.defaults.spangap` (consumer values override on collision):

| Symbol | Value | Why |
|--------|-------|-----|
| `CONFIG_SPIRAM` | `y` | The single PSRAM switch. Registers the PSRAM heap and gates every SPIRAM branch above. A no-PSRAM board sets `=n`. |
| `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL` | `0` | Every untagged C `malloc()` goes to PSRAM. Internal DRAM is scarce and WiFi needs it; the few control structures that must be internal are pinned explicitly, not via a size heuristic. |
| `CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL` | `65536` | Reserves 64 KB of internal DRAM for DMA-capable allocations (WiFi static RX, the SD-on-SPI read bounce, mbedTLS/lwIP) that all peak together at boot. |
| `CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY` | `y` | Enables `PSRAM_BSS` / `EXT_RAM_BSS_ATTR`. Opt-in per symbol — enabling it alone moves nothing. |

---

For the forensic rules behind these placements — why PSRAM-placement crashes are
deterministic and surface far away, the task-cleanup-hook trap, the
WiFi-headroom test, and the corruption-debugging toolkit — see
[memory-internals.md](memory-internals.md).
