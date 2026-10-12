# linux_test — Linux-target build of the KVS WebRTC SDK

The same SDK component graph as `examples/webrtc_classic`
(`app_webrtc` + `kvs_signaling` + `kvs_webrtc` + `esp_webrtc_utils`)
compiled for the **ESP-IDF Linux target** so it builds and runs on a
normal POSIX host (Linux / macOS). It connects to a real Kinesis Video
Streams signaling channel as **MASTER**, completes
signaling → DTLS → SRTP with any peer that joins as VIEWER, and streams
H.264 + Opus from file-backed sample frames.

It exercises the SDK's full signaling and media path without hardware.

## Prerequisites

- ESP-IDF **5.5**, **6.0**, **6.1** or master, with the Linux-target preview.
- A host toolchain the IDF Linux target needs (`cmake`, `ninja`, a C
  compiler). On macOS the Xcode command-line tools are enough.
- AWS credentials with KVS signaling permissions and a signaling
  channel (created on first run if it doesn't exist).
- A CA bundle on the host (auto-detected — see below).

## What differs from webrtc_classic

| Concern | `webrtc_classic` (ESP target) | `linux_test` (Linux target) |
|---|---|---|
| AWS credentials | Kconfig (`CONFIG_AWS_*`) | env vars (`AWS_ACCESS_KEY_ID` etc.) |
| Channel name    | Kconfig (`CONFIG_AWS_KVS_CHANNEL_NAME`) | env var (`KVS_CHANNEL_NAME`) |
| WiFi / BLE prov | `app_wifi_prov` + provisioning flow | host networking stack |
| NVS init        | `nvs_flash_init` | not needed (creds are env) |
| SNTP time sync  | `esp_webrtc_time_sntp_*` | host clock |
| Media capture   | camera + mic drivers | file-backed sample frames |
| Signaling stack | `esp_websocket_client` (default) | `libwebsockets` (`CONFIG_USE_ESP_WEBSOCKET_CLIENT=n`) |
| Termination     | runs until reset | `TEST_DURATION_SEC` countdown or Ctrl+C |

## Build

```bash
. $IDF_PATH/export.sh              # ESP-IDF 5.5, 6.0, 6.1 or master
export KVS_SDK_PATH=$PWD/../..     # repo root; SDK CMake reads this
cd examples/linux_test
idf.py --preview set-target linux
idf.py build
```

Produces `build/linux_test.elf`. Use `-B <dir>` for a side build dir
(e.g. `idf.py -B build_idf55 ...`) when switching IDF versions so the
two configs don't clobber each other.

## Run

`main.c` reads its config from the environment:

| Env var | Required | Default | Purpose |
|---|---|---|---|
| `KVS_CHANNEL_NAME` | yes | — | signaling channel name |
| `AWS_ACCESS_KEY_ID` | yes | — | AWS access key |
| `AWS_SECRET_ACCESS_KEY` | yes | — | AWS secret key |
| `AWS_SESSION_TOKEN` | no | — | STS session token (temporary creds) |
| `AWS_DEFAULT_REGION` | no | `us-west-2` | KVS region |
| `KVS_CA_CERT_PATH` | no | auto-detect | CA bundle path (see below) |
| `KVS_FRAMES_DIR` | no | `samples` | dir holding `h264SampleFrames/` + `opusSampleFrames/` |
| `TEST_DURATION_SEC` | no | `60` | run duration in seconds, `0` = until Ctrl+C |

The CA bundle is auto-detected in this order:
`/etc/ssl/certs/ca-certificates.crt` (Debian/Ubuntu, incl. the
`espressif/idf` image) → `/etc/ssl/cert.pem` (macOS) →
`/etc/pki/tls/certs/ca-bundle.crt` (Fedora/RHEL). Override with
`KVS_CA_CERT_PATH` if none apply.

The sample frames ship in the upstream SDK submodule under
`amazon-kinesis-video-streams-webrtc-sdk-c/samples/`, so point
`KVS_FRAMES_DIR` there (or copy them next to the binary):

```bash
export AWS_ACCESS_KEY_ID=...
export AWS_SECRET_ACCESS_KEY=...
export AWS_DEFAULT_REGION=us-west-2
export KVS_CHANNEL_NAME=my-test-channel
export KVS_FRAMES_DIR=$PWD/../../amazon-kinesis-video-streams-webrtc-sdk-c/samples
export TEST_DURATION_SEC=120
./build/linux_test.elf
```

## Test with a real viewer

1. Start `linux_test.elf` as above; wait for `Signaling connected`.
2. Open the AWS KVS WebRTC test page:
   <https://awslabs.github.io/amazon-kinesis-video-streams-webrtc-sdk-js/examples/index.html>
3. Fill in the **same** region, channel name, and AWS credentials, set
   the client to **Viewer**, and click **Start Viewer**.
4. The device logs `Received offer` → `Sent answer` → `Peer connected`,
   and the browser shows the looping H.264 sample video with Opus audio.

The channel and credentials must match on both sides. Temporary
credentials also require `AWS_SESSION_TOKEN` on the device and the
matching token field on the test page.

## Automated end-to-end check

`tests/linux_test_e2e/run.sh` runs `linux_test.elf` against an aiortc viewer on
a fresh channel and checks the H.264 it receives against the sample frames
bit for bit. It needs the same AWS variables and `python3`; see the script
header for the options.
