/* Global C++ allocator override.
 *
 * Replaces libstdc++'s operator new/delete so every `new` and every standard-
 * library allocation (std::string/vector/map/...) flows through the central
 * policy in mem.h (gp_alloc → PSRAM-preferred, internal on no-PSRAM targets).
 * These are *replaceable* functions per the C++ standard, so a strong definition
 * here wins over the library's without linker tricks.
 *
 * gp_alloc calls heap_caps_malloc directly, so the C++ heap bypasses the
 * SPIRAM_MALLOC_ALWAYSINTERNAL threshold that governs the C malloc() path — i.e.
 * the bulk std:: surface goes to PSRAM regardless of how that threshold is tuned
 * for IDF's own driver allocations. Per-task heap attribution (top/heapDump) is
 * unaffected: it walks heap blocks by owner, not the malloc symbol. */
#include <new>
#include <cstdlib>
#include "mem.h"

static inline void* gp_new(std::size_t n) {
    void* p = gp_alloc(n ? n : 1);     /* new(0) must return a unique non-null ptr */
#if __cpp_exceptions
    if (!p) throw std::bad_alloc();
#else
    if (!p) std::abort();              /* minimal/no-PSRAM builds may drop -fexceptions */
#endif
    return p;
}

void* operator new  (std::size_t n)                                  { return gp_new(n); }
void* operator new[](std::size_t n)                                  { return gp_new(n); }
void* operator new  (std::size_t n, const std::nothrow_t&) noexcept  { return gp_alloc(n ? n : 1); }
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept  { return gp_alloc(n ? n : 1); }

#if __cpp_aligned_new
void* operator new  (std::size_t n, std::align_val_t a)              { return gp_alloc_aligned((size_t)a, n ? n : 1); }
void* operator new[](std::size_t n, std::align_val_t a)              { return gp_alloc_aligned((size_t)a, n ? n : 1); }
void* operator new  (std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept { return gp_alloc_aligned((size_t)a, n ? n : 1); }
void* operator new[](std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept { return gp_alloc_aligned((size_t)a, n ? n : 1); }
#endif

void operator delete  (void* p) noexcept                             { gp_free(p); }
void operator delete[](void* p) noexcept                             { gp_free(p); }
void operator delete  (void* p, std::size_t) noexcept                { gp_free(p); }
void operator delete[](void* p, std::size_t) noexcept                { gp_free(p); }
void operator delete  (void* p, const std::nothrow_t&) noexcept      { gp_free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept      { gp_free(p); }
#if __cpp_aligned_new
void operator delete  (void* p, std::align_val_t) noexcept           { gp_free(p); }
void operator delete[](void* p, std::align_val_t) noexcept           { gp_free(p); }
void operator delete  (void* p, std::size_t, std::align_val_t) noexcept   { gp_free(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept   { gp_free(p); }
#endif
