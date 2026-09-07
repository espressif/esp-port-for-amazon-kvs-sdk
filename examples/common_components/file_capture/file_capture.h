/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * File-backed video + audio capture interfaces for the linux_test
 * master. Implements the same `media_stream_video_capture_t` /
 * `media_stream_audio_capture_t` shape that `examples/webrtc_classic`
 * gets from `media_stream` on ESP targets, but uses plain libc
 * `fopen`/`fread` so it works on the IDF Linux target without
 * requiring the camera ISP / hw H.264 / SPIFFS dependencies that
 * tie media_stream/src/[*].c to ESP hardware.
 *
 * Frame layout on disk (matches the upstream KVS C SDK samples):
 *   FRAMES_DIR/h264SampleFrames/frame-NNNN.h264   (1500 frames, ~30 fps)
 *   FRAMES_DIR/opusSampleFrames/sample-NNN.opus   (001..618, 20 ms each)
 */

#pragma once

#include "media_stream.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Configure the directory containing h264SampleFrames/ and
 *        opusSampleFrames/. Must be called before init() of either
 *        capture interface. Path is copied; caller may free.
 */
void file_capture_set_frames_dir(const char *dir);

/**
 * @brief File-backed video capture. Loops through frame-0001.h264 ..
 *        frame-1500.h264 at the fps from `video_capture_config_t`.
 *
 * Call from a FreeRTOS task (e.g. app_main): the first call creates the task that
 * paces and publishes the frames. One capture runs at a time.
 */
media_stream_video_capture_t *file_capture_get_video_if(void);

/**
 * @brief File-backed audio capture. Loops through sample-001.opus ..
 *        sample-618.opus at one frame per `frame_duration_ms`.
 */
media_stream_audio_capture_t *file_capture_get_audio_if(void);

#ifdef __cplusplus
}
#endif
