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

#include "driver/gpio.h"
#include "esp_log.h"
#include "hal/gpio_ll.h"
#include "soc/gpio_struct.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

/* Shared-bus serialization lock (see header). Created at static-init so it is
 * always valid before any task transacts — no first-caller race. */
static StaticSemaphore_t s_busMtxBuf;
static SemaphoreHandle_t s_busMtx = xSemaphoreCreateMutexStatic(&s_busMtxBuf);

void spiHelperBusLock(void)   { xSemaphoreTake(s_busMtx, portMAX_DELAY); }
void spiHelperBusUnlock(void) { xSemaphoreGive(s_busMtx); }

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

esp_err_t spiHelperEnsureGpioIsr(int intr_flags) {
    /* One-shot for the whole app. Boot init paths run sequentially, so a plain
     * flag is enough; the INVALID_STATE handling below keeps even a racing
     * second caller correct (and quiet). */
    static bool s_installed = false;
    if (s_installed) return ESP_OK;

    /* Disarm every pin interrupt a previous life of the firmware left enabled.
     * Per-pin interrupt config lives in the GPIO peripheral, which esp_restart
     * does not reset (its reset list covers radio, timers, SPI, UART, DMA,
     * crypto — not GPIO), so a pin armed before a soft reset is still armed
     * now — while the handler table this service is about to create starts
     * empty. The install below unmasks the GPIO interrupt on its CPU, and a
     * stale level-typed pin whose source still asserts then storms it: with no
     * handler registered there is nothing to quiet or disable the pin, the ISR
     * clears its status, the level re-latches, and it never exits — interrupt
     * watchdog, panic, and (the panic reboot being another soft reset) the same
     * storm again next boot, forever, until a power cycle clears the pin.
     * Handlers can only be added after the service exists, so at this point
     * every enabled pin is stale by definition — sweep them all. */
    for (int pin = 0; pin < SOC_GPIO_PIN_COUNT; pin++) {
        if (!GPIO_IS_VALID_GPIO((gpio_num_t)pin)) continue;
        if (GPIO.pin[pin].int_ena == 0) continue;
        gpio_intr_disable((gpio_num_t)pin);
        gpio_set_intr_type((gpio_num_t)pin, GPIO_INTR_DISABLE);
    }
    gpio_ll_clear_intr_status(&GPIO, ~0u);
    gpio_ll_clear_intr_status_high(&GPIO, ~0u);

    /* Silence IDF gpio.c's ESP_LOGE("gpio", "GPIO isr service already
     * installed") — a duplicate install is expected here, not an error. */
    esp_log_level_t prev = esp_log_level_get("gpio");
    esp_log_level_set("gpio", ESP_LOG_NONE);
    esp_err_t r = gpio_install_isr_service(intr_flags);
    esp_log_level_set("gpio", prev);

    if (r == ESP_OK || r == ESP_ERR_INVALID_STATE /* someone beat us */) {
        s_installed = true;
        return ESP_OK;
    }
    err("spiHelperEnsureGpioIsr: %s", esp_err_to_name(r));
    return r;
}
