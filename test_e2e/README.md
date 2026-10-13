# End-to-end streaming tests

`test_app/` covers pure functions on the host. These tests do the opposite: they drive
real hardware on real Wi-Fi against a real AWS signaling channel and assert that media
actually flows peer-to-peer. They exist to answer one question quickly — *did this change
break streaming?* — which is what you want when bumping `esp_hosted`, mbedTLS, or IDF.

Covers `webrtc_classic` single-chip and split mode (`streaming_only` on the P4 +
`signaling_only` on the C6). The viewer is shared by both.

## What it checks

`test_e2e_streaming`:

1. Device boots and raises `WebRTC Initialized`.
2. Wi-Fi connects, driven over the existing `wifi-set` console command — no rebuild to
   change AP, and no dependence on BLE provisioning.
3. The signaling state machine walks **every** state in order, asserted one by one:
   `1 NEW → 2 GET_CREDENTIALS → 3 DESCRIBE → 5 GET_ENDPOINT → 6 GET_ICE_CONFIG →
   7 READY → 8 CONNECTING → 9 CONNECTED` (`SIGNALING_CLIENT_STATE`, `Include.h`).
   `4 CREATE` is skipped for a channel that already exists. A state reached out of
   order, or skipped, fails here.
4. The per-phase timing summary follows, confirming each control-plane call reported a
   duration rather than being skipped.
5. An aiortc viewer joins the channel, offers, and receives media.
6. Device side: answer sent, peer reaches CONNECTED, streaming started.
7. **P2P**: neither end of the selected ICE candidate pair is a TURN `relay`. Pass
   `--allow-relay` on networks that genuinely need TURN.
8. No KVS error events, no liveness-watchdog trips.

`test_e2e_data_channel` opens a data channel and asserts the message round trip, which
exercises SCTP-over-DTLS — a separate transport from RTP that can regress on its own.

## Setup

```bash
pip install -r requirements.txt
export WIFI_SSID=... WIFI_PASSWORD=...
export AWS_ACCESS_KEY_ID=... AWS_SECRET_ACCESS_KEY=... AWS_DEFAULT_REGION=us-east-1
export AWS_SESSION_TOKEN=...       # only for temporary/STS credentials
```

Credentials reach the device through `sdkconfig.creds`, which is **gitignored** and
generated from the environment on first run. Write it by hand instead for the IoT Core
cert flow — the generator leaves an existing file alone:

```
CONFIG_IOT_CORE_ENABLE_CREDENTIALS=y
CONFIG_AWS_IOT_CORE_CREDENTIAL_ENDPOINT="..."
CONFIG_AWS_IOT_CORE_THING_NAME="..."
```

Credentials are deliberately *not* pushed over the console: the REPL caps a command line
at 256 bytes with a 256-byte UART RX buffer, which an STS session token (~1 KB)
overruns and a multi-line PEM cannot use at all. Kconfig takes the 1 KB token happily.

## Build and run

The config overlay lives with the example as `sdkconfig.ci.e2e`, next to the other
`sdkconfig.ci.*` fragments; add your board overlay to the chain too. `sdkconfig.creds`
stays here in `test_e2e/` because it is one secret shared by every example the tests
drive, and the split-mode follow-up needs the same keys from two of them.

The overlay chain must be in the same invocation as `set-target` — `set-target`
regenerates `sdkconfig`, and an existing one otherwise wins for *choice* symbols.

```bash
cd ../examples/webrtc_classic
idf.py -B build_e2e -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.defaults.esp32p4;sdkconfig.defaults.p4_eye.esp32p4;sdkconfig.ci.e2e;../../test_e2e/sdkconfig.creds" \
       set-target esp32p4
idf.py -B build_e2e build flash

cd ../../test_e2e
pytest pytest_webrtc_classic.py --e2e --target esp32p4 \
       --port /dev/cu.usbmodem12101 --build-dir build_e2e -s
```

Note `--port`, not `-p`: pytest's own `-p` loads a *plugin*, and passing a device path
there fails with `No module named '/dev/cu'`. Pass `--build-dir` too whenever you built
into a directory other than `build/`.

### Camera or sample files

The capture source is a compile-time `#ifdef`, so it is chosen by the overlay chain, not
on the command line. By default the hardware test streams from the camera. To stream the
SPIFFS sample frames instead, add one fragment:

```
...;sdkconfig.defaults.p4_eye.esp32p4;sdkconfig.ci.e2e;sdkconfig.ci.filecap;../../test_e2e/sdkconfig.creds
```

Use files when the number you care about is packet loss across an `esp_hosted` or IDF
change — a live camera varies with scene and rate control, so run-to-run comparisons
carry that noise. Use the camera when you want CSI capture and H.264 encode covered;
`filecap` skips both. `sdkconfig.ci.emu` sets it directly, since ESP-EMU models no camera.

Whichever you build, every test logs `capture source: ...` next to its metrics, so a
baseline always records which one produced it and the two are never compared by accident.
The value comes from `<--build-dir>/config/sdkconfig.json` of the binary under test, not
from the example's `sdkconfig`: that file is shared by all of an example's build dirs and
only describes the last configure. The channel check reads the same file, from
`signaling_only/<same build-dir name>` in split mode, so build both halves into
directories of the same name.

Without `--e2e` every test skips, so this directory is inert in normal collection and in
the existing `test_host` CI job.

Options: `--kvs-channel`, `--kvs-region`, `--media-timeout`, `--connect-timeout`,
`--allow-relay`, `--max-video-loss`.

If components resolve from another checkout you have a stale `KVS_SDK_PATH` exported —
`unset KVS_SDK_PATH`. A path that no longer exists now fails configure outright instead
of misbuilding quietly.

## Split mode (ESP32-P4 + ESP32-C6)

`pytest_split_mode.py` covers the two-chip deployment: the C6 runs `signaling_only` and
holds the KVS WebSocket, the P4 runs `streaming_only` and does RTP straight to the
viewer. Media never round-trips through the C6.

It is a **single-DUT** test despite involving two chips. On a one-USB-port kit like
P4-EYE there is no separate C6 console, so it borrows the paths the firmware already
has:

- the C6's log arrives on the P4's console, re-printed with a `[SLAVE]` prefix by
  `slave_flasher`'s monitor task; the P4's own lines carry `[HOST]`. Every assertion is
  anchored on one tag or the other, so a C6 check cannot be satisfied by a P4 line.
- the C6's console is written through the P4's `write_slave <cmd>` command, which is how
  the test sends `wifi-set` to the chip that actually owns Wi-Fi.

Boards with a separate C6 UART (`p4_function_ev_board_*`) could run this as a two-DUT
test instead; the single-port path is the more constrained one, so it is what this covers.

### Build both halves

AWS credentials live on the **C6** — `streaming_only` has no `CONFIG_AWS_*` at all. The
C6 image is delivered by the P4: `streaming_only/CMakeLists.txt` packs
`target-firmware/` into a `slave` SPIFFS partition and `flash_slave()` programs the C6
over the internal UART at boot.

```bash
# 1) C6 half -- signaling_only, with the credentials
cd examples/signaling_only
idf.py -B build_e2e -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.defaults.esp32c6;sdkconfig.defaults.p4_eye_pair.esp32c6;sdkconfig.ci.e2e;../../test_e2e/sdkconfig.creds" \
       set-target esp32c6
idf.py -B build_e2e build

# 2) hand its images to the P4
DEST=../streaming_only/target-firmware
cp build_e2e/bootloader/bootloader.bin           "$DEST/bootloader.bin"
cp build_e2e/partition_table/partition-table.bin "$DEST/partition-table.bin"
cp build_e2e/signaling_only.bin                  "$DEST/app.bin"

# 3) P4 half -- streaming_only, which packs the above into its slave partition
cd ../streaming_only
idf.py -B build_e2e -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.defaults.esp32p4;sdkconfig.defaults.p4_eye.esp32p4;sdkconfig.ci.e2e" \
       set-target esp32p4
idf.py -B build_e2e build flash

# 4) run it
cd ../../test_e2e
pytest pytest_split_mode.py --e2e --target esp32p4 \
       --port /dev/cu.usbmodem12101 --build-dir build_e2e --connect-timeout 180 -s
```

First boot after a C6 image change is slow — the P4 reprogrammes the C6 over UART
(~1.5 MB), which is why `--connect-timeout 180` and the test's own 180 s slave-boot
budget are generous.

### What it adds over the single-chip test

The bridge IPC, which has no analogue in `webrtc_classic`: the offer arrives on the C6,
is queued while the P4 is not yet ready, and is flushed across the link —

```
[SLAVE] Enqueueing OFFER message, waiting for P4 READY signal
[SLAVE] Enqueued message (4372 bytes), queue size: 1/24
[SLAVE] Flushing 1 queued messages to P4
```

and the test asserts `Queue full, dropping newest message` **never** appears, because the
adapter frees flushed messages with no retry — one drop loses a viewer's offer outright.

## Emulator (no hardware)

ESP-EMU models the P4↔C6 SDIO transport between two emulator processes, so the pair can
run with no board attached. `run_emu.sh` brings both up.

Verified working under emulation, with no board attached:

- boot of both images, the C6 enumerating over SDIO and exchanging capabilities with the
  P4 host
- Wi-Fi via the emulated C6 and TLS out to AWS through `--net user`
- the **full signaling walk** 1 → 9 to `Signaling Connected`
- a viewer's offer arriving and the device creating a peer session for it
- **file-based capture feeding the encoder** (`TX H.264 IDR accounting: total=1550`)

**Not working: ICE, so no media.** The emulated device sits at `192.168.4.2` behind the
emulator's user-mode NAT and no candidate pair forms, so the session never reaches
CONNECTED and no answer is sent (`not_connected` climbs in the `DIAG frame_cb` line).
Worth trying next: a routable address via `--net tap` (Linux) or `--net vmnet` (macOS),
or forcing TURN relay. Until then the emulator covers the signaling and bridge half —
which is the half that needs no camera, and the half an `esp_hosted` change is most
likely to break.
Other constraints:

- **No camera or H.264 encoder** is modelled. `CONFIG_MEDIA_STREAM_CAPTURE_FROM_FILE=y`
  streams the 60 H.264 sample frames already in the SPIFFS image instead. It swaps
  video only; audio stays on the real microphone path.
- **Use the plain `esp32p4` chain, not a board overlay.** The emulator's P4 boot stack
  assumes the rev ≥ 3 layout (`CONFIG_ESP32P4_REV_MIN_FULL=301`), while `p4_eye` and
  friends set `ESP32P4_REV_MIN_0` and will not boot there.

### Build and run

```bash
# esp-emu itself (Rust; needs a toolchain new enough for lockfile v4, i.e. 1.78+)
git clone <esp-emulator> && cd esp-emulator && cargo build --release

# C6 co-processor
cd examples/network_adapter
idf.py -B build_emu set-target esp32c6 && idf.py -B build_emu build
(cd build_emu && python -m esptool --chip esp32c6 merge_bin -o merged_flash.bin @flash_args)

# P4 host
cd ../webrtc_classic
idf.py -B build_emu -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.defaults.esp32p4;sdkconfig.ci.emu;../../test_e2e/sdkconfig.creds" \
       set-target esp32p4
idf.py -B build_emu build
(cd build_emu && python -m esptool --chip esp32p4 merge_bin -o merged_flash.bin @flash_args)

# both, bridged
cd ../..
./test_e2e/run_emu.sh start     # logs in /tmp/kvs-emu/{c6,p4}.log
./test_e2e/run_emu.sh logs
./test_e2e/run_emu.sh stop
```

Point `ESP_EMU` at the binary if it is not at `~/work/esp-emulator/target/release/esp-emu`.

### Use an esp-emu with the PSRAM mode-register model

**Build the emulator from `master`, not `feat/hosted-emu-test-coverage`.** The older
branch does not model PSRAM mode-register reads, and that one gap breaks WebRTC
entirely:

```
emulator does not model PSRAM mode-register reads
  -> esp_psram misidentifies the die (vendor UNKNOWN, gen 2, 128 Mbit, Variable latency)
  -> the PSRAM heap is registered without MALLOC_CAP_DMA
  -> esp_aes_dma_core.c's MALLOC_CAP_DMA|MALLOC_CAP_8BIT|MALLOC_CAP_SPIRAM finds no heap
  -> "Failed to allocate memory for start alignment buffer"
  -> libsrtp's AES self-test fails -> srtp_init() fails
  -> initKvsWebRtc() returns 0x5b000005 (STATUS_SRTP_INIT_FAILED, PeerConnection.c:1941)
```

It looks like a crypto or memory-pressure problem and is neither — it fails with ~405 KB
internal and 16 MB PSRAM free, because it is a capability mismatch, not capacity.
Allocating that exact combination at boot returns PSRAM on hardware (`0x48016a40`) and on
emu master (`0x48016200`), and NULL on the old branch. On master the emulator reports the
die exactly as a P4-EYE does — vendor `0xd` (AP), generation 4, 256 Mbit, Fixed latency —
and hardware AES runs clean, so `sdkconfig.ci.emu` leaves it enabled.

Known remaining gap: on emu master the P4 currently stalls during ESP-Hosted SDIO
bring-up (`H_SDIO_DRV: Write thread started`, no bridge traffic), so the full split-mode
run is not yet reproducible there. The bridge worked on the older branch, so this looks
like a regression or an interface change between the two.

## Debugging the viewer alone

`kvs_viewer.py` runs standalone against any channel, which separates "the viewer is
wrong" from "the device is wrong":

```bash
python kvs_viewer.py --channel ESP32Channel --region us-east-1 --duration 20 --data-channel -v
```

Exits non-zero if no video RTP arrived.

## Example baseline (IDF 5.5)

Example figures from passing runs, to show what a healthy session looks like. They
depend on the network, chip revision, sdkconfig and IDF version, so compare a change
against your own baseline on the same setup, not against these. Rates, not totals —
totals scale with `--media-timeout`:

| | webrtc_classic | split mode (p4_eye) | split mode (P4+C5 core) |
|---|---|---|---|
| selected candidate pair | `host` / `host` | `host` / `host` | `host` / `host` |
| effective video fps | 4.0 | 11.8 | **7.2–30.1** |
| first decoded frame | ~18 s | ~12 s | **~11–14 s** |
| device send rate | `send_v_fps` 14–23 | `send_v_fps` 16–25, `fail=0` | — |
| audio | yes | yes | **no codec on this board** |
| data channel | opens and echoes | not covered yet | not covered yet |

Split mode decodes at roughly three times classic's rate and starts sooner, which fits
the architecture: the P4 talks RTP straight to the viewer instead of the media sharing
the hosted link with signaling. Keep that in mind before comparing classic numbers
against split ones.

Single-chip detail:

| | observed |
|---|---|
| signaling walk to CONNECTED | ~13 s from boot |
| selected candidate pair | `host` / `host` — direct, no TURN |
| device send rate | `send_v_fps` 14–23, `send_a_fps` 23–46, `fail=0` |
| first decoded video frame | ~18 s after the offer |
| data channel | opens and echoes |

Packet loss is logged on every run (`inbound video: N packets, M lost`), counted per
SSRC; record it for your setup before relying on `--max-video-loss`. The device's own
`stats: STA: h2s{in(flowctrl_drop[N] ...)}` counter is the esp_hosted-side number to
watch. ~18 s to the first frame is slow; the cause is not established.

The test asserts on *packets* and only logs decoded frames: aiortc's H.264 decoder
rejects some valid streams, so frames alone would make it flaky.

## ESP-IDF version

Both suites pass on **IDF 5.5 and IDF 6.0** (esp32p4 / p4_eye). IDF 6 is markedly better
on the media path — on otherwise identical runs it decoded ~2900 video frames against
~140 on 5.5, with the first frame arriving at ~9 s instead of ~18 s. Worth keeping in mind when comparing numbers: **always compare within
one IDF version.**

Pick the IDF by pointing `IDF_PATH` at the right checkout in the same command as
`idf.py`; note that `export.sh` honours an already-set `IDF_PATH`, so sourcing a
different version's `export.sh` on top of an existing environment silently keeps the old
one. Check `idf.py --version` if a result looks surprising.

## Known flakiness

Measured over four consecutive runs of `test_e2e_streaming` on the same board and AP:
**3 passed, 1 failed**. The failure was `device never answered the offer` — the device
was connected to signaling (state 9) but never logged `Received Offer`, so AWS did not
deliver it. A likely contributor is that `ESP32Channel` is the default channel name for
*every* example in this repo, so any other board on the account joins the same channel
as a competing master. Use a channel of your own via `--kvs-channel` (and rebuild, since
the device's channel is compiled in — `conftest.py` fails early if the two disagree).

Both tests pass individually and have passed together, but running them back to back in
one invocation is also not fully reliable: the second viewer can get `no SDP_ANSWER received`
while the device is still tearing down the first peer (it only notices a viewer left
when its own ICE timeout fires — see the power-save note in the split-mode test). The
office AP used for these runs also drops connections periodically (`Host is unreachable`,
`ESP_ERR_ESP_TLS_FAILED_CONNECT_TO_HOST`), which signaling recovers from by reconnecting
but which can still eat a session.

If a run fails at the viewer stage with `answer=False` and `remote_cands=0`, re-run that
single test before investigating:

```bash
pytest pytest_webrtc_classic.py::test_e2e_streaming --e2e ...
```

Making the tests fully independent of each other means either a reset between them or
waiting out the peer teardown; neither is done yet.

## Known issues found by this test

- `[KVS Event] Sent Answer.` is logged **before** the WebSocket write is confirmed. On a
  failed write the device reports a message it never sent. Do not treat that line as
  proof of delivery.
- **The receive-path video player costs ~50 KB of internal RAM even when it fails to
  start.** On p4_eye `bsp_display_new` fails (`SPI init failed`) so there is no display,
  but the tinyh264 decoder is allocated anyway. Internal RAM then bottoms out at
  **~20 KB free with only ~13 KB contiguous**, and `esp-aes` cannot get the contiguous
  `MALLOC_CAP_DMA|MALLOC_CAP_INTERNAL` block it needs for its DMA descriptor array —
  which is what broke `createRtcCertificate()` and the TLS write carrying the SDP answer.

  With `CONFIG_MEDIA_STREAM_ENABLE_VIDEO_PLAYER` off the floor is **~66 KB free /
  ~31 KB contiguous** and hardware AES runs clean, so `sdkconfig.ci.e2e` keeps hardware
  AES enabled. Measured with `mem-dump` across a full session:

  | metric (internal heap) | player on (failed) | player off (current) |
  |---|---|---|
  | min-ever free | 12.4 KB | **66 KB** |
  | worst contiguous block | 13.3 KB | **31 KB** |
  | `esp-aes` failures | 26 | **0** |

  Watch **min-ever free and worst contiguous block**, not current free — current free
  swings by ~300 KB per session as normal working set and says nothing about safety.

  There is **no leak**: `video_player_adapter.c`'s failure path closes and deletes the
  decoder, and `esp_h264_dec_del()` calls `h264bsdShutdown()` + `h264bsdFree()`. The
  ~160 KB is simply held internally while the player lives, because tinyh264 ships as a
  prebuilt `libtinyh264.a` that allocates with libc `malloc` and exposes no allocator
  hook — and `sdkconfig.defaults` selects `CONFIG_SPIRAM_USE_CAPS_ALLOC`, under which
  `malloc()` never returns PSRAM. The player's own buffers are already external
  (`FRAME_ALLOC_CAPS`); only the decoder is internal.

  `examples/webrtc_classic/sdkconfig.ci.playermem` fixes that without touching the
  prebuilt library, by selecting IDF's own default for the `SPIRAM_USE` choice:

  ```
  CONFIG_SPIRAM_USE_MALLOC=y
  CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=4096
  ```

  Measured with the player back **on** and hardware AES **on**:

  | | player on, CAPS_ALLOC | player on, USE_MALLOC |
  |---|---|---|
  | min-ever internal free | 12.4 KB | **46.6 KB** |
  | worst contiguous block | 13.3 KB | **31 KB** |
  | `esp-aes` failures | 26 | **0** |

  So the player and hardware AES can coexist. It is not a complete move — baseline
  internal free still falls 366 → 316 KB, since the 4096-byte threshold leaves
  tinyh264's many small allocations internal. Switching the whole project to
  `SPIRAM_USE_MALLOC` is a global allocation-policy change (PSRAM is slower), so it is
  offered as an overlay rather than flipped in `sdkconfig.defaults`.
- **The device takes its ICE credentials from the wrong m-line.** aiortc gives every
  m-line its own `ice-ufrag`/`ice-pwd` even while offering `a=group:BUNDLE 0 1`, and
  the KVS master signs its connectivity checks with the credentials of the **last**
  m-line it parses while bundling onto the **first** m-line's transport. Every check
  it sends then carries a password no surviving connection holds, the viewer answers
  `400 Bad Request`, and ICE fails to nominate with
  `STATUS_ICE_FAILED_TO_NOMINATE_CANDIDATE_PAIR` (`0x5a000013`) — even though the
  reverse direction works, so the viewer briefly reports `completed` before consent
  expires. Observed as `OFFER a=ice-ufrag:0Q6f` (video) / `EyAm` (audio) against the
  device sending `USERNAME='EyAm:AlXN'` while the surviving connection held `0Q6f`.

  `_offer_sdp()` works around it by publishing one credential set across the bundle
  group, which is what BUNDLE means anyway. Without that, split mode never streams.
  Worth fixing in the SDK: the bundle-tag m-line's credentials are the correct ones.
- **aiortc's `getStats()` reports one SSRC per receiver.** Every inbound-rtp entry of a
  receiver shares an id, so once the device retransmits on its RTX stream that entry
  replaces the media stream's. Observed: 126 packets reported against 2957 received.
  The viewer reads each receiver's SSRCs directly, excluding RTX, and reports
  retransmissions separately as `video_rtx`.
- A viewer that does not advertise `a=ice-options:trickle` puts the device in
  non-trickle mode, where it withholds the answer until its own gathering finishes —
  20 s when TURN is unreachable (`403 Forbidden IP`). aiortc cannot trickle, so the
  viewer advertises trickle anyway and lets the device trickle to it.
- `CONFIG_ESP32P4_SELECTS_REV_LESS_V3`, set by several board overlays, is declared in no
  `Kconfig` and is silently ignored.

## Notes

- There is no hardware runner in CI (every `.gitlab-ci.yml` job is `tags: [build]`), so
  this is a local test for now. Wiring it into CI needs a runner with a board, internet,
  and AWS secrets.
- `sdkconfig.ci.e2e` turns BLE provisioning **off** — `webrtc_classic`'s own defaults
  enable it, which would park an unattended boot waiting for a phone.
- The P2P assertion reads the SDK's `Selected pair ...` line. It is `DLOGP`, which maps
  to `ESP_LOG_ERROR`, so it survives the KVS logger's default WARN gate with no extra
  config — it appears as an `E (...)` line despite not being an error.
- The C6's logs are interleaved into the P4 console with a `[SLAVE]` prefix.
