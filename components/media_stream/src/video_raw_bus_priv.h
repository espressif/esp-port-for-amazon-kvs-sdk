/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @brief Producer-side entry points for the raw-frame bus.
 *
 * Not part of the public API: only the capture pump publishes frames and only the
 * capture lifecycle drains them. Consumers use video_raw_sink.h.
 */

#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "video_raw_sink.h"
#include "esp_video_if.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Create the bus primitives. Idempotent; also called from registration.
 */
esp_err_t video_raw_bus_init(void);

/**
 * @brief Publish one camera frame to every enabled sink.
 *
 * TAKES OWNERSHIP of @p fb. The bus returns it to the driver once the last holder
 * releases it, which may be before this call returns (no async sink took it) or long
 * after (a sink is still working on it). The caller must NOT release it.
 *
 * Returns as soon as every sink has been offered the frame. That is fast unless a sink
 * opted into VIDEO_RAW_OVERFLOW_BLOCK, which is the documented exception.
 *
 * @param fb         Frame from esp_video_if_get_frame()
 * @param fourcc     VIDEO_FOURCC_* describing what @p fb actually holds
 * @param camera_fps Capture rate, used for per-sink fps_limit gating; 0 disables gating
 */
void video_raw_bus_publish(video_fb_t *fb, uint32_t fourcc, uint8_t camera_fps);

/**
 * @brief Microseconds the pump spent inside the bus since the last call, and reset.
 *
 * Folded into the grabber's 1 s enc_fps line so a drop in capture rate can be attributed
 * to the sinks, or exonerated, without correlating two logs.
 */
uint32_t video_raw_bus_take_window_us(void);

/**
 * @brief Stop accepting frames and wake anything waiting.
 *
 * Step one of teardown, and it must run BEFORE the caller waits for the pump to park: a
 * pump blocked inside a VIDEO_RAW_OVERFLOW_BLOCK sink will not park on its own, so
 * waiting first would mean waiting behind the very sink being shut down.
 *
 * Cheap and non-blocking. Idempotent.
 */
void video_raw_bus_halt(void);

/**
 * @brief Resume accepting frames after a halt. Called when capture restarts.
 */
void video_raw_bus_resume(void);

/**
 * @brief Release every queued and in-flight frame.
 *
 * Drains all queues and waits (bounded) for consumers mid-callback to finish. Sinks stay
 * registered. Run it after the pump has parked.
 *
 * Must complete before the caller unmaps or frees the capture buffers.
 *
 * @return true when every frame was accounted for, false when the wait expired with a sink
 *         still holding one - which also means that sink may still be inside its callback.
 *         A caller about to free anything the callback can reach must check this: the
 *         alternative to leaking is a use-after-free.
 */
bool video_raw_bus_drain(void);

#ifdef __cplusplus
}
#endif
