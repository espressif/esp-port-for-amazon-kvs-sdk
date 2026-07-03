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
// Provides: init, start, stop, get_frame, release_frame, deinit
```

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
