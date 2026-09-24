# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Repository structure

- Migrated ESP-IDF port from the `beta-reference-esp-port` branch of
  `awslabs/amazon-kinesis-video-streams-webrtc-sdk-c` into this standalone
  repository. The upstream SDK is now a git submodule with platform patches
  applied via `git am patches/*.patch`.
- Bumped the webrtc-c submodule to **1.20.0** (`4dd058d5`) and dropped the two
  SDP-renegotiation patches — that flow is now upstream (awslabs#2214). The
  patch set is down to three (ESP platform, Opus cap, ICE EALREADY). 1.20.0 also
  brings opt-in mbedTLS 4 support (awslabs#2325), unused on IDF 5.5.

### Added

- **Raw-frame sink bus** — the capture loop is a pump: it publishes each camera
  frame and moves on, and N consumers share the buffer by reference with a
  per-sink queue depth, overflow policy and fps limit. The H.264 encoder is just
  another sink. `VIDEO_RAW_MODE_CONVERTED` hands a consumer a PPA-scaled copy and
  returns the camera buffer immediately. See `include/video_raw_sink.h`.
- **Per-sink rate control** — one congestion controller per transport instead of
  a process-wide singleton, arbitrated as the strict minimum over every enabled,
  still-reporting controller. Transports report either send latency or upload-queue
  occupancy; the encoder side moved to `video_rate_ctrl_priv.h`.
- **`webrtc_person_detect` example** — esp-dl pedestrian detection, an LCD preview
  and WebRTC, all three on one camera, each start/stoppable from the console.
  ESP32-P4 only.
- **`media_stream_caps.h`** — capability macros (esp_video capture, hardware H.264,
  esp_image_effects, PPA) replacing raw `CONFIG_IDF_TARGET_*` tests at the capture
  pipeline's guard sites.
- **ESP-IDF v6 support** — builds against IDF v6.0/v6.1 (mbedTLS 4 / TF-PSA-Crypto)
  alongside v5.4/v5.5. The SDK crypto adapts to mbedTLS 4 through PSA, the port
  stays on libsrtp 2.x (`>=2.8.0~1`, which carries the mbedTLS-4/PSA backend from
  esp-protocols#1114), and `KVS_USE_MBEDTLS4` is defined for IDF >= 6 so signaling
  stops sending an OpenSSL-style cipher list. No `USE_LIBSRTP3` is required; two
  webrtc-c patches are: `patches/0004` (entropy-less RNG from PSA, upstream
  awslabs#2385) and `patches/0005` (fingerprint via `mbedtls_md()`).

- **SDP re-negotiation** — re-offer from the same peer re-uses an active
  session or replaces a terminated one.
- **BLE Wi-Fi provisioning** via the `network_provisioning` component in a
  shared `app_common/app_wifi_prov` module. Supported on ESP32-S3, ESP32-C6,
  and ESP32-P4 (BLE on C6 coprocessor through `esp_hosted`).
- **Split-mode enhancements** — bridge command framework with protobuf
  chunking, ICE server bridge for TURN credential sharing, light-sleep
  integration, snapshot over the bridge with SPIRAM-preferred payloads.
- **`simple_video_server` example** — standalone HTTP MJPEG / snapshot server
  with no WebRTC dependency.
- **Ring buffer peek API** — made public with peek support.
- **Kconfig stack sizes and signaling init retry** with exponential backoff;
  WebSocket stale-event guard and timeout race hardening.
- **ESP Launchpad integration** for `esp_camera` on ESP32-S3 — one-click
  browser flashing via GitHub Pages. `launchpad.toml` is generated with
  `${{ github.repository_owner }}` so it auto-adapts to any fork.
- **Media pipeline** — JPEG snapshot, YUV420 → RGB565 conversion, OV2710
  sensor bring-up path.
- **Camera interface selection** — `media_stream` can bring up MIPI-CSI, DVP,
  SPI or USB-UVC cameras, chosen in menuconfig. BSP boards still go through
  `bsp_camera_start()`.
- **UVC H.264 passthrough** on ESP32-P4 — a UVC camera's own H.264 is sent
  without re-encoding (`CONFIG_MEDIA_STREAM_UVC_PASSTHROUGH_H264`).

### Changed

- **Camera Kconfig renamed `CONFIG_EXAMPLE_*` → `CONFIG_MEDIA_STREAM_*`** under
  *Media Stream Configuration → Video Initialization Configuration*. Only the
  prefix changes, e.g. `CONFIG_EXAMPLE_ENABLE_MIPI_CSI_CAM_SENSOR` →
  `CONFIG_MEDIA_STREAM_ENABLE_MIPI_CSI_CAM_SENSOR`. This covers the
  `ENABLE_*_CAM_*`, `SCCB_*`, `MIPI_CSI_*`, `DVP_*`, `SPI_*`, `USB_*` and
  `SELECT_JPEG_*` groups.

  **Migration:** Kconfig ignores unknown keys without a warning, so old names in
  an `sdkconfig` or `sdkconfig.defaults*` overlay just stop applying and the
  camera falls back to defaults.

### Fixed

- Jitter buffer dropping multiple valid packets (increased buffer size and
  queue depth).
- OV2710 black frames (sensor detection, flip-control skip, ioctl access).
- Camera memory fragmentation (switched to USERPTR with pre-allocated SPIRAM
  buffers).
- Spinlock crash on camera deinit (race between DQBUF and close).
- Signaling cache invalidation for corrupted / empty endpoints.
- Duplicate signaling state callbacks and WebSocket reconnection after error.
- Use-after-free crash in `esp_camera` (static config, CLI registration
  before Wi-Fi wait).
- Cleanup loop abort on non-fatal errors — Wi-Fi disconnect no longer
  permanently breaks session cleanup.
- Cached TURN servers not applied to new peer connections.
- `global_media_started` not reset when session count reaches 0 — media
  restart after disconnect cycles.
- `CHECK_SIGNALING_CREDENTIALS_EXPIRATION` guard against NULL credentials
  (crash on reconnect).
- Deferred cached ICE server apply to the work queue — avoids 22 s+ stall on
  offer processing.
- Moved KVS threads to SPIRAM, fixed thread-naming collision with lwIP.

### CI and testing

- GitHub Actions build matrix for ESP-IDF v5.4 (9 example/target combos) and
  v5.5 (11 combos) using `espressif/idf` Docker containers.
- GitLab CI configuration for internal builds.
- Unit-test infrastructure on the ESP-IDF Linux host target covering base64,
  CRC32, hex encoding, signaling serializer, and state machine.
- Sphinx + Doxygen documentation build job.
- ESP Launchpad deployment workflow (GitHub Pages).

### Documentation

- Migration guide in `README.md` for users moving from the old
  `beta-reference-esp-port` branch.
- `API_USAGE.md` and `docs/custom_signaling.md` covering the simplified API and
  custom signaling protocols via the pluggable interface architecture.
- Wi-Fi provisioning instructions in example READMEs.

### Known deferred

Tracked for follow-up PRs:

- Display / video-decode path for received video (`video_player_adapter.c`
  is currently a stub).
- First-class M5Stack Tab5 support (partial commits already on `main`).
- Acoustic Echo Cancellation in the audio send path.
- Consolidation of signaling implementations into `components/`.
