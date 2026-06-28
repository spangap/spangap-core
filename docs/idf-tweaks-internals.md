# Local ESP-IDF tweaks — internals

Maintainer reference for the IDF/toolchain workarounds. The
[guide](idf-tweaks.md) describes what each tweak does; this document is the
guard-by-guard remediation for after an IDF upgrade, plus the `spi_helper`
bus-arbitration mechanism.

The standing rule: a workaround that silently stops applying is worse than the
bug it fixed, because the bug returns without warning. So each one fails the
build or fails loud when its IDF anchor moves.

## 1. Heap task-tracking bypass

**What it anchors on.** Three IDF functions —
`heap_caps_update_per_task_info_alloc`, `_free`, `_realloc` — defined in
`components/heap/heap_caps_base.c` / `heap_caps_init.c`. The `-Wl,--wrap=<fn>`
options redirect every call to `__wrap_<fn>` in
[`src/heap_track_stub.c`](../esp-idf/src/heap_track_stub.c). The stub signatures must
match IDF's exactly (the `_realloc` form carries
`heap, old_ptr, new_ptr, old_size, old_task, new_size, caps`).

**The guard** (`CMakeLists.txt`, configure time): reads both IDF source files
and `FATAL_ERROR`s if any of the three names (matched as `<fn>(`) is missing. If
the file moves or the symbol is renamed/split, the `--wrap` would no-op silently
and restore the interrupt-watchdog crash — the guard catches exactly that.

**If the guard fires after an IDF upgrade:**

1. Check whether the eager-bookkeeping design still exists in
   `heap_task_info.c` — look for `s_task_tracking_mutex` and the
   `xSemaphoreTake`/`Give` calls inside `heap_caps_update_per_task_info_*`.
   - If Espressif removed the mutex or reverted to on-demand walking, **delete
     the whole bypass** — the stub, the `--wrap` options, and the guard. It's no
     longer needed.
   - If they kept the design but renamed/restructured the entry points, update
     `_HEAP_TRACK_FNS` in `CMakeLists.txt`, the matching `--wrap` options, and the
     function names + signatures in `src/heap_track_stub.c` to the new symbols.
2. If per-task heap totals aren't worth the maintenance, the escape hatch is for
   the consuming app to set `CONFIG_HEAP_TASK_TRACKING=n` in its
   `sdkconfig.defaults`. `top` and `heapDump` keep working but report no per-task
   DRAM/PSRAM attribution.

The `FATAL_ERROR` message in `CMakeLists.txt` restates both options inline, so a
maintainer who hits it at build time doesn't need this file.

## 2. `spi_helper` — bus arbitration mechanism

[`src/spi_helper.cpp`](../esp-idf/src/spi_helper.cpp). Three concerns, each solving a
race or a redundant-call problem IDF leaves to the application.

**The serialization lock** is the load-bearing piece. `s_busMtx` is a
`xSemaphoreCreateMutexStatic` over a file-scope `StaticSemaphore_t` — created at
static-init so it is valid before any task transacts, with no first-caller race.
`spiHelperBusLock`/`Unlock` are bare `xSemaphoreTake(portMAX_DELAY)` / `Give`.

The reason it must exist, separate from `spi_master`'s own per-bus mutex:
`esp_lcd`'s color path queues an async DMA and **releases the driver bus lock
before the DMA has finished draining the hardware FIFO**. The bus is
electrically still busy, but `spi_master` considers it free. A co-resident
polling driver (a LoRa modem) that acquires the bus in that window drives a new
transaction into half-drained hardware and panics in `spi_hal_setup_trans`
(`running_cmd == 0`). The contract: the LCD holds `spiHelperBusLock` across
`draw_bitmap` **and** the DMA-completion callback; every other bus driver holds
it around its whole transaction. One global lock — the platform assumes a single
shared SPI bus, true on the T-Deck.

**`spiHelperInitBus`** makes `spi_bus_initialize` idempotent: it temporarily
sets the `"spi"` log tag to `ESP_LOG_NONE`, calls `spi_bus_initialize(host,
cfg, SPI_DMA_CH_AUTO)`, restores the level, and treats both `ESP_OK` and
`ESP_ERR_INVALID_STATE` (already initialized) as success. The first successful
caller's pin config is the one that takes; later callers' configs are ignored.

**`spiHelperEnsureGpioIsr`** installs the shared GPIO ISR service once. A
file-scope `static bool s_installed` short-circuits repeat calls; the actual
`gpio_install_isr_service` is wrapped in the same log-silencing dance and treats
`ESP_ERR_INVALID_STATE` as success, so even a racing second caller stays correct
and quiet. All callers must pass the same `ESP_INTR_FLAG_*` — the first install
wins.

This file deliberately stays minimal: per-device registration, reference-counted
teardown, and per-board CS-pin parking are out of scope until a real second
consumer needs them.
