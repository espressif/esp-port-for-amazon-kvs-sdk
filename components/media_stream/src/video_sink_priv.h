/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @brief Internal dispatch entry points for the sink registries.
 *
 * Not part of the public API: only the grabber (raw side) and the H.264 encoder sink
 * (encoded side) call these.
 */

#pragma once

#include "video_sink.h"
#include "video_raw_sink.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Deliver one encoded frame to every enabled sink, in registration order.
 *
 * Applies each sink's start-on-keyframe gate, times each callback, and updates stats.
 * Called from the encoder, on the encoder task.
 *
 * @param frame Borrowed frame; not retained past this call
 */
void video_sink_dispatch(const video_frame_t *frame);

/**
 * @brief Deliver one raw frame to the registered raw sink, if any.
 *
 * @param frame Borrowed raw frame; not retained past this call
 * @return The sink's return value, or ESP_ERR_INVALID_STATE if none is registered
 */
esp_err_t video_raw_sink_dispatch(const video_frame_raw_t *frame);

/**
 * @brief Whether at least one encoded sink is currently enabled.
 *
 * Lets the encoder skip work when nothing is listening.
 */
bool video_sink_any_enabled(void);

/**
 * @brief Total microseconds spent in sink callbacks since the last call, and reset.
 *
 * Folded into the grabber's 1 s enc_fps line so a drop in capture rate can be
 * attributed to the sinks, or exonerated, without correlating two logs.
 */
uint32_t video_sink_take_window_us(void);

#ifdef __cplusplus
}
#endif
