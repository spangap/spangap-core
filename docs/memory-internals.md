# Memory placement — internals

Maintainer reference for the allocation policy. The [guide](memory.md) is the
API and the decision rules; this document is the durable forensic lessons behind
them and the toolkit for hunting heap corruption. Each rule below is the
distilled, present-tense form of a class of bug that recurs whenever placement
is gotten wrong.

## Rules that hold

- **PSRAM-placement corruption is deterministic and surfaces far away.** A
  FreeRTOS queue/ring/stream-buffer in PSRAM, corrupted via the spinlock/cache
  path, does not crash where it lives. The symptom is a `LoadProhibited` walking
  the cJSON tree, or an `InstructionFetchError` calling a garbage ring-buffer
  function pointer — somewhere unrelated. The crashes are byte-identical and
  repeatable across boots. Fix the *placement* of the sync object, not the
  victim it happened to clobber.

- **It is not MSPI timing.** Dropping octal PSRAM 80→40 MHz and pinning DFS
  (`pm.cpp` `esp_pm_configure` with min == max == 240, DFS off) does not change a
  placement crash — the USB-JTAG fault stayed byte-for-byte identical (same PC
  `0x3c2f8360`, same registers) across boots. Marginal timing produces *varying*
  garbage; a deterministic crash is a reproducible memory bug. So do not go
  chasing a placement bug with clock tweaks; move the sync object to internal
  DRAM. (40 MHz also halves PSRAM bandwidth for nothing and is a revert
  candidate wherever it still lingers.)

- **The cJSON `LoadProhibited` during a flash read was the *same* placement bug,
  not a separate MSPI-timing fault (corrected).** An earlier reading blamed the
  `LoadProhibited` in cJSON / `navigatePath` / `storageGetInt` while reading a
  state file (`cat /state/<file>`) on marginal 80 MHz octal-PSRAM MSPI timing.
  That was wrong. It is almost certainly a downstream *victim* of the broken ITS
  inbox-queue spinlock: an ineffective critical section let racing heap ops
  scribble on `cfgRoot`, and the corruption only faulted later while walking the
  tree. It stopped surfacing once the ITS queue moved to internal RAM — a timing
  fault would not care where a *queue* lives. The board context that reading got
  right still holds (ESP32-S3 S3R8; octal PSRAM and flash both @ 80 MHz on a
  shared MSPI); only the timing conclusion was wrong. There is no confirmed
  MSPI-timing crash on these boards — don't reintroduce a "flash-read faults are
  timing" carve-out.

- **The fix is placement; the precise mechanism is a hypothesis.** What is solid
  and on-device confirmed is that moving an ISR- or critical-section-touched
  FreeRTOS object (queue, stream buffer, ring) out of PSRAM into internal RAM
  makes the crash go away. *Why* is less certain — plausibly the spinlock's
  `S32C1I` atomic being unreliable on external PSRAM, and/or an IRAM ISR or a
  cross-core flash-op cache-disable window touching the PSRAM control block while
  cache is down. Treat the mechanism as a working theory, the fix as fact. Two
  crashes were observed and their fixes confirmed on-device (2026-06-13):
  - **ITS inbox queue** (`its.cpp`, `xQueueCreateWithCaps(depth, sizeof(its_msg*),
    MALLOC_CAP_INTERNAL)`) — the `cli` task's `itsPoll → xQueueReceive` was
    asserting `spinlock_acquire` / `lock->count == 0` (`spinlock.h:142`).
    **CONFIRMED** crash, **CONFIRMED** fix.
  - **USB-JTAG driver ring** (`cli.cpp`) — the IDF driver builds its TX/RX rings
    with plain `xRingbufferCreate` (no caps), so under `ALWAYSINTERNAL=0` they
    land in PSRAM; the serial task's `xRingbufferSend` then jumped a garbage
    PSRAM function pointer (`InstructionFetchError`). Forced internal by wrapping
    `usb_serial_jtag_driver_install` in `heap_caps_malloc_extmem_enable(32 KB)`
    (threshold restored to `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL` afterward).
    **CONFIRMED** crash, **CONFIRMED** fix.
  A queue must go *wholly* internal — FreeRTOS copies items **inside** the lock,
  so a queue's storage can't stay in PSRAM even with a static control/storage
  split. A stream buffer is the exception: IDF copies stream payload **outside**
  the lock, so `its.cpp` splits its link rings — `StaticStreamBuffer_t` control
  block internal via `xStreamBufferCreateStatic`, ring storage left in PSRAM
  (`its_pool_entry_t.sbCtrl` / `.sbStore`, both released in `poolFree`). That
  split was applied *defensively* by the same class of reasoning, **not** from an
  independently observed crash — correct, but lower confidence, and it added real
  complexity for an unobserved bug.

- **`CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=0` is the armed landmine (systemic,
  not yet fixed).** The two fixes above are surgical pins; the root condition —
  every untagged allocation defaulting to PSRAM — still routes other IDF driver
  control structs (UART / SPI / SD ring buffers and queues) into PSRAM, where the
  same bug can fire. The durable fix is to give cJSON explicit PSRAM allocator
  hooks (it has none today and only tolerates PSRAM residence because of `=0`),
  then restore a sane `ALWAYSINTERNAL` threshold so driver control structs
  default to internal — gated on an on-device internal-DRAM headroom check first
  (WiFi-connected + SD + LCD, per the connect-cost rule below). Until then, treat
  any new driver ring/queue as suspect.

- **A FreeRTOS task-cleanup hook must match IDF's *live* hook name, or it
  compiles, links, and is never called.** Under IDF 5.5 the live hook is
  `vTaskPreDeletionHook`, enabled by `CONFIG_FREERTOS_TASK_PRE_DELETION_HOOK`.
  The legacy `vPortCleanUpTCB` (gated by
  `CONFIG_FREERTOS_ENABLE_STATIC_TASK_CLEAN_UP`, never set) is deprecated. A
  cleanup handler defined under the wrong name produces no link error — just
  silence. The consequence is concrete: a self-deleting task that touched ITS
  (canonically `main_task`, which drives boot proxy ops then returns from
  `app_main`) leaves a stale `s_tasks` handle; a later notify routed to that slot
  does `xTaskNotifyGive` into the freed TCB — a use-after-free. `its.cpp` runs
  its slot-nulling cleanup under `vTaskPreDeletionHook`, which is why
  `CONFIG_FREERTOS_TASK_PRE_DELETION_HOOK=y` is mandated in
  `sdkconfig.defaults.spangap`.

- **WiFi's internal-DMA cost is on connect, not scan.** Turning WiFi fully off
  frees almost no DRAM/DMA versus idle scanning or AP mode; the internal-DMA
  pressure appears only when STA actually connects (lwIP/TLS, the 16 static RX
  buffers). The headroom test that matters is therefore **WiFi-connected + SD +
  LCD**, not WiFi-off. A build that looks comfortable idle can still run the DMA
  pool dry the moment it associates.

- **`CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=0` is the measured choice, not the
  default.** The inverse (`=16384`, IDF's internal-preferred default) pulls
  ~28 KB into internal DRAM — almost all of it PSRAM-safe UI/buffer data (LVGL,
  CLI) — and drops DMA-lowest to ~19 KB *with WiFi off*, which does not survive
  WiFi connecting. PSRAM-default plus surgical internal pins beats
  internal-default on these boards.

- **Don't enable `CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP` on display boards.** The
  LCD/LVGL already claim the internal DMA pool; nudging WiFi/lwIP toward PSRAM
  tips the WiFi static RX buffers (16 × ~1.6 KB, internal-DMA-only, cannot move
  to PSRAM) over the edge — "Expected to init 16 rx buffer, actual is 12" → WiFi
  never inits. A display-less board with internal headroom can set it per-board.

## Debugging heap corruption

A toolkit is available for hunting a stray write or use-after-free into internal
DRAM. It is opt-in via Kconfig (off by default — real CPU/throughput cost) and
is **not** preset in `sdkconfig.defaults.spangap`.

- **`CONFIG_SPANGAP_HEAP_INTEGRITY_POLL`** (bool, Component config → spangap-core)
  — the log task calls `heap_caps_check_integrity(MALLOC_CAP_INTERNAL, true)`
  once per **`CONFIG_SPANGAP_HEAP_INTEGRITY_POLL_MS`** (default 1000), aborting
  at the first corrupted block and naming the region. A clobbered FreeRTOS list /
  ring-buffer semaphore, or a notify into a freed TCB, faults at or near the bad
  write instead of much later in unrelated code.

- For the full hunt also enable, via menuconfig (a `select` can't reach a choice
  option, so these aren't bundled):
  - **`CONFIG_HEAP_POISONING_COMPREHENSIVE`** (Component config → Heap memory
    debugging → "Comprehensive") — head/tail canaries + `0xFE` free-fill, which
    is what the integrity poll inspects.
  - **`CONFIG_FREERTOS_CHECK_STACKOVERFLOW_PTRVAL`** (FreeRTOS → Check for stack
    overflow → "Check using canary + PTRVAL").

- **`CONFIG_SPANGAP_WATCH_ADDR`** (hex, default `0x0`) — when non-zero, the log
  task arms a hardware **store-watchpoint** over a 16-byte window around the
  address, on both cores, early at boot (watchpoint slot 1; slot 0 is left for
  IDF/panic/gdbstub). The next instruction that writes anywhere in that window
  faults immediately with its own backtrace.

**Workflow:** the integrity poll reports `CORRUPT HEAP … at 0xADDR` → set
`CONFIG_SPANGAP_WATCH_ADDR` to that address → reflash → the faulting write names
the culprit. The window is aligned down to 16 bytes, so two corrupted words a few
bytes apart share one watchpoint. This assumes the address is stable across boots
(boot-time allocation order is deterministic; confirm the address survives a
reboot). Caveat: a watchpoint on live-then-freed memory fires on the legitimate
*live* write first — which still identifies *what* lives there, half the answer.
