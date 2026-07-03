# signaling

Pluggable **signaling-client** implementations for the WebRTC app layer. Each implements the same
`webrtc_signaling_client_if_t` interface ([`app_webrtc/include/app_webrtc_if.h`](../app_webrtc/include/app_webrtc_if.h)),
so an example picks a backend by setting `app_webrtc_config.signaling_client_if` — the rest of the
stack is unchanged.

| Component | Interface getter | Use |
|-----------|------------------|-----|
| **[kvs_signaling](kvs_signaling/)** | `kvs_signaling_client_if_get()` | AWS Kinesis Video Streams signaling (default). |
| **[apprtc_signaling](apprtc_signaling/)** | `apprtc_signaling_client_if_get()` | AppRTC browser signaling via `webrtc.espressif.com` — no AWS account. |
| **[bridge_signaling](bridge_signaling/)** | `bridge_signaling_client_if_get()` | Split-mode P4 side — relays signaling to the C6 over `webrtc_bridge`. |

Consumers reference these as `${KVS_SDK_PATH}/components/signaling/<name>`.

> **Not here:** `signaling_bridge_adapter` (at [`components/signaling_bridge_adapter`](../signaling_bridge_adapter/))
> is a *peer-connection-if* replacement for the split-mode C6, **not** a signaling client — so it
> lives at the top level rather than under this group.

To add your own signaling backend, implement `webrtc_signaling_client_if_t` — see
[docs/custom_signaling.md](../../docs/custom_signaling.md).
