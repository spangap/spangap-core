/* IDF 5.5 heap-task-tracking eager-bookkeeping bypass.
 *
 * IDF 5.5 rewrote per-task heap tracking from on-demand walking (cheap,
 * 5.4 behavior) to eager bookkeeping under a single global mutex
 * (heap_task_info.c:s_task_tracking_mutex).  Every malloc/free now takes
 * the mutex and does an O(N) linear scan, which produces enough
 * cross-core contention to trip CONFIG_ESP_INT_WDT_TIMEOUT_MS during
 * cJSON_Delete bursts at boot.
 *
 * We don't need the new heap_caps_get_all_task_stat() API — pm.cpp
 * (top, heapDump) only reads the legacy heap_caps_get_per_task_info(),
 * which walks block-owner stamps in multi_heap headers and is
 * unaffected by the new bookkeeping.
 *
 * --wrap link options in main/CMakeLists.txt route IDF's
 * heap_caps_update_per_task_info_{alloc,free,realloc} to these no-ops.
 * Block-owner stamping (MULTI_HEAP_SET_BLOCK_OWNER) stays on, so
 * top/heapDump keep working at zero hot-path cost.
 *
 * If a future IDF renames these functions, the --wrap silently
 * no-ops the rename and the crash returns.  CMakeLists.txt has a
 * guard that greps the IDF tree at configure time to catch that.
 */

#include <stddef.h>
#include <stdint.h>

void __wrap_heap_caps_update_per_task_info_alloc(void *heap, void *ptr,
                                                 size_t size, uint32_t caps) {
    (void)heap; (void)ptr; (void)size; (void)caps;
}

void __wrap_heap_caps_update_per_task_info_free(void *heap, void *ptr) {
    (void)heap; (void)ptr;
}

void __wrap_heap_caps_update_per_task_info_realloc(void *heap, void *old_ptr,
                                                   void *new_ptr,
                                                   size_t old_size,
                                                   void *old_task,
                                                   size_t new_size,
                                                   uint32_t caps) {
    (void)heap; (void)old_ptr; (void)new_ptr;
    (void)old_size; (void)old_task; (void)new_size; (void)caps;
}
