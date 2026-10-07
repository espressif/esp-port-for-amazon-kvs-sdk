# Sample media for file-based capture

`CONFIG_MEDIA_STREAM_CAPTURE_FROM_FILE=y` sends these frames, already encoded,
instead of the camera's and microphone's. It is how the emulator, and any board
whose camera does not come up, can still produce a stream.

    video.pack   60 H.264 frames   2 s at 30 fps
    audio.pack  618 Opus frames    12.36 s, 48 kHz mono, 20 ms per frame

## Why one packed file per medium

The reader used to keep one file per frame and open it on every frame. That cost
two `fopen` calls per frame, so video plus audio came to 160 opens a second.
SPIFFS scans its file list on every open, so the cost grew with the file count.
Measured on the P4-C5 core board:

| files in image | device video rate |
|---|---|
| 60  (video only)        | 25 fps |
| 85  (video + audio)     |  4 fps |
| 681 (video + audio)     | connection dropped |

A pack is opened once. Reading a frame is a seek and a read. The packs are the
only source: without them the file source sends nothing, and logs why once.

## Format

Little endian.

    u32 magic 'PKF1'
    u32 count
    count x { u32 offset, u32 length }
    payload

## Regenerating

Put per-frame files in this directory and run the packer:

    tools/pack_samples.py examples/common_components/app_common/spiffs_image/samples

It reads `frame-*.h264` and `sample-*.opus`, writes the two packs, and leaves the
inputs alone. Delete the inputs afterwards: only the packs need to ship.

### Where the inputs come from

Video: the 60 frames arrived with the initial port. They are the same
kind of content as the upstream set in
`amazon-kinesis-video-streams-webrtc-sdk-c/samples/h264SampleFrames`, but a
different encode -- not byte-identical to it. `video.pack` holds those exact
bytes, and the per-frame files stay in git history, so nothing is lost.

Audio: Bach, Cello Suite No. 1 Prelude (BWV 1007), from Wikimedia Commons under
CC0, so no attribution is required. Any source works. Encode to what the sender
expects, then split the Ogg stream into one file per packet:

    ffmpeg -ss 30 -t 13 -i input.ogg -ac 1 -ar 48000 \
           -af loudnorm=I=-16:TP=-1.5 \
           -c:a libopus -b:a 64000 -vbr off -frame_duration 20 \
           -application audio out.opus

The sender expects 48 kHz mono, 20 ms per frame (`media_stream.c`,
`frame_duration_ms`). Keep those three values in step with it.
