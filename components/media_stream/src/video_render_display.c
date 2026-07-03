/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file video_render_display.c
 * @brief Generic display render backend.
 *
 * Converts decoded I420 YUV frames to RGB565 (CPU fixed-point BT.601 loop)
 * with nearest-neighbour scaling and pushes them to an LVGL canvas that
 * lives on the currently-active screen of whatever display the BSP selector
 * resolved to. Works on any target where `bsp_display_start()` is available.
 *
 * A PPA-accelerated YUV->RGB path can be layered in later without changing
 * the public API - the conversion is already isolated in a single helper
 * below.
 */

#include "sdkconfig.h"

/* The render backend is built on esp_image_effects, which ships only
 * for ESP32-P4 (idf_component.yml restricts the dep to that target).
 * On other targets this translation unit must compile to empty —
 * video_player_adapter.c's NO-PLAYER stub branch is selected via
 * CONFIG_MEDIA_STREAM_ENABLE_VIDEO_PLAYER=n so it never calls the
 * symbols defined below. */
#if CONFIG_IDF_TARGET_ESP32P4

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "bsp/esp-bsp.h"
#include "lvgl.h"

/* Mirror the override BSP's defaults: full-screen single-buffered draw
 * buffer. Registry BSPs that don't export the BUFF_* names land here. */
#ifndef BSP_LCD_DRAW_BUFF_SIZE
#define BSP_LCD_DRAW_BUFF_SIZE   (BSP_LCD_H_RES * BSP_LCD_V_RES)
#endif
#ifndef BSP_LCD_DRAW_BUFF_DOUBLE
#define BSP_LCD_DRAW_BUFF_DOUBLE (0)
#endif

#include "esp_imgfx_color_convert.h"
#include "esp_imgfx_types.h"

#include "video_render_display.h"

/* BSP portability shim: ESP32-P4-Function-EV-Board exposes BSP_LCD_DRAW_BUFF_SIZE
 * / BSP_LCD_DRAW_BUFF_DOUBLE as headers; the P4-EYE managed BSP only exposes
 * Kconfig values. Fall back to the Kconfig path so this file compiles against
 * both. */
#ifndef BSP_LCD_DRAW_BUFF_SIZE
#define BSP_LCD_DRAW_BUFF_SIZE     (BSP_LCD_H_RES * CONFIG_BSP_LCD_DRAW_BUF_HEIGHT)
#endif
#ifndef BSP_LCD_DRAW_BUFF_DOUBLE
#define BSP_LCD_DRAW_BUFF_DOUBLE   CONFIG_BSP_LCD_DRAW_BUF_DOUBLE
#endif

static const char *TAG = "video_render";

/* canvas_buf + i420_scratch feed PPA / LCD via DMA — flag them DMA-capable
 * so they land in the DMA-tagged pool, cache-line aligned. */
#define RENDER_ALLOC_CAPS (MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA | MALLOC_CAP_8BIT)
#define RENDER_LOCK_MS    200

struct video_render_display_ctx {
    video_render_display_cfg_t cfg;
    uint16_t                   canvas_w;
    uint16_t                   canvas_h;
    lv_color_t                *canvas_buf;
    lv_obj_t                  *canvas;
    uint32_t                   frame_count;
    /* FPS overlay state (label + sliding 1s window). */
    lv_obj_t                  *fps_label;
    uint64_t                   fps_window_start_us;
    uint32_t                   fps_window_frames;
    uint32_t                   fps_last_value;
    /* esp_image_effects YUV(I420) -> RGB565 path (HW-accelerated on P4 via
     * PPA / asm-optimized routines). The handle is sized to the canvas so
     * we can hand it cropped-into-canvas-dims I420 every frame. The scratch
     * buffer is the contiguous I420 the converter expects (Y || U || V). */
    esp_imgfx_color_convert_handle_t cc_handle;
    uint8_t                         *i420_scratch;
    uint32_t                         i420_scratch_len;
};

/* Fixed-point BT.601 limited-range YUV -> RGB565.
 *   R = (298*(Y-16) + 409*(V-128) + 128) >> 8
 *   G = (298*(Y-16) - 100*(U-128) - 208*(V-128) + 128) >> 8
 *   B = (298*(Y-16) + 516*(U-128) + 128) >> 8
 */
static inline uint16_t yuv_to_rgb565(int y, int u, int v)
{
    int yp = y - 16;
    if (yp < 0) {
        yp = 0;
    }
    int up = u - 128;
    int vp = v - 128;
    int r = (298 * yp + 409 * vp + 128) >> 8;
    int g = (298 * yp - 100 * up - 208 * vp + 128) >> 8;
    int b = (298 * yp + 516 * up + 128) >> 8;
    if (r < 0)   r = 0;   else if (r > 255) r = 255;
    if (g < 0)   g = 0;   else if (g > 255) g = 255;
    if (b < 0)   b = 0;   else if (b > 255) b = 255;
    /* Panel byte-order is encoded via the LVGL convention
     * CONFIG_LV_COLOR_16_SWAP: when set, the panel wants MSB-first bytes
     * (e.g. ST7789 SPI on P4-EYE); when clear, the panel takes native LE
     * (e.g. EK79007 / ILI9881C DSI). Match accordingly so the same
     * source compiles correctly for both panel families. */
    uint16_t rgb = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
#if CONFIG_LV_COLOR_16_SWAP
    return __builtin_bswap16(rgb);
#else
    return rgb;
#endif
}

/* Pack a center-cropped I420 sub-region into a contiguous (Y || U || V)
 * buffer that esp_imgfx_color_convert_process expects.
 *
 * For src_w == dst_w && src_h >= dst_h (typical 240x320 -> 240x240 phone-
 * portrait case) the crop is purely vertical: take rows
 * [y_off, y_off + dst_h) of Y and rows [y_off/2, y_off/2 + dst_h/2) of U/V.
 *
 * Source-stride may differ from src_w (the decoder may pad), so we copy
 * row-by-row to be stride-safe.
 */
static void pack_i420_center_crop(const uint8_t *y, const uint8_t *u, const uint8_t *v,
                                  uint16_t src_h,
                                  uint16_t y_stride, uint16_t uv_stride,
                                  uint8_t *dst_i420,
                                  uint16_t dst_w, uint16_t dst_h)
{
    const uint16_t y_off = (src_h > dst_h) ? (uint16_t)((src_h - dst_h) / 2) : 0;
    const uint16_t cy_off = y_off >> 1;

    uint8_t *dst_y_p = dst_i420;
    uint8_t *dst_u_p = dst_y_p + (size_t)dst_w * dst_h;
    uint8_t *dst_v_p = dst_u_p + (size_t)(dst_w >> 1) * (dst_h >> 1);

    /* Y plane: dst_h rows of dst_w bytes. */
    for (uint16_t r = 0; r < dst_h; ++r) {
        memcpy(dst_y_p + (size_t)r * dst_w,
               y + (size_t)(y_off + r) * y_stride,
               dst_w);
    }
    /* U / V planes: dst_h/2 rows of dst_w/2 bytes each. */
    const uint16_t uv_w = dst_w >> 1;
    const uint16_t uv_h = dst_h >> 1;
    for (uint16_t r = 0; r < uv_h; ++r) {
        memcpy(dst_u_p + (size_t)r * uv_w,
               u + (size_t)(cy_off + r) * uv_stride,
               uv_w);
        memcpy(dst_v_p + (size_t)r * uv_w,
               v + (size_t)(cy_off + r) * uv_stride,
               uv_w);
    }
}

/* Nearest-neighbour scale of an I420 source into an RGB565 destination.
 *
 * Used as the slow-path fallback when the input geometry rules out the
 * crop+esp_imgfx fast-path (e.g. landscape 320x240 input). For each
 * destination pixel, map to a source (Y, U, V) sample and convert. Per
 * destination row, precompute the source row pointers once and walk with
 * a running src_x accumulator to avoid a division per pixel.
 */
static void render_i420_to_rgb565(const uint8_t *y, const uint8_t *u, const uint8_t *v,
                                  uint16_t src_w, uint16_t src_h,
                                  uint16_t y_stride, uint16_t uv_stride,
                                  uint16_t *dst, uint16_t dst_w, uint16_t dst_h)
{
    const uint32_t x_ratio_q16 = ((uint32_t)src_w << 16) / dst_w;
    const uint32_t y_ratio_q16 = ((uint32_t)src_h << 16) / dst_h;

    uint32_t src_y_q16 = 0;
    for (uint16_t cy = 0; cy < dst_h; ++cy, src_y_q16 += y_ratio_q16) {
        const uint32_t sy = src_y_q16 >> 16;
        const uint8_t *row_y = y + (size_t)sy * y_stride;
        const uint8_t *row_u = u + (size_t)(sy >> 1) * uv_stride;
        const uint8_t *row_v = v + (size_t)(sy >> 1) * uv_stride;
        uint16_t *row_dst = dst + (size_t)cy * dst_w;

        uint32_t src_x_q16 = 0;
        for (uint16_t cx = 0; cx < dst_w; ++cx, src_x_q16 += x_ratio_q16) {
            const uint32_t sx = src_x_q16 >> 16;
            const int yv = row_y[sx];
            const int uv_off = sx >> 1;
            const int uv = row_u[uv_off];
            const int vv = row_v[uv_off];
            row_dst[cx] = yuv_to_rgb565(yv, uv, vv);
        }
    }
}

/* LVGL display refresh monitor — fires once per real screen refresh. We use
 * it to log actual LCD flushes-per-second alongside our "played fps" counter
 * which only measures canvas-invalidate-call rate. */
static void video_render_lvgl_monitor_cb(struct _lv_disp_drv_t *drv,
                                         uint32_t time_ms, uint32_t px)
{
    (void)drv;
    (void)time_ms;
    (void)px;
    static uint32_t total;
    static uint32_t window_count;
    static uint64_t window_start_us;

    total++;
    window_count++;
    uint64_t now = esp_timer_get_time();
    if (window_start_us == 0) {
        window_start_us = now;
    }
    if (now - window_start_us >= 1000000) {
        ESP_LOGI(TAG, "LCD flushes: %" PRIu32 "/s (total=%" PRIu32 ")",
                 window_count, total);
        window_start_us = now;
        window_count = 0;
    }
}

esp_err_t video_render_display_init(const video_render_display_cfg_t *cfg,
                                    video_render_display_handle_t *out_handle)
{
    if (cfg == NULL || out_handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Bring up the BSP display if nobody else has. Some integrations (e.g.
     * a preview path) may have already called bsp_display_start; detect
     * that via lv_disp_get_default() and skip re-init. Using the "with
     * config" variant here so the LVGL task lands on SPIRAM stack with
     * a relaxed timer period - the same shape the standalone example's
     * display_preview.c uses on P4-EYE. Backlight is off by default on
     * P4-EYE; bsp_display_brightness_set is required to see anything. */
    if (lv_disp_get_default() == NULL) {
        bsp_display_cfg_t disp_cfg = {
            .lvgl_port_cfg = {
                /* Bumped from 2 → 4 so the LVGL task is at least at the
                 * same priority as the decode task (decoder feeds canvas;
                 * if LVGL can't preempt to flush, invalidates pile up). */
                .task_priority   = 4,
                .task_stack      = 4096,
                .task_stack_caps = MALLOC_CAP_SPIRAM,
                /* Pin to core 1 so it doesn't fight the audio_encoder /
                 * decode pipeline that lives on core 0. */
                .task_affinity   = 1,
                .timer_period_ms = 10,
                /* Was 200 ms — that's the headline regression: a 15 fps
                 * invalidate stream landing during a nap collapses into
                 * one redraw per nap (~5 fps observed). 16 ms ≈ 60 Hz
                 * which is enough headroom to never coalesce video frames
                 * yet keep idle CPU when no work is pending. */
                .task_max_sleep_ms = 16,
            },
            .buffer_size   = BSP_LCD_DRAW_BUFF_SIZE,
            .double_buffer = BSP_LCD_DRAW_BUFF_DOUBLE,
#ifdef BSP_LCD_MIPI_DSI_LANE_BITRATE_MBPS
            /* The 5.x EV Function Board BSP requires the DSI bus's
             * lane_bit_rate_mbps to be populated by the caller, otherwise
             * esp_lcd_new_dsi_bus() rejects the config as "invalid lane
             * bit rate 0.00" and bsp_display_new_with_handles() aborts.
             * Pull the rate from the BSP's own macro so this stays in
             * sync with whatever DSI panel is selected via Kconfig. */
            .hw_cfg = {
                .hdmi_resolution = BSP_HDMI_RES_NONE,
                .dsi_bus = {
                    .phy_clk_src        = 0,   /* let the driver pick */
                    .lane_bit_rate_mbps = BSP_LCD_MIPI_DSI_LANE_BITRATE_MBPS,
                },
            },
#endif
            .flags = {
                /* P4's GDMA can address PSRAM; lvgl8 esp_lvgl_port_disp
                 * needs the SOC_PSRAM_DMA_CAPABLE gate (mirrors lvgl9).
                 * See downstream_patches/lvgl_port-lvgl8-allow-dma-spiram.patch. */
                .buff_dma    = true,
                .buff_spiram = true,
            },
        };
        if (bsp_display_start_with_config(&disp_cfg) == NULL) {
            /* Most common failure on the EV Function Board 5.2.1 BSP is the
             * GT911 touch I2C probe (esp32_p4_function_ev_board.c:973). The
             * BSP returns NULL there even though lvgl_port_init / lcd_init /
             * brightness_init have already succeeded — bsp_display_lcd_init
             * is registered with LVGL as the default display before touch
             * init runs. Recover that display via lv_disp_get_default() and
             * keep going without touch input. Track upstream BSP fix to
             * make touch-init optional. */
            if (lv_disp_get_default() == NULL) {
                ESP_LOGE(TAG, "bsp_display_start_with_config failed and no LVGL display registered");
                return ESP_FAIL;
            }
            ESP_LOGW(TAG, "bsp_display_start_with_config returned NULL (likely touch probe failure); "
                          "continuing with display-only, no touch input");
        }
        bsp_display_brightness_set(80);
        ESP_LOGI(TAG, "BSP display brought up (brightness 80%%)");
    } else {
        ESP_LOGI(TAG, "BSP display already initialized; reusing");
    }

    if (!bsp_display_lock(RENDER_LOCK_MS)) {
        ESP_LOGE(TAG, "bsp_display_lock timeout on init");
        return ESP_FAIL;
    }
    lv_disp_t *disp = lv_disp_get_default();
    uint32_t disp_w = lv_disp_get_hor_res(disp);
    uint32_t disp_h = lv_disp_get_ver_res(disp);

    /* Hook LVGL's monitor_cb so we can count *real* LCD refreshes (not just
     * canvas-invalidate calls). Fires once per screen refresh completion.
     * Lets us answer "are we actually pushing N fps to the panel, or is LVGL
     * coalescing our invalidates and only flushing far fewer?". */
    if (disp && disp->driver && disp->driver->monitor_cb == NULL) {
        disp->driver->monitor_cb = video_render_lvgl_monitor_cb;
    }
    /* Clear any default styles on the active screen so the canvas gets a
     * clean black background (matches the preview's appearance). */
    lv_obj_remove_style_all(lv_scr_act());
    lv_obj_set_style_bg_color(lv_scr_act(), lv_color_black(), 0);
    lv_obj_set_style_bg_opa(lv_scr_act(), LV_OPA_COVER, 0);
    bsp_display_unlock();

    struct video_render_display_ctx *ctx =
        heap_caps_calloc(1, sizeof(*ctx), RENDER_ALLOC_CAPS);
    if (ctx == NULL) {
        return ESP_ERR_NO_MEM;
    }
    ctx->cfg      = *cfg;
    ctx->canvas_w = (uint16_t)(disp_w < cfg->max_width  ? disp_w : cfg->max_width);
    ctx->canvas_h = (uint16_t)(disp_h < cfg->max_height ? disp_h : cfg->max_height);

    /* Cache-line align the canvas buffer in SPIRAM so later CPU/DMA
     * mixing doesn't step on partial lines. Matches the P4-EYE preview's
     * alloc pattern. */
    const size_t buf_bytes = (size_t)ctx->canvas_w * ctx->canvas_h * sizeof(lv_color_t);
    ctx->canvas_buf = heap_caps_aligned_calloc(64, 1, buf_bytes, RENDER_ALLOC_CAPS);
    if (ctx->canvas_buf == NULL) {
        ESP_LOGE(TAG, "canvas buffer alloc failed (%zu bytes)", buf_bytes);
        heap_caps_free(ctx);
        return ESP_ERR_NO_MEM;
    }

    /* Allocate the scratch I420 buffer + open the color-convert handle once.
     * On P4 this hits the asm-optimized I420 path inside esp_image_effects
     * (handles 16-px-aligned dims; 240 = 15*16, OK). The scratch holds Y || U
     * || V at canvas dims so we can crop into it row-wise per frame. */
    ctx->i420_scratch_len = (uint32_t)ctx->canvas_w * ctx->canvas_h * 3 / 2;
    ctx->i420_scratch = heap_caps_aligned_calloc(64, 1, ctx->i420_scratch_len, RENDER_ALLOC_CAPS);
    if (ctx->i420_scratch == NULL) {
        ESP_LOGE(TAG, "i420 scratch alloc failed (%" PRIu32 " bytes)", ctx->i420_scratch_len);
        heap_caps_free(ctx->canvas_buf);
        heap_caps_free(ctx);
        return ESP_ERR_NO_MEM;
    }
    {
        esp_imgfx_color_convert_cfg_t cc_cfg = {
            .in_res          = { .width = (int16_t)ctx->canvas_w, .height = (int16_t)ctx->canvas_h },
            .in_pixel_fmt    = ESP_IMGFX_PIXEL_FMT_I420,
            /* Panel byte-order, gated on LVGL's canonical signal that each
             * BSP sets for its panel:
             *   CONFIG_LV_COLOR_16_SWAP=y -> MSB-first  (ST7789 SPI, P4-EYE)
             *   CONFIG_LV_COLOR_16_SWAP=n -> native LE  (EK79007/ILI9881C DSI) */
#if CONFIG_LV_COLOR_16_SWAP
            .out_pixel_fmt   = ESP_IMGFX_PIXEL_FMT_RGB565_BE,
#else
            .out_pixel_fmt   = ESP_IMGFX_PIXEL_FMT_RGB565_LE,
#endif
            .color_space_std = ESP_IMGFX_COLOR_SPACE_STD_BT601,
        };
        esp_imgfx_err_t cc_ret = esp_imgfx_color_convert_open(&cc_cfg, &ctx->cc_handle);
        if (cc_ret != ESP_IMGFX_ERR_OK) {
            ESP_LOGE(TAG, "esp_imgfx_color_convert_open failed: %d", (int)cc_ret);
            heap_caps_free(ctx->i420_scratch);
            heap_caps_free(ctx->canvas_buf);
            heap_caps_free(ctx);
            return ESP_FAIL;
        }
    }

    if (!bsp_display_lock(RENDER_LOCK_MS)) {
        esp_imgfx_color_convert_close(ctx->cc_handle);
        heap_caps_free(ctx->i420_scratch);
        heap_caps_free(ctx->canvas_buf);
        heap_caps_free(ctx);
        return ESP_FAIL;
    }
    ctx->canvas = lv_canvas_create(lv_scr_act());
    if (ctx->canvas == NULL) {
        bsp_display_unlock();
        heap_caps_free(ctx->canvas_buf);
        heap_caps_free(ctx);
        ESP_LOGE(TAG, "lv_canvas_create failed");
        return ESP_FAIL;
    }
    /* Strip any default canvas styling (padding, border, scrollbar) and
     * pin the widget at (0,0) with explicit size so LVGL does not layout-
     * drift it off screen as other widgets come/go. Mirrors the preview
     * path setup. */
    lv_obj_remove_style_all(ctx->canvas);
    lv_canvas_set_buffer(ctx->canvas, ctx->canvas_buf,
                         ctx->canvas_w, ctx->canvas_h,
                         LV_IMG_CF_TRUE_COLOR);
    lv_obj_set_pos(ctx->canvas, 0, 0);
    lv_obj_set_size(ctx->canvas, ctx->canvas_w, ctx->canvas_h);

    /* FPS overlay — top-right of the video canvas (so it overlays the
     * frame instead of sitting in empty screen space on big DSI panels).
     * Parented to the canvas so it follows any future canvas re-centering
     * and so its alignment is canvas-relative. LVGL canvas re-blits won't
     * trample child widgets — they're composited over the canvas buffer. */
    ctx->fps_label = lv_label_create(ctx->canvas);
    if (ctx->fps_label) {
        lv_obj_set_style_text_color(ctx->fps_label, lv_color_white(), 0);
        lv_obj_set_style_bg_color(ctx->fps_label, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(ctx->fps_label, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_all(ctx->fps_label, 2, 0);
        lv_label_set_text(ctx->fps_label, "-- fps");
        lv_obj_align(ctx->fps_label, LV_ALIGN_TOP_RIGHT, -2, 2);
    }
    bsp_display_unlock();

    ESP_LOGI(TAG, "render backend ready (canvas %ux%u on %" PRIu32 "x%" PRIu32 " display, %zu bytes)",
             (unsigned)ctx->canvas_w, (unsigned)ctx->canvas_h,
             disp_w, disp_h, buf_bytes);
    *out_handle = ctx;
    return ESP_OK;
}

/* Match the LVGL canvas + esp_imgfx handle to the incoming stream's actual
 * dims (clamped to the configured max). Called lazily on the first frame —
 * the H.264 decoder's resolution is only known once the SPS is parsed, so
 * we can't size the canvas at init time. canvas_buf / i420_scratch stay at
 * the max-size allocation; only the LVGL canvas extents and the
 * esp_imgfx handle's in/out_res are rebound. */
static void resize_canvas_if_needed(video_render_display_handle_t handle,
                                    uint16_t width, uint16_t height)
{
    uint16_t target_w = width  < handle->cfg.max_width  ? width  : handle->cfg.max_width;
    uint16_t target_h = height < handle->cfg.max_height ? height : handle->cfg.max_height;
    if (target_w == handle->canvas_w && target_h == handle->canvas_h) {
        return;
    }
    ESP_LOGI(TAG, "canvas resize %ux%u -> %ux%u (stream %ux%u)",
             (unsigned)handle->canvas_w, (unsigned)handle->canvas_h,
             (unsigned)target_w, (unsigned)target_h,
             (unsigned)width, (unsigned)height);
    handle->canvas_w = target_w;
    handle->canvas_h = target_h;
    handle->i420_scratch_len = (uint32_t)target_w * target_h * 3 / 2;

    /* Reopen the color converter at the new in/out res. */
    if (handle->cc_handle) {
        esp_imgfx_color_convert_close(handle->cc_handle);
        handle->cc_handle = NULL;
    }
    esp_imgfx_color_convert_cfg_t cc_cfg = {
        .in_res          = { .width = (int16_t)target_w, .height = (int16_t)target_h },
        .in_pixel_fmt    = ESP_IMGFX_PIXEL_FMT_I420,
#if CONFIG_LV_COLOR_16_SWAP
        .out_pixel_fmt   = ESP_IMGFX_PIXEL_FMT_RGB565_BE,
#else
        .out_pixel_fmt   = ESP_IMGFX_PIXEL_FMT_RGB565_LE,
#endif
        .color_space_std = ESP_IMGFX_COLOR_SPACE_STD_BT601,
    };
    esp_imgfx_err_t cc_ret = esp_imgfx_color_convert_open(&cc_cfg, &handle->cc_handle);
    if (cc_ret != ESP_IMGFX_ERR_OK) {
        ESP_LOGE(TAG, "color_convert_open failed at %ux%u: %d",
                 (unsigned)target_w, (unsigned)target_h, (int)cc_ret);
    }

    /* Re-bind the LVGL canvas at the new dims and re-centre it. */
    if (bsp_display_lock(RENDER_LOCK_MS)) {
        lv_canvas_set_buffer(handle->canvas, handle->canvas_buf,
                             target_w, target_h, LV_IMG_CF_TRUE_COLOR);
        lv_obj_set_size(handle->canvas, target_w, target_h);
        lv_obj_center(handle->canvas);
        bsp_display_unlock();
    }
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
    if (handle == NULL || y == NULL || u == NULL || v == NULL ||
        width == 0 || height == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (handle->canvas_buf == NULL || handle->canvas == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Adopt the actual stream dims (clamped to max) on first frame and
     * whenever the source resolution changes mid-session. */
    resize_canvas_if_needed(handle, width, height);

    /* Convert+scale outside the display lock so the LVGL mutex is only held
     * briefly for the canvas invalidate.  canvas_buf is our own buffer; no
     * other task touches it.
     *
     * Fast-path: when the source width matches the canvas width and the
     * source is at least as tall, pack a center-cropped I420 into the
     * scratch and let esp_image_effects do the I420 -> RGB565 conversion
     * (HW/asm-optimized on P4). Otherwise fall back to the CPU
     * nearest-neighbour scaler. */
    if (width == handle->canvas_w && height >= handle->canvas_h) {
        pack_i420_center_crop(y, u, v, height, y_stride, uv_stride,
                              handle->i420_scratch,
                              handle->canvas_w, handle->canvas_h);
        esp_imgfx_data_t in = {
            .data     = handle->i420_scratch,
            .data_len = handle->i420_scratch_len,
        };
        esp_imgfx_data_t out = {
            .data     = (uint8_t *)handle->canvas_buf,
            .data_len = (uint32_t)handle->canvas_w * handle->canvas_h * sizeof(lv_color_t),
        };
        esp_imgfx_err_t cc_ret = esp_imgfx_color_convert_process(handle->cc_handle, &in, &out);
        if (cc_ret != ESP_IMGFX_ERR_OK) {
            ESP_LOGW(TAG, "color_convert_process failed (%d), falling back to CPU scaler", (int)cc_ret);
            render_i420_to_rgb565(y, u, v,
                                  width, height,
                                  y_stride, uv_stride,
                                  (uint16_t *)handle->canvas_buf,
                                  handle->canvas_w, handle->canvas_h);
        }
    } else {
        render_i420_to_rgb565(y, u, v,
                              width, height,
                              y_stride, uv_stride,
                              (uint16_t *)handle->canvas_buf,
                              handle->canvas_w, handle->canvas_h);
    }

    /* Re-apply the canvas buffer every frame so LVGL definitely picks up
     * the new pixels. lv_obj_invalidate alone is enough for a static
     * buffer pointer, but re-binding mirrors the preview's pattern and
     * is robust if we later move to ping-pong buffers. */
    if (bsp_display_lock(RENDER_LOCK_MS / 2)) {
        lv_canvas_set_buffer(handle->canvas, handle->canvas_buf,
                             handle->canvas_w, handle->canvas_h,
                             LV_IMG_CF_TRUE_COLOR);
        lv_obj_invalidate(handle->canvas);
        bsp_display_unlock();
    } else {
        ESP_LOGW(TAG, "bsp_display_lock timeout in render; dropping frame");
        return ESP_ERR_TIMEOUT;
    }

    handle->frame_count++;
    if (handle->frame_count == 1) {
        ESP_LOGI(TAG, "first frame rendered (src %ux%u -> canvas %ux%u)",
                 (unsigned)width, (unsigned)height,
                 (unsigned)handle->canvas_w, (unsigned)handle->canvas_h);
    } else if ((handle->frame_count & 0x1F) == 1) {
        ESP_LOGI(TAG, "rendered %" PRIu32 " frames", handle->frame_count);
    }

    /* FPS overlay — accumulate frames over a 1-second window and update the
     * on-screen label only when the value actually changes (saves an LVGL
     * invalidate per frame). */
    handle->fps_window_frames++;
    uint64_t now_us = esp_timer_get_time();
    if (handle->fps_window_start_us == 0) {
        handle->fps_window_start_us = now_us;
    } else if (now_us - handle->fps_window_start_us >= 1000000ULL) {
        uint32_t fps = (uint32_t)(((uint64_t)handle->fps_window_frames * 1000000ULL) /
                                  (now_us - handle->fps_window_start_us));
        if (fps != handle->fps_last_value && handle->fps_label) {
            char buf[16];
            snprintf(buf, sizeof(buf), "%" PRIu32 " fps", fps);
            if (bsp_display_lock(RENDER_LOCK_MS / 4)) {
                lv_label_set_text(handle->fps_label, buf);
                bsp_display_unlock();
            }
            handle->fps_last_value = fps;
        }
        handle->fps_window_frames = 0;
        handle->fps_window_start_us = now_us;
    }
    return ESP_OK;
}

void video_render_display_deinit(video_render_display_handle_t handle)
{
    if (handle == NULL) {
        return;
    }
    if (bsp_display_lock(RENDER_LOCK_MS)) {
        if (handle->fps_label) {
            lv_obj_del(handle->fps_label);
            handle->fps_label = NULL;
        }
        if (handle->canvas) {
            lv_obj_del(handle->canvas);
            handle->canvas = NULL;
        }
        bsp_display_unlock();
    }
    if (handle->cc_handle) {
        esp_imgfx_color_convert_close(handle->cc_handle);
        handle->cc_handle = NULL;
    }
    if (handle->i420_scratch) {
        heap_caps_free(handle->i420_scratch);
    }
    if (handle->canvas_buf) {
        heap_caps_free(handle->canvas_buf);
    }
    ESP_LOGI(TAG, "render backend deinit (%" PRIu32 " frames rendered)",
             handle->frame_count);
    heap_caps_free(handle);
}

#endif /* CONFIG_IDF_TARGET_ESP32P4 */
