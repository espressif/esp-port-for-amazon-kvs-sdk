/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @brief Raw-frame sinks: N consumers, running in parallel, sharing camera buffers by
 *        reference rather than by copy.
 *
 * The grabber task is a pump: dequeue a camera frame, publish it, go back for the next
 * one. 
 *
 * A slow sink cannot lower anyone else's frame rate. That is done with a per-sink queue depth
 * and a per-sink policy for what to discard when it falls behind.
 *
 * The one way to give that guarantee up is by creating a blocking sink using VIDEO_RAW_OVERFLOW_BLOCK.
 *
 * Raw frames are large and the camera only has CONFIG_MEDIA_STREAM_CAM_BUFFER_COUNT of them,
 * so a sink holding one is spending a scarce resource.
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
 * @brief Opaque handle to a registered raw sink
 */
typedef struct video_raw_sink_s *video_raw_sink_handle_t;

/**
 * @brief How a sink receives its frames
 */
typedef enum {
    /**
     * Own task, zero copy: a pointer straight into the camera's buffer, no conversion,
     * no memcpy.
     *
     * Pins that buffer for as long as your work takes, so it draws on the global loan
     * budget (CONFIG_MEDIA_STREAM_CAM_BUFFER_COUNT minus CONFIG_MEDIA_STREAM_CAM_BUFFER_RESERVE)
     * for that whole time.
     *
     * Registration fails with ESP_ERR_NO_MEM when the budget is exhausted - loudly, at
     * bring-up, rather than as unexplained frame loss later.
     */
    VIDEO_RAW_MODE_PASSTHROUGH,

    /**
     * Runs on the PUMP TASK with the camera buffer checked out. The frame is BORROWED and
     * valid only for the duration of the call. Runs synchronously with all other inline
     * sinks
     */
    VIDEO_RAW_MODE_INLINE,
} video_raw_mode_t;

/**
 * @brief What a sink discards when it cannot keep up
 */
typedef enum {
    /** Evict the oldest queued frame and take the new one. Latest-frame-wins. */
    VIDEO_RAW_OVERFLOW_DROP_OLD,

    /** Discard the incoming frame and keep what is already queued. */
    VIDEO_RAW_OVERFLOW_DROP_NEW,

    /**
     * Do not lose frames: the PUMP WAITS until this sink can accept, bounded by
     * block_timeout_ms.
     *
     * While the pump waits it is not dequeuing from the camera, so every other sink stops 
     * receiving frames for the duration and the camera's own buffers back up in the driver.
     */
    VIDEO_RAW_OVERFLOW_BLOCK,
} video_raw_overflow_t;

/**
 * @brief Raw-frame callback (push mode)
 *
 * Runs on the sink's own task for the async modes, or on the pump task for
 * VIDEO_RAW_MODE_INLINE. The frame is released for you when this returns; do not call
 * video_raw_sink_release() from inside it.
 *
 * @param frame Frame to process; borrowed for the duration of the call
 * @param user  Opaque pointer supplied at registration
 */
typedef void (*video_raw_sink_fn_t)(const video_raw_frame_t *frame, void *user);

/**
 * @brief Sink registration parameters
 *
 * Zero-initialise and set what you need; every field has a working default.
 */
typedef struct {
    const char         *name;      /* Short name for logs and stats, e.g. "person-detect" */
    video_raw_mode_t    mode;
    void               *user;      /* Passed back to the callback */

    /**
     * Push mode: set on_frame and the bus runs a task that acquires, calls you, and
     * releases.
     *
     * PULL mode: leave on_frame NULL and drive video_raw_sink_acquire() / _release() from
     * a task of your own.
     */
    video_raw_sink_fn_t on_frame;
    uint32_t            task_stack; /* Push mode only; 0 for the default */
    uint8_t             task_prio;  /* Push mode only; 0 for the default */

    /* Backpressure. All per sink - nothing here is shared with any other sink.
     *
     * queue_depth is how deep a backlog may build. A sink is never working on more
     * than one frame at a time in either mode; in PULL mode a second acquire() before
     * release() is refused, so there is nothing left for a second dial to say.
     *
     * For PASSTHROUGH the queued frames are camera frames, so depth costs camera buffers.
     *
     * queue_depth 0 is legal and means no staging: strictly one frame at a time, claim 1.
     */
    uint8_t              queue_depth;      /* Frames that may wait for you; 0 => no staging */
    video_raw_overflow_t overflow;
    uint32_t             block_timeout_ms; /* BLOCK only; 0 for the Kconfig default */

    /**
     * Deliver at most this many frames per second, dropping the rest before they are ever
     * queued. 0 means every frame.
     */
    uint8_t              fps_limit;
} video_raw_sink_config_t;

/**
 * @brief Register a raw-frame sink
 *
 * The sink starts DISABLED; call video_raw_sink_set_enabled() to begin receiving frames.
 *
 * @param cfg Sink configuration; @c name is required, and @c on_frame is required for
 *            VIDEO_RAW_MODE_INLINE. The struct is copied.
 * @param out Receives the sink handle
 * @return ESP_OK,
 *         ESP_ERR_INVALID_ARG on a malformed config,
 *         ESP_ERR_NOT_FOUND when CONFIG_VIDEO_RAW_SINK_MAX sinks are already registered,
 *         ESP_ERR_NO_MEM when a PASSTHROUGH sink's claim of queue_depth + 1 would exceed
 *         the camera loan budget
 */
esp_err_t video_raw_sink_register(const video_raw_sink_config_t *cfg,
                                  video_raw_sink_handle_t *out);

/**
 * @brief Unregister a sink
 *
 * Disables it, drains its queue, and waits for any in-flight frame to be released. The
 * dispatcher will not be inside your callback when this returns.
 *
 * For a PUSH-mode sink this waits as long as it takes: the task the bus runs for you owns the
 * stack that is freed here, so there is no bound at which giving up would be safe. A callback
 * that can block indefinitely - a socket send with no timeout - will hold up unregistration for
 * exactly as long as it blocks, and is named in a warning once a second while it does.
 *
 * @param sink Handle from video_raw_sink_register()
 * @return ESP_OK or ESP_ERR_INVALID_ARG
 */
esp_err_t video_raw_sink_unregister(video_raw_sink_handle_t sink);

/**
 * @brief Enable or disable frame delivery to one sink
 *
 * Independent per sink and safe to call from any task at any time. Disabling drains
 * whatever is queued, so a re-enabled sink resumes on a current frame rather than a stale
 * one.
 *
 * This gates delivery only. It does not start or stop the camera - that is
 * video_capture_start() / _stop(), which are reference counted.
 *
 * @param sink    Handle from video_raw_sink_register()
 * @param enabled true to receive frames, false to stop
 * @return ESP_OK or ESP_ERR_INVALID_ARG
 */
esp_err_t video_raw_sink_set_enabled(video_raw_sink_handle_t sink, bool enabled);

/**
 * @brief Change one sink's fps limit after registration
 *
 * Same gate as the @c fps_limit config field, moved at runtime. Exists because a
 * congestion controller's target frame rate is not knowable at registration: the encoder
 * sink is told its current target as the ladder moves, and every frame the gate rejects is
 * then dropped ON THE PUMP, before the sink is offered it.
 *
 * @param sink Handle from video_raw_sink_register()
 * @param fps  Frames per second to deliver; 0 for every frame
 * @return ESP_OK or ESP_ERR_INVALID_ARG
 */
esp_err_t video_raw_sink_set_fps_limit(video_raw_sink_handle_t sink, uint8_t fps);

/**
 * @brief Take the next frame (PULL mode only)
 *
 * EVERY successful acquire MUST be matched by exactly one video_raw_sink_release(). A
 * frame you never release is a camera buffer the driver never gets back; leak enough of
 * them and capture stops.
 *
 * ONE FRAME AT A TIME: acquiring again while you still hold a frame is refused. Release
 * what you have, then acquire the next one.
 *
 * @param sink       Handle from a registration with @c on_frame left NULL
 * @param frame      Receives the frame descriptor
 * @param timeout_ms How long to wait for one; 0 polls
 * @return ESP_OK, ESP_ERR_TIMEOUT if no frame arrived, ESP_ERR_INVALID_ARG, or
 *         ESP_ERR_INVALID_STATE if this sink is in push mode or already holds a frame
 */
esp_err_t video_raw_sink_acquire(video_raw_sink_handle_t sink, video_raw_frame_t *frame,
                                 uint32_t timeout_ms);

/**
 * @brief Give a frame back (PULL mode only)
 *
 * Pass the descriptor back exactly as acquired. Releasing a mutated or already-released
 * descriptor is detected and refused
 *
 * @param sink  Handle from video_raw_sink_register()
 * @param frame Frame from video_raw_sink_acquire()
 * @return ESP_OK or ESP_ERR_INVALID_ARG
 */
esp_err_t video_raw_sink_release(video_raw_sink_handle_t sink,
                                 const video_raw_frame_t *frame);

/**
 * @brief Per-sink accounting
 */
typedef struct {
    const char *name;
    bool        enabled;
    uint32_t    delivered;
    uint32_t    dropped_queue;    /* Queue full: DROP_NEW discarded, or DROP_OLD evicted */
    uint32_t    dropped_inflight; /* At the sink's cap of queue_depth + 1 */
    uint32_t    dropped_nobuf;    /* Global camera loan budget exhausted */
    uint32_t    dropped_rate;     /* fps_limit gate */
    uint32_t    dropped_timeout;  /* BLOCK only: block_timeout_ms expired */
    uint64_t    blocked_us;       /* BLOCK only: pump time spent waiting on this sink */
    uint32_t    max_hold_us;      /* Longest acquire -> release observed */
    uint32_t    avg_hold_us;
    uint8_t     in_flight;        /* Frames this sink holds right now */
} video_raw_sink_stats_t;

/**
 * @brief Snapshot the stats of all registered raw sinks
 *
 * @param out   Array to fill
 * @param max   Capacity of @p out
 * @param count Receives the number of entries written
 * @return ESP_OK or ESP_ERR_INVALID_ARG
 */
esp_err_t video_raw_sink_get_stats(video_raw_sink_stats_t *out, size_t max, size_t *count);

/**
 * @brief Reset the per-sink counters (names and enabled state are kept)
 */
void video_raw_sink_reset_stats(void);

#ifdef __cplusplus
}
#endif
