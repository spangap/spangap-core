# Remote access — UPnP / DuckDNS / ACME

Reaching a device from the public internet usually means combining three
independent, optional straddles. Each owns its own setup, keys, and CLI; this
page only explains how they fit together and points at the owners.

- **[upnp](../../upnp)** — punches NAT port mappings on the gateway via
  UPnP/IGD so inbound connections reach the device.
- **[duckdns](../../duckdns)** — keeps a stable public hostname pointed at the
  device's changing IP, and can publish DNS TXT records.
- **[acme](../../acme)** — obtains a real (Let's Encrypt-style, RFC 8555) TLS
  certificate via an HTTP-01 or DNS-01 challenge.

You can use any subset: UPnP without a hostname, a hostname without a
certificate, and so on. They do not depend on each other at compile time.

## The one cross-cutting seam

There is a single coupling that justifies a shared overview: the **DNS-01** ACME
flow needs a DNS provider to publish a TXT record, and that hand-off happens
through ephemeral `dns.*` storage vars rather than a direct call.

- `acme` SETS `dns.txtrecord` with the challenge value it must prove.
- `duckdns` SUBSCRIBES to `dns.txtrecord` and publishes the TXT record on change.
- `duckdns` advertises `dns.txtrecord.capable` when it is configured to serve TXT.
- `acme` reads `dns.txtrecord.capable` to auto-select its method: DNS-01 when a
  provider can serve TXT, otherwise HTTP-01. An explicit `s.acme.method` overrides
  the auto choice.

These ephemeral `dns.*` vars are the integration seam. The storage doc owns the
storage mechanism; each straddle owns its own keys, CLI, and config — see each
straddle's own README for those.

## Where TLS lives

These straddles get a certificate and an address to it. TLS **termination**
itself — serving HTTPS with the obtained cert — belongs to
[spangap-net](../../spangap-net). See its README for the listener and cert
plumbing.
