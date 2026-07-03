# ESP WebRTC Examples

Example applications demonstrating the simplified WebRTC API for ESP32 devices. Each uses the same pluggable interface with smart defaults; they differ in signaling backend, hardware, and deployment mode.

## Choose Your Example

| Example | Use Case | Hardware | AWS Required | Best For |
|---------|----------|----------|--------------|----------|
| **[webrtc_classic](webrtc_classic/README.md)** | **Start here!** Single-device streaming | ESP32/S3/P4 + camera | Yes (KVS) | Learning WebRTC, simple setups |
| **[esp_camera](esp_camera/README.md)** | Browser-compatible streaming | ESP32-CAM modules | No (AppRTC) | Testing, no AWS account |
| **[streaming_only](streaming_only/README.md)** | Power-optimized media device | ESP32-P4 (split mode) | No (bridge) | High performance, power saving |
| **[signaling_only](signaling_only/README.md)** | Always-on signaling device | ESP32-C6 (split mode) | Yes (KVS) | Power optimization, IoT |

`webrtc_classic` and `esp_camera` are **classic mode** (signaling + streaming on one chip).
`signaling_only` + `streaming_only` are the **split-mode pair** (C6 signals, P4 streams). See
[**Deployment Modes**](../README.md#deployment-modes) in the main README for how the two
differ, with diagrams.

## Simplified API

Start from `APP_WEBRTC_CONFIG_DEFAULT()` and plug in only the interfaces you need:

```c
app_webrtc_config_t config = APP_WEBRTC_CONFIG_DEFAULT();
config.signaling_client_if = kvs_signaling_client_if_get();
config.signaling_cfg       = &signaling_config;
config.peer_connection_if  = kvs_peer_connection_if_get();
config.video_capture       = media_stream_get_video_capture_if();
app_webrtc_init(&config);
app_webrtc_run();
```

Smart defaults: **MASTER** role, **H.264 + OPUS**, **trickle ICE + TURN**. The mode follows
the `signaling_client_if` / `peer_connection_if` pair you choose. Override any default with
the `app_webrtc_set_*` functions — see [**API_USAGE.md**](../API_USAGE.md) for the full
configuration reference.

## Building & flashing

Generic setup — clone, ESP-IDF, target/board overlays, console, dual-chip wiring, AWS
credentials, troubleshooting — is in [**docs/building.md**](../docs/building.md). Each example
README adds only its own specifics (board recipe, auth, bring-up).

## What each example README contains

Per-example: hardware requirements, a runnable build/flash recipe, the example's role and how
it's handled, and example-specific troubleshooting.
