/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file video_render_headless.c
 * @brief Render backend for a player with no display.
 *
 * Implements the video_render_display API without a display. Each decoded
 * frame is counted, then dropped. The decoder and the player's
 * fps log run as they do with a display, so the receive path can be tested
 * on a board or an emulator that has no panel.
 */

#include "sdkconfig.h"

#if CONFIG_MEDIA_STREAM_ENABLE_VIDEO_PLAYER && CONFIG_MEDIA_STREAM_PLAYER_HEADLESS

#include <inttypes.h>
#include <stdlib.h>

#include "esp_log.h"

#include "video_render_display.h"

static const char *TAG = "video_render";

struct video_render_display_ctx {
    uint32_t                   frame_count;
    uint16_t                   last_w;
    uint16_t                   last_h;
};

esp_err_t video_render_display_init(const video_render_display_cfg_t *cfg,
                                    video_render_display_handle_t *out_handle)
{
    if (cfg == NULL || out_handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    struct video_render_display_ctx *ctx = calloc(1, sizeof(*ctx));
    if (ctx == NULL) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "headless render backend ready (no display, frames are counted and dropped)");
    *out_handle = ctx;
    return ESP_OK;
}

esp_err_t video_render_display_render_i420(video_render_display_handle_t handle,
                                           const uint8_t *y,
                                           const uint8_t *u,
                                           const uint8_t *v,
                                           uint16_t width,
                                           uint16_t height,
                                           uint16_t y_stride,
                                           uint16_t uv_stride)
{
    (void)y_stride;
    (void)uv_stride;
    if (handle == NULL || y == NULL || u == NULL || v == NULL ||
        width == 0 || height == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (width != handle->last_w || height != handle->last_h) {
        ESP_LOGI(TAG, "stream %ux%u", (unsigned)width, (unsigned)height);
        handle->last_w = width;
        handle->last_h = height;
    }

    /* Same log prefixes as the display backend, so tests can match either. */
    handle->frame_count++;
    if (handle->frame_count == 1) {
        ESP_LOGI(TAG, "first frame rendered (src %ux%u, headless)",
                 (unsigned)width, (unsigned)height);
    } else if ((handle->frame_count & 0x1F) == 1) {
        ESP_LOGI(TAG, "rendered %" PRIu32 " frames", handle->frame_count);
    }
    return ESP_OK;
}

void video_render_display_deinit(video_render_display_handle_t handle)
{
    if (handle == NULL) {
        return;
    }
    ESP_LOGI(TAG, "render backend deinit (%" PRIu32 " frames rendered)",
             handle->frame_count);
    free(handle);
}

#endif /* CONFIG_MEDIA_STREAM_ENABLE_VIDEO_PLAYER && CONFIG_MEDIA_STREAM_PLAYER_HEADLESS */
