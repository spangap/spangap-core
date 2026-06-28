# TLS / HTTPS

`tls.cpp/h` — mbedTLS server with EC P-256 certificate (self-signed or ACME).

- **Cert storage**: cert + key stored as PEM files on `/state/` LittleFS partition (`/state/tls_cert.pem`, `/state/tls_key.pem`). Survives reflash, erased on factory reset.
- **Cert generation**: on boot, if `https_port > 0` and no cert matching `<hostname>.local` exists, generates EC P-256 keypair + self-signed X.509 cert (~2 s on temporary 16 KB task). Cert includes SAN (DNS) + Key Usage extensions (required by browsers). ACME certs (non-self-signed) are preserved across hostname changes.
- **Cert reload**: `tlsReloadCert()` hot-swaps cert from `/state/` without reboot (used by ACME after renewal).
- **Shared config**: single `mbedtls_ssl_config` with the server cert. Network task uses `tlsAccept()` for TLS endpoints. Per-connection `tls_conn_t` allocated in PSRAM.
- **Connection wrappers**: `tlsRead()` / `tlsWrite()` / `tlsClose()` mirror socket semantics. `tlsBytesAvail()` checks the mbedTLS internal buffer.
- **All TLS on network task**: network does TLS termination for any endpoint with `tls=1` in `net_port_msg_t`. Tasks receive plain data via ITS. HTTPS (port 443) → web → static files + WS forwarding. Browser streams via `wss://<host>/rtsp`.
- **Browser log/CLI**: on the shared WebRTC `RTCPeerConnection` as `log:1` and `cli:1` DataChannels (see [webrtc-for-everything.md](webrtc-for-everything.md)). Plain TCP `log_port` / `cli_port` (disabled by default, set to non-zero via `s.net.log_port` / `s.net.cli_port`) remain for `nc` access — stream-mode, no framing.
- **Hardware AES disabled**: `CONFIG_MBEDTLS_HARDWARE_AES=n` — the ESP32-S3 GCM DMA driver has a state machine bug causing intermittent `-1` errors under sustained TLS write load ([esp-idf #12689](https://github.com/espressif/esp-idf/issues/12689)). Software AES at 240 MHz handles streaming fine (~8 ms overhead).

## CLI

- `cert` — show cert info.
- `cert self-signed` — generate if none exists.
- `cert delete` — remove cert+key.

All registered by `tlsInit()`. The `acme` command (`acme renew [days]`) is registered separately by `acmeInit()` (see [remote-access.md](remote-access.md)). Cert generation runs on a temporary 16 KB task (EC key gen needs stack).
