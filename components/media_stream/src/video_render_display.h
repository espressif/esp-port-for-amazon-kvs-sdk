/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file video_render_display.h
 * @brief Generic display render backend for the video player receive path.
 *
 * Accepts decoded I420 YUV frames and pushes them to whatever display the
 * BSP selector has resolved (via LVGL). The conversion + scale path
 * internally chooses PPA when the target supports it (CONFIG_SOC_PPA_SUPPORTED)
 * and a CPU fallback otherwise.
 *
 * This header is stable; the implementation in `video_render_display.c`
 * starts as a no-op logger and is replaced by the real PPA + LVGL impl in
 * a follow-up commit.
 */

#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Opaque render context. */
typedef struct video_render_display_ctx *video_render_display_handle_t;

/** Backend config. Caps bound the canvas + intermediate buffer sizes. */
typedef struct {
    uint16_t max_width;   /*!< Cap on decoded video width (pixels). */
    uint16_t max_height;  /*!< Cap on decoded video height (pixels). */
} video_render_display_cfg_t;

/**
 * @brief Initialize the display render backend.
 *
 * Takes over the BSP display (calls `bsp_display_start*()`) and allocates
 * any intermediate buffers (canvas, RGB565 output, PPA scratch) in SPIRAM.
 *
 * @param cfg         Backend configuration. Required.
 * @param out_handle  Returned opaque handle.
 * @return ESP_OK on success, error otherwise.
 */
esp_err_t video_render_display_init(const video_render_display_cfg_t *cfg,
                                    video_render_display_handle_t *out_handle);

/**
 * @brief Render a single I420 YUV frame to the display.
 *
 * Planes are expected to be contiguous enough for a PPA / CPU scan:
 *  - `y` is `y_stride * height` bytes,
 *  - `u` and `v` are each `uv_stride * height / 2` bytes.
 *
 * The backend copies what it needs; the caller may re-use the buffers
 * immediately on return.
 */
esp_err_t video_render_display_render_i420(video_render_display_handle_t handle,
                                           const uint8_t *y,
                                           const uint8_t *u,
                                           const uint8_t *v,
                                           uint16_t width,
                                           uint16_t height,
                                           uint16_t y_stride,
                                           uint16_t uv_stride);

/** Tear down the backend and free buffers. */
void video_render_display_deinit(video_render_display_handle_t handle);

#ifdef __cplusplus
}
#endif
