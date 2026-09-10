# random — the device's one CSPRNG

```c
#include "random.h"          // or spangap.h

void     randomBytes(void* buf, size_t len);   // keys, seeds, nonces, tokens
uint32_t randomU32(void);
```

Every key, seed, nonce and session token on the device comes from
`randomBytes()`. It is an AES-256 CTR-DRBG (mbedTLS) that `spangapInit()`
seeds as its very first act, and it is safe to call from any task.

## Why not `esp_fill_random()`

The ESP32-S3 hardware RNG is only a true random source while something feeds
it entropy: the RF path once Wi-Fi or Bluetooth is running, or the SAR-ADC
noise source that `bootloader_random_enable()` switches on. The bootloader
switches that source **off** before it jumps to the app. So at the two moments
that matter most, `esp_fill_random()` is a pseudo-random stream seeded from
RC-oscillator jitter, which the vendor does not rate as cryptographically
strong:

- **first boot**, when the Reticulum identity and the SSH host seed are
  generated in `onInit`, before any radio has started;
- **every boot of a radio-off node** (`s.net.wifi.enable=0`, no Bluetooth),
  which never gets an RF entropy source at all, so every link key, ratchet and
  token IV it ever makes would come from the weak stream.

## What `randomInit()` does

1. `bootloader_random_enable()` — turn the ADC noise source on. This must
   happen before any driver initialises the ADC or a radio, which is why
   `randomInit()` is the first line of `spangapInit()`: every board's battery
   ADC and every radio come up in the `onInit` walk, later.
2. Seed the DRBG from `esp_fill_random()` while the source is on.
3. `bootloader_random_disable()` — turn it off again and restore the ADC.

From then on the DRBG reseeds itself from `esp_fill_random()` on mbedTLS's
schedule. Once a radio is up that adds real entropy; on a radio-off node it
adds RC jitter. Mixing a weak reseed into a strongly seeded DRBG never weakens
it, so the boot-time seed is what the security rests on.

A seed failure halts the device: a node that cannot make trustworthy keys must
not mint an identity.

## Rule

`randomBytes()` for anything a secret depends on. `esp_random()` and
`esp_fill_random()` directly only where the bytes carry no security: timing
jitter, protocol sequence-number and tag seeds, the factory-reset overwrite
pattern. Straddles that vendor a primitive with its own RNG hook (ed25519-donna,
mlkem-native, mbedTLS ECP) point that hook at `randomBytes()`.
