/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

/*
 * Congestion-adaptive TX-video rate controller.
 *
 * Strategy: fps and bitrate are tied to a single operating-point LADDER (see
 * video_rate_ctrl.c) rather than tracked independently — each rung is a (fps,
 * bitrate) pair chosen to make sense together. Congestion steps DOWN the ladder
 * (dropping fps + bitrate together, more rungs at once the worse it is);
 * sustained health steps back UP one rung at a time. Starts mid-ladder for a
 * gentle connect, then probes upward only if the uplink keeps up. Hysteresis
 * avoids oscillation.
 *
 * Signals:
 *   - per-frame video send duration (kvs_media) — local congestion signal.
 *   - WebRTC TWCC sender BWE (viewer feedback) — network ceiling that caps how
 *     far recovery may climb (see video_rate_ctrl_set_network_ceiling_bps).
 *
 * Touchpoints:
 *   - kvs_media send loop  -> video_rate_ctrl_report_send_ms()
 *   - grabber encode loop  -> video_rate_ctrl_should_encode() (skip to drop fps)
 *                          -> video_rate_ctrl_pull_bitrate_bps() (apply bitrate)
 *   - RTCP TWCC handler    -> video_rate_ctrl_set_network_ceiling_bps()
 */

/* camera_fps: native capture rate (skip decisions are relative to this).
 * max_bitrate_bps: the configured encoder ceiling (ladder L0 = 100% of this);
 * the controller never raises above it.
 * width/height: encoded resolution, recorded for logging/diagnostics. The
 * ladder bitrates are percentages of max_bitrate_bps, so they already scale
 * with whatever resolution sized that max. */
void video_rate_ctrl_init(uint32_t camera_fps, uint32_t max_bitrate_bps,
                          uint32_t width, uint32_t height);

/* Report measured send duration (ms) for one video frame. Drives the
 * controller's fps/bitrate state machine. */
void video_rate_ctrl_report_send_ms(uint32_t send_ms);

/* Per camera frame: true = encode+send this frame, false = skip it.
 * Skipping is how the effective fps is lowered below the camera rate. */
bool video_rate_ctrl_should_encode(void);

/* Returns a new encoder bitrate (bps) to apply, or 0 if unchanged since
 * the last pull. Grabber applies it via esp_h264_hw_enc_set_bitrate(). */
uint32_t video_rate_ctrl_pull_bitrate_bps(void);

/* Network-side bitrate ceiling from WebRTC TWCC sender BWE (viewer feedback),
 * in bps. The controller caps its target at min(configured max, this); the
 * local send-latency loop then operates within that cap. Pass 0 to clear the
 * ceiling (no network limit). Safe to call from the RTCP handler thread. */
void video_rate_ctrl_set_network_ceiling_bps(uint32_t bps);
