/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include <stdio.h>
#include <inttypes.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "bsp/esp-bsp.h"
#include "lvgl.h"
#include "video_raw_sink.h"
#include "video_capture.h"
#include "detect_sink.h"
#include "preview_display.h"

static const char *TAG = "preview";

#define PREVIEW_MAX_BOXES  PERSON_DETECT_MAX_BOXES
#define PREVIEW_LOCK_MS    100
#define PREVIEW_TASK_STACK (6 * 1024)

static video_raw_sink_handle_t s_sink;

static lv_obj_t  *s_screen;
static lv_obj_t  *s_canvas;
static lv_obj_t  *s_boxes[PREVIEW_MAX_BOXES];
static lv_obj_t  *s_label;
static lv_obj_t  *s_stats;
static lv_color_t *s_canvas_buf;
static uint16_t   s_w, s_h;
static bool       s_stats_visible = true;

/* Rate meters for the two things only this file can see.
 *
 * The capture and encoder rates come from the grabber, which already measures them; the
 * detector's and this display's do not exist anywhere else, so they are counted here.
 * Everything is a difference between two samples a second or so apart rather than an
 * instantaneous figure - at 5 fps an instantaneous reading is either 0 or 5 and tells
 * nobody anything. */
static uint64_t s_meter_last_us;
static uint32_t s_lcd_frames;         /* Frames blitted since the last sample */
static uint32_t s_infer_last;         /* detect_results_t.inferences at the last sample */
static uint16_t s_lcd_dfps;           /* Display and detector rates, in tenths of a frame */
static uint16_t s_det_dfps;           /* per second, so "4.3" survives a 5 fps target. */

/* Copy the converted frame into the canvas.
 *
 * A straight memcpy would do if the panel took native-endian RGB565, but the ST7789 on P4-EYE wants
 * MSB-first, which LVGL expresses as LV_COLOR_16_SWAP. The PPA cannot help here - its byte_swap
 * acts on the INPUT picture and only for ARGB8888/RGB565 inputs, and ours is YUV420 - so the swap
 * rides along with the copy we are doing anyway. At 240x240 and 15 fps that is ~1.7 MB/s. */
static void blit_to_canvas(const uint16_t *src, size_t px)
{
#if CONFIG_LV_COLOR_16_SWAP
    uint16_t *dst = (uint16_t *)s_canvas_buf;
    for (size_t i = 0; i < px; i++) {
        dst[i] = __builtin_bswap16(src[i]);
    }
#else
    memcpy(s_canvas_buf, src, px * sizeof(uint16_t));
#endif
}

/* Reposition the box overlays from the latest detections.
 *
 * These are child lv_objs rather than lv_canvas_draw_rect() calls. Drawing into the canvas would
 * mutate the pixel buffer, so every box would have to be redrawn after each blit, and each
 * lv_canvas_draw_rect() invalidates the canvas by itself. Children composite over the canvas
 * instead, so a re-blit does not trample them and they only move when a detection does.
 *
 * Caller holds the display lock. */
static void update_boxes(void)
{
    detect_results_t res;
    if (detect_sink_get_results(&res) != ESP_OK || res.src_width == 0 || res.src_height == 0) {
        for (int i = 0; i < PREVIEW_MAX_BOXES; i++) {
            lv_obj_add_flag(s_boxes[i], LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }

    /* Detection coordinates are in the model input's space; the canvas is a different size. Both
     * are the same centred crop of the camera frame, so this is one uniform factor per axis. */
    const int n = res.count < PREVIEW_MAX_BOXES ? res.count : PREVIEW_MAX_BOXES;
    for (int i = 0; i < n; i++) {
        const int x = res.boxes[i].x1 * s_w / res.src_width;
        const int y = res.boxes[i].y1 * s_h / res.src_height;
        int w = (res.boxes[i].x2 - res.boxes[i].x1) * s_w / res.src_width;
        int h = (res.boxes[i].y2 - res.boxes[i].y1) * s_h / res.src_height;
        if (w < 2) { w = 2; }
        if (h < 2) { h = 2; }

        lv_obj_set_pos(s_boxes[i], x, y);
        lv_obj_set_size(s_boxes[i], w, h);
        lv_obj_clear_flag(s_boxes[i], LV_OBJ_FLAG_HIDDEN);
    }
    for (int i = n; i < PREVIEW_MAX_BOXES; i++) {
        lv_obj_add_flag(s_boxes[i], LV_OBJ_FLAG_HIDDEN);
    }

    char txt[48];
    snprintf(txt, sizeof(txt), "%d person%s  %" PRIu32 " ms",
             n, n == 1 ? "" : "s", res.infer_us / 1000);
    lv_label_set_text(s_label, txt);
}

/* Recompute the overlay text, at most once a second. Caller holds the display lock.
 *
 * Reads the achieved rates, deliberately, not the rate controller's targets: the useful
 * thing to see on a device that is struggling is what it is managing, and the gap between
 * "enc" here and the ladder's target is exactly the symptom worth noticing. */
static void update_stats(uint64_t now_us)
{
    if (s_meter_last_us == 0) {
        s_meter_last_us = now_us;
        s_lcd_frames    = 0;
        return;   /* No interval yet; the first sample only starts the clock. */
    }
    const uint64_t dt_us = now_us - s_meter_last_us;
    if (dt_us < 1000000ULL) {
        return;
    }

    /* Tenths, computed before the counters are cleared. */
    s_lcd_dfps = (uint16_t)((uint64_t)s_lcd_frames * 10000000ULL / dt_us);

    detect_results_t res;
    if (detect_sink_get_results(&res) == ESP_OK) {
        /* A reset or a restart can move this backwards; report 0 rather than a huge
         * number from an unsigned wrap. */
        const uint32_t d = (res.inferences >= s_infer_last) ? res.inferences - s_infer_last : 0;
        s_det_dfps   = (uint16_t)((uint64_t)d * 10000000ULL / dt_us);
        s_infer_last = res.inferences;
    }

    s_meter_last_us = now_us;
    s_lcd_frames    = 0;

    if (!s_stats_visible) {
        return;
    }

    video_capture_live_stats_t live = {0};
    (void)video_capture_get_live_stats(&live);

    char txt[80];
    snprintf(txt, sizeof(txt),
             "cap %" PRIu32 "  enc %" PRIu32 "  %" PRIu32 "k\n"
             "det %u.%u  lcd %u.%u",
             live.cap_fps, live.enc_fps, live.enc_kbps,
             s_det_dfps / 10, s_det_dfps % 10,
             s_lcd_dfps / 10, s_lcd_dfps % 10);
    lv_label_set_text(s_stats, txt);
}

static void on_preview_frame(const video_raw_frame_t *frame, void *user)
{
    (void)user;

    if (frame->width != s_w || frame->height != s_h) {
        return;   /* Converter geometry disagrees with the canvas; nothing sane to draw. */
    }
    if (!bsp_display_lock(PREVIEW_LOCK_MS)) {
        return;   /* Display busy - drop this frame rather than stall the sink task. */
    }
    /* Canvas-sized RGB565 already: the bus converted it on this sink's task and gave the
     * camera buffer back before calling us, so the blit below holds nothing scarce. */
    blit_to_canvas((const uint16_t *)frame->buffer, (size_t)s_w * s_h);
    s_lcd_frames++;
    update_boxes();
    update_stats(esp_timer_get_time());
    lv_obj_invalidate(s_canvas);
    bsp_display_unlock();
}

static esp_err_t build_ui(void)
{
    lv_disp_t *disp = lv_disp_get_default();
    if (disp == NULL) {
        ESP_LOGE(TAG, "no LVGL display after bsp_display_start()");
        return ESP_FAIL;
    }
    const lv_coord_t dw = lv_disp_get_hor_res(disp);
    const lv_coord_t dh = lv_disp_get_ver_res(disp);

    /* Square preview, as large as the panel allows: the camera frame is cropped to square by the
     * converter, so a non-square canvas would either letterbox or lie about the aspect. */
    s_w = s_h = (uint16_t)(dw < dh ? dw : dh);

    s_canvas_buf = heap_caps_aligned_calloc(64, 1, (size_t)s_w * s_h * sizeof(lv_color_t),
                                            MALLOC_CAP_SPIRAM);
    if (s_canvas_buf == NULL) {
        ESP_LOGE(TAG, "out of PSRAM for a %ux%u canvas", s_w, s_h);
        return ESP_ERR_NO_MEM;
    }

    /* Our own screen rather than lv_scr_act(), so nothing else that brings up LVGL can restyle the
     * ground out from under the preview. */
    s_screen = lv_obj_create(NULL);
    lv_obj_remove_style_all(s_screen);
    lv_obj_set_style_bg_color(s_screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);

    s_canvas = lv_canvas_create(s_screen);
    lv_obj_remove_style_all(s_canvas);
    lv_canvas_set_buffer(s_canvas, s_canvas_buf, s_w, s_h, LV_IMG_CF_TRUE_COLOR);
    lv_obj_center(s_canvas);

    for (int i = 0; i < PREVIEW_MAX_BOXES; i++) {
        s_boxes[i] = lv_obj_create(s_canvas);
        lv_obj_remove_style_all(s_boxes[i]);
        lv_obj_set_style_bg_opa(s_boxes[i], LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_color(s_boxes[i], lv_palette_main(LV_PALETTE_GREEN), 0);
        lv_obj_set_style_border_width(s_boxes[i], 2, 0);
        lv_obj_set_style_border_opa(s_boxes[i], LV_OPA_COVER, 0);
        lv_obj_clear_flag(s_boxes[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(s_boxes[i], LV_OBJ_FLAG_HIDDEN);
    }

    s_label = lv_label_create(s_canvas);
    lv_obj_set_style_text_color(s_label, lv_color_white(), 0);
    lv_obj_set_style_bg_color(s_label, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_label, LV_OPA_50, 0);
    lv_label_set_text(s_label, "starting");
    lv_obj_align(s_label, LV_ALIGN_BOTTOM_LEFT, 2, -2);

    /* Live pipeline rates, top-left and out of the detector's way at the bottom. A child of
     * the canvas for the same reason the boxes are: it composites over the pixels instead
     * of being drawn into them, so the next blit does not erase it and it costs a redraw
     * only when the text actually changes. */
    s_stats = lv_label_create(s_canvas);
    lv_obj_set_style_text_color(s_stats, lv_palette_lighten(LV_PALETTE_YELLOW, 2), 0);
    lv_obj_set_style_bg_color(s_stats, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_stats, LV_OPA_50, 0);
    lv_obj_set_style_pad_all(s_stats, 2, 0);
    lv_obj_set_style_text_line_space(s_stats, 0, 0);
    lv_label_set_text(s_stats, "cap -  enc -  -k\ndet -  lcd -");
    lv_obj_align(s_stats, LV_ALIGN_TOP_LEFT, 2, 2);
    if (!s_stats_visible) {
        lv_obj_add_flag(s_stats, LV_OBJ_FLAG_HIDDEN);
    }

    lv_scr_load(s_screen);
    return ESP_OK;
}

esp_err_t preview_display_init(void)
{
    if (s_sink != NULL) {
        return ESP_OK;
    }

    /* BSP owns panel bring-up: it knows the bus, the pins and the panel driver, and it starts the
     * LVGL port task. Backlight is off after this on P4-EYE, hence the explicit brightness. */
    if (lv_disp_get_default() == NULL) {
        if (bsp_display_start() == NULL) {
            ESP_LOGE(TAG, "bsp_display_start() failed");
            return ESP_FAIL;
        }
        bsp_display_brightness_set(80);
    } else {
        ESP_LOGI(TAG, "display already up; reusing it");
    }

    if (!bsp_display_lock(1000)) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = build_ui();
    bsp_display_unlock();
    if (err != ESP_OK) {
        return err;
    }

    const video_raw_sink_config_t cfg = {
        .name          = "preview",
        .mode          = VIDEO_RAW_MODE_CONVERTED,
        .on_frame      = on_preview_frame,
        .task_stack    = PREVIEW_TASK_STACK,
        /* No staging: one frame at a time, so the claim is a single camera buffer - and it
         * is held only across the conversion, never across the blit. */
        .queue_depth   = 0,
        /* Latest-frame-wins: a preview that falls behind should skip, not accumulate lag. */
        .overflow      = VIDEO_RAW_OVERFLOW_DROP_OLD,
        .fps_limit     = CONFIG_PERSON_DETECT_PREVIEW_FPS,
        .want_fourcc   = VIDEO_FOURCC_RGB565,
        .want_width    = s_w,
        .want_height   = s_h,
    };
    err = video_raw_sink_register(&cfg, &s_sink);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to register the preview sink: %s", esp_err_to_name(err));
        return err;
    }
    video_raw_sink_set_enabled(s_sink, true);

    ESP_LOGI(TAG, "preview ready: %ux%u at up to %d fps", s_w, s_h,
             CONFIG_PERSON_DETECT_PREVIEW_FPS);
    return ESP_OK;
}

esp_err_t preview_display_set_enabled(bool enabled)
{
    if (s_sink == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return video_raw_sink_set_enabled(s_sink, enabled);
}

esp_err_t preview_display_get_rates(uint16_t *det_dfps, uint16_t *lcd_dfps)
{
    if (s_stats == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (det_dfps) { *det_dfps = s_det_dfps; }
    if (lcd_dfps) { *lcd_dfps = s_lcd_dfps; }
    return ESP_OK;
}

esp_err_t preview_display_show_stats(bool show)
{
    if (s_stats == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    /* The meters keep running either way - only the label is hidden - so turning the
     * overlay back on shows a current number instead of a stale one. */
    s_stats_visible = show;
    if (!bsp_display_lock(PREVIEW_LOCK_MS)) {
        return ESP_ERR_TIMEOUT;
    }
    if (show) {
        lv_obj_clear_flag(s_stats, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_stats, LV_OBJ_FLAG_HIDDEN);
    }
    bsp_display_unlock();
    return ESP_OK;
}
