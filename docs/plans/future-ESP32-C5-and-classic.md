# Future ports: ESP32-C5 and classic ESP32

Starting-point notes for porting spangap off ESP32-S3. Not plans of record —
checklists to consult when the work begins.

Two targets are covered because they share a lot of work (re-enabling AES-GCM,
adding `esp32c5` / `esp32` to the component manifest, retuning PSRAM mode, etc.)
but diverge on a few axes (dual-band UI, core count, PSRAM errata).

Read [`../../CLAUDE.md`](../../CLAUDE.md) §"ESP-IDF Specifics" for the S3-side
baseline these notes are diffing against.

# ESP32-C5

The friendliest non-S3 target: PSRAM, USB Serial JTAG, SDMMC, dual-band WiFi 6,
BLE 5. The S3 lock-in points are mostly shape-of-PSRAM, dual-core assumptions,
and one S3-specific silicon bug.

## Single core

- C5 is single-core RISC-V. Every `xTaskCreatePinnedToCore` site needs a
  conditional core argument (or a `SPANGAP_CORE_HEAVY` macro that resolves to
  1 on S3, 0 on C5).
- `CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU1=n` and the "core 1 hosts heavy
  workers" guidance in [`../../CLAUDE.md`](../../CLAUDE.md) become vestigial.
- The webrtc_task `vTaskDelay(1)` workaround is about IDLE0 specifically — on
  C5, IDLE0 is contended by *everything*. Sweep all tasks for whether they
  voluntarily yield enough; some that were "fine on core 1" may starve IDLE0.

## PSRAM shape

- C5 is **quad** PSRAM, typically ≤4 MB. S3 is **OPI 8 MB**.
- `sdkconfig.defaults.spangap`: `CONFIG_SPIRAM_MODE_OCT=y` → `MODE_QUAD`;
  re-tune `CONFIG_SPIRAM_SPEED_*`.
- Audit "PSRAM is plentiful" defaults — they were sized for 8 MB:
  - SCTP rexmit pool (1 MB default in [`webrtc_sctp.h`](../../spangap-core/include/webrtc_sctp.h))
  - `histBuf` and log ring buffer in [`cli.cpp`](../../spangap-core/src/cli.cpp) /
    [`log.cpp`](../../spangap-core/src/log.cpp)
  - ITS stream-buffer pool in [`its.cpp`](../../spangap-core/src/its.cpp) — grows
    on demand and never shrinks
  - Per-task stack defaults in [`compat.h`](../../spangap-core/include/compat.h)
- Nothing *breaks* with less PSRAM, but headroom for cJSON-heavy storage churn
  + WebRTC + log buffering shrinks fast. Smaller defaults + an OOM rehearsal.

## TLS — re-enable AES-GCM

[`tls.cpp:236`](../../spangap-core/src/tls.cpp) restricts ciphersuites to
ChaCha20-Poly1305 specifically to dodge the S3 hardware GCM-DMA bug
(espressif/esp-idf#12689). C5 doesn't have it. Re-enable GCM — better interop
(some clients prefer it) and hardware accel is faster on the C5 crypto block.
Should be a chip-conditional in tls.cpp, not an unconditional flip.

## Dual-band WiFi

The core/browser settings UI is built around 2.4 GHz only today. With 5 GHz:

- **Country code becomes load-bearing.** Wrong regulatory domain → 5 GHz APs
  invisible or refused. Add `s.net.country` key with proper SPA picker. Today
  there is no such key.
- **Band display in the WiFi scanner.** IDF scan results carry the band; SPA
  needs to surface it (label, filter, or band-pref toggle).
- **WPA3/SAE in the security-mode picker.** WiFi 6 deployments lean on SAE.
  Auth-mode dropdown needs it as a first-class option, not a vague "WPA2/WPA3".
- **Per-band channel awareness** if the UI ever shows a channel hint.
- Possible band-lock setting (some users want 5 GHz only, some want 2.4 GHz
  only for range).
- 5 GHz throughput is much higher → revisit lwIP tuning
  (`LWIP_TCP_SND_BUF_DEFAULT`, `LWIP_TCP_WND_DEFAULT`) — current values were
  picked for 2.4 GHz throughput ceilings.

## Power management / USB Serial JTAG

The S3 USB-pullup + light-sleep workaround in
[`pm.cpp`](../../spangap-core/src/pm.cpp) is S3-specific. C5 USB Serial JTAG
handles light sleep differently. Re-verify the deep-sleep / light-sleep / USB
attach-detach matrix on C5 rather than copy-paste — likely simplification.

## Cron / RTC SRAM

`rtcRamSetValid()` and `cronWakeupHandler()` rely on RTC slow memory surviving
deep sleep. C5 RTC SRAM size and persistence semantics differ from S3 — verify
size budget and that the RTC vars survive the sleep modes spangap uses, before
trusting the cron-wakeup path.

## SDMMC

C5 has SDMMC but only slot 0, with narrower pin options than S3's slot 1. SD
pins are already consumer-Kconfig, so no spangap-side code changes — but the
`SPANGAP_SDCARD_4BIT` option may not be reachable on common C5 variants. Update
[`Kconfig`](../../spangap-core/Kconfig) help text accordingly.

## IDF + component manifest

- [`idf_component.yml`](../../spangap-core/idf_component.yml): add `esp32c5` to
  `targets:`. IDF version dep `>=5.5` is fine — C5 was finalized around 5.4.
- `CONFIG_IDF_TARGET="esp32s3"` in `sdkconfig.defaults.spangap` needs to move
  out of the platform defaults (it's per-consumer-board) or be guarded.
- Heap-task-tracking `--wrap` guard in
  [`CMakeLists.txt`](../../spangap-core/CMakeLists.txt) is IDF-version-pinned,
  not chip-pinned — should keep working as long as IDF version matches.

## WireGuard

- `WIREGUARD_x25519_IMPLEMENTATION_DEFAULT` is software-only on RISC-V; bench
  handshake-burst behavior on a single core. The NaCL impl may or may not be
  preferable.
- `CONFIG_LWIP_PPP_SUPPORT=y` netif->state workaround is IDF-version-pinned,
  not chip-pinned — still applies until upstream's fix propagates.

## Bonus — not blocking

- BLE provisioning. Both S3 and C5 have BLE 5 — spangap just doesn't use it
  today. A BLE-provisioning path could replace the AP-mode bootstrap (nicer
  UX than "join the spangap-XXXX SSID, browse to 192.168.4.1"). Not C5-specific
  and not a port blocker, but worth bundling into the same dual-band-UI work
  since both touch the same config-bootstrap surface. Optional module behind
  `CONFIG_SPANGAP_BLE_PROV`.
- WiFi 6 features (TWT, OFDMA) — opt-in tuning, not a port concern.

## Out of scope here

The camera/audio/recording side belongs to the consumer (e.g. seccam), not
spangap-core. Worth flagging for whoever ports seccam: `esp32-camera`'s
LCD_CAM support on C5 is the open question — spangap itself doesn't care.

# Classic ESP32

Classic ESP32 is dual-core Xtensa LX6 with PSRAM, so a surprising amount of S3
scaffolding survives. The port is *easier in code* than C5 (no core-pinning
rework, no UI rework — ESP32 is 2.4 GHz only) but *harder in production
confidence* because of a PSRAM silicon errata that S3 doesn't have.

## Console — UART, not USB Serial JTAG

`CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y` → `CONFIG_ESP_CONSOLE_UART_DEFAULT=y`.
Classic ESP32 has no native USB at all, so the USB-pullup + light-sleep dance
in [`pm.cpp`](../../spangap-core/src/pm.cpp) is irrelevant — delete that path
under a chip guard. Light sleep + UART console interaction is its own story
(UART loses bytes on wake without flow control); re-verify rather than assume.

## PSRAM silicon errata — the real gotcha

Classic ESP32 has a PSRAM cache read-race bug
([esp-chip-errata: CPU subsequent access halted when PSRAM accessed](https://docs.espressif.com/projects/esp-chip-errata/en/latest/esp32/03-errata-description/esp32/cpu-subsequent-access-halted-when-psram-accessed.html)).
ECO3 (silicon rev 3) fixes it; older silicon needs `CONFIG_SPIRAM_CACHE_WORKAROUND=y`,
which compiles in a workaround that costs ~5–10 % on every PSRAM access.

Spangap is **aggressively** PSRAM-heavy — every task stack, every ITS buffer,
every log byte sits in PSRAM. The workaround hits everywhere. Two choices:
1. Require ECO3 silicon in the README and refuse boot below it (`esp_chip_info`
   exposes the revision), or
2. Accept the perf hit globally and ship the workaround on by default.

S3 doesn't have this errata so the question never came up. Whichever path is
chosen, the silicon-revision matrix becomes something users have to know about.

## PSRAM mode

`CONFIG_SPIRAM_MODE_OCT=y` → `MODE_QUAD`. ESP32 PSRAM is SPI/quad — no octal.
Speed knob retune (40 MHz / 80 MHz). Typical sizes 4–8 MB on WROVER variants;
same headroom audit as the C5 chapter applies.

## TLS — re-enable AES-GCM

Same as C5: the GCM-DMA bug is S3-specific. ESP32's hardware AES is fine.
Re-enable GCM as a chip-conditional in [`tls.cpp`](../../spangap-core/src/tls.cpp).

## Same as S3, no rework

- **Dual-core** — core pinning stays as-is. The "core 1 hosts heavy workers"
  guidance still applies; `CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU1=n` stays.
- **WiFi UI** — 2.4 GHz only, no dual-band picker, no country-code work, no
  WPA3/SAE pressure. The whole C5 "Dual-band WiFi" chapter is skipped.

## Performance ceiling drops

Xtensa LX6 is single-issue; LX7 (S3) is dual-issue. Crypto-software paths
slow down materially:
- Software ChaCha20-Poly1305 (when AES-GCM unavailable on the peer)
- WireGuard handshake — x25519 reference impl is the hot path, runs in
  software on Xtensa
- mbedTLS software fallbacks for less common ciphers

With hardware AES-GCM re-enabled, the TLS bulk cipher is fine; the handshake
hot path (ECDHE-ECDSA) gets roughly 30–40 % slower. WebRTC SCTP CPU usage rises.
Probably still fits the budget, but bench before promising parity.

## IRAM headroom

`CONFIG_LWIP_IRAM_OPTIMIZATION=y` fits with less margin on classic ESP32.
Some functions you might want IRAM-resident on S3 won't fit. Likely fine —
just less knob room before linker errors.

## Flash size

Most classic ESP32 modules ship 4 MB; WROVER-B/E goes to 8/16. The partition
generator already takes a flash-size knob, so no code change — just a smaller
`app%` budget on 4 MB modules, possibly forcing `CONFIG_SPANGAP_OTA=n` (which
collapses A/B pairing and reclaims half the app+fixed space — exactly what
that knob is designed for).

## BLE 4.2 + Classic BT

ESP32 has Classic Bluetooth + BLE 4.2 (not BLE 5). Doesn't matter today
because spangap doesn't use Bluetooth. If/when BLE provisioning lands, no
extended advertising / 2M PHY — small UX limitation, not a blocker.

## IDF + component manifest

- [`idf_component.yml`](../../spangap-core/idf_component.yml): add `esp32` to
  `targets:`.
- `CONFIG_IDF_TARGET` move-out (same item as C5).
- Heap-task-tracking `--wrap` guard is IDF-version-pinned, fine.

## Out of scope here

Camera ecosystem on classic ESP32 is **even more** limited than C5 — DVP only,
no LCD_CAM, lower max pixclock. Consumer concern (seccam), not spangap-core.
