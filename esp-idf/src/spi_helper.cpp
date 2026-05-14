/**
 * spi_helper — see header.
 *
 * Single function for now. Grows as platform-shared SPI conventions
 * accumulate (e.g. a per-bus reference count + teardown if we ever need
 * one, or a board-specific "park other CS pins" registry — but only if
 * a real second consumer needs it).
 */
#include "spi_helper.h"
#include "log.h"

#include "esp_log.h"

esp_err_t spiHelperInitBus(spi_host_device_t host,
                           const spi_bus_config_t* bus_config) {
    /* Silence ESP-IDF's own ESP_LOGE("spi", "SPI bus already initialized")
     * across this call — duplicate-init is expected here, not an error. */
    esp_log_level_t prev = esp_log_level_get("spi");
    esp_log_level_set("spi", ESP_LOG_NONE);
    esp_err_t r = spi_bus_initialize(host, bus_config, SPI_DMA_CH_AUTO);
    esp_log_level_set("spi", prev);
    if (r == ESP_OK || r == ESP_ERR_INVALID_STATE /* already initialized */) {
        return ESP_OK;
    }
    err("spiHelperInitBus(host=%d): %s", (int)host, esp_err_to_name(r));
    return r;
}
