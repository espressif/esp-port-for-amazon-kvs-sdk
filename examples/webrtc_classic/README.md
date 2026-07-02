# WebRTC Classic Example - **AWS KVS Streaming**

**Simple AWS KVS Integration** - Complete WebRTC implementation using Amazon Kinesis Video Streams for signaling. This example provides a straightforward way to stream video to browsers via AWS infrastructure.

## What You'll Build

A WebRTC camera that streams video and audio to web browsers using AWS KVS (Kinesis Video Streams) for signaling. Everything runs on a single ESP32 (signaling + streaming), auto-connecting to AWS KVS on startup.

## Features

- **Video Streaming** - H.264 encoding for live video transmission
- **Audio Streaming** - Opus encoding for audio transmission
- **AWS KVS Integration** - Uses Amazon Kinesis Video Streams for signaling
- **Authentication Options** - Direct AWS credentials, IoT Core certificates, or ESP RainMaker
- **Auto-Connect** - Automatically connects to AWS KVS on startup

## Hardware Requirements

| Board | Video | Audio | Notes |
|-------|-------|-------|-------|
| **ESP32-S3-EYE** | Built-in camera | Built-in mic | **Recommended** - Ready to use |
| **ESP32-WROVER-KIT** | Add camera module | Add I2S mic | Requires additional hardware |
| **ESP32-P4-Function-EV-Board** | High-quality camera | Built-in mic | **Premium** - Best performance |

Optional: I2S microphone/speaker for bidirectional audio; external antenna for better Wi-Fi range.

## Build & Flash

```bash
cd examples/webrtc_classic

# Single-chip (ESP32-S3-EYE — recommended starting point):
idf.py set-target esp32s3
idf.py menuconfig                 # Wi-Fi + AWS auth (see below)
idf.py -p <PORT> flash monitor

# ESP32-P4 + C6 dual-chip: flash examples/network_adapter on the C6 first,
# then select your P4 board overlay (chain MUST be in the same set-target call):
idf.py -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.defaults.esp32p4;sdkconfig.defaults.p4_eye.esp32p4" \
       set-target esp32p4
idf.py -p <PORT> flash monitor
```

Board overlays available here: `esp32s3`, `esp32p4`, `p4_eye.esp32p4`, `p4x_eye.esp32p4`,
`p4_function_ev_board_v12.esp32p4`, `p4_function_ev_board_v16.esp32p4`,
`p4_c5_core_board.esp32p4`. For the libwebsockets (ws-off) signaling path, enable it in menuconfig —
see [kvs_signaling](../../components/signaling/kvs_signaling/README.md#signaling-transport-ws-on--ws-off).

For the shared mechanics this recipe relies on — the SDKCONFIG-overlay chain gotcha, the
**ESP32-P4 + C6 dual-chip** ESP-Prog wiring, console-output selection, and generic
troubleshooting — see [**docs/building.md**](../../docs/building.md). Wi-Fi (BLE) provisioning
is documented in [app_common](../common_components/app_common/README.md#wifi-provisioning). The rest of this page
covers the AWS-authentication and viewer steps specific to this example.

### AWS Authentication (choose one)

The `credential` component selects the mode (see [building.md → AWS credentials](../../docs/building.md#aws-credentials)).

**Option A: Direct AWS credentials** — disable `CONFIG_IOT_CORE_ENABLE_CREDENTIALS` in
menuconfig and set `AWS_ACCESS_KEY_ID` / `AWS_SECRET_ACCESS_KEY` there. `main/webrtc_main.c`
reads them:
```c
kvs_signaling_cfg.awsAccessKey   = CONFIG_AWS_ACCESS_KEY_ID;
kvs_signaling_cfg.awsSecretKey   = CONFIG_AWS_SECRET_ACCESS_KEY;
kvs_signaling_cfg.awsSessionToken = CONFIG_AWS_SESSION_TOKEN;
```

**Option B: IoT Core certificates (recommended)** — enable `CONFIG_IOT_CORE_ENABLE_CREDENTIALS`
and place your device cert/key in the SPIFFS image:
```bash
cp /path/to/certificate.pem ../common_components/app_common/spiffs_image/certs/certificate.pem
cp /path/to/private.key     ../common_components/app_common/spiffs_image/certs/private.key
```
See [spiffs_image/certs/README.md](../common_components/app_common/spiffs_image/certs/README.md) for the full
file list. Override the endpoint/role/thing in `main/webrtc_main.c` if needed:
```c
kvs_signaling_cfg.iotCoreCredentialEndpoint = "your-endpoint.credentials.iot.us-east-1.amazonaws.com";
kvs_signaling_cfg.iotCoreCert       = "/spiffs/certs/certificate.pem";
kvs_signaling_cfg.iotCorePrivateKey = "/spiffs/certs/private.key";
kvs_signaling_cfg.iotCoreRoleAlias  = "your_role_alias";
kvs_signaling_cfg.iotCoreThingName  = "your_thing_name";
```

**Option C: ESP RainMaker integration (advanced)** — supply a credential callback for dynamic
renewal and managed memory:
```c
int rmaker_fetch_aws_credentials(uint64_t user_data, /* ... */) {
    esp_rmaker_aws_credentials_t *credentials =
        esp_rmaker_get_aws_security_token("esp-videostream-v1-NodeRole");
    /* set output params, return 0 on success */
}
kvs_signaling_cfg.fetch_credentials_cb = rmaker_fetch_aws_credentials;
```
See the [kvs_webrtc_camera example](https://github.com/espressif/esp-rainmaker/tree/master/examples/kvs_webrtc_camera) in ESP RainMaker for a full implementation.

### View your stream

1. Open the [AWS KVS WebRTC Test Page](https://awslabs.github.io/amazon-kinesis-video-streams-webrtc-sdk-js/examples/index.html)
2. Enter your AWS credentials (same as Option A/B)
3. Enter the channel name configured in menuconfig
4. Select **Join as viewer** → **Start Viewer** — you should see the ESP32 camera feed.

## Architecture Overview

This example demonstrates the simplified WebRTC API:

```c
// Simplified configuration with reasonable defaults
app_webrtc_config_t app_webrtc_config = APP_WEBRTC_CONFIG_DEFAULT();
app_webrtc_config.signaling_client_if = kvs_signaling_client_if_get();
app_webrtc_config.signaling_cfg = &kvs_signaling_cfg;
app_webrtc_config.peer_connection_if = kvs_peer_connection_if_get();

// Media interfaces for bi-directional streaming
app_webrtc_config.video_capture = video_capture;
app_webrtc_config.audio_capture = audio_capture;
app_webrtc_config.audio_player = audio_player;
```

Smart defaults: MASTER role, H.264/OPUS codecs, trickle ICE. Streaming mode is auto-detected
from the media interfaces provided. Override any default via the dedicated configuration
functions (see [API_USAGE.md](../../API_USAGE.md)).

## Troubleshooting

Generic build/Wi-Fi/console issues are in [building.md → Troubleshooting](../../docs/building.md#troubleshooting). Example-specific:

| Problem | Symptoms | Solution |
|---------|----------|----------|
| **AWS authentication failed** | `AWS credentials invalid` | Verify keys in menuconfig or IoT Core certs; test with `aws kinesisvideo list-signaling-channels --region us-east-1` |
| **No video stream** | Viewer connects but no video | Check camera connections and power supply |
| **Poor video quality** | Choppy/pixelated video | Check Wi-Fi signal strength, reduce interference |

Expected event sequence on a successful connection:
```
[KVS Event] WebRTC Initialized → Signaling Connecting → Signaling Connected
[KVS Event] Peer Connected: viewer-XXXXX → Streaming Started for Peer: viewer-XXXXX
```

## Next Steps

- **[streaming_only](../streaming_only/README.md)** + **[signaling_only](../signaling_only/README.md)** - split mode for power optimization
- **[esp_camera](../esp_camera/README.md)** - AppRTC compatible (no AWS needed)
- **[API_USAGE.md](../../API_USAGE.md)** - complete API reference
- **[docs/custom_signaling.md](../../docs/custom_signaling.md)** - custom signaling guide

## License

Apache License 2.0
