# OTA — over-the-air updates

Signed manifest, paired firmware + files update, manual rollback. ECDSA P-256 signatures, no flash encryption, no Secure Boot lockdown — verification is purely application-level for now (see [Future hardening](#future-hardening)).

The **mechanics** below (manifest format, update flow, rollback, storage keys, partition shape spangap-core expects) live in spangap-core. The **distribution side** (signing keys, build scripts, manifest URL, About-panel UI) lives in the consuming app.

## Partition layout (consumer-defined; spangap-core expects this shape)

```
nvs       data, nvs,     0x9000,   0x5000   (20 KB)
otadata   data, ota,     0xe000,   0x2000   (8 KB)
app0      app,  ota_0,   0x10000,  0x240000 (2.25 MB)
app1      app,  ota_1,   0x250000, 0x240000 (2.25 MB)
fixed_a   data, spiffs,  0x490000, 0xC0000  (768 KB, readonly)
fixed_b   data, spiffs,  0x550000, 0xC0000  (768 KB, readonly)
state     data, spiffs,  0x610000, 0x40000  (256 KB)
```

App slot N pairs with fixed slot N: app0↔fixed_a, app1↔fixed_b. The bootloader's `otadata` flips both atomically by association — `fs_init()` reads the running app slot (`esp_ota_get_running_partition`) and mounts the matching fixed partition at `/fixed`. Rollback follows the otadata pair, so a firmware version always sees the matching factory state / web UI it shipped with.

Initial flash populates only `fixed_a` (paired with the default `app0`). `fixed_b` stays empty until the first OTA — `version rollback` before any OTA refuses, since the b-side isn't a valid image yet.

## Signing — consumer responsibility

ECDSA P-256, SHA-256, DER-encoded signature. The consumer owns its key material:

- A private key (e.g. `keys/ota_priv.pem`) — must be gitignored.
- A public key (e.g. `keys/ota_pub.pem`) — committed.
- A generated header that exposes `OTA_PUBKEY_PEM` + `OTA_PUBKEY_PEM_LEN` constants in the consumer's `main/`. The consumer's `app_main()` passes those to `otaInit(pubkey_pem, len)`.

**spangap-core never sees the key material.** The consumer's build pipeline is responsible for keeping a private key out of git, generating a fresh key on first build if missing, regenerating the header before component SRCS scan, and emitting a CMake warning when the dev key was just generated.

The signature covers the manifest bytes as-written. Each binary's SHA-256 is inside the manifest, so the signature transitively binds firmware + files. There is no signature on the binaries themselves — verifying the hash after download is sufficient.

## Manifest

Lives at `s.sys.ota.url` (default empty in core; consumer's `app_main` typically defaults it to its own URL). Companion DER signature at `<manifest_url>.sig`.

```json
{
  "version": "20260504_1800",
  "build_time": 1777910400,
  "build_str": "2026-05-04 18:00 UTC",
  "firmware": {
    "url": "https://example.com/<app>_20260504_1800_firmware.bin",
    "size": 1653504,
    "sha256": "..."
  },
  "files": {
    "url": "https://example.com/<app>_20260504_1800_files.bin",
    "size": 786432,
    "sha256": "..."
  }
}
```

`version` is a UTC `YYYYMMDD_HHMM` string. `build_time` is the same instant in epoch seconds, used for the up-to-date / behind / ahead comparison against the running firmware's `app_build_unix`.

## Update flow

`version upgrade [build_time]` (CLI) or a consumer-side button:

1. Spawn `ota` task on a DRAM stack (16 KB) — partition writes disable the PSRAM cache.
2. Fetch + verify manifest. Aborts on bad signature.
3. Erase the inactive `files` partition, stream-download via `esp_http_client` straight into it (`esp_partition_write` per chunk), verify SHA-256 against the manifest.
4. `esp_ota_begin` → stream-download firmware into the inactive app slot, verify SHA-256 against the manifest.
5. `esp_ota_set_boot_partition(inactive_app)` flips `otadata`. Record `s.sys.ota.committed_at = now`, clear `s.sys.ota.first_boot_at`, force-flush settings, reboot.

Files are written before firmware so a failure during the (longer, flakier) download path leaves the firmware boot pointer untouched. If the firmware download fails after files succeeded, the inactive files slot is now stale but unused — fixed_b only gets mounted when app1 boots. The next upgrade attempt overwrites it.

On boot, [ota.cpp](../spangap-core/src/ota.cpp)'s `otaInit()` stamps `s.sys.ota.first_boot_at` if the running slot's `committed_at` is newer than its `first_boot_at` (i.e. this is the first successful boot since the slot was committed) and the wall clock is sane (`time(NULL) > 1700000000`).

## Rollback

`version rollback` (CLI) — refuses if the inactive slot has no valid `esp_app_desc`. Otherwise calls `esp_ota_set_boot_partition(other)`, records new `committed_at`, reboots. The fixed-slot pairing follows the running app slot at boot, so rollback automatically gets the matching factory state.

Bootloader auto-rollback (`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`) is **not** enabled. "Rollback is possible" simply means "the other slot still holds a valid image." Starting an upgrade overwrites that slot, ending the rollback window — that's the intentional design: rollback stays available until the user commits to the next version.

## CLI

```
version                  fetch manifest, print running + latest, report drift
version upgrade [TS]     start upgrade. Optional pin to a specific version
                         (must equal the manifest's "version" string).
version rollback         reboot into the previous slot if it's valid
```

A consumer that wants automatic update checks at network-up can put `version` in their `/state/net_up` script — manifest fetches will route through `info()` so the browser log window, serial console, and log file all see the result.

## Browser-side: About panel (consumer-supplied)

Consumer apps typically expose an About panel with the running build / committed / first-boot timestamps, latest manifest result, manifest URL (editable), and **Check for update** / **Install update** buttons. The buttons set `ota.check` / `ota.upgrade` ephemerals; the device picks them up via storage subscription and resets the flag when done. While `ota.busy=1`, both buttons should be disabled. Progress messages surface in the log window.


## Storage keys

| Key | Persisted | Description |
|---|---|---|
| `s.sys.ota.url` | yes | Manifest URL (consumer-defined default) |
| `s.sys.ota.committed_at` | yes | Epoch when `set_boot_partition` was called for this slot |
| `s.sys.ota.first_boot_at` | yes | Epoch of first successful boot of this slot |
| `s.sys.ota.version` | yes | Module schema version (bumps install new defaults) |
| `ota.busy` | no | 1 while a download is in flight |
| `ota.update_available` | no | 1 if last manifest fetch found a newer build |
| `ota.latest_build_time` | no | Epoch of the last fetched manifest |
| `ota.check`/`ota.upgrade`/`ota.rollback` | no | Browser → device action triggers |

## Distribution side (consumer-owned)

Each consumer ships its own release script that:

1. Refuses to run with uncommitted/untracked changes (so git always has an archive of every binary served via OTA).
2. Tags the build (`git tag $TS`).
3. Builds with the build-time tag passed in.
4. Signs the manifest with the consumer's private key (the sig covers what spangap-core's `verifyManifestSig` will check on the device).
5. Uploads firmware + files binaries + manifest + .sig to the consumer's distribution server.
6. Pushes the tag.

(The release script and signing tool are app-side; nothing about them is platform code.)

## Future hardening

Currently we have **signature checking, no lockdown** — anyone with physical access can flash whatever they like over USB. The pieces are in place to ratchet up later without protocol changes:

- **Flash encryption** — eFuse-bound XTS-AES key, content can't be read off the chip. Doesn't protect OTA payloads in flight.
- **Secure Boot V2** — eFuse-bound public key digest, bootloader refuses unsigned firmware. Permanent burn — one-way door.
- **Encrypted OTA** — `esp_encrypted_img` component wraps the firmware payload with an RSA-wrapped AES key so the OTA bytes are also ciphertext. Useful only after flash encryption (otherwise the decryption key is just sitting in plaintext flash).

For now the wire confidentiality is whatever HTTPS gives us, and the OTA chain of trust is the embedded ECDSA public key. Lose the consumer's private key → can't push updates anymore. Leak it → attacker can serve their own signed firmware to any device that resolves `s.sys.ota.url` to their server.
