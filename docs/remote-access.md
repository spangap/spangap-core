# Remote access — UPnP / DuckDNS / ACME

Remote-access stack: UPnP punches NAT holes, DuckDNS provides a stable hostname, ACME gets a real TLS certificate. All three are optional and independent but work together.

## UPnP (`upnp.cpp/h`)

- **SSDP discovery**: raw UDP multicast M-SEARCH to 239.255.255.250:1900. Sends twice (routers may miss the first). Case-insensitive header parsing. Fetches root device XML, finds WANIPConnection (or WANPPPConnection) service controlURL.
- **Port forwarding**: SOAP `AddPortMapping` for HTTPS (TCP, `https_port`) and DataChannel (UDP, `webrtc_port`). `upnp_ext_port` overrides the external HTTPS port (0 = same as internal). Lease 3600 s, falls back to 0 if the router rejects.
- **External IP**: SOAP `GetExternalIPAddress` — cached, value visible in `upnp` CLI status.
- **Renewal**: cron-driven via `*/15 * * * * N upnp update` (installed on first boot). The CLI command spawns the same `upnpSyncTask` (6 KB temp task) used at start-up. No internal `esp_timer` — single source of truth for periodicity. DuckDNS updates are NOT piggy-backed here; they have their own `*/15 * * * * N duckdns update` entry.
- **Lifecycle**: `upnpInit()` registers `NET_EV_UPSTREAM_UP` / `NET_EV_UPSTREAM_DOWN` callbacks + CLI (UPnP only matters when there's a gateway router to talk to). `upnpStart()` on STA upstream-up does the initial discover + map. `upnpStop()` on STA upstream-down deletes all mappings.
- **Config**: `s.upnp.enable` (0=off), `s.upnp.ext_port` (0=same).

## DuckDNS (`duckdns.cpp/h`)

- **DNS update**: HTTPS GET to `www.duckdns.org/update` with domain + token + optional IP. Uses `esp_http_client` with cert bundle.
- **TXT records**: subscribes to `dns.txtrecord` ephemeral config var. On change, spawns temp task to set/clear TXT via DuckDNS API. Sets `dns.txtrecord.capable=1` at init if configured.
- **Periodic**: `duckdns update` CLI command, run from cron (`*/15 * * * * N duckdns update`). UPnP no longer triggers DuckDNS updates — single source of periodicity.
- **Config**: `s.duckdns.domain` (subdomain only), `s.duckdns.token`.

## ACME (`acme.cpp/h`)

- **Protocol**: ACMEv2 (RFC 8555) with DNS-01 or HTTP-01 challenge.
- **Method selection**: `s.acme.method` — `"DNS-01"`, `"HTTP-01"`, or `""` (auto: DNS-01 if `dns.txtrecord.capable`, else HTTP-01).
- **DNS-01**: sets `dns.txtrecord` ephemeral var (subscribers like DuckDNS publish it), then resolves `_acme-challenge.<fqdn>` TXT via UDP query to 8.8.8.8 to verify propagation before responding to CA.
- **HTTP-01**: writes challenge file to `s.acme.webdir` (default `/state/.well-known/acme-challenge`), served by web task via the `/.well-known` mapping.
- **Crypto**: ES256 JWS signatures (EC P-256 + SHA-256). DER-to-raw signature conversion for JWS. Base64url encoding via mbedTLS.
- **Account**: EC P-256 account key stored at `/state/acme_key.pem`. Account URL in `s.acme.url`. Created automatically on first run.
- **Cert storage**: PEM cert chain at `/state/tls_cert.pem`, domain key at `/state/tls_key.pem`.
- **Renewal**: `acme renew [days]` CLI command (bare `acme` shows status). Default crontab: `0 3 * * 0 N acme renew 30`. Also called by `acmeCheck()` on boot after `waitForTime`.
- **Task**: 16 KB temp task on core 0. ~30–60 s for full flow.
- **Config**: `s.acme.enable` (0=off), `s.net.dns.fqdn` (full domain), `s.acme.method`, `s.acme.webdir`, `s.acme.url` (account URL).
