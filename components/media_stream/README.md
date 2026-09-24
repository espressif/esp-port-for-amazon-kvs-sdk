# Media Stream Component

This component provides unified media interfaces for the Amazon Kinesis Video Streams WebRTC SDK for ESP-IDF. It abstracts hardware-specific implementations and provides a consistent API for video capture, audio capture, video playback, and audio playback.

## Overview

The Media Stream component serves as the hardware abstraction layer between the WebRTC SDK and ESP32 media devices. It handles:

- **Video Capture**: Camera frame capture and H.264 encoding
- **Audio Capture**: Microphone audio capture and Opus encoding
- **Video Playback**: H.264 frame decoding and display
- **Audio Playback**: Opus frame decoding and audio output

## Core Components

### Video Components
- **`H264FrameGrabber`**: Captures camera frames, encodes using H.264 encoder, and queues encoded frames
- **`video_player_adapter` + `video_render_display`** (receive path): Decodes inbound H.264 with the `esp_h264` software decoder and renders to an LVGL canvas on the BSP's display. Off by default; enabled via `CONFIG_MEDIA_STREAM_ENABLE_VIDEO_PLAYER=y`. See the [Video Player (receive path)](#video-player-receive-path) section.

### Audio Components
- **`OpusFrameGrabber`**: Records I2S audio data, encodes using Opus encoder, and queues encoded frames
- **`OpusAudioPlayer`**: Decodes Opus frames and plays through I2S audio output using ring buffer

## API Interfaces

The component provides four main interface types:

### Video Capture Interface (`media_stream_video_capture_t`)
```c
media_stream_video_capture_t *video_capture = media_stream_get_video_capture_if();
// Provides: init, start, stop, deinit (all reference counted)
//           get_frame, release_frame  -- MJPEG only, deprecated; see below
```
Encoded H.264 frames are delivered through `video_sink.h`, not pulled — see
[Multiple video consumers](#multiple-video-consumers).

### Audio Capture Interface (`media_stream_audio_capture_t`)
```c
media_stream_audio_capture_t *audio_capture = media_stream_get_audio_capture_if();
// Provides: init, start, stop, get_frame, release_frame, deinit
```

### Video Player Interface (`media_stream_video_player_t`)
```c
media_stream_video_player_t *video_player = media_stream_get_video_player_if();
// Provides: init, start, stop, play_frame, deinit
```

### Audio Player Interface (`media_stream_audio_player_t`)
```c
media_stream_audio_player_t *audio_player = media_stream_get_audio_player_if();
// Provides: init, start, stop, play_frame, deinit
```

## Multiple video consumers

More than one consumer can receive the encoded video stream at the same time, and
each can be started and stopped independently. There is one way in: register a
sink.

### Encoded-frame sinks (`video_sink.h`)

Register a callback and get every encoded frame:

```c
static void on_frame(const video_frame_t *frame, void *user)
{
    /* BORROWED: valid only for this call. Copy what you need and return. */
}

video_sink_handle_t sink;
const video_sink_config_t cfg = { .name = "myapp", .on_frame = on_frame };
video_sink_register(&cfg, &sink);
video_sink_set_enabled(sink, true);   /* independent on/off, any time */
```

Sinks are dispatched **sequentially on the encoder task**. Nothing is queued per
sink, so nothing can overflow — but all sinks share one per-frame budget (33 ms at
30 fps) with the encoder itself, and a slow callback lowers the capture rate for
everyone. Copy and return; no network I/O, no blocking waits.

Every sink always **starts on a keyframe**: on enable its gate is armed and frames
are withheld until the next `VIDEO_FRAME_TYPE_I`. This is not optional, because a
mid-GOP P-frame decodes against reference frames the consumer never received.
No IDR is requested, so the wait is at most one GOP.

Because the contract is timing-sensitive, it is measured rather than assumed:

- `video_sink_get_stats()` reports per-sink `max_us` / `avg_us` and `gate_held`.
- A callback slower than `CONFIG_VIDEO_SINK_SLOW_WARN_US` (default 5 ms) is named
  in a rate-limited warning.
- The grabber's 1 s `enc_fps:` line carries `sink_ms=<spent>/<window>`.

### Adaptive rate control (`video_rate_ctrl.h`)

Opt-in, and **one controller per sink** rather than one per system:

```c
video_rate_ctrl_handle_t rc;
video_rate_ctrl_create("myapp", &rc);
video_rate_ctrl_enable(rc, true);          /* only from a path that also reports */

/* whichever of these matches how your transport congests */
video_rate_ctrl_report_send_ms(rc, ms);            /* real-time: a slow send   */
video_rate_ctrl_report_queue(rc, used, capacity);  /* buffered: a filling queue */
```

Each controller walks its own 8-rung ladder of (fps, bitrate) pairs — congestion
steps down, sustained health steps back up one rung. Tying the two together means
an operating point is always a combination someone chose, instead of two counters
drifting apart.

```
encoder fps = min(target fps),  encoder bitrate = min(target bitrate)
```

Worst sink wins. Overshooting the slower consumer corrupts its stream rather than
just degrading it, so the arbiter cannot do better than the most constrained sink.

Two signal shapes, one ladder, because they are not interchangeable:

- **Send duration** is a *rate* signal and self-normalising — dropping fps widens
  the per-frame budget, so the same send time reads as less congested and the
  descent arrests itself.
- **Queue occupancy** is a *level* signal and does not self-normalise. Lowering
  fps slows the fill, but the existing backlog still has to drain. It is therefore
  evaluated on a fixed slow cadence with a trend term, and only counts as healthy
  when the queue is both shallow *and* not growing.

Enabling a controller without a *working* signal is worse than not enabling it.
The reports are the only thing that lets a controller climb back up, so a signal
that always reads healthy produces a controller that only ever climbs — it will
ratchet to the top rung and stay there, whatever the link is doing. Check that the
signal actually moves before turning a controller on; the WebRTC sink is created
disabled for exactly this reason. A controller that goes silent for 5 s is marked
stale and dropped from the minimum — it keeps its rung, so it resumes where it
left off — which is what stops a wedged sender from pinning the encoder.

The ladder has two columns and they are not equally available. fps is only a lever
upstream of an encoder we own: the arbitrated target becomes the encoder sink's
`fps_limit`, and the raw bus drops the **raw** frame on the pump, before encode. Where the camera hands us already-encoded H.264, dropping a frame
corrupts everything up to the next IDR, so `video_rate_ctrl_init()` is told
`fps_actuable = false`, every rung reports the native rate, and the stats show no fps
reduction — because there is none. On UVC H.264 passthrough the camera owns bitrate and
keyframes too, so `video_rate_ctrl_init()` is not called at all and every controller
stays inert; do not read an inert controller as a working one.

`video_rate_ctrl_get_stats()` reports every controller plus the arbitrated result,
which is what an application's `sink-stats`-style console command prints.

### Lifecycle

`video_capture_init/start/stop/deinit` are reference counted, so several
consumers can hold the camera at once: it comes up for the first and is torn down
after the last. The first caller's `video_capture_config_t` sets the profile and
later callers join it — a mismatch is logged, not silently applied.

`examples/webrtc_person_detect` runs three consumers against one camera — WebRTC,
person detection and an LCD preview — each start/stoppable from the console.

> **Note on the file-based source.** `media_stream_get_file_video_capture_if()`
> serves `.h264` files from SPIFFS through the pull API, and
> `examples/streaming_only` can select it with its `USE_FILE_STREAM` toggle
> (compiled out by default). Since consumers now take frames from the sink
> registry, that toggle no longer feeds WebRTC — the file source produces nothing
> into the registry. Making it a sink producer (a small paced task calling the
> internal dispatch) is the fix if the path is wanted again.

### Raw frames (`video_raw_sink.h`)

Where the encoded registry above fans out compressed frames, this one fans out the
camera's raw output — to any number of consumers, running in parallel, sharing
buffers by reference rather than by copy.

The capture task is a **pump**: dequeue a frame, publish it, go back for the next.
Everything that does work, the H.264 encoder included, is a sink on its own task.
A camera buffer returns to the driver when the last holder releases it, not when
the pump moves on.

**The property this guarantees:** a slow sink cannot lower anyone else's frame
rate. A model doing 60 ms of inference does not cost the encoder a frame. It is
bought with two per-sink dials — queue depth, and what to discard when the sink
falls behind (`DROP_OLD` / `DROP_NEW` / `BLOCK`).

Queue depth is the only number: a sink may have `queue_depth + 1` frames
outstanding — depth staged plus the one being worked on — and that same figure is
its claim on the camera pool, summed across sinks and checked at registration.
A depth of 0 means no staging: one frame at a time, claiming one buffer.

Three modes:

| Mode | Runs on | Claim on the camera |
|---|---|---|
| `INLINE` | the pump, frame borrowed for the call | none |
| `PASSTHROUGH` | its own task, pointer into the camera buffer | held for as long as your work takes |
| `CONVERTED` | its own task, PPA-scaled into a buffer it owns | held only for the length of the pass |

`CONVERTED` is the right mode for most consumers and the counter-intuitive one:
converting 1920x1080 to a model's 224x224 in one PPA pass moves 3% as much data as
pinning the full frame would, uses no CPU, and hands the camera buffer back the
moment the pass finishes — before your callback runs — so a 60 ms inference is off
the camera's clock entirely. The pass crops to the target aspect ratio at the same
time, for free. Both async modes draw on the loan budget and are checked against it
at registration; `CONVERTED` simply gives its share back sooner.

Push mode (supply `on_frame`) or pull mode (leave it NULL and drive
`video_raw_sink_acquire()` / `_release()` from your own task). Pull is for a
consumer that already owns a worker with a model loaded — pushing one of those
through a callback just makes it copy the frame into a queue of its own. Either
way a sink works on one frame at a time: a second `acquire()` before the matching
`_release()` is refused.

Drop counters are split by cause, because "this sink is too slow"
(`dropped_queue`), "this sink is still busy with the last frame"
(`dropped_inflight`), "the
camera pool is exhausted by someone else" (`dropped_nobuf`) and "you asked for
fewer" (`dropped_rate`) call for four different fixes.

`examples/webrtc_person_detect` runs three sinks against one camera — encoder,
inference and LCD preview — and is the worked proof of the guarantee above.

#### Pixel formats

One representation, everywhere: the `uint32_t fourcc` on `video_raw_frame_t`, from
the `VIDEO_FOURCC_*` constants in `video_capture.h`. Values match the V4L2 code of
the same name, and `esp32p4_frame_grabber.c` static-asserts that they still do.

Of the codes declared, the capture path only ever produces three:

| fourcc | Produced when |
|---|---|
| `VIDEO_FOURCC_O_UYY_E_VYY` | CSI/DVP sensor — the P4 ISP's semi-packed YUV420, and the only layout the hardware H.264 encoder accepts |
| `VIDEO_FOURCC_YUYV` | UVC camera, raw capture |
| `VIDEO_FOURCC_H264` | UVC camera, `MEDIA_STREAM_UVC_PASSTHROUGH_H264` — a compressed access unit, not a surface |

The rest (`I420`, `UYVY`, `RGB565`, `RGB24`, `GREY`, `SBGGR8`) exist to name a
conversion target or a format the tree does not yet capture.

As a `want_fourcc` on a `CONVERTED` sink only **`RGB565`** and **`RGB24`** are legal.
The PPA takes YUV as a source but not as a destination, and a planar destination
would need a stride the frame descriptor cannot express. Anything else is rejected at
registration, not silently per frame.

> **Why not just use `V4L2_PIX_FMT_*`?** Two reasons. `video_capture.h` is public and
> `esp_video` is not a dependency on every target, so `linux/videodev2.h` cannot be
> pulled in from there. More importantly the driver's fourcc is *wrong* on the P4:
> esp_video asks for `V4L2_PIX_FMT_YUV420` on the CSI path, but the ISP emits
> O_UYY_E_VYY, which V4L2 has no code for. `'YU12'` therefore lands on a buffer that
> is not I420, and `VIDEO_FOURCC_I420` is deliberately that same value — so the pump
> re-labels rather than forwards. Forwarding it verbatim would hand the PPA the wrong
> colour mode on every CSI frame.

## Quick Usage

### Basic Integration with WebRTC

```c
#include "media_stream.h"
#include "app_webrtc.h"

// Get media interfaces
media_stream_video_capture_t *video_capture = media_stream_get_video_capture_if();
media_stream_audio_capture_t *audio_capture = media_stream_get_audio_capture_if();
media_stream_video_player_t *video_player = media_stream_get_video_player_if();
media_stream_audio_player_t *audio_player = media_stream_get_audio_player_if();

// Configure WebRTC with media interfaces
app_webrtc_config_t webrtc_config = APP_WEBRTC_CONFIG_DEFAULT();
webrtc_config.video_capture = video_capture;
webrtc_config.audio_capture = audio_capture;
webrtc_config.video_player = video_player;
webrtc_config.audio_player = audio_player;

// Initialize and run WebRTC
app_webrtc_init(&webrtc_config);
app_webrtc_run();
```

### Direct Interface Usage

```c
// Initialize video capture
video_capture_config_t config = {
    .resolution = VIDEO_RESOLUTION_VGA,
    .fps = 30,
    .codec = VIDEO_CODEC_H264
};

video_capture_handle_t handle;
video_capture->init(&config, &handle);
video_capture->start(handle);

// Get H.264 encoded frames
video_frame_t *frame;
esp_err_t ret = video_capture->get_frame(handle, &frame, 1000);
if (ret == ESP_OK) {
    // Process H.264 frame data (frame->buffer, frame->len)
    video_capture->release_frame(handle, frame);
}
```

## Custom Implementation

You can implement custom media interfaces for specialized hardware:

```c
// Implement interface functions
static esp_err_t my_camera_init(video_capture_config_t *config, video_capture_handle_t *handle) {
    // Initialize your custom camera hardware
    return ESP_OK;
}

// Create interface instance
media_stream_video_capture_t* my_custom_camera_if_get(void) {
    static media_stream_video_capture_t interface = {
        .init = my_camera_init,
        .start = my_camera_start,
        .get_frame = my_camera_get_frame,  // Must return H.264 encoded frames
        .release_frame = my_camera_release_frame,
        .stop = my_camera_stop,
        .deinit = my_camera_deinit
    };
    return &interface;
}

// Use custom interface
webrtc_config.video_capture = my_custom_camera_if_get();
```

## Frame Requirements

- **Video frames**: Must be H.264 encoded with proper SPS/PPS headers
- **Audio frames**: Must be Opus encoded at 48kHz sample rate
- **Timestamps**: Should be monotonic and represent presentation time
- **Memory management**: Proper allocation/deallocation via release_frame()

## Dependencies

- ESP-IDF components: `driver`, `esp_codec`, `esp_cam`
- External codecs: H.264 encoder/decoder, Opus encoder/decoder
- Hardware: Camera module, I2S audio interface

## Documentation

- **API Reference**: See header files in `include/` directory
- **Developer Guide**: `../../docs/en/api-reference/media_stream.rst`
- **Examples**: `../../examples/` - Working WebRTC applications
- **Configuration**: `../../docs/en/api-reference/webrtc_config.rst`

## Supported Hardware

- **ESP32-S3** with camera modules (OV2640, OV3660, etc.)
- **ESP32-P4** with advanced camera and audio capabilities
- **ESP32** with external camera/audio via I2S/SPI
- **Custom hardware** via interface implementation

## Video Player (receive path)

The receive path lets the device decode an inbound H.264 video stream from a
WebRTC peer connection and render it on a connected display. It is disabled
by default so builds that only send video stay lean.

### Enabling

1. Turn on the Kconfig:

   ```
   CONFIG_MEDIA_STREAM_ENABLE_VIDEO_PLAYER=y
   ```

   Tune the queue depth, task stack, priority, and max resolution caps under
   `Media Stream Configuration -> Video Player (receive path)` in menuconfig.

2. Wire the interface into `app_webrtc_config.video_player`. In
   `webrtc_classic/main/webrtc_main.c` this is a one-line assignment:

   ```c
   media_stream_video_player_t *video_player = media_stream_get_video_player_if();
   ...
   app_webrtc_config.video_player = video_player;
   ```

3. Ensure the target has a display resolved by `bsp_selector` and LVGL. The
   render backend calls `bsp_display_start()` and creates an LVGL canvas on
   the active screen.

### Pipeline

```
  RTP -> H.264 Annex-B NAL bytes
     |
     v
  video_player_play_frame()  (copy into SPIRAM frame descriptor, enqueue)
     |
     v
  decode task  (per-player, SPIRAM task stack)
     |
     v
  esp_h264_dec_sw_new / _process   -> I420 YUV
     |
     v
  video_render_display_render_i420()
     |-- fixed-point BT.601 YUV->RGB565
     |-- nearest-neighbour scale to the LVGL canvas size
     v
  LVGL canvas on BSP display
```

### Target support

| Target       | Decoder       | Convert / scale | Notes                                  |
|--------------|---------------|-----------------|----------------------------------------|
| ESP32-P4     | `esp_h264` SW | CPU YUV->RGB565 | Tested on P4-EYE; any P4 display board works via bsp_selector + LVGL |
| ESP32-S3    | `esp_h264` SW | CPU YUV->RGB565 | Needs a display-capable BSP            |
| Other P-class | `esp_h264` SW | CPU YUV->RGB565 | BSP + LVGL required                    |

A PPA-accelerated YUV->RGB path (using `ppa_do_scale_rotate_mirror`) is a
future optimisation; the conversion is isolated in a single helper in
`video_render_display.c` so it can be dropped in without touching the
public API.

### Resolution cap

The player is capped at **320x240** by default. This matches the P4-EYE
LCD (240x240) and is the realistic ceiling for sustained live playback
with the `esp_h264` software decoder + CPU YUV->RGB565 convert path.
The peer can advertise larger resolutions; the decoder still runs, but
the render canvas is sized to the cap. Tighten further via
`CONFIG_MEDIA_STREAM_PLAYER_MAX_WIDTH` / `_MAX_HEIGHT`.

### Memory footprint

All sizeable allocations live in SPIRAM:
- decode task stack (`CONFIG_MEDIA_STREAM_PLAYER_TASK_STACK`, default 8 KB),
- per-frame NAL buffer copies in the queue,
- the RGB565 canvas buffer (`canvas_w x canvas_h x 2` bytes; at the 320x240
  cap, ~150 KB).

Only tiny control structs and the FreeRTOS queue itself (pointer-sized
slots) stay on the internal heap.

### Backpressure

The frame queue drops non-keyframes when full and drains completely on a
keyframe so the decoder resynchronises from a fresh GOP. On decode error
the adapter enters a "wait for next IDR" state until a keyframe arrives.
