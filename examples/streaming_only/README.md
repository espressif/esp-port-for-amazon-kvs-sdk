# Streaming-Only Example — Split-Mode Device (ESP32-P4)

The **streaming half** of the split-mode pair: this P4 firmware does only the peer connection
(media capture, H.264 encode, RTP/SRTP), while its partner [signaling_only](../signaling_only/README.md)
on the ESP32-C6 holds the AWS KVS signaling channel. The P4 sleeps until the C6 wakes it for an
incoming viewer.

See [**Deployment Modes → Split Mode**](../../README.md#deployment-modes) in the main README for
how the pair splits the WebRTC pipeline across the two chips (with diagram). This page covers the
P4 (streaming) side; flash the C6 side from [signaling_only](../signaling_only/README.md).

## Features

- **H.264 hardware encoding** on the ESP32-P4
- **Opus audio**, with optional bidirectional audio (I2S mic + speaker)
- **Power-optimized** — the P4 deep-sleeps when no viewer is connected and wakes on demand
- **Bridge IPC** — signaling messages arrive over the `webrtc_bridge` link from the C6

## Hardware Requirements

| Component | Requirement | Notes |
|-----------|-------------|-------|
| **Main board** | ESP32-P4 Function EV Board (or P4-EYE) | Has both P4 + C6 onboard |
| **Camera** | OV2640 / OV3660 / OV5640 | Built-in on the Function EV Board |
| **Network processor** | ESP32-C6 (onboard) | Runs [signaling_only](../signaling_only/README.md) |

Optional: I2S mic/speaker for audio, SD card for local recording. Use a 5V/2A supply for full
performance.

## Build & Flash — two-device system

Split mode is **two firmwares on the same P4+C6 board**: the C6 runs `signaling_only`, the P4 runs
this example. Flash the C6 first so signaling is up before the P4 looks for it.

```bash
# 1) C6 — signaling side (handles AWS KVS). Build/flash from the signaling_only example:
cd ../signaling_only
idf.py set-target esp32c6
idf.py menuconfig         # Wi-Fi + AWS creds (ACCESS_KEY/SECRET_KEY/REGION/CHANNEL)
idf.py -p <C6_PORT> flash monitor

# 2) P4 — streaming side (this example; no AWS creds needed here):
cd ../streaming_only
idf.py -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.defaults.esp32p4;sdkconfig.defaults.p4_eye.esp32p4" \
       set-target esp32p4
idf.py menuconfig         # same Wi-Fi SSID/password as the C6
idf.py -p <P4_PORT> flash monitor
```

Board overlays here: `esp32p4`, `p4_eye.esp32p4`, `p4x_eye.esp32p4`,
`p4_function_ev_board_v12.esp32p4`, `p4_function_ev_board_v16.esp32p4`, `p4_c5_core_board.esp32p4`.
For the SDKCONFIG-overlay chain rules, console-output selection, and the ESP-Prog C6 wiring, see
[**docs/building.md**](../../docs/building.md).

Expected once both are up: the C6 connects to KVS and reports "ready"; the P4 connects the bridge
and enters power-save; when a viewer connects, the C6 wakes the P4 and streaming starts.

## How it works

```c
app_webrtc_config_t app_webrtc_config = APP_WEBRTC_CONFIG_DEFAULT();
app_webrtc_config.signaling_client_if = bridge_signaling_client_if_get(); // signaling via the C6
app_webrtc_config.signaling_cfg       = &bridge_config;
app_webrtc_config.peer_connection_if  = kvs_peer_connection_if_get();         // full WebRTC on the P4
app_webrtc_config.video_capture       = video_capture;
app_webrtc_config.audio_capture       = audio_capture;
```

The P4 plugs the **bridge** signaling client (instead of the KVS one) into the same `app_webrtc`
interface; signaling messages are serialized over the `webrtc_bridge` link to the C6. Media never
round-trips through the C6 — the P4 talks RTP/SRTP directly to the viewer.

| State | ESP32-C6 | ESP32-P4 |
|-------|----------|----------|
| **Idle** | Active (signaling) | Deep sleep |
| **Streaming** | Active (signaling) | Active (streaming) |

## Troubleshooting

Generic build/Wi-Fi/console issues: [building.md → Troubleshooting](../../docs/building.md#troubleshooting). Split-mode specific:

| Problem | Symptoms | Solution |
|---------|----------|----------|
| **Bridge not connected** | P4 can't reach C6 | Confirm `network_adapter`/C6 signaling is flashed and the SDIO link is up; reset both |
| **C6 has no AWS connection** | Bridge up but no signaling | Verify C6 AWS creds + network access |
| **P4 won't wake** | C6 signals but P4 stays asleep | Check power supply and bridge IPC config |
| **No video / poor quality** | Viewer connects, no/choppy video | Check P4 camera, reduce resolution, check Wi-Fi |

## Related

- **[signaling_only](../signaling_only/README.md)** — the partner C6 firmware
- **[webrtc_classic](../webrtc_classic/README.md)** — single-device alternative
- **[ice_server_bridge.md](../../docs/ice_server_bridge.md)** — bridge architecture details
- **[API_USAGE.md](../../API_USAGE.md)** / **[docs/custom_signaling.md](../../docs/custom_signaling.md)**

## License

Apache License 2.0
