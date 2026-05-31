# Web Server

HTTP/HTTPS server for static files, WebSocket config sync, and RTSP stream proxy. Include `web.h`.

## Usage

```cpp
webInit();  // creates task on core 1, opens http_port (80) + https_port (443)
```

Open `https://<s.net.hostname>.local/` in a browser (or `http://` by IP).

## Endpoints

| Path | Method | Description |
|------|--------|-------------|
| `/` | GET | Serves `index.html` from LittleFS (gzipped) |
| `/<file>` | GET | Serve any file from LittleFS (tries `<file>.gz` first) |
| `/` | WS | Config sync WebSocket (Pinia store ↔ cfg store) |
| `/rtsp` | WS | RTSP stream proxy (consumer-supplied — web exposes a generic WS upgrade hook for any task that registers a path) |

## LittleFS

Static files are served from the LittleFS partition mounted at `/fixed`. Files are gzipped at build time and served with `Content-Encoding: gzip`.

### Building and flashing

The **LittleFS** `fixed` partition image is built by the consuming app's project-level `CMakeLists.txt` (typically via `littlefs_create_partition_image`). Web assets are deployed to the consumer's `data/webroot/` via the consumer's build pipeline (e.g. `web-interface/deploy.sh`).

```bash
idf.py build                              # builds <app>.bin and fixed.bin
idf.py -p /dev/tty.usbmodem2101 flash    # flashes app + partition table + fixed
```

## URL → filesystem mappings (factory wiring)

Installed by `main.cpp` (config key prefix `s.web.map[]`). URL prefixes match the underlying mount points so a URL in the browser address bar is the same string as the device filesystem path.

| URL prefix      | Filesystem            | Auth   | `index` | `dav` | Notes |
|-----------------|-----------------------|--------|---------|-------|-------|
| `/`             | `/fixed/webroot`      | —      | SPA     | —     | The Quasar SPA. Try `<file>.gz` first, fall back to `index.html` for extensionless paths. |
| `/sdcard`       | `/sdcard`             | admin  | yes     | yes   | Recordings + log files browser; WebDAV-mountable. |
| `/state`        | `/state`              | admin  | yes     | yes   | Live config (`storage/root.json`, externals, certs) for inspection / hand-edit. |
| `/fixed`        | `/fixed`              | admin  | yes     | yes   | Read-only LittleFS — webroot, factory_state, additional_state. |
| `/.well-known`  | `/state/.well-known`  | —      | no      | —     | ACME HTTP-01 challenges. |

`index=1` mappings emit HTML directory listings; `dav=1` mappings additionally accept WebDAV verbs (PUT, DELETE, MKCOL, MOVE, COPY, PROPFIND, LOCK, UNLOCK). `auth="admin"` requires a valid session cookie for the admin realm.

## Architecture

Single task on core 1. Polls for incoming HTTP connections with `select()`. Subscribes to `net.up` ephemeral var via `storageSubscribeChanges` for network state changes — opens server sockets when network comes up, closes them on network down. Blocks on `ulTaskNotifyTake` + `itsPoll` when server socket is closed.

### Endpoint registration via ITS aux

Endpoints (RTSP, log, CLI, storage/config) register via ITS aux messages (`web_path_msg_t` for WS paths, `net_port_msg_t` for TCP ports). Tasks send these during their init to register URL prefixes and TCP listen ports. On a matching HTTP request, the web task injects the HTTP headers back (`itsServerInject`) and forwards the connection to the target task (`itsServerForward`). This centralizes port management and TLS handling in the web/net tasks — consumer tasks never see raw sockets.

## Web Utility API (`web.h`)

HTTP/WS convenience functions for tasks receiving forwarded connections. Tasks include `web.h` and call these directly (not via the web task).

- **`webGetHeader(itsHandle, buf, maxLen, timeoutMs)`** — read HTTP request headers from ITS handle (injected data). Returns header length.
- **`webHeaderField(hdr, len, "Field", out, outLen)`** — extract a header field value.
- **`webGetMethod(hdr, len, out, outLen)`** — extract HTTP method (`GET`, `POST`, etc.).
- **`webGetPath(hdr, len, out, outLen)`** — extract URL path (no leading `/`, no query string).
- **`webGetQuery(hdr, len, "key", out, outLen)`** — extract query parameter value.
- **`webSendResponse(h, status, contentType, body, bodyLen)`** — send full HTTP response.
- **`webSendStatus(h, status)`** — send status-only response (204, 404, etc.).
- **`wsUpgrade(h, hdr, hdrLen)`** — WS upgrade from pre-read headers. Extracts key, sends 101.
- **`wsUpgrade(h, must, timeoutMs)`** — convenience: reads headers internally, does upgrade. `must=false` injects headers back on non-WS.
- **`wsReadFrame(h, buf, size, &len, &binary)`** — read one WS frame (handles unmasking, ping/pong).
- **`wsSendText(h, data, len)`** / **`wsSendBinary(h, data, len)`** — send WS frames.
- **`wsSendClose(h)`** — send WS close frame.

Typical task flow for a forwarded WS connection:
```c
// In onConnect (runs on task's own context via ITS_MSG_FORWARD):
if (connectData->ws) {
    wsUpgrade(handle);  // reads HTTP, sends 101
    // From here: wsReadFrame() for input, wsSendText() for output
}
```
