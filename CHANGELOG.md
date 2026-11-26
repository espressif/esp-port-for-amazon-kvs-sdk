# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Changed

- SNTP time sync moved from `esp_webrtc_utils` to the examples' `app_common`, as
  `app_time.h`. `esp_webrtc_time_sntp_time_sync_and_wait()` and `_no_wait()` are now
  `app_time_sync_and_wait()` and `app_time_sync_no_wait()`.
- `esp_webrtc_utils` no longer depends on `webrtc_bridge`, esp_hosted or `mqtt`.
- `media_stream` no longer depends on `esp_webrtc_utils`, so a camera-only build gets no
  networking or console components from it.
- `app_webrtc` no longer depends on `webrtc_bridge`. Single-chip builds such as the S3
  `webrtc_classic` no longer resolve esp_hosted.
- The P4 co-processor time client is now an opt-in call,
  `app_time_sync_from_coprocessor()`, built when the project has `webrtc_bridge`. It
  replaces `esp_webrtc_time_register_bridge_cmd_handlers()` and the `GET_TIME` request
  inside time sync. No example calls it; `streaming_only` shows where to.
- `app_time` drops the code for IDF older than 5.0. The minimum supported IDF is 5.3.

## [1.0.0-rc1] - 2026-11-23

### Repository structure

- Removed the `components/esp_hosted` submodule (esp-hosted-mcu 2.x). Every example
  takes esp_hosted 3.x from the component registry. A clone made before this keeps
  the old tree as an untracked directory. Remove it with
  `rm -rf components/esp_hosted .git/modules/components/esp_hosted`.
- Removed the `network_coprocessor` component. It wrapped the slave firmware of the
  in-tree esp_hosted 2.x submodule, and no example uses it since `network_adapter`
  and the split examples take esp_hosted 3.x from the component registry.
- Migrated ESP-IDF port from the `beta-reference-esp-port` branch of
  `awslabs/amazon-kinesis-video-streams-webrtc-sdk-c` into this standalone
  repository. The upstream SDK is now a git submodule with platform patches
  applied via `git am patches/*.patch`.
- Moved the webrtc-c submodule to the `develop` commit of awslabs#2146: release
  **1.20.0** plus these upstream changes:
  - the relay candidate filter (awslabs#2388) and the TURN rejected-peer timeout
    fix (awslabs#2387);
  - the rtpmap payload-type fix for 32-bit platforms (awslabs#2405);
  - mbedTLS: no cipher list on any build (awslabs#2409), the DTLS fingerprint through
    `mbedtls_md()` (awslabs#2392) and the RNG from PSA on mbedTLS 4 (awslabs#2385);
  - srflx candidates from a pre-resolved STUN address (awslabs#2410), one STUN transaction
    ID per srflx candidate (awslabs#2435) and the STUN cache re-arm start time (awslabs#2407);
  - RTX retransmissions with their own TWCC sequence number (awslabs#2418), and
    `transceiverOnFrameDropped()` (awslabs#2438);
  - `PREFER_DYNAMIC_ALLOCS` (awslabs#2146): the signaling payload, URL and JSON buffers and the
    TURN channel-data buffer on the heap. It replaces that part of the local patch 0001.
  - signaling clock skew correction on the WSS connect (awslabs#2422), and CMake 4 builds
    (awslabs#2442);
  - the `pSeenTransceivers` initialisation (awslabs#2415);
  - negotiated data channels with an explicit stream id (awslabs#2399) and
    `iceServersFinal` (awslabs#2400);
  - signaling: `freeSignalingClient()` during a reconnect (awslabs#2401), the lws
    service-loop wake after a send (awslabs#2417), and fetch, disconnect and delete
    refused during a free (awslabs#2425). The ESP signaling port (`Signaling_esp.c`,
    `LwsApiCalls_esp.c`) follows awslabs#2401 and awslabs#2425: it refuses a free from its own
    tasks (the esp_websocket task, the work queue), and waits for queued message
    jobs before it frees the client.
  - For awslabs#2401's check, `GETTID()` now returns a per-thread id (the task handle on
    chip, `pthread_self()` on the host). It was a constant, so every free while a
    signaling thread ran was refused. A refused free now leaves the client and
    everything it uses allocated, instead of freeing them under it.

  1.20.0 includes the SDP-renegotiation flow (awslabs#2214) and opt-in mbedTLS 4
  support (awslabs#2325). The local patch set is listed in `patches/README.md`.

### Added

- `CONFIG_KVS_WEBRTC_VIDEO_QUEUE_DEPTH` sets how many encoded video frames can
  wait for the WebRTC sender (2–16, default 3). Raise it for sources whose sends
  can stall for longer than depth / fps.
- **Headless video player** (`CONFIG_MEDIA_STREAM_PLAYER_HEADLESS`): the receive
  path decodes inbound H.264 without starting a display, and counts the decoded
  frames. It lets the player run under ESP-EMU, which models no display
  (`webrtc_classic/sdkconfig.ci.player_headless`). CI builds it on its own and in
  the emulator chain.
- **P4 core boards and the P4X-C5 Function EV Board:** board overlays for the ESP32-P4
  core boards with a C5 or C6 co-processor, and for the ESP32-P4X-C5 Function EV Board.
- **Host deep sleep with co-processor light sleep:** with `CONFIG_EXAMPLE_ENABLE_POWER_SAVE`,
  the `streaming_only` host deep-sleeps when no viewer is connected. The `signaling_only`
  co-processor stays in light sleep, holds the Wi-Fi link and the signaling session, and
  wakes the host for an offer or for traffic addressed to it.
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
  measured baseline.

  Split mode (P4 + C6) is covered too, as a single-DUT test: the C6's log is read off
  the P4's console via the `[SLAVE]` prefix and its console is driven through the P4's
  `write_slave` command, so a one-USB-port kit needs no ESP-Prog. It additionally
  asserts the bridge offer handoff and that the adapter never drops a queued message.

  The split pair also runs under ESP-EMU with no hardware (`--emu`): the full KVS path
  from bring-up to a viewer's answer, and the capture path alone through
  `streaming_only`'s `media-selftest` command. `apprtc_viewer.py` checks media without
  AWS against `esp_camera` in a fixed AppRTC room. CI builds the e2e and emulator
  overlay chains and collects the suite; it runs no test that needs hardware or AWS.

- **File capture as a WebRTC source.** The file source publishes to the sink registry,
  paced to the frame interval, and reads one packed file per medium
  (`samples/video.pack`, `audio.pack`; `tools/pack_samples.py`) instead of one file per
  frame. `streaming_only` selects audio and video sources independently.

- **ESP-IDF v6 support** — builds against IDF v6.0/v6.1 (mbedTLS 4 / TF-PSA-Crypto)
  alongside v5.5. The SDK crypto adapts to mbedTLS 4 through PSA, the port
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

- **PROFILE logs:** the new `CONFIG_KVS_PROFILE_LOG` option prints the SDK's PROFILE
  lines (session setup timings, the selected candidate pair) at INFO. By default they
  print at DEBUG. The e2e overlays turn it on, because the P2P check reads the
  `Selected pair` line.
- **WebSocket buffer:** `signaling_only`, `webrtc_classic` and `webrtc_person_detect`
  set `CONFIG_WS_BUFFER_SIZE` to 4096. The signed signaling URL with an STS session
  token is about 2.2 KB, and the 2048-byte buffer could not hold the handshake request.
  Existing projects: set `CONFIG_WS_BUFFER_SIZE=4096` in `sdkconfig`, or regenerate it.
  Apps that use `kvs_signaling` with `esp_websocket_client` need the same.
- **Split-mode Wi-Fi buffers on the C6:** `signaling_only` on the C6, and every
  `streaming_only` overlay for a C6 pair, use 10 static RX and 12 static TX Wi-Fi
  buffers (16 and 16 before). The C6 has no PSRAM, and these buffers live in its
  internal RAM. With 16 and 16, a C6 that also runs an MQTT client (the RainMaker
  split cameras) could not forward an offer to the P4 (`ESP_ERR_NO_MEM`). Both sides
  change together, so the co-processor still sees the host's own Wi-Fi config and
  does not re-initialise Wi-Fi. Regenerate a saved `sdkconfig` on both the P4 and the
  C6. If only one side changes, the C6 re-initialises Wi-Fi at every host boot and
  wake.
- **Split-mode host sleep:** `streaming_only` deep-sleeps the host 1 s after the last
  session ends (was 10 s). A session that fails before it connects now starts the
  timer too, so the host no longer stays awake until the next viewer. The boot idle
  timer stays at 20 s. An offer that arrives while no session is live restarts the
  20 s timer, so the host stays up while that viewer negotiates.
- **`app_webrtc` events:** `APP_WEBRTC_EVENT_STREAMING_STARTED` and
  `APP_WEBRTC_EVENT_STREAMING_STOPPED` are raised once per session, as a pair. A
  re-offer on a live session raised `STREAMING_STARTED` again, and a session whose
  offer was rejected before media started raised `STREAMING_STOPPED` alone, so an
  app that counted sessions from these events could miscount. A session that has
  ended does not start again. `APP_WEBRTC_EVENT_RECEIVED_OFFER` is now raised when
  an offer arrives; it was declared but never raised.
- **`signaling_only` bridge commands:** the signaling bridge adapter initialises
  `bridge_cmd` itself. An app no longer has to call `bridge_cmd_init()` before
  `app_webrtc_init()`.
- **Split-mode Wi-Fi init config:** the `streaming_only` board overlays send the
  co-processor's own Wi-Fi init config: Enterprise support off, no cache TX buffers
  on the C6 boards, and 10 static RX buffers on the P4X-C5 boards. The co-processor
  no longer stops and re-initialises Wi-Fi at every host boot and wake. Regenerate
  a saved `sdkconfig` to pick this up. The P4X-C5 core board's C5 has no PSRAM and
  still differs on the TX buffer type, so it re-initialises Wi-Fi as before.
- **LVGL 9:** `media_stream` and the examples require `lvgl ^9`, so an app on LVGL 8
  can no longer resolve `media_stream`. Overlays drop `CONFIG_LV_COLOR_16_SWAP` (the
  BSP sets esp_lvgl_port's `swap_bytes`), rename `CONFIG_LV_DISP_DEF_REFR_PERIOD` to
  `CONFIG_LV_DEF_REFR_PERIOD`, and set `CONFIG_LV_USE_CLIB_MALLOC=y` to avoid LVGL 9's
  static 64 KB internal pool. An overlay that still sets the old refresh key falls back
  to LVGL's default period without a warning. Regenerate a saved `sdkconfig`.
- `app_wifi_prov_init()` accepts a default event loop the app created first.
- **Co-processor flash layout:** `network_adapter` puts its partition table at
  `0xC000` and `ota_0` at `0x20000` on every target, like `signaling_only` and the
  RainMaker split cameras. The P4-EYE and P4X-EYE overlays of `esp_camera`,
  `webrtc_classic` and `webrtc_person_detect` flash it there. This fixes
  `webrtc_classic` on the P4-C5 core board, whose overlay already used `0xC000` and
  `0x20000` and so wrote `network_adapter` to the wrong offsets. The overlays no longer
  set the flasher addresses, because `slave_flasher`'s defaults match every image.
  A co-processor reflashed over UART with the new layout loses the Wi-Fi credentials
  in its NVS once, so the board must be provisioned again. Hosted OTA cannot cross this
  change: the new app reads its partition table at `0xC000`, so on a co-processor with
  the old layout it finds no NVS and aborts at every boot. Reflash the co-processor over
  UART once. The M5Stack Tab5's C6, which only hosted OTA can update, keeps the old
  layout.
- **Co-processor PSRAM:** `network_adapter` enables PSRAM on the chips that support it
  (ESP32, ESP32-S3, ESP32-C5, ESP32-C61), and Wi-Fi and lwIP allocate from it. On the
  P4-C5 core board, BLE provisioning of a standalone example no longer reboots the C5.
  With everything in internal RAM, about 16 KB was left while BLE advertised, and its
  BLE controller asserted on the first large packet from the host. A module without
  PSRAM still boots.
- Session setup prints fewer lines at the default log level. The setup details
  (ICE servers, offer and answer, session count) print with
  `app_webrtc_set_log_level(3)`. The SDK PROFILE timings print at DEBUG under the
  `kvs_profile` tag (`KVS_PROFILE_LOG_TAG`). `app_webrtc_set_log_level(8)` turns
  them on. Log lines no longer show the TURN username.
- **esp_hosted 3.x:** `network_adapter`, `webrtc_classic`, `esp_camera`,
  `webrtc_person_detect` and the WebRTC bridge use the registry component
  `espressif/esp_hosted` 3.x in place of the in-tree 2.x submodule. ESP-IDF 5.5 is the
  minimum.
- **Split mode on esp_hosted 3.x:** `signaling_only` and `streaming_only` use esp_hosted
  3.x and its network split.
- **Co-processor console without the flasher:** `CONFIG_SLAVE_CONSOLE_ENABLE` forwards the
  co-processor log and registers `write_slave` on its own. It replaces
  `CONFIG_SLAVE_FLASHER_POST_FLASH_MONITOR`, and `sdkconfig.rename` maps the old name.
  `write_slave` now needs `CONFIG_SLAVE_CONSOLE_ENABLE=y`. A project that set
  `CONFIG_SLAVE_FLASHER_POST_FLASH_MONITOR=n` loses it. After flashing, the host
  releases the co-processor UART, so without the console there is no line to send on.
- `KVS_SDK_PATH` pointing at a directory that does not exist (or one with no
  `components/kvs_webrtc`) is now a configure-time `FATAL_ERROR` instead of a warning
  that scrolled past while every `kvs_*` component silently resolved elsewhere.
- `slave_flasher`: missing or empty co-processor images fail at build time;
  configure and `menuconfig` work without them.
- **`APP_WEBRTC_EVENT_SENT_ANSWER`** is raised once signaling has sent or
  buffered the answer, not before the send. A failed answer raises
  `APP_WEBRTC_EVENT_SIGNALING_ERROR` with the peer id instead. Apps that act on
  `SENT_ANSWER`, such as the power-save handler stopping its park timer, now see
  it only for an answer that goes out or will be re-sent. See `API_USAGE.md` for
  a buffered answer that never goes out.
- The ws-off (`CONFIG_USE_ESP_WEBSOCKET_CLIENT=n`) signaling path uses the upstream
  libwebsockets callbacks and send path. See `patches/README.md`.
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

- ESP32 and ESP32-S3 overlays of `signaling_only`, `simple_video_server`, `esp_camera` and
  `webrtc_classic` turn esp_hosted off with `CONFIG_ESP_HOSTED=n`. They used
  `CONFIG_ESP_HOSTED_ENABLED`, which esp_hosted 3.x does not have, so an example with no
  setting in its base `sdkconfig.defaults`, such as `signaling_only` on the ESP32-S3, built
  with esp_hosted on. On the S3 its SDIO host takes GPIO 19, the USB D- pin, and the
  USB-Serial/JTAG console stops.
- **TURN over TLS (`turns:` servers) never worked on ESP.**
  - The ESP TLS layer (`port/Tls_esp.c`) was a stub. It never started a
    handshake, and it would have sent data in plaintext, so every TLS TURN
    allocation timed out.
  - Normal networks hid this because ICE used UDP or host candidates. Behind a
    UDP-blocking firewall, viewers got the answer but no media.
  - It is now the SDK's mbedTLS code, verifying the server against the ESP-IDF
    certificate bundle, so TURN over TLS needs no CA file on the device.
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
- `app_webrtc_terminate()` never finished on ESP and gave up after 45 s.
  - usrsctp's two rwlocks were never created: ESP-IDF rejects the attribute
    object they were initialised with. SCTP teardown then waited forever for a
    try-lock that could not succeed (esp_usrsctp bump to 0.9.5.0-247-esp.4).
    The locks also never locked anything until now. A failed init of these
    locks retries on their first use, and a re-init after a terminate starts
    from a clean handle.
  - The peer connection client's free then took the stats lock after freeing it.

  Terminate now completes instead of timing out after 45 s (about 15 s with
  the network up, most of it the signaling disconnect timeout).

### CI and testing

- GitHub Actions builds 13 example and target combinations on ESP-IDF v5.5 in
  `espressif/idf` containers. It also runs the host unit tests and builds the
  Linux-target examples (`linux_test`, `esp_camera`).
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

- Acoustic Echo Cancellation in the audio send path.

[Unreleased]: https://github.com/espressif/esp-port-for-amazon-kvs-sdk/compare/v1.0.0-rc1...HEAD
[1.0.0-rc1]: https://github.com/espressif/esp-port-for-amazon-kvs-sdk/releases/tag/v1.0.0-rc1
