# ESP-IDF Port of Amazon Kinesis Video Streams WebRTC SDK

[![Build Examples & Docs](https://github.com/espressif/esp-port-for-amazon-kvs-sdk/actions/workflows/build.yml/badge.svg)](https://github.com/espressif/esp-port-for-amazon-kvs-sdk/actions/workflows/build.yml)

<a href="https://espressif.github.io/esp-launchpad/?flashConfigURL=https://espressif.github.io/esp-port-for-amazon-kvs-sdk/launchpad.toml"><img alt="Try it with ESP Launchpad" src="https://espressif.github.io/esp-launchpad/assets/try_with_launchpad.png" width="200"></a>

This repo gives you a drop-in ESP-IDF port of the [**Amazon Kinesis Video Streams WebRTC SDK C**](https://github.com/awslabs/amazon-kinesis-video-streams-webrtc-sdk-c) for building real-time audio/video products on ESP32-family SoCs. It combines the upstream SDK with ESP-IDF components and BSP hooks. You also get ready-to-run examples (single-device and dual-chip split) and `esp_hosted` Wi-Fi co-processor support, so you can get a board streaming with a few Kconfig changes.

## Customizations Added by the Repo

- ESP-IDF component glue (`components/`), Kconfig options, and `idf_component.yml` manifests.
- ESP-targeted examples under `examples/` (single-device, split-mode, signaling-only, network-adapter).
- Per-component `port/` files that swap in ESP-IDF / lwIP / mbedTLS implementations for the upstream's POSIX assumptions (TLS, WebSocket, signaling I/O).
- A small set of [platform patches](patches/README.md) — each tagged as *aligned with an upstream PR* (drops once the PR lands) or *ESP-IDF-specific* (stays long-term).

The upstream SDK is included here as a git submodule at
[amazon-kinesis-video-streams-webrtc-sdk-c/](amazon-kinesis-video-streams-webrtc-sdk-c/) — referred to as *the upstream SDK* below.

This repo replaces the upstream `esp_port/` tree (see [Migration](#migrating-from-beta-reference-esp-port-branch)).

## Examples

Pick the example that matches your hardware and connectivity model:

| Example | Architecture | Hardware | Use Case |
|---------|--------------|----------|----------|
| **[webrtc_classic](examples/webrtc_classic/)** | `kvs_signaling + kvs_peer_connection` | ESP32 / S3 / P4 + camera | Learning WebRTC, single-device, AWS integration — **start here**. Dual-chip classic: also flash [network_adapter](examples/network_adapter/) on the C6 |
| **[esp_camera](examples/esp_camera/)** | `apprtc_signaling + kvs_peer_connection` | ESP32-CAM modules | Browser compatibility, **no AWS account needed** |
| **[streaming_only](examples/streaming_only/)** | `bridge_signaling + kvs_peer_connection` | ESP32-P4 (main processor) | High-performance streaming, power-optimized split mode |
| **[signaling_only](examples/signaling_only/)** | `kvs_signaling + bridge_peer_connection` | ESP32-C6/C5 (network processor) | Always-on connectivity, paired with `streaming_only` |
| **[RainMaker camera examples](https://github.com/espressif/esp-rainmaker/tree/master/examples/camera)** | this port + RainMaker cloud | ESP32-P4 + C6/C5 | End-to-end: cloud onboarding, BLE provisioning, OTA, mobile-app pairing. Free to evaluate; path to a private deployment |

Each example's README has its build-and-run steps; the shared build mechanics — clone &
submodules, ESP-IDF setup, target + board overlays, dual-chip — are in [docs/building.md](docs/building.md).

## Features

- **Two-way audio + video** — H.264 video TX/RX and Opus mono audio TX/RX. STUN, TURN, NACK, RTCP, data channels all wired in. HW H.264 encoder on ESP32-P4; SW H.264 decoder (tinyh264) for RX.
- **Pluggable signaling + peer-connection** — out-of-the-box AWS KVS signaling client; pick any combination (KVS signaling + KVS peer, bridge signaling + KVS peer for split mode, etc.). Custom protocols via the same interface — see the [Custom Signaling Guide](docs/custom_signaling.md).
- **Single-chip and split-mode architectures** — same components on one ESP32-class SoC or as ESP32-P4 host + ESP32-C6 / C5 co-processor over SDIO. Split mode keeps the P4 in deep sleep while the C6 holds always-on signaling; wake the P4 when a viewer connects.
- **Smart defaults** — start from `APP_WEBRTC_CONFIG_DEFAULT()` and plug in only the interfaces you need (signaling, peer connection, video capture, etc.). See [API_USAGE.md](API_USAGE.md).
- **Credentials** — AWS access-key, IoT Core (TLS client cert), or runtime-injected (via RainMaker claim flow) — all behind one `credential` component.
- **ESP-IDF integration** — `idf_component.yml` manifests, Kconfig flags, BSP hooks, `esp_hosted` co-processor wiring. Platform patches against the upstream SDK auto-apply at CMake configure time (opt out via `CONFIG_KVS_AUTO_APPLY_PATCHES`).

## Deployment Modes

The port supports two ways to split the WebRTC pipeline across one or two ESP32 SoCs. Pick the mode that matches your hardware and power budget.

### Classic Mode — signaling + streaming on the same chip

Both signaling and streaming run on the same SoC. The two functional blocks are the SDK's pluggable interfaces — `signaling_client_if` (Signaling / AWS KVS) and `peer_connection_if` (peer connection + media) — and classic mode runs them together. Two configurations:

**Single-chip** — one ESP32 / ESP32-S3 (e.g. ESP32-S3-EYE, ESP32-WROVER-KIT) runs both:

```
┌──────────────────────────────────┐
│  ESP32 / ESP32-S3  (single SoC)  │
│         ┌─────────────┐          │
│         │  Signaling  │          │
│         │  (AWS KVS)  │          │
│         └─────────────┘          │
│                +                 │
│      ┌───────────────────┐       │
│      │  Peer connection  │       │
│      │  + media stream   │       │
│      └───────────────────┘       │
└──────────────────────────────────┘
```

**Two-chip** — P4 runs the full stack; the C6 is just a transparent network adapter (esp_hosted forwards packets over SDIO):

```
┌─────────────────────────┐
│         ESP32-P4        │
│     ┌─────────────┐     │
│     │  Signaling  │     │             ┌─────────────────┐
│     │  (AWS KVS)  │     │             │     ESP32-C6    │
│     └─────────────┘     │     SDIO    │  Wi-Fi NIC      │
│            +            │◄───────────►│  (transparent)  │
│  ┌───────────────────┐  │             └─────────────────┘
│  │  Peer connection  │  │
│  │  + media stream   │  │
│  └───────────────────┘  │
└─────────────────────────┘
```

Build with the [webrtc_classic](examples/webrtc_classic/) example. For the two-chip case, also flash [examples/network_adapter](examples/network_adapter/) on the C6.

### Split Mode — signaling and streaming on separate chips

A power-optimized dual-chip layout: the always-on Wi-Fi co-processor (`ESP32-C6` or `ESP32-C5`) holds the KVS signaling connection, and the main processor (`ESP32-P4`) wakes from deep sleep only when a viewer actually connects.

Here the two pluggable blocks are split *across* the chips — Signaling on the C6, peer connection on the P4 — with the network stack itself split (lwip split, via esp_hosted):

```
┌────────────────────────┐               ┌────────────────────────┐
│  ESP32-P4 (streaming)  │               │  ESP32-C6 (signaling)  │
│  ┌─────────────────┐   │   lwip split  │  ┌─────────────────┐   │
│  │ Peer connection │   │◄─────────────►│  │    Signaling    │   │
│  │  + media stream │   │               │  │    (AWS KVS)    │   │
│  └─────────────────┘   │               │  └─────────────────┘   │
└────────────────────────┘               └────────────────────────┘
```

How it works:
- `esp_hosted` lets the network stack run on **both** chips, sharing the same IP but with non-overlapping port ranges.
- The C6 keeps the WebSocket to KVS open continuously. It costs negligible power.
- When a SDP offer arrives, the C6 wakes the P4 over SDIO and forwards only the essential signaling messages. The P4 establishes the RTP/SCTP session directly with the viewer — media never round-trips through the C6.

Two binaries pair up:
- [signaling_only](examples/signaling_only/) on the C6 — KVS signaling client + RPC bridge to the host.
- [streaming_only](examples/streaming_only/) on the P4 — RTP/SCTP/SRTP + media capture and encode.

Trade-offs:
- **Lower idle power** — the P4 stays in deep sleep while no viewer is connected. The C6 alone draws ~30 mA on its WebSocket.
- **Instant wake-up** — both chips have IP addresses already; once the P4 boots, it can RTP-stream within hundreds of milliseconds.
- **More moving parts** — two firmware images, an OTA story for both, and an IPC contract between the two examples. See the per-example READMEs for the bring-up details.

You can validate either mode against the [AWS WebRTC test viewer](https://awslabs.github.io/amazon-kinesis-video-streams-webrtc-sdk-js/examples/index.html).

## Building & Flashing

[**docs/building.md**](docs/building.md) is the full guide — clone & submodules, SDK patch
auto-apply, ESP-IDF v5.4/v5.5 setup, picking an example + target, board overlays (the
`SDKCONFIG_DEFAULTS` chain), ws-on vs ws-off, console output, the ESP32-P4 + ESP32-C6 dual-chip
setup, AWS credentials, and troubleshooting. (The Quick Start above has the short recipe.)

## Related repos

| Repo | Contains |
|---|---|
| [espressif/esp-rainmaker](https://github.com/espressif/esp-rainmaker) | End-to-end RainMaker camera reference on Espressif's IoT cloud. Consumes this port via `KVS_SDK_PATH`, layers cloud claim, BLE provisioning, OTA, and mobile-app pairing on top. Start here for cloud onboarding / fleet management — free to evaluate, with a path to a private RainMaker deployment for production. |
| [awslabs/amazon-kinesis-video-streams-webrtc-sdk-c](https://github.com/awslabs/amazon-kinesis-video-streams-webrtc-sdk-c) | Upstream KVS WebRTC C SDK. Tracked as a submodule + small set of [patches](patches/README.md). |

**Matter Camera spec** — planned. SDP renegotiation patches 0001 / 0002 are aligned with [awslabs#2214](https://github.com/awslabs/amazon-kinesis-video-streams-webrtc-sdk-c/pull/2214) toward that goal.

## Documentation

For detailed API usage and implementation guides:

- **[API_USAGE.md](API_USAGE.md)** — API reference, configuration options, and usage examples for all deployment modes.
- **[docs/custom_signaling.md](docs/custom_signaling.md)** — guide to implementing custom signaling protocols against the pluggable signaling interface.
- **[docs/connection_steps.md](docs/connection_steps.md)** — stage-by-stage walkthrough of the KVS WebRTC connection flow (credentials → signaling → ICE → peer connection); handy when debugging a connection.
- **[patches/README.md](patches/README.md)** — the patch ledger (what each patch does, alignment status with upstream PRs).

## Migrating from `beta-reference-esp-port` Branch

This repository replaces the `esp_port/` subdirectory that previously lived inside the upstream [awslabs/amazon-kinesis-video-streams-webrtc-sdk-c](https://github.com/awslabs/amazon-kinesis-video-streams-webrtc-sdk-c) repository on the [beta-reference-esp-port branch](https://github.com/awslabs/amazon-kinesis-video-streams-webrtc-sdk-c/tree/beta-reference-esp-port).

### What Changed

The `esp_port/` directory prefix has been removed. Components and examples now live at the repository root:

| Before (upstream `beta-reference-esp-port`) | After (this repo) |
|---|---|
| `${KVS_SDK_PATH}/esp_port/components/<name>` | `${KVS_SDK_PATH}/components/<name>` |
| `${KVS_SDK_PATH}/esp_port/examples/<name>` | `${KVS_SDK_PATH}/examples/<name>` |

### Updating `idf_component.yml` References

If your project uses `idf_component.yml` to reference KVS SDK components via `path:` or `override_path:`, remove the `esp_port/` segment from every path:

```diff
  app_webrtc:
-   path: ${KVS_SDK_PATH}/esp_port/components/app_webrtc
+   path: ${KVS_SDK_PATH}/components/app_webrtc
    version: "*"
```

This applies to all component references: `app_webrtc`, `esp_webrtc_utils`, `kvs_webrtc`, `kvs_signaling`, `media_stream`, `network_coprocessor`, `signaling_bridge_adapter`, etc.

### No API Changes

All component APIs remain the same. No changes are needed in application source code (`.c` / `.h` files).

## License

This project and the upstream SDK are licensed under Apache-2.0. See [LICENSE](LICENSE) and [NOTICE](NOTICE).
