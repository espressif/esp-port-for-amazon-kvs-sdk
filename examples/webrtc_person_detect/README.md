# WebRTC + person detection + LCD preview

One camera, three consumers running in parallel:

```
camera ─▶ pump ─▶ raw bus ─┬─ "h264-enc" passthrough ─▶ H.264 ─▶ KVS WebRTC (start-webrtc)
                           ├─ "detect"   converted ──▶ esp-dl pedestrian detection
                           └─ "preview"  converted ──▶ LCD, with boxes drawn over it

The pump only dequeues and publishes; each sink runs its own task.
```

The two **local** consumers - preview and detection - run from boot and need no network.
WebRTC is an encoded sink and starts from the console; **it does not run by default**, so
the camera, the LCD and the detector all work on a device that has never reached AWS.

The point of the example is the middle row. Person detection takes roughly **60 ms a frame** on the
ESP32-P4 — nearly two frame periods at 30 fps. It runs anyway, on its own task, and **costs the
encoder nothing**: `media_stream`'s raw-frame bus gives each sink its own queue depth, its own cap
on frames in flight, and its own policy for what to drop when it falls behind.

The LCD shows the **local camera**, not a peer's stream. `media_stream`'s inbound decode path is
switched off in this build.

## Why the boxes lag the picture

They will, by about one inference period, and that is by design. The preview sink and the detect
sink are independent tasks running at independent rates, so the canvas draws the most recent
detection *available*, which generally belongs to an earlier frame than the one on screen. Making
them agree would mean holding each frame until inference finished — that is, throwing away the
property the example exists to demonstrate.

## Hardware

**ESP32-P4-EYE.** It is the board in this repo with a working LCD (240x240 ST7789 over SPI) and a
camera on the same die as the encoder. Wi-Fi comes from the on-board ESP32-C6 over SDIO
(`esp_hosted`).

## Build

The P4-EYE overlay sets `CONFIG_SLAVE_FLASHER_ENABLE=y`, so the P4 programs the C6 in-system from a
SPIFFS partition. The C6 images are **not in git** and the build refuses to configure without them:

```bash
cd examples/network_adapter
idf.py set-target esp32c6
idf.py build

DEST=../webrtc_person_detect/target-firmware
cp build/bootloader/bootloader.bin           "$DEST/bootloader.bin"
cp build/partition_table/partition-table.bin "$DEST/partition-table.bin"
cp build/network_adapter.bin                 "$DEST/app.bin"
```

Then the example itself. Pass the overlay chain in the **same** invocation as `set-target` — a bare
`set-target` first would generate an `sdkconfig` whose choice symbols then win over the overlay:

```bash
cd examples/webrtc_person_detect
idf.py -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.defaults.esp32p4;sdkconfig.defaults.p4_eye.esp32p4" \
       set-target esp32p4
idf.py menuconfig      # Wi-Fi credentials + AWS credentials
idf.py -p <PORT> flash monitor
```

The detection model is written to its own flash partition, not linked into the app — the app slot
has nowhere near 435 KB spare. `idf.py flash` writes it along with everything else; there is no
separate step.

### AWS credentials

Same as `examples/webrtc_classic`. The default path is IoT Core certificates: leave
`CONFIG_IOT_CORE_ENABLE_CREDENTIALS=y` and drop your cert and key into
`../common_components/app_common/spiffs_image/certs/`. Set the channel name and region in
`menuconfig` under *AWS Security Credentials*.

## Running

The preview starts as soon as the camera does — it does not wait for a peer, because the example
brings the camera up itself rather than letting the WebRTC session do it. (Capture is reference
counted, so every holder can have it.)

Nothing talks to AWS until you ask it to:

```
start-webrtc                      # then view it from the KVS WebRTC test page
sink-stats                        # who is running, and what each sink is getting
```

`sink-stats` then shows the encoded sink and the two raw sinks side by side: the detector
and the preview keep their own rates off the raw bus whatever the viewer's link is doing.

### Console

| Command | What it does |
|---|---|
| `start-webrtc` / `stop-webrtc` | WebRTC signaling and streaming, at any time |
| `detect on` / `detect off` | Enable or disable inference at runtime |
| `detect-stats` | Per-sink delivery and drop counters, and the last inference time |
| `sink-stats` | The above plus the encoded sinks and the whole rate-control state |
| `sink-stats-reset` | Zero the raw and encoded counters |
| `rate-ceiling <bps\|off>` | Force the WebRTC controller's network ceiling — see below |
| `trigger-offer <peer_id>` | Start an offer to a viewer |
| `wifi-set <ssid> <pass>` | Set credentials without reflashing |
| `mem-dump` | Heap and PSRAM usage |

`detect-stats` is the one worth knowing. Each drop counter names a different cause, because each
calls for a different fix:

| Counter | Meaning |
|---|---|
| `d:rate` | Skipped by the sink's own fps limit. Expected, and large. |
| `d:queue` | The sink fell behind and its oldest queued frame was evicted. |
| `d:flight` | The sink already holds as many frames as it is allowed. |
| `d:nobuf` | The camera's buffer budget is exhausted — by some *other* sink. |
| `d:cvt` | The PPA conversion failed. |

Converted sinks are cheaper than passthrough but not free: the queue holds camera frames and one
more is pinned while the PPA runs. `d:nobuf` therefore means the pool is undersized for the
borrowers actually registered, not that some sink is misbehaving.

## Rate control under congestion

When the uplink cannot carry the stream, `video_rate_ctrl` walks down a ladder of
fps/bitrate rungs. The fps half of that lands here as **this sink's fps limit and nobody
else's**: the capture loop pushes the arbitrated target into the encoder sink with
`video_raw_sink_set_fps_limit()`, and the bus's per-sink gate drops the difference on the
pump. So a congested viewer costs the encoder frames and costs inference and the LCD
nothing — they keep their 5 and 15 fps while `enc_fps` falls.

Three things follow, and they are all visible in `sink-stats`:

- a frame skipped for congestion never enters a queue, never pins one of the four camera
  buffers, and never wakes the encoder task;
- it is counted as `d:rate` **on the encoder's row**, which is what tells "the ladder asked
  for fewer" apart from "the encoder fell behind" (`d:queue`);
- `cap_fps` does not move. If it does, the cause is the camera or the pump, not congestion.

Before the gate moved to the bus it lived inside the encoder callback, where it multiplied
with the bus's own drops instead of capping them: a 15-of-30 target behind a bus delivering
20 produced 10 fps, not 15, and only while congested — so the ladder was steering against a
frame rate that was not happening.

### Testing it

Congestion is forced rather than found. `rate-ceiling` writes the same field TWCC feedback
writes, so the ladder descends for the reason it would descend in the field, on demand and
to a known number:

```
rate-ceiling 300000     # ladder descends, encoder d:rate climbs, detect/preview flat
sink-stats
rate-ceiling off        # recovery
```

A viewer must be connected or there is no controller to write to. The whole cycle, sampled
and reported per sink, is one command:

```bash
tools/s31-rate-control-harness/run_pd_congestion.sh
```

## Configuration

Under *Person Detection Example* in `menuconfig`:

| Option | Default | Notes |
|---|---|---|
| `PERSON_DETECT_FPS` | 5 | Inference rate. The model tops out near 16 fps on P4. |
| `PERSON_DETECT_PREVIEW_FPS` | 15 | LCD refresh rate, independent of inference. |
| `PERSON_DETECT_SCORE_THRESHOLD` | 70 | Percent confidence to report a detection. |
| `PERSON_DETECT_ENABLE_AT_BOOT` | y | Clear it to measure the pipeline without inference. |

Turning inference off and watching `enc_fps:` stay put is the quickest way to see that the bus is
doing its job.

## Reading the two fps lines

The log carries one per second, and the pair is the diagnostic — which of them moved says where to
look:

```
I cap_fps: in=25 (bus_ms=3/1000)      <- the pump: frames dequeued from the camera
I enc_fps: out=25 (sink_ms=142/1000)  <- the encoder sink: frames encoded and sent
```

| Symptom | Where the problem is |
|---|---|
| `cap_fps` down | the camera, the ISP, or something blocking the pump |
| `cap_fps` steady, `enc_fps` down | the encoder, or a sink sitting on camera buffers |
| `bus_ms` large | a sink chose `VIDEO_RAW_OVERFLOW_BLOCK` and is stalling the pump |
| `sink_ms` large | the RTP send is slow — a congested uplink |

Before the pump and the encoder were split these were necessarily the same number, and a drop could
not be attributed to either end.

## How the conversion works

Both new sinks are `VIDEO_RAW_MODE_CONVERTED`. The PPA does colour conversion, cropping and scaling
in a single hardware pass, on the sink's own task, writing into a small buffer the sink owns; the
camera buffer goes back the moment that pass finishes, before the callback runs.

That is cheaper than it sounds — and cheaper than the zero-copy alternative. A 1920x1080 YUV420
frame is 2.97 MiB; the model wants 224x224 RGB565, which is 98 KiB. Handing the model the full frame
would mean pinning one of only four camera buffers for the whole 60 ms inference. Converting instead
moves 3% as much data, uses no CPU, and holds a camera buffer for the 13 ms of the pass instead of
the 60 ms of the inference — which is what keeps four borrowers inside the loan budget.

The crop matters too. `pedestrian_detect` stretches its input to 224x224 without letterboxing, so
feeding it a 16:9 frame would squash people along the axis they are tallest in. The converter takes
the largest centred square instead, in the same pass, for free.

## Files

| File | Role |
|---|---|
| `main/main.c` | App bring-up, camera start, console commands |
| `main/app_ctrl.h` | Start/stop for the WebRTC side, and the one camera profile |
| `main/webrtc_side.c` | WebRTC lifecycle, so it can start and stop at runtime |
| `main/person_detect.cpp` | esp-dl C++ behind a C API — the rest of the example stays C |
| `main/detect_sink.c` | The inference sink (pull mode) and the shared result snapshot |
| `main/preview_display.c` | BSP display bring-up, LVGL canvas, box overlays, preview sink |

## Notes

- `pedestrian_detect` detects one class: person. For other classes you would want `coco_detect`,
  but be aware YOLO11n runs ~2.5 s a frame on P4 at 640x640 — far too slow for a live pipeline.
- Detection boxes are drawn as child LVGL objects over the canvas rather than into its pixel buffer,
  so a preview re-blit does not erase them and they only move when a detection does.
- The panel wants MSB-first RGB565 (`CONFIG_LV_COLOR_16_SWAP=y`). The PPA cannot do that swap for a
  YUV input, so it happens during the copy into the canvas.
