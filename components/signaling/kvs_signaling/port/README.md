# kvs_signaling — ESP-IDF port files

This directory holds the ESP-IDF replacements for the upstream KVS WebRTC SDK's
signaling and WebSocket-API layers. They are compiled in when
`CONFIG_USE_ESP_WEBSOCKET_CLIENT=y` (default); when disabled, the build falls
back to the upstream POSIX-flavored sources under
`amazon-kinesis-video-streams-webrtc-sdk-c/src/source/Signaling/`.

| Port file | Replaces upstream | Why |
|---|---|---|
| `Signaling_esp.c` / `Signaling_esp.h` | `src/source/Signaling/Signaling.c` | esp_work_queue-based signaling worker (the upstream version assumes a POSIX-thread environment). |
| `LwsApiCalls_esp.c` | `src/source/Signaling/LwsApiCalls.c` | libwebsockets glue that uses ESP-IDF's libwebsockets fork (split heap allocations, SPIRAM-friendly buffers, dynamic payload mode under `PREFER_DYNAMIC_ALLOCS`). |
| `DataBuffer.c` / `DataBuffer.h` | (no upstream equivalent — helper) | Reassembly buffer for fragmented signaling payloads. Internal to the port. |

## Maintenance contract

When the corresponding upstream file evolves on awslabs/develop, this port file
needs a manual sync — the build does NOT pull upstream changes into the port
automatically. To check for drift:

```bash
# Diff the active part of LwsApiCalls.c against our port (helps spot upstream
# changes that we should mirror)
diff -u amazon-kinesis-video-streams-webrtc-sdk-c/src/source/Signaling/LwsApiCalls.c \
        components/kvs_signaling/port/LwsApiCalls_esp.c
```

## Gating Kconfig

`CONFIG_USE_ESP_WEBSOCKET_CLIENT` (in `Kconfig.projbuild`) — when on, this port
is compiled in and the upstream files are excluded. Default is on for all
ESP-targeted builds.
