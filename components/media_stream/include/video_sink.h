/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @brief Encoded-video sink registry: multiple consumers, sequential dispatch
 *
 * The encoder produces one H.264 stream and every registered sink sees every frame,
 * in registration order, on the encoder task. Frames are BORROWED - see
 * video_sink_encoded_fn_t. Nothing is queued per sink, so nothing can overflow, and
 * consumers can be enabled and disabled independently at runtime.
 *
 * The pipeline lifecycle stays where it always was: video_capture_init()/_start()/
 * _stop()/_deinit(), which are now reference counted so several consumers can hold
 * the camera at once. Registering a sink does not start the camera; enabling a sink
 * only opens the delivery gate.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "video_capture.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Opaque handle to a registered encoded-video sink
 */
typedef struct video_sink_s *video_sink_handle_t;

/**
 * @brief Encoded-frame callback
 *
 * CONTRACT - read this before writing one:
 *
 *  - @p frame is BORROWED. It is valid only for the duration of this call.
 *
 *  - It runs on the encoder task, sequentially with every other registered sink. Your
 *    time is charged against the per-frame budget (33 ms at 30 fps) shared by all sinks.
 *
 * @param frame Borrowed encoded frame (H.264 Annex-B access unit)
 * @param user  Opaque pointer supplied at registration
 */
typedef void (*video_sink_encoded_fn_t)(const video_frame_t *frame, void *user);

/**
 * @brief Sink registration parameters
 */
typedef struct {
    const char             *name;     /* Short name for logs and stats, e.g. "putmedia" */
    video_sink_encoded_fn_t on_frame; /* Callback, see the contract above */
    void                   *user;     /* Passed back to the callback */
} video_sink_config_t;

/**
 * @brief Register an encoded-video sink
 *
 * The sink starts DISABLED; call video_sink_set_enabled() to begin receiving frames.
 *
 * Every sink always starts on a keyframe.
 * @param cfg Sink configuration; @c name and @c on_frame are required
 * @param out Receives the sink handle
 * @return ESP_OK, ESP_ERR_INVALID_ARG, ESP_ERR_NO_MEM, or ESP_ERR_NOT_FOUND when
 *         CONFIG_VIDEO_SINK_MAX sinks are already registered
 */
esp_err_t video_sink_register(const video_sink_config_t *cfg, video_sink_handle_t *out);

/**
 * @brief Unregister a sink
 *
 * Disables it first if needed. Safe to call while capture is running; the dispatcher
 * will not be inside the callback when this returns.
 *
 * @param sink Handle from video_sink_register()
 * @return ESP_OK or ESP_ERR_INVALID_ARG
 */
esp_err_t video_sink_unregister(video_sink_handle_t sink);

/**
 * @brief Enable or disable frame delivery to one sink
 *
 * Independent per sink and safe to call at any time from any task. Enabling re-arms the
 * start-on-keyframe gate, so a sink that is disabled and re-enabled mid-GOP resumes at
 * the next keyframe rather than mid-reference-chain.
 *
 * This gates delivery only. It does not start or stop the camera - that is
 * video_capture_start()/_stop(), which are reference counted.
 *
 * @param sink    Handle from video_sink_register()
 * @param enabled true to receive frames, false to stop
 * @return ESP_OK or ESP_ERR_INVALID_ARG
 */
esp_err_t video_sink_set_enabled(video_sink_handle_t sink, bool enabled);

/**
 * @brief Per-sink accounting
 *
 * @c max_us and @c avg_us are the numbers that matter in this model: they show who is
 * consuming the shared per-frame budget.
 */
typedef struct {
    const char *name;
    bool        enabled;
    uint32_t    calls;      /* Frames delivered to the callback */
    uint32_t    gate_held;  /* Frames withheld while waiting for the first keyframe */
    uint32_t    max_us;     /* Longest callback observed */
    uint32_t    avg_us;     /* Mean callback duration */
} video_sink_stats_t;

/**
 * @brief Snapshot the stats of all registered sinks
 *
 * @param out   Array to fill
 * @param max   Capacity of @p out
 * @param count Receives the number of entries written
 * @return ESP_OK or ESP_ERR_INVALID_ARG
 */
esp_err_t video_sink_get_stats(video_sink_stats_t *out, size_t max, size_t *count);

/**
 * @brief Reset the per-sink counters (names and enabled state are kept)
 */
void video_sink_reset_stats(void);

#ifdef __cplusplus
}
#endif
