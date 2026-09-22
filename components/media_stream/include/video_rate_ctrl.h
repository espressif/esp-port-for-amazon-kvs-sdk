/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

/*
 * (OPT IN) Congestion-adaptive TX-video rate control, one controller per sink.
 *
 * There is one H.264 encoder and every sink receives every encoded frame, so "per
 * sink" means a controller per sink: each transport runs its own ladder position,
 * its own congestion signal and its own enable state, and the encoder is driven at
 * the strict minimum over every enabled, still-reporting controller. Worst sink wins.
 *
 * A transport creates a controller, enables it only from a path that also reports
 * congestion, and feeds it one of two signals: how long a frame took to send
 * (real-time transports) or how full its upload queue is (store-and-forward). The
 * encoder side of the API is internal to the frame grabber.
 *
 * Design notes, measurements and the ladder itself: components/media_stream/README.md.
 */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Opaque handle to one sink's rate controller
 */
typedef struct video_rate_ctrl_s *video_rate_ctrl_handle_t;

/**
 * @brief Create a controller for one sink
 *
 * Starts DISABLED and contributes nothing to arbitration until enabled. Legal before
 * the camera is up - a transport may come up before the capture does.
 *
 * @param name Short name for logs and stats, e.g. "webrtc"
 * @param out  Receives the handle
 * @return ESP_OK, ESP_ERR_INVALID_ARG, ESP_ERR_NO_MEM, or ESP_ERR_NOT_FOUND when
 *         CONFIG_VIDEO_RATE_CTRL_MAX controllers already exist
 */
esp_err_t video_rate_ctrl_create(const char *name, video_rate_ctrl_handle_t *out);

/**
 * @brief Destroy a controller and drop it from arbitration
 *
 * Optional. A transport that starts and stops repeatedly should create once and toggle
 * video_rate_ctrl_enable() instead: that keeps its learned ladder position across
 * sessions and avoids teardown races.
 *
 * @param h Handle, or NULL (ignored)
 */
void video_rate_ctrl_destroy(video_rate_ctrl_handle_t h);

/**
 * @brief Find an existing controller by the name it was created with
 *
 * For code that has to reach a controller it did not create - a console command forcing a
 * network ceiling to test the ladder, or a diagnostic - without threading the handle
 * through every layer in between. The transport that created it still owns its lifetime:
 * this is a lookup, not a reference, and the handle is only good while that transport is
 * up.
 *
 * @param name Name passed to video_rate_ctrl_create()
 * @param out  Receives the handle
 * @return ESP_OK, ESP_ERR_INVALID_ARG, or ESP_ERR_NOT_FOUND
 */
esp_err_t video_rate_ctrl_find(const char *name, video_rate_ctrl_handle_t *out);

/**
 * @brief Opt in to (or out of) adaptive rate control for this sink
 *
 * Enable only from a path that also reports congestion: the reports are the only thing
 * that lets a controller climb back up. A controller that goes quiet is dropped from
 * arbitration after a timeout but keeps its rung, so it resumes where it left off.
 * Enabling starts at the ladder's conservative rung; disabling withdraws the controller.
 *
 * @param h      Handle, or NULL (ignored)
 * @param enable true to run adaptive control for this sink
 */
void video_rate_ctrl_enable(video_rate_ctrl_handle_t h, bool enable);

/**
 * @brief Report how long one frame took to send (RATE signal)
 *
 * For real-time transports that hand a frame to the network synchronously. Compared
 * against the per-frame budget implied by this controller's target fps. Ignored while
 * disabled.
 *
 * @param h       Handle, or NULL (ignored)
 * @param send_ms Measured send duration for one video frame, in milliseconds
 */
void video_rate_ctrl_report_send_ms(video_rate_ctrl_handle_t h, uint32_t send_ms);

/**
 * @brief Report upload-queue occupancy (LEVEL signal)
 *
 * For store-and-forward transports whose backpressure is a growing queue. Cheap to call
 * per frame: samples accumulate and the ladder is re-evaluated on a slow cadence.
 *
 * Report on EVERY frame, and never fall silent. A controller that stops reporting goes
 * stale, and a stale controller is dropped from arbitration entirely - so silence does
 * not mean "hold the current rung", it means the encoder returns to its native rate.
 * That is the opposite of what a struggling transport wants: suppressing this call
 * during a stall is precisely how a transport speeds the encoder up at the moment it is
 * unable to accept a single frame.
 *
 * When a transport is refusing frames upstream of the queue, report @p capacity rather
 * than the true occupancy: the queue may have drained behind the refusal, and an
 * occupancy of zero would read as perfect health while the stream is broken. Saturation
 * is the conservative reading, and video_rate_ctrl_report_overflow() still covers data
 * actually lost.
 *
 * @param h        Handle, or NULL (ignored)
 * @param used     Buffers currently queued
 * @param capacity Queue capacity; 0 is ignored
 */
void video_rate_ctrl_report_queue(video_rate_ctrl_handle_t h,
                                  uint32_t used, uint32_t capacity);

/**
 * @brief Report that the transport actually lost data to congestion
 *
 * The unambiguous signal: something was discarded, so it needs no smoothing to be
 * believed and steps down at once. It still respects the interval a step-down needs to
 * reach the encoder, because a change that has not landed yet cannot be why the queue
 * is still overflowing; a sink that keeps overflowing descends one step per interval.
 *
 * @param h Handle, or NULL (ignored)
 */
void video_rate_ctrl_report_overflow(video_rate_ctrl_handle_t h);

/**
 * @brief Cap how far this controller may recover, from a network estimate
 *
 * WebRTC TWCC sender BWE (viewer feedback), in bps: the target is capped at
 * min(encoder max, this). Recorded but not acted on while disabled, so it is already
 * in force if the sink later opts in.
 *
 * @param h   Handle, or NULL (ignored)
 * @param bps Estimated available bitrate, or 0 for no network limit
 */
void video_rate_ctrl_set_network_ceiling_bps(video_rate_ctrl_handle_t h, uint32_t bps);

/* ------------------------------------------------------------------------- */
/* Diagnostics                                                                */
/* ------------------------------------------------------------------------- */

/**
 * @brief Per-controller snapshot
 *
 * @c level and @c target_fps show where each sink independently wants to run;
 * comparing them against each other is how arbitration is verified.
 */
typedef struct {
    const char *name;
    bool        enabled;
    bool        stale;              /* Excluded: stopped reporting */
    uint32_t    level;              /* Ladder index; 0 is best quality */
    uint32_t    target_fps;
    uint32_t    target_bitrate_bps;
    uint32_t    load_pct;           /* Smoothed load, 100 == this controller's own
                                     * congestion threshold, so a latency sink and a
                                     * queue sink compare. */
    uint32_t    net_ceiling_bps;    /* 0 = no network limit */
    uint32_t    reports;
    uint32_t    probe_bar_level;    /* Best rung recovery may re-enter; 0 = unrestricted.
                                     * Raised when a rung congests, decays one rung at a
                                     * time. */
} video_rate_ctrl_stats_t;

/**
 * @brief What the one shared encoder is actually doing
 *
 * A property of the encoder, not of any sink. @c committed_bps against @c requested_bps
 * says whether the ladder's decisions are reaching the hardware at all.
 */
typedef struct {
    uint32_t requested_bps;    /* Last value pushed toward the encoder */
    uint32_t committed_bps;    /* What the encoder said it took; 0 = never reported */
    bool     fps_actuable;     /* Can we drop frames before encode */
    bool     bitrate_actuable; /* Does a bitrate change reach the encoder */
    bool     fps_pin_wanted;   /* Operator asked for a constant frame rate */
    bool     fps_pinned;       /* ...and it is currently in effect */
    uint32_t pinned_floor_bps; /* Bitrate below which the pin is given up; 0 = n/a */
    uint32_t floor_bps;        /* Floor every rung is clamped up to */
    uint32_t deepest_level;    /* Deepest rung worth entering at this floor */
} video_rate_ctrl_encoder_stats_t;

/**
 * @brief Snapshot the shared encoder's side of the story
 *
 * @param out Receives the encoder stats
 * @return ESP_OK or ESP_ERR_INVALID_ARG
 */
esp_err_t video_rate_ctrl_get_encoder_stats(video_rate_ctrl_encoder_stats_t *out);

/**
 * @brief Snapshot every controller, plus the arbitrated result
 *
 * @param out       Array to fill
 * @param max       Capacity of @p out
 * @param count     Receives the number of entries written
 * @param enc_fps   If non-NULL, receives the arbitrated encoder fps (0 = native)
 * @param enc_bps   If non-NULL, receives the arbitrated encoder bitrate (0 = native)
 * @return ESP_OK or ESP_ERR_INVALID_ARG
 */
esp_err_t video_rate_ctrl_get_stats(video_rate_ctrl_stats_t *out, size_t max, size_t *count,
                                    uint32_t *enc_fps, uint32_t *enc_bps);

#ifdef __cplusplus
}
#endif
