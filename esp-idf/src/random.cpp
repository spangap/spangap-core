/* random.cpp — the device's one CSPRNG. See include/random.h for why
 * esp_fill_random alone is not enough on this chip. */
#include "random.h"
#include "log.h"

/* The ADC noise source the bootloader RNG window opens is a chip peripheral;
 * on the host esp_fill_random is the entropy source on its own. */
#if !CONFIG_IDF_TARGET_LINUX
#include "bootloader_random.h"
#endif
#include "esp_random.h"
#include "mbedtls/ctr_drbg.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <stdlib.h>

static mbedtls_ctr_drbg_context s_drbg;
static SemaphoreHandle_t        s_lock  = nullptr;
static bool                     s_ready = false;

/* Entropy callback for the DRBG. At seed time this runs inside the
 * bootloader_random_enable() window, so the hardware RNG is fed by the ADC
 * noise source; at reseed time it runs whenever the DRBG decides, fed by the
 * RF path if a radio is up and by RC-oscillator jitter otherwise. Mixing a
 * weak reseed into a strongly seeded DRBG never weakens it. */
static int entropySource(void*, unsigned char* out, size_t len) {
    esp_fill_random(out, len);
    return 0;
}

extern "C" void randomInit(void) {
    if (s_ready) return;
    static const unsigned char pers[] = "spangap-core drbg";

#if !CONFIG_IDF_TARGET_LINUX
    bootloader_random_enable();
#endif
    mbedtls_ctr_drbg_init(&s_drbg);
    int rc = mbedtls_ctr_drbg_seed(&s_drbg, entropySource, nullptr, pers, sizeof(pers) - 1);
#if !CONFIG_IDF_TARGET_LINUX
    bootloader_random_disable();
#endif

    if (rc != 0) {
        /* No DRBG means no trustworthy keys; a device in that state must not
         * mint an identity. Halt rather than silently degrade. */
        err("random: ctr_drbg seed failed (%d), halting", rc);
        abort();
    }
    s_lock  = xSemaphoreCreateMutex();
    s_ready = true;
}

extern "C" void randomBytes(void* buf, size_t len) {
    if (!s_ready) {
        /* Only reachable if a caller runs before spangapInit(); nothing in the
         * boot sequence does. Fall back rather than crash, but say so. */
        err("random: randomBytes before randomInit");
        esp_fill_random(buf, len);
        return;
    }
    unsigned char* p = (unsigned char*)buf;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    while (len) {
        size_t n = len > MBEDTLS_CTR_DRBG_MAX_REQUEST ? MBEDTLS_CTR_DRBG_MAX_REQUEST : len;
        if (mbedtls_ctr_drbg_random(&s_drbg, p, n) != 0) {
            err("random: ctr_drbg_random failed, halting");
            abort();
        }
        p   += n;
        len -= n;
    }
    xSemaphoreGive(s_lock);
}

extern "C" uint32_t randomU32(void) {
    uint32_t r;
    randomBytes(&r, sizeof(r));
    return r;
}
