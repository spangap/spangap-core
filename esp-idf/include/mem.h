#pragma once
/* Central allocation policy.
 *
 * Two intents, one place to change them:
 *   gp_alloc   — "could be PSRAM": the default sink for bulk data. Prefer PSRAM,
 *                fall back to internal so a full PSRAM never hard-fails. On a
 *                no-PSRAM target (CONFIG_SPIRAM unset) it is plain internal.
 *   dram_alloc — "must be internal": ISR-touched / critical-section / accessed
 *                during a flash cache-disable window (FreeRTOS control structs).
 *   dma_alloc  — "must be DMA-capable" (implies internal).
 *
 * The global operator new/delete (src/mem_new.cpp) route through gp_alloc, so the
 * entire C++ heap — every `new`, every std::string/vector/map — follows this
 * policy without per-site changes. C code that wants the policy calls gp_alloc
 * directly. free()/heap_caps_free() handle any of these, so gp_free is universal.
 *
 * CONFIG_SPIRAM is the single switch: PSRAM boards set it (in their straddle),
 * no-PSRAM boards leave it off ("0 MB"). It is also what registers the PSRAM
 * heap, so the SPIRAM branch only compiles where it can succeed. */
#include <stddef.h>
#include "sdkconfig.h"
#include "esp_heap_caps.h"
#include "esp_attr.h"

/* Static-storage counterpart to gp_alloc. The heap wrappers (operator new /
 * gp_alloc) route dynamic allocations to PSRAM, but a `static T x[N];` is .bss
 * reserved by the linker — no allocator runs, so it always lands in internal
 * DRAM. PSRAM_BSS places such an array in external RAM instead, the .bss
 * equivalent of the "could be PSRAM" intent. Same constraint as gp_alloc's
 * opposite (dram_alloc): NEVER use on data touched from an ISR, under a
 * spinlock, or during a flash cache-disable window — that must stay internal.
 * Requires CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY (sdkconfig.defaults.spangap);
 * degrades to internal .bss when PSRAM (or that option) is absent. */
#if CONFIG_SPIRAM && CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY
#define PSRAM_BSS EXT_RAM_BSS_ATTR
#else
#define PSRAM_BSS
#endif

#ifdef __cplusplus
extern "C" {
#endif

static inline void* gp_alloc(size_t n) {
#if CONFIG_SPIRAM
    void* p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
    return p ? p : heap_caps_malloc(n, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#else
    return heap_caps_malloc(n, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#endif
}

static inline void* gp_calloc(size_t count, size_t size) {
#if CONFIG_SPIRAM
    void* p = heap_caps_calloc(count, size, MALLOC_CAP_SPIRAM);
    return p ? p : heap_caps_calloc(count, size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#else
    return heap_caps_calloc(count, size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#endif
}

static inline void* gp_alloc_aligned(size_t align, size_t n) {
#if CONFIG_SPIRAM
    void* p = heap_caps_aligned_alloc(align, n, MALLOC_CAP_SPIRAM);
    return p ? p : heap_caps_aligned_alloc(align, n, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#else
    return heap_caps_aligned_alloc(align, n, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#endif
}

/* Must be internal DRAM (control structures touched from ISR / critical section
 * / during a flash cache-disable window). Never PSRAM, on any target. */
static inline void* dram_alloc(size_t n) {
    return heap_caps_malloc(n, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

/* Must be DMA-capable (implies internal). */
static inline void* dma_alloc(size_t n) {
    return heap_caps_malloc(n, MALLOC_CAP_DMA);
}

/* Realloc that works across either heap (keeps the block where it is). */
static inline void* gp_realloc(void* p, size_t n) {
    return heap_caps_realloc(p, n, MALLOC_CAP_DEFAULT);
}

/* Frees anything from the helpers above (internal or PSRAM). */
static inline void gp_free(void* p) { heap_caps_free(p); }

#ifdef __cplusplus
}
#endif
