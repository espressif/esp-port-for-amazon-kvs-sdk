# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Changed

- `KVS_SDK_PATH` pointing at a directory that does not exist (or one with no
  `components/kvs_webrtc`) is now a configure-time `FATAL_ERROR` instead of a warning
  that scrolled past while every `kvs_*` component silently resolved elsewhere.

### Repository structure

- Migrated ESP-IDF port from the `beta-reference-esp-port` branch of
  `awslabs/amazon-kinesis-video-streams-webrtc-sdk-c` into this standalone
  repository. The upstream SDK is now a git submodule with platform patches
  applied via `git am patches/*.patch`.
- Moved the webrtc-c submodule to `develop` at `efdec3bf`: release **1.20.0** plus
  the relay candidate filter (awslabs#2388), the TURN rejected-peer timeout fix
  (#2387), the rtpmap payload-type fix for 32-bit platforms (#2405), no cipher list
  on any mbedTLS build (#2409), srflx candidates from a pre-resolved STUN address
  (#2410), the DTLS fingerprint through `mbedtls_md()` (#2392), the RNG from PSA
  on mbedTLS 4 (#2385) and the `pSeenTransceivers` initialisation (#2415). 1.20.0
  brought the SDP-renegotiation flow upstream (awslabs#2214), so the two local
  patches for it are dropped, and opt-in mbedTLS 4 support (awslabs#2325); #2392,
  #2385 and #2415 replace local patches 0005, 0004 and 0006. The previous pin
  `4dd058d5` was the head of the `release-v1.20.0` branch (awslabs#2383); a commit
  on `develop` stays fetchable when release branches are deleted. The current patch set is listed in `patches/README.md`.

### Added

- **`esp_camera` on the IDF linux target** — the AppRTC example runs on a host
  with sample frames in place of the camera. Builds on IDF 5.5, 6.0, 6.1 and
  master.
- **`linux_test` host example** — the SDK as a KVS master on the IDF linux target
  (Linux and macOS hosts), streaming file-backed H.264 + Opus; `tests/linux_test_e2e`
  checks what an aiortc viewer receives against the sample frames. Builds on IDF
  5.5, 6.0, 6.1 and master.
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
- **End-to-end streaming test** (`test_e2e/`) — drives real hardware against a real KVS
  channel with an aiortc viewer, asserting the full `SIGNALING_CLIENT_STATE` walk in
  order, that video RTP reaches the viewer (and audio, where the board has a codec),
  that the selected ICE candidate pair is peer-to-peer rather than TURN-relayed, and
  that the data channel opens and echoes.
  Complements `test_app/`, which only covers pure functions. Opt-in via `--e2e`; device
  credentials come from a gitignored `test_e2e/sdkconfig.creds`. See its README for the
  measured baseline and the issues it already surfaced.

  Split mode (P4 + C6) is covered too, as a single-DUT test: the C6's log is read off
  the P4's console via the `[SLAVE]` prefix and its console is driven through the P4's
  `write_slave` command, so a one-USB-port kit needs no ESP-Prog. It additionally
  asserts the bridge offer handoff and that the adapter never drops a queued message.

  The split pair also runs under ESP-EMU with no hardware (`--emu`): the full KVS path
  from bring-up to a viewer's answer, and the capture path alone through
  `streaming_only`'s `media-selftest` command. `apprtc_viewer.py` checks media without
  AWS against `esp_camera` in a fixed AppRTC room. CI builds the e2e and emulator
  overlay chains and collects the suite; it runs no test that needs hardware or AWS.

- **File capture feeds WebRTC again.** The file source publishes to the sink registry,
  paced to the frame interval, and reads one packed file per medium
  (`samples/video.pack`, `audio.pack`; `tools/pack_samples.py`) instead of one file per
  frame. `streaming_only` selects audio and video sources independently.

- **ESP-IDF v6 support** — builds against IDF v6.0/v6.1 (mbedTLS 4 / TF-PSA-Crypto)
  alongside v5.4/v5.5. The SDK crypto adapts to mbedTLS 4 through PSA, the port
  stays on libsrtp 2.x (`>=2.8.0~1`, which carries the mbedTLS-4/PSA backend from
  esp-protocols#1114). No `USE_LIBSRTP3` is required, and no webrtc-c patch: the
  entropy-less RNG from PSA (awslabs#2385), the fingerprint through `mbedtls_md()`
  (awslabs#2392) and signaling sending no OpenSSL-style cipher list on mbedTLS builds
  (awslabs#2409) are upstream.

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

- **`APP_WEBRTC_EVENT_SENT_ANSWER`** is raised once signaling has sent or
  buffered the answer, not before the send. A failed answer raises
  `APP_WEBRTC_EVENT_SIGNALING_ERROR` with the peer id instead. Apps that act on
  `SENT_ANSWER`, such as the power-save handler stopping its park timer, now see
  it only for an answer that goes out or will be re-sent. See `API_USAGE.md` for
  a buffered answer that never goes out.
- Patch 0001 no longer carries the libwebsockets callback and send-path rewrite
  from awslabs#2134. Only the ws-off (`CONFIG_USE_ESP_WEBSOCKET_CLIENT=n`) path
  used it; see `patches/README.md` for what replaced each part.
- esp_camera: `join-room` leaves the current room first instead of failing with
  "Already connected"; a separate `disconnect` is no longer needed.
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

- **KVS signaling:** an SDP answer sent while the signaling WebSocket was
  reconnecting was lost, and the viewer never connected. This happens when a
  viewer offer wakes a parked host and the link drops before the answer goes out
  (Wi-Fi re-init, AP drop, AWS FIN).
  - A failed answer is now buffered, one per peer, and re-sent on reconnect
    ahead of that peer's candidates. An answer more than 30 s old is dropped,
    since its session has timed out by then.
  - The client counts as disconnected from the moment it leaves CONNECTED, so
    the re-send buffer is used during a WebSocket-error reconnect too, which
    never passes DISCONNECTED.

- **AppRTC:** a WebSocket frame larger than one read (typically an SDP offer or
  answer) arrives as several events; each was parsed on its own and failed, so the
  call never connected. Chunks are now reassembled before the frame is handled,
  for frames up to 128 KB.

- An offer waiting for its answer was evicted by the liveness watchdog after 30 s,
  so a device-created AppRTC room had to be joined within ~40 s. The new
  `kvs_peer_connection_set_answer_timeout()` lets an offer wait longer, with the
  30 s connect deadline running from the answer; esp_camera sets 10 minutes. Other
  offerers keep the 30 s.
- AppRTC: a full outgoing queue (the websocket still connecting) dropped its oldest
  message, which could be the answer. It now drops the oldest ICE candidate and never
  an offer or answer, and holds 32 messages instead of 10. Dropped and purged
  messages also leaked their payload. Messages sent while others still wait now
  queue behind them, so candidates no longer overtake the answer, and a drain that
  loses the websocket stops instead of spinning. An offer or answer whose send fails
  while connected gets one more try.
- AppRTC: events still queued from a replaced websocket (a rejoin, or a connect to
  another room) are dropped instead of tearing down or feeding the new connection,
  and the reconnect timer no longer races a disconnect or connect over the client.
  On an automatic rejoin the server's initiator role now applies instead of the
  previous one.
- AppRTC: when the device created the room and sent the offer, the browser's
  answer was dropped for lack of a matching session, so no media flowed.
- `app_webrtc_terminate()` called from an app task raced the WebRTC task's own
  teardown, and a terminate with a live session never finished.
- `signalingMessageReceived()` read uninitialised locals on its NULL-argument
  error path; the same pattern in the SDK's
  `findTransceiversByRemoteDescription()` is fixed upstream (awslabs#2415).
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
- SCTP init stalling 1 s on every boot. usrsctp handed its lwIP calls to the
  tcpip thread even when already on it, then waited for itself (esp_usrsctp bump).

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
