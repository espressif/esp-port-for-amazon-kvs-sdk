/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @brief Frame conversion through esp_image_effects, for targets without a PPA (the ESP32-S3).
 *
 * Same API, same crop geometry and the same pool discipline as the PPA converter in
 * video_frame_convert.c, so a VIDEO_RAW_MODE_CONVERTED sink does not care which one it got.
 * The pass is esp_image_effects' crop, scale and colour conversion in sequence.
 */

#include "media_stream_caps.h"
#if MEDIA_STREAM_HAS_ESP_VIDEO_CAPTURE && !MEDIA_STREAM_HAS_PPA

#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_imgfx_crop.h"
#include "esp_imgfx_scale.h"
#include "esp_imgfx_color_convert.h"
#include "video_frame_convert.h"

static const char *TAG = "video_convert";

/* Output buffers are only ever touched by the CPU, so no cache-line rule applies; 16 keeps
 * rows word-aligned for the consumer. */
#define CONV_ALIGN      16
#define MAX_POOL_DEPTH  4

struct video_frame_convert_s {
    video_frame_convert_cfg_t cfg;
    uint8_t                   src_bpp;
    size_t                    buf_size;
    uint8_t                  *buf[MAX_POOL_DEPTH];
    QueueHandle_t             free_idx;              /* Indices of buffers nobody holds */

    /* Crop, recomputed only when the source resolution changes. Owned by the sink's own
     * task, like everything else in a conversion, so no lock. */
    uint16_t                  geo_src_w;
    uint16_t                  geo_src_h;
    uint16_t                  off_x;
    uint16_t                  off_y;
    uint16_t                  block_w;
    uint16_t                  block_h;

    /* Stages, in order; NULL when the stage would be a copy. Intermediates stay in the
     * camera's own format. With no colour conversion (source and destination formats
     * match), the last stage that runs writes straight into the output buffer. */
    esp_imgfx_crop_handle_t          crop;
    esp_imgfx_scale_handle_t         scale;
    esp_imgfx_color_convert_handle_t cvt;
    uint8_t                         *crop_buf;
    uint8_t                         *scale_buf;

    uint32_t                  us_sum;
    uint32_t                  us_max;
    uint32_t                  samples;
};

/* At 5 fps this is a line every thirteen seconds. */
#define CONV_TIMING_EVERY  64

/* Bytes per pixel of the packed formats the crop and scale stages work in. */
static uint8_t packed_bpp(uint32_t fourcc)
{
    return fourcc == VIDEO_FOURCC_RGB24 ? 3 : 2;
}

static size_t frame_size(uint32_t fourcc, uint32_t w, uint32_t h)
{
    return fourcc == VIDEO_FOURCC_I420 ? (size_t)w * h * 3 / 2 : (size_t)w * h * packed_bpp(fourcc);
}

static esp_imgfx_pixel_fmt_t imgfx_fmt(uint32_t fourcc)
{
    switch (fourcc) {
    case VIDEO_FOURCC_YUYV:   return ESP_IMGFX_PIXEL_FMT_YUYV;
    case VIDEO_FOURCC_UYVY:   return ESP_IMGFX_PIXEL_FMT_UYVY;
    case VIDEO_FOURCC_RGB565: return ESP_IMGFX_PIXEL_FMT_RGB565_LE;
    case VIDEO_FOURCC_I420:   return ESP_IMGFX_PIXEL_FMT_I420;
    default:                  return ESP_IMGFX_PIXEL_FMT_RGB888;   /* VIDEO_FOURCC_RGB24 */
    }
}

esp_err_t video_frame_convert_check_dst(uint32_t dst_fourcc)
{
    if (dst_fourcc != VIDEO_FOURCC_RGB565 && dst_fourcc != VIDEO_FOURCC_RGB24 &&
        dst_fourcc != VIDEO_FOURCC_I420) {
        ESP_LOGE(TAG, "target format '%.4s' cannot be a converter output (RGB565, RGB24 or I420)",
                 (const char *)&dst_fourcc);
        return ESP_ERR_NOT_SUPPORTED;
    }
    return ESP_OK;
}

esp_err_t video_frame_convert_check(uint32_t src_fourcc, uint32_t dst_fourcc)
{
    if (src_fourcc != VIDEO_FOURCC_YUYV && src_fourcc != VIDEO_FOURCC_UYVY &&
        src_fourcc != VIDEO_FOURCC_RGB565 && src_fourcc != VIDEO_FOURCC_RGB24) {
        ESP_LOGE(TAG, "source format '%.4s' is not a converter input (YUYV, UYVY, RGB565 or RGB24)",
                 (const char *)&src_fourcc);
        return ESP_ERR_NOT_SUPPORTED;
    }
    return video_frame_convert_check_dst(dst_fourcc);
}

/* ------------------------------------------------------------------ stages */

static void stages_close(struct video_frame_convert_s *c)
{
    if (c->crop) {
        esp_imgfx_crop_close(c->crop);
        c->crop = NULL;
    }
    if (c->scale) {
        esp_imgfx_scale_close(c->scale);
        c->scale = NULL;
    }
    if (c->cvt) {
        esp_imgfx_color_convert_close(c->cvt);
        c->cvt = NULL;
    }
    heap_caps_free(c->crop_buf);
    heap_caps_free(c->scale_buf);
    c->crop_buf = c->scale_buf = NULL;
}

/* The largest centred block of the source at the destination's aspect ratio, as the PPA
 * converter crops it, so a detector's boxes map back the same way on either target. The
 * stages are then built for it; one that would only copy is left out. */
static esp_err_t stages_open(struct video_frame_convert_s *c, uint32_t src_w, uint32_t src_h)
{
    stages_close(c);

    const uint32_t dw = c->cfg.dst_width;
    const uint32_t dh = c->cfg.dst_height;
    uint32_t block_w = src_w;
    uint32_t block_h = (src_w * dh) / dw;
    if (block_h > src_h) {
        block_h = src_h;
        block_w = (src_h * dw) / dh;
    }
    c->block_w = block_w & ~1u;
    c->block_h = block_h & ~1u;
    c->off_x   = ((src_w - c->block_w) / 2) & ~1u;
    c->off_y   = ((src_h - c->block_h) / 2) & ~1u;

    const esp_imgfx_pixel_fmt_t in_fmt = imgfx_fmt(c->cfg.src_fourcc);
    const bool need_scale = c->block_w != dw || c->block_h != dh;
    const bool need_cvt   = c->cfg.src_fourcc != c->cfg.dst_fourcc;
    esp_imgfx_err_t e = ESP_IMGFX_ERR_OK;

    if (c->block_w != src_w || c->block_h != src_h) {
        esp_imgfx_crop_cfg_t cc = {
            .in_res       = { src_w, src_h },
            .in_pixel_fmt = in_fmt,
            .cropped_res  = { c->block_w, c->block_h },
            .x_pos        = c->off_x,
            .y_pos        = c->off_y,
        };
        e = esp_imgfx_crop_open(&cc, &c->crop);
        if (e != ESP_IMGFX_ERR_OK) {
            goto fail;
        }
        if (need_scale || need_cvt) {
            c->crop_buf = heap_caps_aligned_alloc(CONV_ALIGN, (size_t)c->block_w * c->block_h * c->src_bpp,
                                                  MALLOC_CAP_SPIRAM);
            if (c->crop_buf == NULL) {
                goto fail;
            }
        }
    }
    if (need_scale) {
        esp_imgfx_scale_cfg_t sc = {
            .in_res       = { c->block_w, c->block_h },
            .in_pixel_fmt = in_fmt,
            .scale_res    = { dw, dh },
            .filter_type  = ESP_IMGFX_SCALE_FILTER_TYPE_DOWN_RESAMPLE,
        };
        e = esp_imgfx_scale_open(&sc, &c->scale);
        if (e != ESP_IMGFX_ERR_OK) {
            goto fail;
        }
        if (need_cvt) {
            c->scale_buf = heap_caps_aligned_alloc(CONV_ALIGN, (size_t)dw * dh * c->src_bpp, MALLOC_CAP_SPIRAM);
            if (c->scale_buf == NULL) {
                goto fail;
            }
        }
    }
    if (need_cvt) {
        esp_imgfx_color_convert_cfg_t vc = {
            .in_res          = { dw, dh },
            .in_pixel_fmt    = in_fmt,
            .out_pixel_fmt   = imgfx_fmt(c->cfg.dst_fourcc),
            .color_space_std = ESP_IMGFX_COLOR_SPACE_STD_BT601,
        };
        e = esp_imgfx_color_convert_open(&vc, &c->cvt);
        if (e != ESP_IMGFX_ERR_OK) {
            goto fail;
        }
    }

    c->geo_src_w = src_w;
    c->geo_src_h = src_h;
    ESP_LOGI(TAG, "crop %ux%u at (%u,%u) of %" PRIu32 "x%" PRIu32 " -> %" PRIu32 "x%" PRIu32,
             c->block_w, c->block_h, c->off_x, c->off_y, src_w, src_h, dw, dh);
    return ESP_OK;

fail:
    ESP_LOGE(TAG, "esp_image_effects setup failed (%d)", (int)e);
    stages_close(c);
    c->geo_src_w = c->geo_src_h = 0;
    return ESP_FAIL;
}

static esp_err_t stages_run(struct video_frame_convert_s *c, const video_raw_frame_t *src, uint8_t *dst)
{
    const esp_imgfx_data_t out = { .data = dst, .data_len = (uint32_t)c->buf_size };
    esp_imgfx_data_t in = { .data = src->buffer, .data_len = src->len };
    esp_imgfx_data_t tmp;
    if (c->crop) {
        tmp = c->crop_buf ? (esp_imgfx_data_t){ c->crop_buf, (uint32_t)c->block_w * c->block_h * c->src_bpp }
                          : out;
        if (esp_imgfx_crop_process(c->crop, &in, &tmp) != ESP_IMGFX_ERR_OK) {
            return ESP_FAIL;
        }
        in = tmp;
    }
    if (c->scale) {
        tmp = c->scale_buf ? (esp_imgfx_data_t){ c->scale_buf, (uint32_t)c->cfg.dst_width * c->cfg.dst_height * c->src_bpp }
                           : out;
        if (esp_imgfx_scale_process(c->scale, &in, &tmp) != ESP_IMGFX_ERR_OK) {
            return ESP_FAIL;
        }
        in = tmp;
    }
    if (c->cvt) {
        esp_imgfx_data_t o = out;
        return esp_imgfx_color_convert_process(c->cvt, &in, &o) == ESP_IMGFX_ERR_OK ? ESP_OK : ESP_FAIL;
    }
    if (in.data == src->buffer) {
        memcpy(dst, src->buffer, c->buf_size);   /* Same size, same format: nothing to do but copy. */
    }
    return ESP_OK;
}

/* ------------------------------------------------------------------ lifecycle */

esp_err_t video_frame_convert_init(void)
{
    return ESP_OK;   /* No shared hardware client to set up. */
}

esp_err_t video_frame_convert_create(const video_frame_convert_cfg_t *cfg,
                                     video_frame_convert_handle_t *out)
{
    if (cfg == NULL || out == NULL || cfg->dst_width == 0 || cfg->dst_height == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    /* I420 subsamples 2x2. A YUYV/UYVY source is cropped and scaled in its own format, and
     * esp_image_effects only scales YUV 4:2:2 to an even size (an odd width would split a
     * two-pixel macropixel). */
    const bool yuv422_src = cfg->src_fourcc == VIDEO_FOURCC_YUYV || cfg->src_fourcc == VIDEO_FOURCC_UYVY;
    if ((cfg->dst_fourcc == VIDEO_FOURCC_I420 || yuv422_src) &&
        ((cfg->dst_width | cfg->dst_height) & 1)) {
        ESP_LOGE(TAG, "'%.4s' -> '%.4s' needs an even size, not %ux%u",
                 (const char *)&cfg->src_fourcc, (const char *)&cfg->dst_fourcc,
                 cfg->dst_width, cfg->dst_height);
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = video_frame_convert_check(cfg->src_fourcc, cfg->dst_fourcc);
    if (err != ESP_OK) {
        return err;
    }

    const uint8_t depth = cfg->pool_depth ? cfg->pool_depth : 2;
    if (depth > MAX_POOL_DEPTH) {
        ESP_LOGE(TAG, "pool depth %u exceeds %d", depth, MAX_POOL_DEPTH);
        return ESP_ERR_INVALID_ARG;
    }

    struct video_frame_convert_s *c = calloc(1, sizeof(*c));
    if (c == NULL) {
        return ESP_ERR_NO_MEM;
    }
    c->cfg = *cfg;
    c->cfg.pool_depth = depth;
    c->src_bpp  = packed_bpp(cfg->src_fourcc);
    c->buf_size = frame_size(cfg->dst_fourcc, cfg->dst_width, cfg->dst_height);

    c->free_idx = xQueueCreate(depth, sizeof(uint8_t));
    if (c->free_idx == NULL) {
        video_frame_convert_destroy(c);
        return ESP_ERR_NO_MEM;
    }
    for (uint8_t i = 0; i < depth; i++) {
        c->buf[i] = heap_caps_aligned_calloc(CONV_ALIGN, 1, c->buf_size, MALLOC_CAP_SPIRAM);
        if (c->buf[i] == NULL) {
            ESP_LOGE(TAG, "out of PSRAM for a %u-buffer pool of %u bytes", depth,
                     (unsigned)c->buf_size);
            video_frame_convert_destroy(c);
            return ESP_ERR_NO_MEM;
        }
        xQueueSend(c->free_idx, &i, 0);
    }

    ESP_LOGI(TAG, "esp_image_effects converter '%.4s' -> '%.4s' %ux%u, %u buffers of %u bytes",
             (const char *)&cfg->src_fourcc, (const char *)&cfg->dst_fourcc,
             cfg->dst_width, cfg->dst_height, depth, (unsigned)c->buf_size);
    *out = c;
    return ESP_OK;
}

void video_frame_convert_destroy(video_frame_convert_handle_t c)
{
    if (c == NULL) {
        return;
    }
    for (int i = 0; i < MAX_POOL_DEPTH; i++) {
        if (c->buf[i]) {
            heap_caps_free(c->buf[i]);
        }
    }
    if (c->free_idx) {
        vQueueDelete(c->free_idx);
    }
    stages_close(c);
    free(c);
}

/* ------------------------------------------------------------------ conversion */

esp_err_t video_frame_convert_run(video_frame_convert_handle_t c,
                                  const video_raw_frame_t *src, video_raw_frame_t *dst)
{
    if (c == NULL || src == NULL || dst == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (src->fourcc != c->cfg.src_fourcc) {
        ESP_LOGE(TAG, "source is '%.4s', converter was built for '%.4s'",
                 (const char *)&src->fourcc, (const char *)&c->cfg.src_fourcc);
        return ESP_ERR_INVALID_STATE;
    }
    if (src->len < (size_t)src->width * src->height * c->src_bpp) {
        return ESP_ERR_INVALID_SIZE;   /* A short frame would read past the camera buffer. */
    }
    if ((c->geo_src_w != src->width || c->geo_src_h != src->height) &&
        stages_open(c, src->width, src->height) != ESP_OK) {
        return ESP_FAIL;
    }

    uint8_t idx;
    if (xQueueReceive(c->free_idx, &idx, 0) != pdTRUE) {
        return ESP_ERR_NO_MEM;   /* Every buffer is out with the consumer. Caller counts the drop. */
    }

    const int64_t t0 = esp_timer_get_time();
    if (stages_run(c, src, c->buf[idx]) != ESP_OK) {
        xQueueSend(c->free_idx, &idx, 0);
        return ESP_FAIL;
    }
    const uint32_t us = (uint32_t)(esp_timer_get_time() - t0);

    c->us_sum += us;
    if (us > c->us_max) {
        c->us_max = us;
    }
    if (++c->samples >= CONV_TIMING_EVERY) {
        ESP_LOGI(TAG, "'%.4s'->'%.4s' %ux%u: cpu avg %" PRIu32 " us (max %" PRIu32 ") over %"
                 PRIu32 " frames", (const char *)&c->cfg.src_fourcc,
                 (const char *)&c->cfg.dst_fourcc, c->cfg.dst_width, c->cfg.dst_height,
                 c->us_sum / c->samples, c->us_max, c->samples);
        c->us_sum = c->us_max = c->samples = 0;
    }

    *dst = (video_raw_frame_t){
        .buffer       = c->buf[idx],
        .len          = c->buf_size,
        .width        = c->cfg.dst_width,
        .height       = c->cfg.dst_height,
        .fourcc       = c->cfg.dst_fourcc,
        .timestamp_us = src->timestamp_us,
        .seq          = src->seq,
        .slot         = idx,      /* Pool index, not a camera slot - owned says which. */
        .generation   = 0,
        .owned        = true,
    };
    return ESP_OK;
}

void video_frame_convert_release(video_frame_convert_handle_t c, uint8_t index)
{
    if (c == NULL || index >= c->cfg.pool_depth) {
        return;
    }
    xQueueSend(c->free_idx, &index, 0);
}

#endif /* MEDIA_STREAM_HAS_ESP_VIDEO_CAPTURE && !MEDIA_STREAM_HAS_PPA */
