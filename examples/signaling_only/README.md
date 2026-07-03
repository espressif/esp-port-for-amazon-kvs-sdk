# Signaling-Only Example — Always-On Signaling Hub (ESP32-C6)

The **signaling half** of the split-mode pair: this C6 firmware stays connected to AWS KVS and
handles all WebRTC signaling (SDP offer/answer, ICE), while its partner
[streaming_only](../streaming_only/README.md) on the ESP32-P4 deep-sleeps until a viewer
connects. Low idle power with always-on connectivity — suited to battery-powered cameras.

See [**Deployment Modes → Split Mode**](../../README.md#deployment-modes) in the main README for
how the pair splits the pipeline across the two chips (with diagram). This page covers the C6
(signaling) side; flash the P4 side from [streaming_only](../streaming_only/README.md).

## Features

- **AWS KVS signaling** — full offer/answer + ICE-candidate exchange
- **Bridge protocol** — relays signaling to/from the P4 over `webrtc_bridge`, and wakes it on demand
- **Always-on, low power** — keeps the KVS WebSocket open while the P4 sleeps
- **Auto-reconnect** and ICE-server refresh

## Hardware Requirements

| Component | Requirement | Notes |
|-----------|-------------|-------|
| **Main board** | ESP32-P4 Function EV Board (P4 + C6 onboard) | This firmware runs on the **C6** |
| **Partner** | ESP32-P4 | Runs [streaming_only](../streaming_only/README.md) |

3.3V/500mA is enough for C6 signaling-only operation. Wi-Fi with internet access required.

## Build & Flash

Flash the C6 (this example) **first**, then flash `streaming_only` on the P4.

```bash
cd examples/signaling_only
idf.py set-target esp32c6
idf.py menuconfig         # Wi-Fi (BLE provisioning) + AWS KVS settings (below)
idf.py -p <C6_PORT> flash monitor
```

Board overlays: `esp32c6`, `esp32c5`. For the libwebsockets (ws-off) signaling path, enable it
in menuconfig — see [kvs_signaling](../../components/signaling/kvs_signaling/README.md#signaling-transport-ws-on--ws-off).
The C6 has no onboard UART — flash it via an **ESP-Prog** (wiring pinout in
[**docs/building.md → Dual-chip setup**](../../docs/building.md#dual-chip-setup-esp32-p4--esp32-c6)).
Wi-Fi (BLE) provisioning is documented in [app_common](../common_components/app_common/README.md#wifi-provisioning);
`wifi-set <ssid> <password>` also works at runtime.

**AWS KVS settings** (menuconfig → Example Configuration):
```
AWS_ACCESS_KEY_ID       = "AKIA..."
AWS_SECRET_ACCESS_KEY   = "your-secret-key"
AWS_DEFAULT_REGION      = "us-east-1"
AWS_KVS_CHANNEL         = "my-camera-channel"
```

Once both are up: the C6 connects to KVS and waits for the bridge; when the P4 connects, the
system is ready; a viewer's SDP offer wakes the P4, which then streams RTP directly to the viewer
(media never round-trips through the C6).

## CLI commands

The C6 console exposes commands that proxy to the streaming P4 over the bridge:

| Command | Description |
|---------|-------------|
| `query-resolution` | Query the camera resolution from the streaming device (P4) |
| `query-snapshot [quality]` | Capture a JPEG snapshot from the P4 |
| `trigger-offer` | Send a trigger-offer message to the P4 for a given peer |
| `wake-up` | Wake the host (P4) from deep sleep |

## How it works

```c
app_webrtc_config_t app_webrtc_config = APP_WEBRTC_CONFIG_DEFAULT();
app_webrtc_config.signaling_client_if = kvs_signaling_client_if_get();    // KVS signaling on the C6
app_webrtc_config.signaling_cfg       = &g_kvsSignalingConfig;
app_webrtc_config.peer_connection_if  = bridge_peer_connection_if_get();  // bridge to the P4
```

The C6 plugs the **bridge** peer-connection interface (which intentionally has no `create_session`)
into `app_webrtc`, so instead of running a local peer connection it relays signaling to the P4 over
the bridge.

## Troubleshooting

Generic build/Wi-Fi/console issues: [building.md → Troubleshooting](../../docs/building.md#troubleshooting). Split-mode specific:

| Problem | Symptoms | Solution |
|---------|----------|----------|
| **AWS connection failed** | `KVS connection error` | Verify credentials, region, channel name |
| **No streaming device** | Bridge never connects | Confirm the P4 is running `streaming_only` and the SDIO link is up |
| **ICE refresh failed** | `ICE refresh failed` | Check network connectivity and AWS quotas |
| **Wi-Fi disconnects** | `Connection lost` | Check signal strength and power supply |

## Related

- **[streaming_only](../streaming_only/README.md)** — the partner P4 firmware (required)
- **[webrtc_classic](../webrtc_classic/README.md)** — single-device alternative
- **[ice_server_bridge.md](../../docs/ice_server_bridge.md)** — bridge architecture details
- **[API_USAGE.md](../../API_USAGE.md)** / **[docs/custom_signaling.md](../../docs/custom_signaling.md)**

## License

Apache License 2.0
