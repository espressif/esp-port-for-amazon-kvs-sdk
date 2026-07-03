# app_webrtc

The application-layer component: a unified, high-level WebRTC API with a **pluggable** signaling /
peer-connection architecture. Examples build on it and choose their backends at config time — the
rest of the application stays backend-agnostic.

## Role

`app_webrtc` sits above interchangeable signaling and peer-connection implementations:

- **app_webrtc** — application API + WebRTC state machine / session management (this component)
- **signaling backend** — e.g. [kvs_signaling](../signaling/kvs_signaling/), [apprtc_signaling](../signaling/apprtc_signaling/), [bridge_signaling](../signaling/bridge_signaling/)
- **peer-connection backend** — e.g. [kvs_webrtc](../kvs_webrtc/), or the split-mode bridge peer connection

Both are selected via `app_webrtc_config_t` and consumed through the common interfaces in
`app_webrtc_if.h`, so a backend is swapped without touching application code.

> The component currently compiles a small subset of upstream KVS SDK sources it depends on
> directly; these move behind the interfaces over time.

## Files

- `include/app_webrtc.h` — application API (init / run / terminate + the config setters).
- `include/app_webrtc_if.h` — the pluggable `webrtc_signaling_client_if_t` / `webrtc_peer_connection_if_t` interfaces.
- `src/app_webrtc.c` — application logic + state machine (`app_webrtc_internal.h` for internals).

## Usage

```c
#include "app_webrtc.h"

app_webrtc_config_t config = APP_WEBRTC_CONFIG_DEFAULT();
config.signaling_client_if = kvs_signaling_client_if_get();   // or apprtc_signaling_client_if_get() / bridge_signaling_client_if_get()
config.peer_connection_if  = kvs_peer_connection_if_get();    // or bridge_peer_connection_if_get() (split-mode signaling-only)
config.video_capture       = media_stream_get_video_capture_if();

app_webrtc_init(&config);
app_webrtc_run();
/* ... */
app_webrtc_terminate();
```

`APP_WEBRTC_CONFIG_DEFAULT()` applies smart defaults (MASTER role, H.264/OPUS, trickle ICE + TURN).
Override with `app_webrtc_set_role()`, `app_webrtc_set_ice_config()`, `app_webrtc_set_codecs()`,
`app_webrtc_enable_media_reception()`, `app_webrtc_set_log_level()`. Full reference:
[API_USAGE.md](../../API_USAGE.md).

## Backend pairings (per example)

| Example | `signaling_client_if` | `peer_connection_if` |
|---------|-----------------------|----------------------|
| webrtc_classic | `kvs_signaling` | `kvs_peer_connection` |
| esp_camera | `apprtc_signaling` | `kvs_peer_connection` |
| streaming_only | `bridge_signaling` | `kvs_peer_connection` |
| signaling_only | `kvs_signaling` | `bridge_peer_connection` |

To add your own backend, implement the interface — see [docs/custom_signaling.md](../../docs/custom_signaling.md).
