# ESP Camera WebRTC Example — AppRTC Compatible

**No AWS account needed.** Streams ESP32 camera video to a browser using AppRTC-compatible
signaling via Espressif's server at `webrtc.espressif.com`. Create or join rooms from the CLI,
then open the room URL in any browser. Great for development and testing.

This is a **classic-mode** example (signaling + peer connection on one chip) — it just swaps the
KVS signaling client for the AppRTC one. See [Deployment Modes](../../README.md#deployment-modes)
for the bigger picture.

## Features

- **H.264 video + Opus audio**, bi-directional
- **Browser-compatible** AppRTC signaling — works with Chrome, Firefox, Edge, Safari
- **CLI room control** — MASTER (create rooms) or VIEWER (join rooms)
- **No AWS** — uses Espressif's free AppRTC server

## Hardware Requirements

| Board | Video | Audio | Notes |
|-------|-------|-------|-------|
| **ESP32-S3-EYE** | Built-in camera | Built-in mic | **Recommended** |
| **ESP32-CAM** | OV2640 | Add I2S mic | Affordable |
| **ESP32-P4-Function-EV-Board** | High-quality camera | Built-in mic | Best performance |
| **ESP32-P4-EYE** | OV2710 (MIPI-CSI) | Built-in mic | On-board C6 for Wi-Fi; `p4_eye.esp32p4` overlay |

Optional I2S mic/speaker for audio.

## Build & Flash

```bash
cd examples/esp_camera
idf.py set-target esp32s3        # or esp32 (ESP32-CAM), esp32p4
idf.py menuconfig                # Wi-Fi (BLE provisioning) + AppRTC settings (below)
idf.py -p <PORT> flash monitor
```

Board overlays: `esp32s3`, `esp32p4`, `esp32c6`, `p4_eye.esp32p4`, `p4x_eye.esp32p4`. For the
overlay chain, console-output selection, and the P4+C6 dual-chip wiring, see
[**docs/building.md**](../../docs/building.md). Wi-Fi (BLE) provisioning is documented in
[app_common](../common_components/app_common/README.md#wifi-provisioning) (`wifi-set <ssid> <password>` also works
at runtime).

**AppRTC settings** (menuconfig → Example Configuration):
```
APPRTC_ROLE_TYPE      = 0   # 0=MASTER (creates rooms), 1=VIEWER (joins rooms)
APPRTC_AUTO_CONNECT   = y   # auto-connect on boot, or manual CLI mode
APPRTC_USE_FIXED_ROOM = n   # use a fixed room ID or create a new one
```

## CLI commands

| Command | Description |
|---------|-------------|
| `join-room new` | Create a new room (MASTER) |
| `join-room <room_id>` | Join an existing room |
| `get-room` | Show the current room |
| `get-role` | Show role (MASTER / VIEWER) |
| `status` | Connection status + room URL |
| `disconnect` | Leave the room |
| `retry-room` | Retry a failed connection |

## View your stream

- **Auto-connect (default):** the device creates/joins a room on boot and logs the URL —
  `Room URL: https://webrtc.espressif.com/r/<room_id>`. Open it in a browser.
- **Manual:** `join-room new` → `status` to read the room URL → open it.

The browser will ask for camera/mic permissions (for the return path); allow them and the ESP32
feed appears.

## Architecture

```c
app_webrtc_config_t app_webrtc_config = APP_WEBRTC_CONFIG_DEFAULT();
app_webrtc_config.signaling_client_if = apprtc_signaling_client_if_get(); // AppRTC, not KVS
app_webrtc_config.signaling_cfg       = &apprtc_config;
app_webrtc_config.peer_connection_if  = kvs_peer_connection_if_get();
app_webrtc_config.video_capture       = video_capture;
app_webrtc_config.audio_capture       = audio_capture;
app_webrtc_set_role(WEBRTC_CHANNEL_ROLE_TYPE_MASTER);
```

Same `app_webrtc` interface as the other examples — only the signaling client differs.

## Troubleshooting

Generic build/Wi-Fi/console issues: [building.md → Troubleshooting](../../docs/building.md#troubleshooting). Example-specific:

| Problem | Symptoms | Solution |
|---------|----------|----------|
| **AppRTC connection failed** | Can't reach `webrtc.espressif.com` | Check internet access / firewall |
| **No video in browser** | Browser stuck "connecting" | Check camera connections and power |
| **No audio** | No sound in browser | Verify mic connections |

## Related

- **[webrtc_classic](../webrtc_classic/README.md)** — AWS KVS signaling version
- **[signaling_only](../signaling_only/README.md)** + **[streaming_only](../streaming_only/README.md)** — split mode
- **[API_USAGE.md](../../API_USAGE.md)** / **[docs/custom_signaling.md](../../docs/custom_signaling.md)**

## License

Apache License 2.0
