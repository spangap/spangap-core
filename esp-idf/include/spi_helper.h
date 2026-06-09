/**
 * spi_helper — shared SPI bus arbitration for the platform.
 *
 * ESP32-S3 boards like LilyGo T-Deck Plus share one SPI host (FSPI /
 * SPI2_HOST) across multiple chips: TFT/EPD display, microSD, LoRa
 * modem. ESP-IDF's `spi_master` driver already arbitrates *between*
 * registered devices on a bus (per-bus mutex around each transaction);
 * what's missing at the application layer is "who owns the
 * `spi_bus_initialize` call." This helper makes that idempotent so any
 * driver can call it during its own init without worrying about who
 * else might run first.
 *
 * Typical use:
 *
 *   spi_bus_config_t bus = { .sclk_io_num = 40, .mosi_io_num = 41,
 *                            .miso_io_num = 38, .quadwp_io_num = -1,
 *                            .quadhd_io_num = -1, ... };
 *   spiHelperInitBus(SPI2_HOST, &bus);   // safe to call N times
 *   spi_device_interface_config_t dev = { ... };
 *   spi_bus_add_device(SPI2_HOST, &dev, &handle);
 *
 * The pin config in `bus_config` is honoured by the first successful
 * caller; subsequent callers' configs are ignored (the bus is already
 * up with the original pins). Per-board pin maps belong in the
 * consuming app's board.h or a future board-aware helper.
 *
 * This file does not own per-device registration — each driver still
 * calls `spi_bus_add_device` and `spi_device_*` itself.
 */
#pragma once

#include "driver/spi_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Idempotent `spi_bus_initialize`. Returns ESP_OK if the bus is
 *  already up (regardless of who initialized it) or comes up cleanly.
 *  Suppresses the IDF SPI driver's `"SPI bus already initialized"`
 *  error log so duplicate calls don't pollute output. */
esp_err_t spiHelperInitBus(spi_host_device_t host,
                           const spi_bus_config_t* bus_config);

/** Serialize access to the shared SPI bus across drivers that the spi_master
 *  bus lock can't coordinate by itself. esp_lcd's color transfer queues an
 *  async DMA and then RELEASES the driver bus lock before the DMA finishes
 *  draining the hardware; a co-resident polling driver (e.g. a LoRa modem)
 *  that grabs the bus in that window panics in spi_hal_setup_trans
 *  ("running_cmd == 0"). Every task that drives the shared bus must bracket
 *  its access with these: the LCD holds it across draw_bitmap AND the DMA
 *  completion; other drivers hold it around their transaction. One global lock
 *  — the platform assumes a single shared SPI bus (true on T-Deck). */
void spiHelperBusLock(void);
void spiHelperBusUnlock(void);

/** Install the shared GPIO ISR service exactly once for the whole app.
 *  The display's input INT lines (touch / button / trackball) and the LoRa
 *  modem's DIO path both need gpio_install_isr_service(); whichever inits
 *  second otherwise trips IDF gpio.c's ESP_LOGE("gpio", "GPIO isr service
 *  already installed") even though the ESP_ERR_INVALID_STATE return is
 *  harmless. Route every caller through here: the first installs, the rest
 *  are no-ops, and IDF's log line is suppressed across the call. All callers
 *  must pass the same ESP_INTR_FLAG_* — the first one wins. Returns ESP_OK
 *  if the service is up (regardless of who installed it). */
esp_err_t spiHelperEnsureGpioIsr(int intr_flags);

#ifdef __cplusplus
}
#endif
