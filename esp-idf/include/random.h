#pragma once
/* Random bytes for keys, seeds, nonces and tokens.
 *
 * esp_fill_random() is a hardware RNG only while an entropy source feeds it:
 * the RF path once Wi-Fi or Bluetooth is up, or the SAR-ADC noise source that
 * bootloader_random_enable() switches on. The bootloader switches that source
 * off before jumping to the app, so on a fresh boot — where the long-lived
 * identity keys are generated, before any radio starts — and on a node that
 * never brings a radio up, esp_fill_random is a pseudo-random stream seeded
 * from RC-oscillator jitter, which the vendor does not rate as
 * cryptographically strong.
 *
 * randomInit() is the first thing spangapInit() does: it turns the ADC
 * entropy source on, seeds an AES-256 CTR-DRBG from the hardware RNG while
 * the source is on, and turns it off again before any driver touches the ADC
 * or a radio. Every key, seed, nonce and token on the device draws from that
 * DRBG through randomBytes(). The DRBG reseeds itself from esp_fill_random as
 * it runs; that adds entropy once a radio is up and never subtracts any.
 *
 * Use esp_random()/esp_fill_random() directly only where the bytes carry no
 * security: timing jitter, protocol sequence-number seeds, the factory-reset
 * overwrite pattern. */
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Seed the DRBG inside a bootloader_random_enable() window. Called once by
 *  spangapInit() before anything else; harmless if called again. */
void randomInit(void);

/** Fill `buf` with `len` bytes from the DRBG. Safe from any task. */
void randomBytes(void* buf, size_t len);

/** One 32-bit word from the DRBG. */
uint32_t randomU32(void);

#ifdef __cplusplus
}
#endif
