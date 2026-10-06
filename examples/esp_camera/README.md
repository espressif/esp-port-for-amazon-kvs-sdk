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
- **CLI room control** — create a room for a browser to join, or join a room a browser created
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
APPRTC_USE_FIXED_ROOM = n   # use APPRTC_ROOM_ID instead of a new random room
APPRTC_ROLE_TYPE      = 1   # shown by get-role and in logs; no effect with AppRTC (see below)
APPRTC_AUTO_CONNECT   = y   # only changes the boot log: the device always connects on boot
```

With AppRTC the offer direction follows the room, not the role: **the device sends the offer when
it is first in the room** (it created the room, or reached the fixed room first) and **answers
when it joins a room a browser is already in**.

## Build & run on the Linux host

esp_camera also builds for the IDF **linux** target: the AppRTC client and the WebRTC stack run
on the host, the upstream SDK's sample H.264 + Opus frames stand in for the camera and mic, and
there is no Wi-Fi or NVS. The console runs on the terminal with the same commands as on a board
(below), plus `exit`. Useful to try the browser flow without a board.

```bash
cd examples/esp_camera
export KVS_SDK_PATH=$(git rev-parse --show-toplevel)
idf.py --preview -D SDKCONFIG_DEFAULTS=sdkconfig.defaults.linux set-target linux
idf.py build

export KVS_FRAMES_DIR=$KVS_SDK_PATH/amazon-kinesis-video-streams-webrtc-sdk-c/samples
export APPRTC_ROOM_ID=myroom         # optional: join this room; unset creates a new one
export TEST_DURATION_SEC=0           # 0 = until `exit` or Ctrl+C
./build/esp_camera.elf
```

The log prints the room (`https://webrtc.espressif.com/r/<room>`); open it in a browser, or run
`join-room <id>` to move the running device to a room the browser created. Stop with `exit` or
Ctrl+C (a second Ctrl+C quits at once). ESP-IDF 5.5 to 6.1 need the host `esp_timer` shim in
`esp_webrtc_utils` (included); 6.2 implements it natively.

## CLI commands

| Command | Description |
|---------|-------------|
| `join-room new` | Leave the current room and create a new one; a browser then joins its URL. With `APPRTC_USE_FIXED_ROOM=y` this rejoins the fixed room instead |
| `join-room <room_id>` | Leave the current room and join `<room_id>`, typically one a browser created |
| `get-room` | Show the current room |
| `get-role` | Show the configured role (`APPRTC_ROLE_TYPE`; no effect with AppRTC) |
| `status` | Connection status + room URL |
| `disconnect` | Leave the room |
| `retry-room [new\|<room_id>]` | Reconnect after a failure: to the current room, a new one, or `<room_id>` |

## How a session runs

1. **Boot.** The device creates a room (or joins the Kconfig `CONFIG_APPRTC_ROOM_ID` when
   `APPRTC_USE_FIXED_ROOM=y`; on the host the `APPRTC_ROOM_ID` environment variable instead joins
   an existing room) and logs its URL, then waits for a browser to open it. If the server cannot be reached, the
   boot connect is retried with a growing delay.
2. **Browser opens the device's room.** The device is first in the room, so it posts its offer
   when it registers; the server keeps it and hands it to the browser on joining. The browser
   answers and the stream starts. An offer nobody answers is kept for up to 10 minutes, then the
   session is freed.
3. **Device joins a browser's room.** Create a room in the browser first, then run
   `join-room <room_id>` on the device: the browser's offer is waiting in the room, so the device
   answers it. The device process keeps running across `join-room` calls; there is no need to
   restart it.
4. **A failed `join-room` is not retried.** If it cannot reach `webrtc.espressif.com` (for example
   a network timeout), the console logs `Failed to join/create room` and the device stays out of a
   room. Run `retry-room` (same room) or `retry-room new`; wait for one command to finish (its URL
   or error is logged) before issuing the next. Once in a room, a dropped websocket is reconnected
   automatically (up to 5 attempts); if that fails too, use `retry-room`.

## View your stream

- **Auto-connect (default):** the device creates/joins a room on boot and logs the URL —
  `Room URL: https://webrtc.espressif.com/r/<room_id>`. Open it in a browser.
- **Another room:** `join-room new` → `status` to read the room URL → open it.

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
