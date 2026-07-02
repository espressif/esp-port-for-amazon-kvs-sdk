# KVS Signaling Component

This component provides AWS Kinesis Video Streams (KVS) signaling functionality for ESP32 platforms.

## Overview

The `kvs_signaling` component handles:
- KVS signaling client implementation
- WebSocket-based signaling communication
- HTTP API calls for KVS services
- Clock skew correction and retry logic
- Integration with AWS IoT credentials

## Architecture

This component was separated from the main `kvs_webrtc` component to provide better separation of concerns:
- **kvs_signaling**: Handles signaling protocol and communication
- **kvs_webrtc**: Focuses on WebRTC peer connections and media

## Files

### Headers
- `kvs_signaling.h` - Main signaling client interface
- `port/Signaling_esp.h` - ESP-specific signaling implementation

### Sources
- `kvs_signaling.c` - KVS signaling client wrapper implementation
- `port/Signaling_esp.c` - ESP WebSocket signaling implementation
- `port/LwsApiCalls_esp.c` - HTTP API calls with ESP adaptations

## Dependencies

- **esp_common** - ESP-IDF common functionality
- **esp_timer** - Timer functionality
- **esp_http_client** - HTTP client for API calls
- **json** - JSON parsing
- **libwebsockets** - WebSocket implementation
- **mbedtls** - TLS/crypto functionality
- **kvs_utils** - KVS utility functions
- **signaling_serializer** - Message serialization
- **credential** - AWS credential management

## Usage

Include the header in your application:
```c
#include "kvs_signaling.h"
```

Add the component to your CMakeLists.txt:
```cmake
REQUIRES "kvs_signaling"
```

## Configuration

### Signaling transport (ws-on / ws-off)

`CONFIG_USE_ESP_WEBSOCKET_CLIENT` selects the WebSocket transport (default `y`):

- **ws-on (`y`, default)** — `esp_websocket_client`; builds `port/Signaling_esp.c`
  + `port/LwsApiCalls_esp.c`, lighter on stack/memory.
- **ws-off (`n`)** — the upstream `libwebsockets` path. It needs two companion
  settings alongside the switch (set all three together):
  - `CONFIG_APP_WEBRTC_TASK_STACK_SIZE=32768` — the lws create path is stack-hungry.
  - `CONFIG_LWS_WITH_THREADPOOL=y` — the lws worker threadpool.

Flip these in `idf.py menuconfig` (*KVS Signaling Configuration* + *WebRTC run task
stack size* + the libwebsockets component options), or add them to your
`sdkconfig.defaults`. There is no separate overlay file to append.

### Other options

- `CONFIG_KVS_AUTO_APPLY_PATCHES` — Auto-apply webrtc-c platform patches at CMake configure time (default y). The hook is mirrored in `kvs_signaling/CMakeLists.txt` so builds that pull only this component (e.g. `signaling_only` on the C6) also trigger patch application. See the main repo [`README.md`](../../README.md) for opt-out.

## Examples

See the following examples for usage:
- `webrtc_classic` - Full KVS signaling with WebRTC
- `signaling_only` - Signaling without peer connections
