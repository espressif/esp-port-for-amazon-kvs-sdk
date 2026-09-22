/*
 * SPDX-FileCopyrightText: 2025-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @brief Adapter implementation for video_capture.h using H264FrameGrabber
 */

#include <string.h>
#include <stdbool.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "video_capture.h"
#include "H264FrameGrabber.h"
#include "MJPEGFrameGrabber.h"

#include "media_stream_caps.h"
#if MEDIA_STREAM_HAS_ESP_VIDEO_CAPTURE
#include "driver/jpeg_encode.h"
#include "esp_video_if.h"
#include "esp_cache.h"
#if MEDIA_STREAM_HAS_IMG_EFFECTS
#include "esp_imgfx_color_convert.h"
#include "esp_imgfx_scale.h"
#endif

/* Extern declarations for P4 snapshot interceptor functions */
extern esp_err_t esp32p4_snapshot_intercept_frame(uint8_t *buf, size_t buf_size,
                                                   size_t *len, uint16_t *width,
                                                   uint16_t *height,
                                                   video_frame_pixformat_t *pixfmt,
                                                   uint32_t timeout_ms);
extern esp_err_t esp32p4_snapshot_direct_grab(uint8_t *buf, size_t buf_size,
                                               size_t *len, uint16_t *width,
                                               uint16_t *height,
                                               video_frame_pixformat_t *pixfmt,
                                               uint32_t timeout_ms);
#endif

static const char *TAG = "video_capture_adapter";

typedef struct {
    video_capture_config_t config;
    bool initialized;
    bool running;
    video_codec_type_t codec_type;
} video_capture_context_t;

/* -------------------------------------------------------------------------- */
/*  Reference-counted pipeline lifecycle                                      */
/*                                                                            */
/*  The camera and encoder are a process-wide singleton, but several consumers */
/*  may now hold them at once. Without counting, the first consumer to call    */
/*  stop() or deinit() would tear the camera down under the others - which is  */
/*  exactly what stopped PutMedia and WebRTC from coexisting. Each handle      */
/*  contributes at most one init reference and one start reference, so a       */
/*  double stop() or a stop() on a never-started handle cannot unbalance it.   */
/* -------------------------------------------------------------------------- */

static SemaphoreHandle_t s_lifecycle_lock;
static uint32_t          s_init_refs;
static uint32_t          s_start_refs;
static video_capture_config_t s_active_profile;

static esp_err_t lifecycle_lock_init(void)
{
    if (s_lifecycle_lock == NULL) {
        s_lifecycle_lock = xSemaphoreCreateRecursiveMutex();
        if (s_lifecycle_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    return ESP_OK;
}

static inline void lifecycle_lock(void)   { xSemaphoreTakeRecursive(s_lifecycle_lock, portMAX_DELAY); }
static inline void lifecycle_unlock(void) { xSemaphoreGiveRecursive(s_lifecycle_lock); }

/* The second and later callers do not get to reconfigure a running pipeline.
 * Say so rather than silently ignoring their settings - which is what used to
 * happen, via camera_and_encoder_init()'s singleton early-return. */
static void warn_profile_mismatch(const video_capture_config_t *want)
{
    const video_capture_config_t *have = &s_active_profile;
    if (want->codec != have->codec ||
        want->resolution.width  != have->resolution.width ||
        want->resolution.height != have->resolution.height ||
        want->resolution.fps    != have->resolution.fps ||
        want->bitrate != have->bitrate) {
        ESP_LOGW(TAG,
                 "Capture already running as codec=%d %ux%u@%u %" PRIu32 "kbps; "
                 "ignoring request for codec=%d %ux%u@%u %" PRIu32 "kbps",
                 have->codec, have->resolution.width, have->resolution.height,
                 have->resolution.fps, have->bitrate,
                 want->codec, want->resolution.width, want->resolution.height,
                 want->resolution.fps, want->bitrate);
    }
}

esp_err_t video_capture_init(video_capture_config_t *config, video_capture_handle_t *ret_handle)
{
    if (config == NULL || ret_handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    // Check supported codecs
    if (config->codec != VIDEO_CODEC_H264 && config->codec != VIDEO_CODEC_MJPEG) {
        ESP_LOGE(TAG, "Only H264 and MJPEG codecs are supported");
        return ESP_ERR_NOT_SUPPORTED;
    }

    esp_err_t ret = lifecycle_lock_init();
    if (ret != ESP_OK) {
        return ret;
    }

    video_capture_context_t *ctx = calloc(1, sizeof(video_capture_context_t));
    if (ctx == NULL) {
        return ESP_ERR_NO_MEM;
    }

    // Save configuration
    memcpy(&ctx->config, config, sizeof(video_capture_config_t));
    ctx->codec_type = config->codec;
    ctx->running = false;

    lifecycle_lock();

    if (s_init_refs > 0) {
        /* Already up. Join it rather than reinitialising underneath the others. */
        if (ctx->codec_type != s_active_profile.codec) {
            ESP_LOGE(TAG, "Capture already running with codec %d, cannot also serve codec %d",
                     s_active_profile.codec, ctx->codec_type);
            lifecycle_unlock();
            free(ctx);
            return ESP_ERR_INVALID_STATE;
        }
        warn_profile_mismatch(config);
        s_init_refs++;
        ctx->initialized = true;
        lifecycle_unlock();
        *ret_handle = ctx;
        ESP_LOGI(TAG, "Joined running capture (init refs=%" PRIu32 ")", s_init_refs);
        return ESP_OK;
    }

    // Initialize the camera and encoder based on codec type
    if (config->codec == VIDEO_CODEC_H264) {
        // Initialize H264 encoder with configuration
        ret = camera_and_encoder_init(config);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize H264 camera and encoder: %d", ret);
            lifecycle_unlock();
            free(ctx);
            return ret;
        }
    } else if (config->codec == VIDEO_CODEC_MJPEG) {
        // Initialize MJPEG encoder
        ret = mjpeg_camera_and_encoder_init(
            config->resolution.width,
            config->resolution.height,
            config->quality
        );
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize MJPEG camera and encoder: %d", ret);
            lifecycle_unlock();
            free(ctx);
            return ret;
        }
    }

    memcpy(&s_active_profile, config, sizeof(s_active_profile));
    s_init_refs = 1;
    ctx->initialized = true;
    lifecycle_unlock();

    *ret_handle = ctx;
    return ESP_OK;
}

esp_err_t video_capture_start(video_capture_handle_t handle)
{
    video_capture_context_t *ctx = (video_capture_context_t *)handle;
    if (ctx == NULL || !ctx->initialized) {
        return ESP_ERR_INVALID_ARG;
    }

    lifecycle_lock();
    if (ctx->running) {
        lifecycle_unlock();
        return ESP_OK;  /* Idempotent per handle, so refs stay balanced */
    }

    if (s_start_refs == 0 && ctx->codec_type == VIDEO_CODEC_H264) {
        esp_err_t ret = h264_encoder_start();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to start H264 encoder: %d", ret);
            lifecycle_unlock();
            return ret;
        }
    }
    s_start_refs++;
    ctx->running = true;
    ESP_LOGI(TAG, "Capture started (start refs=%" PRIu32 ")", s_start_refs);
    lifecycle_unlock();
    return ESP_OK;
}

esp_err_t video_capture_stop(video_capture_handle_t handle)
{
    video_capture_context_t *ctx = (video_capture_context_t *)handle;
    if (ctx == NULL || !ctx->initialized) {
        return ESP_ERR_INVALID_ARG;
    }

    lifecycle_lock();
    if (!ctx->running) {
        lifecycle_unlock();
        return ESP_OK;
    }
    ctx->running = false;
    if (s_start_refs > 0) {
        s_start_refs--;
    }

    if (s_start_refs == 0 && ctx->codec_type == VIDEO_CODEC_H264) {
        /* Last one out stops the encoder. */
        esp_err_t ret = h264_encoder_stop();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to stop H264 encoder: %d", ret);
            lifecycle_unlock();
            return ret;
        }
    } else {
        ESP_LOGI(TAG, "Capture still held by %" PRIu32 " consumer(s), leaving encoder running",
                 s_start_refs);
    }
    lifecycle_unlock();
    return ESP_OK;
}

esp_err_t video_capture_get_frame(video_capture_handle_t handle, video_frame_t **frame, uint32_t wait_ms)
{
    video_capture_context_t *ctx = (video_capture_context_t *)handle;
    if (ctx == NULL || !ctx->initialized || !ctx->running || frame == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* H.264 no longer has a pull path. It used to be backed by an output queue in
     * the grabber, filled by a built-in "legacy-pull" sink that existed purely so
     * kvs_webrtc would not have to change; every consumer paid for that queue
     * whether or not it pulled. Encoded frames now go out through the video_sink
     * registry only.
     *
     * Fail loudly rather than returning ESP_ERR_TIMEOUT, which a caller would
     * reasonably read as "the camera is merely slow" and retry forever. */
    if (ctx->codec_type == VIDEO_CODEC_H264) {
        static bool warned;
        if (!warned) {
            warned = true;
            ESP_LOGE(TAG, "video_capture_get_frame() is not supported for H.264. "
                          "Register a callback with video_sink_register() instead "
                          "(see video_sink.h; kvs_media.c is a worked example).");
        }
        return ESP_ERR_NOT_SUPPORTED;
    }

    // Allocate a video_frame_t to return
    video_frame_t *output_frame = calloc(1, sizeof(video_frame_t));
    if (output_frame == NULL) {
        return ESP_ERR_NO_MEM;
    }

    if (ctx->codec_type == VIDEO_CODEC_MJPEG) {
        // Get a frame using the MJPEG API
        esp_mjpeg_out_buf_t *mjpeg_frame = get_mjpeg_encoded_frame();
        if (mjpeg_frame == NULL) {
            free(output_frame);
            return ESP_ERR_TIMEOUT;
        }

        // Fill in the frame data
        output_frame->buffer = mjpeg_frame->buffer;
        output_frame->len = mjpeg_frame->len;
        output_frame->timestamp = esp_timer_get_time(); // Use current time as timestamp
        output_frame->type = VIDEO_FRAME_TYPE_I; // All MJPEG frames are keyframes

        // Free the mjpeg_frame structure (but not the buffer which is now owned by output_frame)
        free(mjpeg_frame);
    }

    *frame = output_frame;
    return ESP_OK;
}

__attribute__((weak)) esp_err_t video_capture_set_bitrate(video_capture_handle_t handle, uint32_t bitrate_kbps)
{
    return ESP_OK;
}

__attribute__((weak)) esp_err_t video_capture_get_bitrate(video_capture_handle_t handle, uint32_t *bitrate_kbps)
{
    if (bitrate_kbps != NULL) {
        *bitrate_kbps = 500; /* Default bitrate */
    }
    return ESP_OK;
}

__attribute__((weak)) esp_err_t video_capture_request_keyframe(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t video_capture_get_active_resolution(video_resolution_t *resolution)
{
    if (resolution == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
#if MEDIA_STREAM_HAS_ESP_VIDEO_CAPTURE && CONFIG_USE_ESP_VIDEO_IF
    esp_err_t ret = esp_video_if_get_resolution(resolution);
    if (ret == ESP_ERR_INVALID_STATE) {
        /* Not started yet (the SDP is built first): report what it will ask for. */
        ret = esp_video_if_get_expected_resolution(resolution);
    }
    return ret;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t video_capture_release_frame(video_capture_handle_t handle, video_frame_t *frame)
{
    if (handle == NULL || frame == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (frame->buffer != NULL) {
        free(frame->buffer);
    }
    free(frame);
    return ESP_OK;
}

esp_err_t video_capture_deinit(video_capture_handle_t handle)
{
    video_capture_context_t *ctx = (video_capture_context_t *)handle;
    if (ctx == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!ctx->initialized) {
        free(ctx);
        return ESP_ERR_INVALID_ARG;
    }

    /* Drop this handle's start reference first, so a caller that skips stop()
     * cannot strand the encoder in the running state. */
    video_capture_stop(handle);

    lifecycle_lock();
    ctx->initialized = false;
    if (s_init_refs > 0) {
        s_init_refs--;
    }

    if (s_init_refs == 0) {
        // Last consumer out tears the hardware down
        if (ctx->codec_type == VIDEO_CODEC_MJPEG) {
            mjpeg_encoder_deinit();
        } else if (ctx->codec_type == VIDEO_CODEC_H264) {
            // Deinitialize H264 encoder and camera hardware
            h264_encoder_deinit();
        }
        memset(&s_active_profile, 0, sizeof(s_active_profile));
        ESP_LOGI(TAG, "Last consumer released, camera torn down");
    } else {
        ESP_LOGI(TAG, "Capture still held by %" PRIu32 " consumer(s), camera stays up",
                 s_init_refs);
    }
    lifecycle_unlock();

    // Free our context
    free(ctx);
    return ESP_OK;
}

/* -------------------------------------------------------------------------- */
/*  JPEG Snapshot                                                             */
/* -------------------------------------------------------------------------- */

/* The JPEG snapshot path is built on esp_image_effects, a P4-only dependency.
 * Targets without it fall through to the ESP_ERR_NOT_SUPPORTED stubs below. */
#if MEDIA_STREAM_HAS_IMG_EFFECTS

/**
 * Convert O_UYY_E_VYY (ESP32-P4 ISP native YUV420) to RGB565 using esp_image_effects.
 *
 * The ESP32-P4 ISP/camera outputs YUV420 in O_UYY_E_VYY format (semi-packed):
 *   Odd lines:  U Y Y U Y Y ...
 *   Even lines: V Y Y V Y Y ...
 * This is NOT standard I420 planar (YYYY...UU...VV).
 *
 * P4 chip rev < 3 JPEG HW encoder lacks native YUV420 support, so we convert
 * to RGB565 and encode with JPEG_ENCODE_IN_FORMAT_RGB565 instead.
 */
#if CONFIG_ESP_REV_MIN_FULL < 300
static esp_err_t convert_yuv420_to_rgb565(const uint8_t *yuv_buf, size_t yuv_len,
                                          uint16_t width, uint16_t height,
                                          uint8_t **rgb_out, size_t *rgb_len)
{
    size_t expected_yuv = (size_t)width * height * 3 / 2;
    size_t out_size = (size_t)width * height * 2;

    if (yuv_len < expected_yuv) {
        ESP_LOGE(TAG, "YUV buffer too small: %zu < %zu", yuv_len, expected_yuv);
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t *out = heap_caps_aligned_alloc(64, out_size, MALLOC_CAP_SPIRAM);
    if (!out) {
        ESP_LOGE(TAG, "Failed to alloc RGB565 buffer (%zu bytes)", out_size);
        return ESP_ERR_NO_MEM;
    }

    esp_imgfx_color_convert_cfg_t cfg = {
        .in_res = { .width = width, .height = height },
        .in_pixel_fmt = ESP_IMGFX_PIXEL_FMT_O_UYY_E_VYY,
        .out_pixel_fmt = ESP_IMGFX_PIXEL_FMT_RGB565_LE,
        .color_space_std = ESP_IMGFX_COLOR_SPACE_STD_BT709,
    };

    esp_imgfx_color_convert_handle_t cvt = NULL;
    esp_imgfx_err_t err = esp_imgfx_color_convert_open(&cfg, &cvt);
    if (err != ESP_IMGFX_ERR_OK || !cvt) {
        ESP_LOGE(TAG, "esp_imgfx_color_convert_open failed: %d", err);
        free(out);
        return ESP_FAIL;
    }

    esp_imgfx_data_t in_data = { .data = (uint8_t *)yuv_buf, .data_len = yuv_len };
    esp_imgfx_data_t out_data = { .data = out, .data_len = out_size };

    err = esp_imgfx_color_convert_process(cvt, &in_data, &out_data);
    esp_imgfx_color_convert_close(cvt);

    if (err != ESP_IMGFX_ERR_OK) {
        ESP_LOGE(TAG, "esp_imgfx_color_convert_process failed: %d", err);
        free(out);
        return ESP_FAIL;
    }

    *rgb_out = out;
    *rgb_len = out_size;
    return ESP_OK;
}
#endif

/**
 * One-shot JPEG encode: creates a HW JPEG encoder, encodes one frame, and
 * deletes the encoder. Suitable for infrequent snapshot captures.
 *
 * @param yuv_buf     Input YUV420 buffer (passed directly to HW encoder)
 * @param yuv_len     Length of the input buffer
 * @param width       Image width
 * @param height      Image height
 * @param quality     JPEG quality (1-100)
 * @param jpeg_out    Pointer to store allocated JPEG buffer (caller frees)
 * @param jpeg_len    Pointer to store JPEG data length
 * @return ESP_OK on success
 */
static esp_err_t jpeg_one_shot_encode(uint8_t *yuv_buf, size_t yuv_len,
                                      uint16_t width, uint16_t height,
                                      uint8_t quality,
                                      uint16_t out_w, uint16_t out_h,
                                      uint8_t **jpeg_out, size_t *jpeg_len)
{
    if (!yuv_buf || !jpeg_out || !jpeg_len || width == 0 || height == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t *conv_buf = NULL;
    uint8_t *scaled_buf = NULL;
    uint8_t *out_buf = NULL;
    jpeg_encoder_handle_t encoder = NULL;
    esp_err_t ret;

    /* Determine encoder input buffer and format based on chip revision */
    uint8_t *enc_src = yuv_buf;
    size_t enc_src_len = yuv_len;
    jpeg_enc_input_format_t src_type;
    jpeg_down_sampling_type_t sub_sample;

#if CONFIG_ESP_REV_MIN_FULL < 300
    /* P4 rev < 3: HW encoder lacks YUV420 support.
     * Camera outputs O_UYY_E_VYY format (ISP native YUV420).
     * Convert O_UYY_E_VYY → RGB565 via esp_image_effects, then encode as RGB565. */
    size_t conv_len = 0;
    ret = convert_yuv420_to_rgb565(yuv_buf, yuv_len, width, height, &conv_buf, &conv_len);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "O_UYY_E_VYY→RGB565 conversion failed: %s", esp_err_to_name(ret));
        goto cleanup;
    }
    enc_src = conv_buf;
    enc_src_len = conv_len;
    src_type = JPEG_ENCODE_IN_FORMAT_RGB565;
    sub_sample = JPEG_DOWN_SAMPLING_YUV422;
    ESP_LOGI(TAG, "Converted O_UYY_E_VYY (%zu bytes) → RGB565 (%zu bytes)", yuv_len, conv_len);

    /* Optional downscale to a requested resolution. Downscale-only: only when
     * the request fits within the raw frame (out <= raw). If it would upscale
     * (out > raw) or is 0, keep the native frame unchanged. */
    if (out_w != 0 && out_h != 0 && out_w <= width && out_h <= height &&
        (out_w != width || out_h != height)) {
        uint32_t scaled_size = 0;
        esp_imgfx_resolution_t dst_res = { .width = out_w, .height = out_h };
        if (esp_imgfx_get_image_size(ESP_IMGFX_PIXEL_FMT_RGB565_LE, &dst_res, &scaled_size) == ESP_IMGFX_ERR_OK) {
            scaled_buf = heap_caps_aligned_calloc(64, 1, scaled_size, MALLOC_CAP_SPIRAM);
        }
        esp_imgfx_scale_handle_t sh = NULL;
        esp_imgfx_scale_cfg_t scfg = {
            .in_res = { .width = width, .height = height },
            .in_pixel_fmt = ESP_IMGFX_PIXEL_FMT_RGB565_LE,
            .scale_res = { .width = out_w, .height = out_h },
            .filter_type = ESP_IMGFX_SCALE_FILTER_TYPE_BILINEAR,
        };
        esp_imgfx_data_t scin = { .data = enc_src, .data_len = enc_src_len };
        esp_imgfx_data_t scout = { .data = scaled_buf, .data_len = scaled_size };
        if (scaled_buf && esp_imgfx_scale_open(&scfg, &sh) == ESP_IMGFX_ERR_OK) {
            esp_imgfx_err_t sret = esp_imgfx_scale_process(sh, &scin, &scout);
            esp_imgfx_scale_close(sh);
            if (sret == ESP_IMGFX_ERR_OK) {
                enc_src = scaled_buf;
                enc_src_len = scaled_size;
                width = out_w;
                height = out_h;
                ESP_LOGI(TAG, "Downscaled snapshot to %" PRIu16 "x%" PRIu16, out_w, out_h);
            } else {
                ESP_LOGW(TAG, "Snapshot scale failed (%d); using native resolution", sret);
            }
        } else {
            ESP_LOGW(TAG, "Snapshot scale setup failed; using native resolution");
        }
    } else if (out_w != 0 && out_h != 0 && (out_w > width || out_h > height)) {
        ESP_LOGI(TAG, "Requested %" PRIu16 "x%" PRIu16 " exceeds raw %" PRIu16 "x%" PRIu16 "; using native",
                 out_w, out_h, width, height);
    }
#else
    /* P4 rev >= 3: HW encoder supports YUV420 natively */
    src_type = JPEG_ENCODE_IN_FORMAT_YUV420;
    sub_sample = JPEG_DOWN_SAMPLING_YUV420;
    if (out_w != 0 && out_h != 0) {
        ESP_LOGW(TAG, "Snapshot downscale not implemented on the YUV420 path; using native %" PRIu16 "x%" PRIu16, width, height);
    }
#endif

    /* Create a temporary JPEG encoder engine */
    jpeg_encode_engine_cfg_t enc_cfg = {
        .intr_priority = 0,
        .timeout_ms = 3000,
    };
    ret = jpeg_new_encoder_engine(&enc_cfg, &encoder);
    if (ret != ESP_OK || !encoder) {
        ESP_LOGE(TAG, "Failed to create JPEG encoder engine: %s", esp_err_to_name(ret));
        goto cleanup;
    }

    /* Allocate output buffer via JPEG allocator (ensures DMA2D-compatible memory) */
    size_t out_buf_size = 0;
    jpeg_encode_memory_alloc_cfg_t out_mem_cfg = {
        .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER,
    };
    out_buf = (uint8_t *) jpeg_alloc_encoder_mem(width * height, &out_mem_cfg, &out_buf_size);
    if (!out_buf) {
        ESP_LOGE(TAG, "Failed to allocate JPEG output buffer");
        ret = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    /* Configure and encode */
    jpeg_encode_cfg_t config = {
        .width = width,
        .height = height,
        .src_type = src_type,
        .sub_sample = sub_sample,
        .image_quality = quality,
    };

    uint32_t out_len = 0;
    ret = jpeg_encoder_process(encoder, &config, enc_src, enc_src_len,
                               out_buf, out_buf_size, &out_len);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "JPEG encoding failed: %s", esp_err_to_name(ret));
        goto cleanup;
    }

    *jpeg_out = out_buf;
    *jpeg_len = (size_t)out_len;
    out_buf = NULL;  /* Ownership transferred to caller */

    ESP_LOGI(TAG, "JPEG snapshot encoded: %" PRIu16 "x%" PRIu16 " quality=%" PRIu8 " size=%" PRIu32,
             width, height, quality, out_len);

cleanup:
    if (encoder) {
        jpeg_del_encoder_engine(encoder);
    }
    free(conv_buf);
    free(scaled_buf);
    free(out_buf);
    return ret;
}

esp_err_t video_capture_get_snapshot_scaled(uint8_t **jpeg_buf, size_t *jpeg_len,
                                            uint8_t quality, uint16_t out_width,
                                            uint16_t out_height, uint32_t timeout_ms)
{
    if (!jpeg_buf || !jpeg_len) {
        return ESP_ERR_INVALID_ARG;
    }

    *jpeg_buf = NULL;
    *jpeg_len = 0;

    if (quality == 0 || quality > 100) {
        quality = 80;  /* Default quality */
    }

    esp_err_t ret;
    uint16_t width = 0, height = 0;
    size_t raw_len = 0;
    video_frame_pixformat_t pixfmt = PIXFMT_YUV420;

    /* Allocate a raw frame buffer large enough for the maximum expected resolution.
     * YUV420 = 1.5 bytes/pixel. For 1920x1080 that's ~3.1 MB. */
    size_t raw_buf_size = 1920 * 1080 * 3 / 2;  /* Max expected YUV420 frame size */
    uint8_t *raw_buf = heap_caps_aligned_calloc(64, 1, raw_buf_size, MALLOC_CAP_SPIRAM);
    if (!raw_buf) {
        ESP_LOGE(TAG, "Failed to allocate raw frame buffer for snapshot");
        return ESP_ERR_NO_MEM;
    }

    if (h264_encoder_is_running()) {
        /* Path 1: Encoder is running - intercept a frame from the pipeline */
        ESP_LOGI(TAG, "Snapshot: intercepting frame from active encoder pipeline");
        ret = esp32p4_snapshot_intercept_frame(raw_buf, raw_buf_size,
                                               &raw_len, &width, &height,
                                               &pixfmt, timeout_ms);
    } else {
        /* Path 2: Encoder is NOT running - directly grab from camera */
        ESP_LOGI(TAG, "Snapshot: directly grabbing frame from camera");
        ret = esp32p4_snapshot_direct_grab(raw_buf, raw_buf_size,
                                           &raw_len, &width, &height,
                                           &pixfmt, timeout_ms);
    }

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to capture raw frame for snapshot: %s", esp_err_to_name(ret));
        free(raw_buf);
        return ret;
    }

    ESP_LOGI(TAG, "Raw frame captured: %" PRIu16 "x%" PRIu16 " len=%zu pixfmt=%d", width, height, raw_len, pixfmt);

    /* JPEG-encode the raw frame (optionally downscaled to out_width x out_height) */
    ret = jpeg_one_shot_encode(raw_buf, raw_len, width, height, quality,
                               out_width, out_height, jpeg_buf, jpeg_len);

    /* Free the raw frame buffer */
    free(raw_buf);

    return ret;
}

esp_err_t video_capture_get_snapshot(uint8_t **jpeg_buf, size_t *jpeg_len,
                                     uint8_t quality, uint32_t timeout_ms)
{
    return video_capture_get_snapshot_scaled(jpeg_buf, jpeg_len, quality, 0, 0, timeout_ms);
}

void video_capture_snapshot_free(uint8_t *jpeg_buf)
{
    if (jpeg_buf) {
        free(jpeg_buf);
    }
}

#else /* !MEDIA_STREAM_HAS_IMG_EFFECTS */

esp_err_t video_capture_get_snapshot_scaled(uint8_t **jpeg_buf, size_t *jpeg_len,
                                            uint8_t quality, uint16_t out_width,
                                            uint16_t out_height, uint32_t timeout_ms)
{
    (void)quality;
    (void)out_width;
    (void)out_height;
    (void)timeout_ms;
    if (jpeg_buf) *jpeg_buf = NULL;
    if (jpeg_len) *jpeg_len = 0;
    ESP_LOGW(TAG, "JPEG snapshot not supported on this target");
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t video_capture_get_snapshot(uint8_t **jpeg_buf, size_t *jpeg_len,
                                     uint8_t quality, uint32_t timeout_ms)
{
    return video_capture_get_snapshot_scaled(jpeg_buf, jpeg_len, quality, 0, 0, timeout_ms);
}

void video_capture_snapshot_free(uint8_t *jpeg_buf)
{
    if (jpeg_buf) {
        free(jpeg_buf);
    }
}

#endif /* MEDIA_STREAM_HAS_IMG_EFFECTS */
