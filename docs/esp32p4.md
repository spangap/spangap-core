# The ESP32-P4

```
board straddle: target: esp32p4      → IDF_TARGET for the whole build
 ↓
sdkconfig.defaults.spangap.esp32p4   layered by IDF right after sdkconfig.defaults.spangap
 ↓
before app_main   esp_hosted's constructor resets the co-processor and opens SDIO
 ↓
esp_wifi_*        esp_wifi_remote forwards each call to the co-processor
NimBLE            host only; HCI rides the same ESP-Hosted link (VHCI)
 ↓
pmInit            DFS between the boot clock and the crystal, no light sleep
console           UART driver installed when the board's console is a UART
```

The P4 has no radio. Its Wi-Fi and Bluetooth belong to a co-processor on the
same board — an ESP32-C6 on the boards here — running Espressif's ESP-Hosted
slave firmware, reached over SDIO. Everything above the radio is unchanged.

## What the platform does differently

- **Wi-Fi.** IDF gives the P4 the `esp_wifi` headers and no implementation.
  spangap-core depends on `espressif/esp_wifi_remote` and `espressif/esp_hosted`
  for this target only (`idf_component.yml`), in the versions that pair with the
  co-processor's firmware. Every `esp_wifi_*` call the platform makes is one
  esp_wifi_remote forwards. ESP-NOW is not, so `iface-espnow` is S3-only.
- **Wi-Fi settings.** The co-processor's copies of the Wi-Fi Kconfig are named
  `WIFI_RMT_*`; `sdkconfig.defaults.spangap.esp32p4` restates the platform's
  Wi-Fi policy under those names.
- **Bluetooth.** NimBLE runs as a host with no local controller
  (`CONFIG_BT_NIMBLE_TRANSPORT_UART=n`, `CONFIG_ESP_HOSTED_ENABLE_BT_NIMBLE=y`).
  spangap-ble's controller-only calls — transmit power, the internal-RAM earmark
  — compile only where `CONFIG_BT_CONTROLLER_ENABLED`.
- **Power.** `pmInit` scales between `CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ` and the
  crystal, and automatic light sleep is off: the co-processor signals traffic on
  a line the P4 cannot wake on.
- **PSRAM.** In-package, hex mode, 200 MHz from the platform; a board on
  revision-3 silicon may raise it to 250 MHz.
- **Console.** A board whose USB port is a USB-to-UART bridge sets
  `CONFIG_ESP_CONSOLE_UART_DEFAULT`; the serial task then installs the UART
  driver so a framed request is not lost in the 128-byte hardware FIFO.
- **SD card.** The SDMMC controller has two slots and the co-processor has one
  of them, so `fs_mount_sd` keeps the host's per-slot teardown flag, takes its
  slot from `CONFIG_SPANGAP_SDCARD_SDMMC_SLOT`, and powers the card's pins from
  `CONFIG_SPANGAP_SDCARD_LDO_CHAN` when the board says they need it.

## Pitfalls

- **The co-processor firmware is not ours.** esp_hosted's host and slave halves
  exchange RPCs whose layout moves between releases; the host versions pinned in
  `idf_component.yml` are the ones that match the firmware the board ships with.
  Changing them means reflashing the co-processor.
- **Silicon revision 3 is a different chip to IDF.** An image supports revisions
  below 3.0 (`CONFIG_ESP32P4_SELECTS_REV_LESS_V3` + `CONFIG_ESP32P4_REV_MIN_100`)
  or from 3.0 up (`CONFIG_ESP32P4_REV_MIN_300`), never both, so the board states
  which it carries. On the wrong family the second-stage bootloader dies on its
  first instruction — `Illegal instruction` at the `entry` address the ROM just
  printed — and the chip loops there. The ROM banner names the family:
  `esp32p4-eco2` is pre-v3. A board sold on both needs one straddle per family.
