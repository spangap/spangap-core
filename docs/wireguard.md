# WireGuard VPN

ESP-IDF WireGuard integration based on `trombik/esp_wireguard` (wireguard-lwip core).

## Vendored fork

`spangap-core/src/esp_wireguard/` — vendored from trombik/esp_wireguard v0.9.0. Includes PR #45 crash fixes (NULL check in `wireguardif_output`, IP clear before teardown, `wireguardif_fini` after `netif_remove`).

The fork exists only because of the `netif->state` workaround (see Key Fixes below). When trombik's upstream propagates the fix to a registry-published version, drop the vendored sources and depend on `trombik/esp_wireguard` directly via `idf_component.yml`.

**Modifications from upstream:**
- Internal headers (`wireguard.h`, `crypto.h`, `wireguard-platform.h`) are exposed via `PRIV_INCLUDE_DIRS "src/esp_wireguard"` so `wg.cpp` can access crypto/platform APIs for key generation and public-key derivation.

### Crypto

Per-packet ChaCha20-Poly1305 runs inline on lwIP's `tcpip_thread` (~20% CPU at 14fps HTTPS streaming). tiT has no competing workload, so offloading to a crypto task buys nothing. Handshake crypto (Curve25519, every ~2 min) also runs in the tcpip_thread.

## Lifecycle

[`wg.cpp`](../spangap-core/src/wg.cpp) — not a task. Registers `NET_EV_UPSTREAM_UP`/`UPSTREAM_DOWN`/`CFG_CHANGED`/`POLL` callbacks with net (UPSTREAM, not UP — WG needs internet to reach the peer, no point starting in AP-only mode). Runs on net's task context.

- Auto-starts tunnel on STA upstream-up if `s.wg.enable=1`
- PM lock: `PM_NO_LIGHT_SLEEP` "wg" held while tunnel active
- Polls `esp_wireguardif_peer_is_up()` every 5s via NET_EV_POLL, logs connect/disconnect transitions, updates `wg.up` config for UI

## Config

Config keys (defaults installed by `wgInit` in [wg.cpp](../spangap-core/src/wg.cpp) via `storageDefaultTree`):

| Key | Default | Description |
|-----|---------|-------------|
| `s.wg.enable` | 0 | Enable WireGuard (auto-start on STA upstream up) |
| `secrets.wg.key` | (empty) | Base64 Curve25519 private key (never sent to browser) |
| `s.wg.address` | (empty) | Tunnel IP address (e.g. `10.0.0.2`) |
| `s.wg.netmask` | `255.255.255.0` | Tunnel netmask |
| `s.wg.endpoint` | (empty) | Server hostname or IP, optional `:port` (default 51820) |
| `s.wg.peer_pubkey` | (empty) | Server's base64 public key |
| `s.wg.keepalive` | 25 | Persistent keepalive interval (seconds, 0=off) |

Non-persisted: `wg.up` (live status, 0/1, `storageSet` ephemeral var for UI).

## CLI

```
wg                  show tunnel status + local public key
wg up               enable tunnel (set s.wg.enable=1)
wg down             disable tunnel (set s.wg.enable=0)
wg keygen           generate private key, save to storage, print public key
```

## Setup

```bash
# On ESP32:
wg keygen                                    # prints public key
save s.wg.address=10.0.0.2
save s.wg.endpoint=my.server.com:51820
save s.wg.peer_pubkey=<server's public key>
save s.wg.enable=1

# On WireGuard server, add peer:
[Peer]
PublicKey = <public key from wg keygen>
AllowedIPs = 10.0.0.2/32
```

## Streaming Performance

**Browser (WebSocket/TCP) through WireGuard**: throughput limited by `TCP_SND_BUF / RTT`. With 16KB window and 100ms RTT to server: ~1.3 Mbps max. Fine for config/CLI, slow for video.

**Native client with UDP through WireGuard**: UDP-based protocols (e.g. an app-level RTSP/RTP server in the consumer) get full link bandwidth — UDP packets pass through the tunnel without TCP flow control overhead. This is the recommended remote streaming path for any high-bitrate video/audio path.

The web task passes the client's IP address through the ITS connect payload (`web_connect_t.clientAddr`) so a consumer's RTSP-style server knows where to send UDP packets. Uses lwIP `ip_addr_t` (IPv4/v6 ready).

## sdkconfig requirements (consumer-side)

```
CONFIG_LWIP_PPP_SUPPORT=y                # workaround: ESP-IDF 5.x netif->state conflict
CONFIG_LWIP_TCPIP_CORE_LOCKING=y         # allows wg_crypt to call lwIP with LOCK_TCPIP_CORE
CONFIG_LWIP_TCPIP_CORE_LOCKING_INPUT=y   # same for input path
```

## Key Fixes

- **4KB stack overflow**: Curve25519 scalar multiply needs ~1KB stack. Original 4KB task stack silently corrupted handshake state, producing invalid-looking packets that the server received but couldn't validate. Bumped to 8KB.
- **`netif->state` conflict**: ESP-IDF 5.x stores `esp_netif` pointer in `netif->state`, but wireguard-lwip stores `wireguard_device` there. Workaround: `CONFIG_LWIP_PPP_SUPPORT=y` makes esp-netif use `netif_get_client_data()` instead.
- **Crypto on tcpip_thread**: per-packet ChaCha20-Poly1305 runs inline on lwIP's tcpip_thread (~20% CPU at 14fps). Offloading to a separate crypto task buys nothing since tiT has no competing workload.
- **UDP RTP client address**: RTSP task used `getpeername(-1)` for UDP destination (always 0.0.0.0) because connections go through ITS proxy. Fixed by adding `clientAddr` to `web_connect_t`.
- **`listen_port = 0`**: using 51820 as local bind port caused issues. Set to 0 (ephemeral) — the ESP32 is a client, not a server.
