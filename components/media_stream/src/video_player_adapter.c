/*
 * SPDX-FileCopyrightText: 2025-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file video_player_adapter.c
 * @brief Receive-path video player: H.264 Annex-B -> esp_h264 SW decoder
 *        -> generic display render backend.
 *
 * When CONFIG_MEDIA_STREAM_ENABLE_VIDEO_PLAYER is off the old no-op stub
 * is retained so media_stream's video_player interface keeps linking on
 * boards that don't need the receive path.
 */

#include <inttypes.h>
#include <stdbool.h>
#include <string.h>

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"
#include "video_player.h"

static const char *TAG = "video_player_adapter";

#ifndef CONFIG_MEDIA_STREAM_ENABLE_VIDEO_PLAYER

/* ------------------------------------------------------------------ */
/*                     No-op stub (Kconfig disabled)                  */
/* ------------------------------------------------------------------ */

typedef struct {
    video_player_config_t config;
    bool initialized;
    bool running;
} video_player_context_t;

esp_err_t video_player_init(video_player_config_t *config, video_player_handle_t *ret_handle)
{
    if (config == NULL || ret_handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_LOGW(TAG, "video player disabled (CONFIG_MEDIA_STREAM_ENABLE_VIDEO_PLAYER off) - stub active");
    video_player_context_t *ctx = calloc(1, sizeof(*ctx));
    if (ctx == NULL) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(&ctx->config, config, sizeof(*config));
    ctx->initialized = true;
    *ret_handle = ctx;
    return ESP_OK;
}

esp_err_t video_player_start(video_player_handle_t handle)
{
    video_player_context_t *ctx = (video_player_context_t *)handle;
    if (ctx == NULL || !ctx->initialized) {
        return ESP_ERR_INVALID_ARG;
    }
    ctx->running = true;
    return ESP_OK;
}

esp_err_t video_player_stop(video_player_handle_t handle)
{
    video_player_context_t *ctx = (video_player_context_t *)handle;
    if (ctx == NULL || !ctx->initialized) {
        return ESP_ERR_INVALID_ARG;
    }
    ctx->running = false;
    return ESP_OK;
}

esp_err_t video_player_play_frame(video_player_handle_t handle, const uint8_t *data,
                                  uint32_t len, bool is_keyframe)
{
    video_player_context_t *ctx = (video_player_context_t *)handle;
    if (ctx == NULL || !ctx->initialized || !ctx->running || data == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    (void)is_keyframe;
    return ESP_OK;
}

esp_err_t video_player_clear_buffer(video_player_handle_t handle)
{
    return handle ? ESP_OK : ESP_ERR_INVALID_ARG;
}

esp_err_t video_player_get_buffer_status(video_player_handle_t handle, uint32_t *available_frames)
{
    if (handle == NULL || available_frames == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *available_frames = 0;
    return ESP_OK;
}

esp_err_t video_player_deinit(video_player_handle_t handle)
{
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    free(handle);
    return ESP_OK;
}

#else /* CONFIG_MEDIA_STREAM_ENABLE_VIDEO_PLAYER */

/* ------------------------------------------------------------------ */
/*               Real receive path implementation                     */
/* ------------------------------------------------------------------ */

#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "esp_h264_dec.h"
#include "esp_h264_dec_param.h"
#include "esp_h264_dec_sw.h"

#include "h264_nalu_utils.h"
#include "video_render_display.h"

#define FRAME_ALLOC_CAPS (MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA | MALLOC_CAP_8BIT)

/** One queued encoded frame. Owned by the queue until the decode task frees it. */
typedef struct {
    uint8_t  *data;
    uint32_t  len;
    bool      is_keyframe;
} video_frame_desc_t;

typedef struct {
    video_player_config_t        config;

    /* Decoder */
    esp_h264_dec_handle_t        dec;
    esp_h264_dec_param_handle_t  dec_param;

    /* Render backend */
    video_render_display_handle_t render;

    /* Queue + task */
    QueueHandle_t                queue;
    TaskHandle_t                 task;
    volatile bool                stopping;
    volatile bool                wait_for_idr;

    /* Stream state */
    uint16_t                     width;
    uint16_t                     height;

    /* Stats */
    uint32_t                     frames_queued;
    uint32_t                     frames_dropped;
    uint32_t                     frames_rendered;

    /* Per-second rate sampling. Snapshot the cumulative counters at the
     * start of each 1 s window from the receive callback, then dump the
     * deltas as rx/played fps. */
    uint64_t                     fps_window_start_us;
    uint32_t                     fps_prev_queued;
    uint32_t                     fps_prev_dropped;
    uint32_t                     fps_prev_rendered;

    bool                         initialized;
    bool                         running;
} video_player_context_t;

/** Sentinel pushed into the queue to wake the task on stop. */
#define FRAME_SENTINEL ((video_frame_desc_t *)(uintptr_t)0x1)

static void frame_desc_free(video_frame_desc_t *f)
{
    if (f == NULL || f == FRAME_SENTINEL) return;
    if (f->data) heap_caps_free(f->data);
    heap_caps_free(f);
}

static void drain_queue(QueueHandle_t q)
{
    video_frame_desc_t *f = NULL;
    while (xQueueReceive(q, &f, 0) == pdTRUE) {
        frame_desc_free(f);
    }
}

static esp_err_t ensure_stream_resolution(video_player_context_t *ctx)
{
    if (ctx->width != 0 && ctx->height != 0) {
        return ESP_OK;
    }
    esp_h264_resolution_t res = { 0 };
    if (esp_h264_dec_get_resolution(ctx->dec_param, &res) != ESP_H264_ERR_OK) {
        return ESP_FAIL;
    }
    if (res.width == 0 || res.height == 0) {
        return ESP_FAIL;
    }
    ctx->width  = res.width;
    ctx->height = res.height;
    ESP_LOGI(TAG, "stream resolution: %ux%u", ctx->width, ctx->height);
    return ESP_OK;
}

static void render_decoded_frame(video_player_context_t *ctx,
                                 const esp_h264_dec_out_frame_t *out)
{
    if (ensure_stream_resolution(ctx) != ESP_OK) {
        return;
    }
    const uint16_t W = ctx->width;
    const uint16_t H = ctx->height;
    if (out->outbuf == NULL || out->out_size < (uint32_t)W * H * 3u / 2u) {
        return;
    }
    /* I420 planar layout: Y then U then V, each plane width-contiguous. */
    const uint8_t *y = out->outbuf;
    const uint8_t *u = y + (size_t)W * H;
    const uint8_t *v = u + ((size_t)W * H / 4u);
    if (video_render_display_render_i420(ctx->render, y, u, v, W, H, W, W / 2) == ESP_OK) {
        ctx->frames_rendered++;
    }
}

static void decode_task(void *arg)
{
    video_player_context_t *ctx = (video_player_context_t *)arg;
    ESP_LOGI(TAG, "decode task start");

    while (!ctx->stopping) {
        video_frame_desc_t *f = NULL;
        if (xQueueReceive(ctx->queue, &f, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (f == FRAME_SENTINEL) {
            break;
        }
        if (f == NULL) {
            continue;
        }

        /* Belt-and-braces: if a previous decode left us waiting for an IDR
         * and this frame isn't a keyframe, skip it without feeding the
         * decoder. play_frame() already filters new arrivals, but a queued
         * non-IDR fragment that was enqueued *before* the error would
         * otherwise reach the decoder and can corrupt tinyh264 internal
         * state (causing a load-fault crash on the next process call). */
        if (ctx->wait_for_idr && !f->is_keyframe) {
            ctx->frames_dropped++;
            frame_desc_free(f);
            continue;
        }

        /* A prior reset may have failed to recreate the decoder. Skip
         * decoding until a recreate succeeds (retry here, cheap on the
         * error path) rather than dereferencing a NULL handle. */
        if (ctx->dec == NULL) {
            esp_h264_dec_cfg_sw_t recover_cfg = { .pic_type = ESP_H264_RAW_FMT_I420 };
            if (esp_h264_dec_sw_new(&recover_cfg, &ctx->dec) != ESP_H264_ERR_OK || ctx->dec == NULL) {
                ctx->dec = NULL;
                ctx->frames_dropped++;
                frame_desc_free(f);
                continue;
            }
            ctx->wait_for_idr = true;
        }

        esp_h264_dec_in_frame_t  in  = { 0 };
        esp_h264_dec_out_frame_t out = { 0 };
        in.raw_data.buffer = f->data;
        in.raw_data.len    = f->len;

        while (in.raw_data.len > 0) {
            out = (esp_h264_dec_out_frame_t){ 0 };
            in.consume = 0;
            esp_h264_err_t err = esp_h264_dec_process(ctx->dec, &in, &out);
            if (err != ESP_H264_ERR_OK) {
                ESP_LOGW(TAG, "decode error %d, resetting decoder, waiting for next IDR", (int)err);
                /* tinyh264 retains corrupted internal state across a decode
                 * error — the DPB / ref-pic-list still references freed
                 * pictures, so the next esp_h264_dec_process (even on a clean
                 * IDR) load-faults in h264bsdReorderRefPicList.
                 *
                 * esp_h264_dec_close()/open() are NO-OP stubs in the SW
                 * decoder (they just return OK) — only del+new actually runs
                 * h264bsdShutdown/h264bsdAlloc and clears the storage. So a
                 * real reset is a full teardown + recreate. */
                esp_h264_dec_del(ctx->dec);
                ctx->dec = NULL;
                esp_h264_dec_cfg_sw_t reset_cfg = { .pic_type = ESP_H264_RAW_FMT_I420 };
                if (esp_h264_dec_sw_new(&reset_cfg, &ctx->dec) != ESP_H264_ERR_OK || ctx->dec == NULL) {
                    ESP_LOGE(TAG, "decoder recreate after reset failed — RX video stalled");
                    ctx->dec = NULL;
                }
                ctx->wait_for_idr = true;
                /* Drain any frames already enqueued from before this error. */
                drain_queue(ctx->queue);
                break;
            }
            if (out.out_size > 0) {
                if (ctx->frames_rendered == 0) {
                    ESP_LOGI(TAG, "first decoded frame: %" PRIu32 " bytes, frame_type=%d",
                             out.out_size, (int)out.frame_type);
                }
                render_decoded_frame(ctx, &out);
                if ((ctx->frames_rendered & 0x1F) == 1) {
                    ESP_LOGI(TAG, "decoded %" PRIu32 " frames (queued=%" PRIu32
                             " dropped=%" PRIu32 ")",
                             ctx->frames_rendered, ctx->frames_queued, ctx->frames_dropped);
                }
            }
            if (in.consume == 0) {
                /* Decoder refuses to advance: bail rather than spin. */
                break;
            }
            if (in.consume >= in.raw_data.len) {
                break;
            }
            in.raw_data.buffer += in.consume;
            in.raw_data.len    -= in.consume;
        }
        frame_desc_free(f);
    }

    /* Drain any remaining frames so we don't leak on shutdown. */
    drain_queue(ctx->queue);
    ESP_LOGI(TAG, "decode task exit (queued=%" PRIu32 " rendered=%" PRIu32 " dropped=%" PRIu32 ")",
             ctx->frames_queued, ctx->frames_rendered, ctx->frames_dropped);
    vTaskDeleteWithCaps(NULL);
}

esp_err_t video_player_init(video_player_config_t *config, video_player_handle_t *ret_handle)
{
    if (config == NULL || ret_handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (config->codec != VIDEO_PLAYER_CODEC_H264) {
        ESP_LOGE(TAG, "only H.264 is supported");
        return ESP_ERR_NOT_SUPPORTED;
    }

    video_player_context_t *ctx =
        heap_caps_calloc(1, sizeof(*ctx), FRAME_ALLOC_CAPS);
    if (ctx == NULL) {
        return ESP_ERR_NO_MEM;
    }
    ctx->config       = *config;
    ctx->wait_for_idr = true; /* nothing valid until first IDR arrives */

    /* Create the decoder. */
    esp_h264_dec_cfg_sw_t dec_cfg = { .pic_type = ESP_H264_RAW_FMT_I420 };
    if (esp_h264_dec_sw_new(&dec_cfg, &ctx->dec) != ESP_H264_ERR_OK ||
        ctx->dec == NULL) {
        ESP_LOGE(TAG, "esp_h264_dec_sw_new failed");
        goto fail;
    }
    if (esp_h264_dec_sw_get_param_hd(ctx->dec, &ctx->dec_param) != ESP_H264_ERR_OK ||
        ctx->dec_param == NULL) {
        ESP_LOGE(TAG, "esp_h264_dec_sw_get_param_hd failed");
        goto fail;
    }
    if (esp_h264_dec_open(ctx->dec) != ESP_H264_ERR_OK) {
        ESP_LOGE(TAG, "esp_h264_dec_open failed");
        goto fail;
    }

    /* Init render backend. */
    const video_render_display_cfg_t rcfg = {
        .max_width  = CONFIG_MEDIA_STREAM_PLAYER_MAX_WIDTH,
        .max_height = CONFIG_MEDIA_STREAM_PLAYER_MAX_HEIGHT,
    };
    if (video_render_display_init(&rcfg, &ctx->render) != ESP_OK) {
        ESP_LOGE(TAG, "video_render_display_init failed");
        goto fail;
    }

    /* Frame queue holds pointers, so it is small. */
    ctx->queue = xQueueCreate(CONFIG_MEDIA_STREAM_PLAYER_QUEUE_DEPTH,
                              sizeof(video_frame_desc_t *));
    if (ctx->queue == NULL) {
        ESP_LOGE(TAG, "xQueueCreate failed");
        goto fail;
    }

    ctx->initialized = true;
    *ret_handle = ctx;
    ESP_LOGI(TAG, "video player ready (queue depth %d, max %dx%d)",
             CONFIG_MEDIA_STREAM_PLAYER_QUEUE_DEPTH,
             CONFIG_MEDIA_STREAM_PLAYER_MAX_WIDTH,
             CONFIG_MEDIA_STREAM_PLAYER_MAX_HEIGHT);
    return ESP_OK;

fail:
    if (ctx->queue)    vQueueDelete(ctx->queue);
    if (ctx->render)   video_render_display_deinit(ctx->render);
    if (ctx->dec) {
        esp_h264_dec_close(ctx->dec);
        esp_h264_dec_del(ctx->dec);
    }
    heap_caps_free(ctx);
    return ESP_FAIL;
}

esp_err_t video_player_start(video_player_handle_t handle)
{
    video_player_context_t *ctx = (video_player_context_t *)handle;
    if (ctx == NULL || !ctx->initialized || ctx->running) {
        return ESP_ERR_INVALID_STATE;
    }
    ctx->stopping = false;
    /* RX video decoder pinned to core 1 — paired with video_encoder. Audio
     * path (encoder/decoder/i2s_read/i2s_write) is pinned to core 0, so video
     * work doesn't preempt audio under bidirectional load. */
    BaseType_t ok = xTaskCreateWithCaps(
        decode_task, "vplayer_dec",
        CONFIG_MEDIA_STREAM_PLAYER_TASK_STACK,
        ctx,
        CONFIG_MEDIA_STREAM_PLAYER_TASK_PRIORITY,
        &ctx->task, MALLOC_CAP_SPIRAM);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreateWithCaps failed");
        return ESP_ERR_NO_MEM;
    }
    ctx->running = true;
    return ESP_OK;
}

esp_err_t video_player_stop(video_player_handle_t handle)
{
    video_player_context_t *ctx = (video_player_context_t *)handle;
    if (ctx == NULL || !ctx->initialized) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!ctx->running) {
        return ESP_OK;
    }
    ctx->stopping = true;
    /* Wake the task: push a sentinel after draining. */
    drain_queue(ctx->queue);
    video_frame_desc_t *sentinel = FRAME_SENTINEL;
    xQueueSend(ctx->queue, &sentinel, portMAX_DELAY);

    /* Wait for the task to exit (it calls vTaskDelete(NULL)). */
    while (ctx->task != NULL && eTaskGetState(ctx->task) != eDeleted) {
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    ctx->task = NULL;
    ctx->running = false;
    return ESP_OK;
}

esp_err_t video_player_play_frame(video_player_handle_t handle, const uint8_t *data,
                                  uint32_t len, bool is_keyframe)
{
    video_player_context_t *ctx = (video_player_context_t *)handle;
    if (ctx == NULL || !ctx->initialized || !ctx->running || data == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Log first inbound video frame and periodic call count so it's obvious
     * whether peer is actually sending us video vs. just negotiating the
     * track without traffic. */
    if (ctx->frames_queued + ctx->frames_dropped == 0) {
        ESP_LOGI(TAG, "first inbound video frame: %" PRIu32 " bytes, is_keyframe=%d",
                 len, (int)is_keyframe);
    }

    /* Cross-check: scan the buffer for SPS/IDR and trust whichever arrives. */
    h264_nal_scan_result_t scan = h264_scan_nals(data, len, H264_NAL_DIR_RX);
    const bool key = is_keyframe || scan.has_idr;

    if (ctx->wait_for_idr && !key) {
        ctx->frames_dropped++;
        return ESP_OK;
    }
    if (key) {
        ctx->wait_for_idr = false;
    }

    /* If the queue is full: keyframes evict existing frames so we re-sync from
     * a fresh GOP; non-keyframes are dropped. */
    if (uxQueueSpacesAvailable(ctx->queue) == 0) {
        if (!key) {
            ctx->frames_dropped++;
            return ESP_OK;
        }
        drain_queue(ctx->queue);
    }

    video_frame_desc_t *f = heap_caps_calloc(1, sizeof(*f), FRAME_ALLOC_CAPS);
    if (f == NULL) {
        ctx->frames_dropped++;
        return ESP_ERR_NO_MEM;
    }
    f->data = heap_caps_malloc(len, FRAME_ALLOC_CAPS);
    if (f->data == NULL) {
        heap_caps_free(f);
        ctx->frames_dropped++;
        return ESP_ERR_NO_MEM;
    }
    memcpy(f->data, data, len);
    f->len = len;
    f->is_keyframe = key;

    if (xQueueSend(ctx->queue, &f, 0) != pdTRUE) {
        frame_desc_free(f);
        ctx->frames_dropped++;
        return ESP_ERR_NO_MEM;
    }
    ctx->frames_queued++;

    /* Roll the 1 s rate window. rx counts every inbound frame (queued OR
     * dropped); played counts what actually hit the canvas. The gap between
     * the two is the bottleneck — queue-full drops, decode lag, or render
     * lag — and the absolute rx number tells us if the peer is sending. */
    uint64_t now_us = esp_timer_get_time();
    if (ctx->fps_window_start_us == 0) {
        ctx->fps_window_start_us = now_us;
        ctx->fps_prev_queued     = ctx->frames_queued;
        ctx->fps_prev_dropped    = ctx->frames_dropped;
        ctx->fps_prev_rendered   = ctx->frames_rendered;
    } else if (now_us - ctx->fps_window_start_us >= 1000000ULL) {
        uint64_t window_us = now_us - ctx->fps_window_start_us;
        uint32_t rx_delta     = (ctx->frames_queued + ctx->frames_dropped)
                              - (ctx->fps_prev_queued + ctx->fps_prev_dropped);
        uint32_t played_delta = ctx->frames_rendered - ctx->fps_prev_rendered;
        uint32_t drop_delta   = ctx->frames_dropped  - ctx->fps_prev_dropped;
        uint32_t rx_fps     = (uint32_t)((uint64_t)rx_delta     * 1000000ULL / window_us);
        uint32_t played_fps = (uint32_t)((uint64_t)played_delta * 1000000ULL / window_us);
        ESP_LOGI(TAG, "fps: rx=%" PRIu32 " played=%" PRIu32 " (window drops=%" PRIu32 ")",
                 rx_fps, played_fps, drop_delta);
        ctx->fps_window_start_us = now_us;
        ctx->fps_prev_queued     = ctx->frames_queued;
        ctx->fps_prev_dropped    = ctx->frames_dropped;
        ctx->fps_prev_rendered   = ctx->frames_rendered;
    }

    return ESP_OK;
}

esp_err_t video_player_clear_buffer(video_player_handle_t handle)
{
    video_player_context_t *ctx = (video_player_context_t *)handle;
    if (ctx == NULL || !ctx->initialized) {
        return ESP_ERR_INVALID_ARG;
    }
    drain_queue(ctx->queue);
    ctx->wait_for_idr = true;
    return ESP_OK;
}

esp_err_t video_player_get_buffer_status(video_player_handle_t handle, uint32_t *available_frames)
{
    video_player_context_t *ctx = (video_player_context_t *)handle;
    if (ctx == NULL || !ctx->initialized || available_frames == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *available_frames = (uint32_t)uxQueueMessagesWaiting(ctx->queue);
    return ESP_OK;
}

esp_err_t video_player_deinit(video_player_handle_t handle)
{
    video_player_context_t *ctx = (video_player_context_t *)handle;
    if (ctx == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (ctx->running) {
        video_player_stop(handle);
    }
    if (ctx->queue) {
        drain_queue(ctx->queue);
        vQueueDelete(ctx->queue);
    }
    if (ctx->render) {
        video_render_display_deinit(ctx->render);
    }
    if (ctx->dec) {
        esp_h264_dec_close(ctx->dec);
        esp_h264_dec_del(ctx->dec);
    }
    heap_caps_free(ctx);
    return ESP_OK;
}

#endif /* CONFIG_MEDIA_STREAM_ENABLE_VIDEO_PLAYER */
