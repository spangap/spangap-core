# Local ESP-IDF tweaks

Things diptych-core does to work around ESP-IDF or toolchain issues *without* editing the IDF tree. Each tweak has a guard that fails the build (or fails loud at runtime) when the underlying IDF construct it relies on changes — silent reversion is the failure mode we never want.

If you upgrade ESP-IDF and a guard fires, this file is the index of what to look at.

## Heap task tracking — eager-bookkeeping bypass (IDF 5.5+)

**Symptom on regression:** `Guru Meditation Error: Core 0 panic'ed (Interrupt wdt timeout on CPU0)` early in boot, with a backtrace ending in `xPortEnterCriticalTimeout` → `xQueueGenericSend` → `heap_caps_update_per_task_info_free` from inside any path that frees many small allocations in a row (storage `commitPatch` doing `cJSON_Delete` is the canonical trigger).

**Root cause:** IDF 5.5 rewrote `components/heap/heap_task_info.c` from 129 lines (5.4: on-demand walking, zero hot-path cost) to ~1000 lines (5.5: eager bookkeeping under a single global `s_task_tracking_mutex`, O(N) linear scan on every `malloc` and `free`). When two cores hammer the mutex simultaneously, the FreeRTOS kernel-internal queue spinlock backing `xSemaphoreTake/Give` spins long enough to trip `CONFIG_ESP_INT_WDT_TIMEOUT_MS` (default 300 ms).

The legacy `heap_caps_get_per_task_info()` API still works the same way — it walks block-owner stamps in `multi_heap` block headers, which `MULTI_HEAP_SET_BLOCK_OWNER` writes at every allocation. That's the cheap part of `CONFIG_HEAP_TASK_TRACKING`. The `top` CLI command ([`pm.cpp:407`](../diptych-core/src/pm.cpp#L407)) and `heapDump()` ([`pm.cpp:534`](../diptych-core/src/pm.cpp#L534)) only use that legacy walk — neither needs the new `heap_caps_get_all_task_stat()` API.

**The bypass:** [src/heap_track_stub.c](../diptych-core/src/heap_track_stub.c) provides three no-op functions; [CMakeLists.txt](../diptych-core/CMakeLists.txt) adds `-Wl,--wrap=heap_caps_update_per_task_info_{alloc,free,realloc}` linker options so all IDF call sites resolve to the no-ops. Block-owner stamping is unaffected, so `top` and `heapDump` show the same per-task DRAM/PSRAM columns as before, with zero alloc/free overhead — same behavior as IDF 5.4.

**Guard:** the same `CMakeLists.txt` reads `${IDF_PATH}/components/heap/heap_caps_base.c` and `${IDF_PATH}/components/heap/heap_caps_init.c` at configure time and fails the build with `FATAL_ERROR` if any of the three function names is missing — the only way the `--wrap` intercept can silently revert.

**If the guard fires (after IDF upgrade):**

1. Check if the eager-bookkeeping design is still in `heap_task_info.c` (look for `s_task_tracking_mutex` and the `xSemaphoreTake` calls in `heap_caps_update_per_task_info_*`).
   - If Espressif removed the mutex / went back to on-demand, remove this whole bypass — it's no longer needed.
   - If they kept the design but renamed the entry points, update `_HEAP_TRACK_FNS` in `diptych-core/CMakeLists.txt`, the `--wrap` options, and the function names + signatures in `diptych-core/src/heap_track_stub.c`.
2. If you don't need per-task heap totals at all, the simple escape is for the consuming app to set `CONFIG_HEAP_TASK_TRACKING=n` in its `sdkconfig.defaults`. `top` and `heapDump` keep working but report no per-task DRAM/PSRAM attribution.
