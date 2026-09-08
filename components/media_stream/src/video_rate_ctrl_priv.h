/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @brief Encoder-side entry points of the rate controller.
 *
 * Not part of the public API: only the frame grabber calls these. They describe the one
 * shared encoder to the ladder (what it can actuate, what it committed, how long a change
 * takes to land) and collect the arbitrated decisions to apply to it. Transports use
 * video_rate_ctrl.h and never see this side.
 *
 * Lock order: a sink callback may report congestion while video_sink's dispatch lock is
 * held, so the only nesting is sink_lock -> vrate_lock. Nothing in video_rate_ctrl.c may
 * call into video_sink, or that becomes a deadlock cycle.
 */

#pragma once

#include "video_rate_ctrl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Declare the encoder the controllers are steering
 *
 * Idempotent and independent of controller lifetime: runs on the first camera bring-up
 * and again on a restart, whereas a transport may have created and enabled its
 * controller long before. Any controller already enabled is re-seeded to the ladder's
 * starting rung.
 *
 * @param camera_fps      Native capture rate; skip decisions are relative to this
 * @param max_bitrate_bps Encoder ceiling (ladder L0); no controller ever asks for more.
 *                        On UVC passthrough, the bitrate the camera COMMITTED, not the
 *                        one requested
 * @param width           Encoded width, for the pinned-frame-rate floor and logs
 * @param height          Encoded height, likewise
 * @param fps_actuable    True when we own the encoder and can drop a raw frame before
 *                        encode. False on UVC H.264 passthrough, where dropping an encoded
 *                        frame corrupts the stream to the next IDR; the ladder's fps
 *                        column is then pinned to camera_fps and only bitrate moves
 */
void video_rate_ctrl_init(uint32_t camera_fps, uint32_t max_bitrate_bps,
                          uint32_t width, uint32_t height, bool fps_actuable);

/**
 * @brief The frame rate the encoder should be fed, or 0 for the camera's own rate
 *
 * Lowering fps is done by not delivering frames, and it is the RAW BUS that does it: the
 * caller pushes this number into the encoder's sink as its fps limit
 * (video_raw_sink_set_fps_limit()) and the bus's per-sink gate drops the rest on the pump.
 *
 * It used to be a per-frame `should_encode()` decision taken on the encoder task, which
 * was wrong in exactly the case that matters: the bus ALSO drops frames when the encoder
 * falls behind, and two independent skip decisions multiply. A 15-of-30 target behind a
 * bus delivering 20 produced 10 fps, not 15, and only while the link was congested - so
 * the ladder was steering against a frame rate that was never happening.
 *
 * Reflects the strict minimum target across all eligible controllers; 0 when none is
 * enabled. One lock-free atomic load, safe from any task.
 */
uint32_t video_rate_ctrl_target_fps(void);

/**
 * @brief Fetch a new encoder bitrate to apply, or 0 if unchanged since the last pull
 *
 * The pending target is held until taken, so a caller that rate limits itself delays a
 * change rather than losing it. Also where controller staleness is noticed, because it is
 * the one per-frame touchpoint every path has - the passthrough path never reads the fps
 * target. Encoder task only.
 */
uint32_t video_rate_ctrl_pull_bitrate_bps(void);

/**
 * @brief Report what the encoder actually agreed to (UVC passthrough)
 *
 * The camera clamps freely, so the committed and requested bitrates differ and only one
 * of them is true. Global, because there is one encoder. Diagnostics only: this never
 * feeds change detection, or a camera answering every request with the same number
 * would look like a permanently pending change.
 *
 * @param bps What the encoder reported committing, or 0 to forget
 */
void video_rate_ctrl_report_committed_bitrate_bps(uint32_t bps);

/**
 * @brief Raise the floor the ladder may descend to
 *
 * For an encoder that refuses to go below some rate: every rung under it would ask for
 * something it never honours. Absolute bps for the current resolution, in the same terms
 * as video_rate_ctrl_init()'s max_bitrate_bps; clamped to [built-in floor, max].
 *
 * @param bps Lowest rate the encoder will actually honour, or 0 to reset
 */
void video_rate_ctrl_set_min_bitrate_bps(uint32_t bps);

/**
 * @brief Declare how long an actuation takes to reach the encoder
 *
 * The queue front-end must not re-evaluate faster than a step-down can show up in the
 * signal. Set this where the encoder is reached over a rate-limited control transfer or
 * a stream restart, or the ladder walks every rung before the first change lands.
 *
 * @param ms Round-trip actuation period, or 0 for an encoder driven directly
 */
void video_rate_ctrl_set_actuation_period_ms(uint32_t ms);

/**
 * @brief Declare whether a bitrate change reaches the encoder at all
 *
 * Companion to video_rate_ctrl_init()'s fps_actuable, but discovered at runtime: some
 * UVC cameras honour the extension-unit transaction and ignore the value. With neither
 * column actuable every controller is frozen at its rung and reported as inert, so
 * `sink-stats` stops describing a control loop that does not exist.
 *
 * @param actuable Whether a bitrate change reaches the encoder
 */
void video_rate_ctrl_set_bitrate_actuable(bool actuable);

/**
 * @brief Record that the frame rate is (or is no longer) being held constant
 *
 * A pinned frame rate is only deliverable while the bitrate can pay for that many
 * frames; below ~0.015 bits per pixel per frame the encoder sheds frames unpredictably
 * instead. So while pinned the ladder stops at that floor, and if congestion persists
 * there it gives the pin up - published through video_rate_ctrl_pull_fps_pin() for the
 * caller to actuate - and re-asserts it once the arbitrated bitrate can afford it again.
 *
 * Records what the actuator has ALREADY done; it does not actuate.
 *
 * @param pinned           true if the frame rate is now being held constant
 * @param operator_request true when this is what the operator asked for, which the
 *                         automatic re-assert then aims at. false when acknowledging a
 *                         change collected from video_rate_ctrl_pull_fps_pin(): that must
 *                         not overwrite the standing request, or the pin never comes back
 */
void video_rate_ctrl_set_fps_pinned(bool pinned, bool operator_request);

/**
 * @brief Collect a frame-rate pin change the ladder wants applied
 *
 * Consumes the pending change. Polled rather than called back because applying it
 * restarts the camera stream, which may only happen where no capture buffer is held.
 *
 * @return -1 nothing pending, 0 release the pin, 1 assert it
 */
int video_rate_ctrl_pull_fps_pin(void);

#ifdef __cplusplus
}
#endif
