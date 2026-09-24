/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @brief Internal dispatch entry points for the encoded sink registry.
 *
 * Not part of the public API: only the H.264 encoder sink calls these. The raw side has
 * its own producer header, video_raw_bus_priv.h.
 */

#pragma once

#include "video_sink.h"

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
