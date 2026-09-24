/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @brief Raw-frame sink: the seam between the camera grabber and whatever consumes
 *        raw frames. Today that is exactly one thing, the H.264 encoder.
 *
 * The grabber loop pulls a frame from the camera, runs the snapshot interceptor and the
 * preprocess hook, applies the rate-control gate, and then hands the frame to the
 * registered raw sink. Splitting it here means the encoder is no longer hardcoded into
 * the grab loop.
 *
 * There is deliberately ONE slot. Fanning raw frames out to several consumers needs a
 * refcounted shared queue (raw frames are large and the V4L2 buffer must be returned
 * promptly), which is a separate piece of work. Registering a second sink fails loudly
 * rather than silently doing something surprising. When the shared queue lands this
 * grows to N and gains a handle; sink implementations do not change.
 */

#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "video_capture.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Raw-frame sink
 *
 * CONTRACT: @c frame is BORROWED and valid only for the duration of the call. The
 * callback runs on the grabber task while the camera buffer is still checked out, so it
 * directly back-pressures the camera - return promptly.
 */
typedef struct {
    const char *name;
    esp_err_t (*on_frame)(const video_frame_raw_t *frame, void *user);
    void       *user;
} video_raw_sink_t;

/**
 * @brief Register the raw-frame sink
 *
 * @param sink Sink description; @c name and @c on_frame are required. The struct is
 *             copied, so it need not outlive the call.
 * @return ESP_OK, ESP_ERR_INVALID_ARG, or ESP_ERR_NOT_SUPPORTED if a sink is already
 *         registered (single slot, see the file comment)
 */
esp_err_t video_raw_sink_register(const video_raw_sink_t *sink);

/**
 * @brief Unregister the raw-frame sink
 *
 * @return ESP_OK
 */
esp_err_t video_raw_sink_unregister(void);

/**
 * @brief Whether a raw sink is currently registered
 */
bool video_raw_sink_is_registered(void);

#ifdef __cplusplus
}
#endif
