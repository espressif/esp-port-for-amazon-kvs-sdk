# libwebsockets

ESP-IDF packaging of the upstream [libwebsockets](https://libwebsockets.org/) library (v4.4.0),
plus a small `lws_timegm` shim. It provides the WebSocket client used by the KVS signaling path
when the **ws-off** transport is selected (`CONFIG_USE_ESP_WEBSOCKET_CLIENT=n`) instead of the
default `esp_websocket_client`.

The ws-off path is heavier (it needs a larger `webrtc_run` task stack and the lws worker
threadpool) but follows the upstream SDK's signaling implementation more closely. See
[docs/building.md → ws-on vs ws-off](../../docs/building.md#ws-on-vs-ws-off-signaling-transport).

## Contents

- `libwebsockets/` — the upstream library sources, wired into the ESP-IDF build.
- `lws_timegm.c` / `lws_timegm_shim.h` — a portable `timegm()` for parsing server GMT timestamps
  (the platform's `mktime` assumes local time, which mis-parses the server's GMT `Date` header).

## Version

Tracks upstream libwebsockets **4.4.0** (the `version:` in `idf_component.yml` mirrors it).
