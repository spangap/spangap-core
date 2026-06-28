# Local ESP-IDF tweaks

`spangap-core` works around ESP-IDF and toolchain issues **without editing the
IDF tree** — every workaround lives in this straddle's own sources, build
config, or sdkconfig defaults. The principle that makes this safe:

> Every workaround carries a guard that fails the build (or fails loud at
> runtime) when the underlying IDF construct it relies on changes. **Silent
> reversion is the failure mode this is built to prevent** — a workaround that
> quietly stops applying after an IDF upgrade, restoring the original crash.

If you upgrade ESP-IDF and a guard fires, [idf-tweaks-internals.md](idf-tweaks-internals.md)
is the index of what each guard protects and how to remediate it.

## Heap task-tracking — eager-bookkeeping bypass (IDF 5.5+)

IDF 5.5 rewrote per-task heap tracking from on-demand walking (cheap, the 5.4
behaviour) to **eager bookkeeping** under a single global mutex: every `malloc`
and `free` now takes `heap_task_info.c`'s `s_task_tracking_mutex` and does an
O(N) linear scan. Under two-core contention — canonically a `cJSON_Delete` burst
at boot freeing many small blocks in a row — the kernel queue spinlock backing
the semaphore spins long enough to trip `CONFIG_ESP_INT_WDT_TIMEOUT_MS`, and the
device panics with an interrupt-watchdog timeout early in boot.

spangap-core doesn't need that new bookkeeping. `pm.cpp`'s `top` CLI command and
`heapDump()` read only the **legacy** `heap_caps_get_per_task_info()`, which
walks block-owner stamps written into `multi_heap` headers by
`MULTI_HEAP_SET_BLOCK_OWNER` at every allocation — the cheap half of
`CONFIG_HEAP_TASK_TRACKING`, unaffected by the new path.

**The bypass:** [`src/heap_track_stub.c`](../esp-idf/src/heap_track_stub.c) provides
no-op `__wrap_` versions of three functions, and `CMakeLists.txt` adds linker
options
`-Wl,--wrap=heap_caps_update_per_task_info_{alloc,free,realloc}` so every IDF
call site resolves to the no-ops. Block-owner stamping stays on, so `top` and
`heapDump` show the same per-task DRAM/PSRAM columns at zero alloc/free overhead —
exactly the 5.4 behaviour. `CONFIG_HEAP_TASK_TRACKING=y` stays set (it's what
enables the block-owner stamps).

**The guard:** the same `CMakeLists.txt` reads
`${IDF_PATH}/components/heap/heap_caps_base.c` and `heap_caps_init.c` at
configure time and `FATAL_ERROR`s the build if any of the three wrapped function
names is missing — the only way the `--wrap` intercept could silently revert.
Remediation steps are in the [internals](idf-tweaks-internals.md#1-heap-task-tracking-bypass).

## Shared SPI bus — `spi_helper`

On boards like the T-Deck Plus, one SPI host (FSPI / `SPI2_HOST`) is shared
across the display, microSD, and LoRa modem — they share SCK/MOSI/MISO. IDF's
`spi_master` driver already arbitrates *between registered devices* on a bus
(a per-bus mutex around each transaction); `spi_helper`
([`include/spi_helper.h`](../esp-idf/include/spi_helper.h)) supplies the application-layer
pieces IDF doesn't:

| Function | Purpose |
|----------|---------|
| `spiHelperInitBus(host, bus_config)` | Idempotent `spi_bus_initialize`. Any driver can call it during its own init without caring who ran first; the first successful caller's pin config wins, later calls return `ESP_OK` for an already-up bus. IDF's `"SPI bus already initialized"` error log is suppressed across the call. |
| `spiHelperBusLock()` / `spiHelperBusUnlock()` | One global lock serializing access the `spi_master` bus lock can't coordinate by itself. `esp_lcd`'s color transfer queues an async DMA and then *releases* the driver bus lock before the DMA finishes draining the hardware; a co-resident polling driver (LoRa) that grabs the bus in that window panics in `spi_hal_setup_trans`. The LCD must hold this across `draw_bitmap` **and** DMA completion; other drivers hold it around their transaction. |
| `spiHelperEnsureGpioIsr(intr_flags)` | Installs the shared GPIO ISR service exactly once for the whole app. The display's input INT lines (touch/button/trackball) and the LoRa modem's DIO path both call `gpio_install_isr_service()`; whichever runs second otherwise logs an error. The first caller installs, the rest are no-ops; all callers must pass the same `ESP_INTR_FLAG_*` (first one wins). |

Per-device registration is **not** owned here — each driver still calls
`spi_bus_add_device` / `spi_device_*` itself. The lock is one global mutex: the
platform assumes a single shared SPI bus (true on T-Deck).

## Other IDF-defaults tweaks

Surfaced from `sdkconfig.defaults.spangap` and `CMakeLists.txt` because they
work around an IDF default that is wrong for this platform:

- **`CONFIG_FATFS_SECTOR_512=y`** — FATFS sector size 512 B (IDF defaults the
  choice to 4096, which exists only to serve 4K-sector media or wear-levelled
  FAT-on-flash). `FF_MAX_SS` is set from this choice; every per-file cache and
  the volume window is sized to it. Internal flash here is LittleFS, not FAT, and
  the SD card is physically 512 B, so dropping to 512 shrinks each per-file cache
  + the window 8× (~25 KB across `max_files` + the volume) with no downside.

- **`CONFIG_WL_SECTOR_SIZE_512=y`** — the wear-levelling layer's sector size.
  There is **no** WL partition (flash is LittleFS), so this is inert today, but
  it's pinned to 512 so nothing silently inherits the 4096 default if a WL-backed
  FAT volume is ever added. (Note it does *not* drive `FF_MAX_SS` — that's
  `CONFIG_FATFS_SECTOR_512` above.)

- **`SOC_SDMMC_PSRAM_DMA_CAPABLE=1`, defined only when
  `CONFIG_SPANGAP_SDCARD_BUS_SPI`** (in `CMakeLists.txt`, scoped to the `sdmmc`
  component). `sdmmc_cmd.c` bounces every PSRAM-sourced sector through a fresh
  `heap_caps_malloc(512, MALLOC_CAP_DMA)` per write, gated on the SDMMC
  *peripheral's* `SOC_SDMMC_PSRAM_DMA_CAPABLE` (unset on the S3) — irrelevant to
  SD-on-SPI, where the SDSPI host already bounces through its own reused per-slot
  buffer. Forcing the cap on lets `sdmmc_cmd` skip its redundant per-write alloc,
  which otherwise runs constantly and fails with `ESP_ERR_NO_MEM` (0x101) when the
  internal DMA pool is tight (WiFi + LCD). A real SDMMC-peripheral board
  (`BUS_SDMMC`) keeps the bounce — its dedicated DMA genuinely can't reach PSRAM.

## Platform helpers — `compat.h`

[`include/compat.h`](../esp-idf/include/compat.h) is the header-only grab-bag of
small inline helpers every straddle uses to paper over IDF/libc rough edges.
They are convenience wrappers, not guarded workarounds, so they carry no
build-time guard.

| Helper | What it does |
|--------|--------------|
| `millis()` | Uptime in ms from `esp_timer_get_time()` (Arduino-style). |
| `delay(ms)` | `vTaskDelay` that drops the auto CPU boost while sleeping and restores it after — see [power-management.md](power-management.md). `delay(0)` is a bare yield. |
| `safeStrncpy(dst, src, n)` | `strncpy` that always NUL-terminates and `ESP_LOGE`s on truncation. |
| `utcOffsetMinutes(t)` | Local UTC offset in minutes for an epoch time (compares `localtime_r` vs `gmtime_r`, month-wrap corrected). |
| `fpsToIntervalMs(fps)` | fps config value → interval ms (`>0` = fps, `<0` = 1 frame every `-fps` s, `0` = disabled). |
| `fmtWallClock(buf, len)` | `"Mar 27 16:23:15.342"` wall-clock string. |
| `fmtElapsed(secs, buf, len)` | Elapsed seconds as `"3s"` / `"1m22s"` / `"2h3m"` / `"3d5h"` (zero components dropped). |
| `fmtSize(bytes, buf, len)` | Byte count with 3 significant digits (`"1.23kB"`, `"45.6MB"`). |
| `fmtBps(bps, buf, len)` | Bandwidth with 3 significant digits (`"1.23kbps"`, `"45.6Mbps"`). |

`compat.h` also declares two facilities owned by other functions, kept here so
every module shares one definition: the task-stack spawner
(`spawnTask`/`killSelf`/`stack_caps_t`, documented in [memory.md](memory.md)) and
the central RTC-RAM validity word (`rtcRamValid`/`rtcRamSetValid`, documented in
[power-management-internals.md](power-management-internals.md)).

---

For the exhaustive workaround/guard list with post-upgrade remediation, and the
`spi_helper` bus-arbitration mechanism, see
[idf-tweaks-internals.md](idf-tweaks-internals.md).
