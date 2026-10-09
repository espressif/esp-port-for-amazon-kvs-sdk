/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @brief Software (openh264) backend for h264_enc.h, on targets with no H.264 hardware.
 *
 * Everything that touches the encoder instance happens in h264_enc_encode_borrow(), on the
 * encoder sink's task. Bitrate and frame-rate changes and keyframe requests arrive from other
 * tasks (the rate controller, the capture pump, a viewer's PLI) and are only recorded there,
 * then applied before the next encode, so openh264 is never reconfigured in the middle of a
 * frame.
 */

#include "media_stream_caps.h"
#if MEDIA_STREAM_HAS_SW_H264_ENC

#include <stdatomic.h>
#include <string.h>
#include <inttypes.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_h264_enc_single_sw.h"
#include "h264_enc.h"

static const char *TAG = "h264_sw_enc";

/* Same clamp and deadband as the hardware wrapper, so the rate controller sees one
 * behaviour whichever encoder it is steering. */
#define MIN_BITRATE          (100 * 1024)
#define MAX_BITRATE          (2 * 1024 * 1024)
#define MIN_BITRATE_DELTA    (10 * 1024)

/* A PLI is answered at most once a second; a keyframe that recent already serves it. */
#define IDR_FORCE_MIN_INTERVAL_US  (1000 * 1000)
/* Frames to wait for the GOP=1 override to produce an IDR before recreating the encoder. */
#define IDR_FORCE_MAX_FRAMES       2

#define TIMING_EVERY  64

static struct {
    esp_h264_enc_handle_t          enc;
    esp_h264_enc_param_sw_handle_t param;
    esp_h264_enc_cfg_sw_t          cfg;
    esp_h264_enc_in_frame_t        in_frame;
    esp_h264_enc_out_frame_t       out_frame;
    uint32_t                       frame_count;
    uint64_t                       first_ts_us;   /* capture time of pts 0 */

    _Atomic uint32_t pending_bitrate;   /* 0 = nothing pending */
    _Atomic uint32_t pending_fps;       /* 0 = nothing pending */
    atomic_bool      idr_requested;
    bool             gop_overridden;
    uint8_t          frames_since_force;
    int64_t          last_force_us;
    uint32_t         open_failures;

    uint32_t us_sum;
    uint32_t us_max;
    uint32_t samples;
} s_enc;

/* Retried on every frame after a failed recreate, so the log is rate-limited. */
static esp_err_t enc_open(void)
{
    esp_h264_err_t ret = esp_h264_enc_sw_new(&s_enc.cfg, &s_enc.enc);
    if (ret == ESP_H264_ERR_OK) {
        ret = esp_h264_enc_open(s_enc.enc);
        if (ret == ESP_H264_ERR_OK) {
            ret = esp_h264_enc_sw_get_param_hd(s_enc.enc, &s_enc.param);
        }
        if (ret != ESP_H264_ERR_OK) {
            esp_h264_enc_close(s_enc.enc);
            esp_h264_enc_del(s_enc.enc);
        }
    }
    if (ret != ESP_H264_ERR_OK) {
        s_enc.enc = NULL;
        s_enc.param = NULL;
        if (s_enc.open_failures++ % 32 == 0) {
            ESP_LOGE(TAG, "encoder open failed: %d (count=%" PRIu32 ")", (int)ret,
                     s_enc.open_failures);
        }
        return ESP_FAIL;
    }
    s_enc.open_failures = 0;
    return ESP_OK;
}

static void enc_close(void)
{
    if (s_enc.enc) {
        esp_h264_enc_close(s_enc.enc);
        esp_h264_enc_del(s_enc.enc);
        s_enc.enc = NULL;
        s_enc.param = NULL;
    }
}

esp_err_t h264_enc_setup(h264_enc_user_cfg_t *cfg)
{
    if (cfg == NULL || cfg->enc_cfg.fps == 0 || cfg->enc_cfg.gop == 0 ||
        (cfg->enc_cfg.pic_type != ESP_H264_RAW_FMT_YUYV && cfg->enc_cfg.pic_type != ESP_H264_RAW_FMT_I420)) {
        return ESP_ERR_INVALID_ARG;
    }
    h264_enc_destroy();

    s_enc.cfg = cfg->enc_cfg;
    /* Worst case is an IDR of an unusually busy scene; at QVGA that is a few tens of KB, so a
     * raw frame's worth of room is ample. */
    const size_t out_len = (size_t)s_enc.cfg.res.width * s_enc.cfg.res.height * 2;
    s_enc.out_frame.raw_data.buffer = heap_caps_aligned_alloc(16, out_len, MALLOC_CAP_SPIRAM);
    s_enc.out_frame.raw_data.len = out_len;
    if (s_enc.out_frame.raw_data.buffer == NULL) {
        ESP_LOGE(TAG, "no PSRAM for a %u byte output buffer", (unsigned)out_len);
        return ESP_ERR_NO_MEM;
    }
    if (enc_open() != ESP_OK) {
        h264_enc_destroy();
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "software encoder %ux%u @ %u fps, gop %u, %" PRIu32 " bps, qp %u-%u",
             s_enc.cfg.res.width, s_enc.cfg.res.height, s_enc.cfg.fps, s_enc.cfg.gop,
             s_enc.cfg.rc.bitrate, s_enc.cfg.rc.qp_min, s_enc.cfg.rc.qp_max);
    return ESP_OK;
}

void h264_enc_destroy(void)
{
    enc_close();
    heap_caps_free(s_enc.out_frame.raw_data.buffer);
    memset(&s_enc, 0, sizeof(s_enc));
}

uint32_t h264_enc_input_fourcc(void)
{
    return s_enc.cfg.pic_type == ESP_H264_RAW_FMT_I420 ? VIDEO_FOURCC_I420 : VIDEO_FOURCC_YUYV;
}

esp_err_t h264_enc_set_bitrate(uint32_t bitrate)
{
    if (bitrate < MIN_BITRATE) {
        bitrate = MIN_BITRATE;
    }
    if (bitrate > MAX_BITRATE) {
        bitrate = MAX_BITRATE;
    }
    const uint32_t curr = s_enc.cfg.rc.bitrate;
    if (bitrate > curr + MIN_BITRATE_DELTA || bitrate + MIN_BITRATE_DELTA < curr) {
        atomic_store(&s_enc.pending_bitrate, bitrate);
        return ESP_OK;
    }
    return ESP_FAIL;
}

esp_err_t h264_enc_set_fps(uint32_t fps)
{
    if (fps == 0 || fps > UINT8_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    atomic_store(&s_enc.pending_fps, fps);
    return ESP_OK;
}

uint32_t h264_enc_get_bitrate(void)
{
    const uint32_t pending = atomic_load(&s_enc.pending_bitrate);
    return pending ? pending : s_enc.cfg.rc.bitrate;
}

esp_err_t h264_enc_request_idr(void)
{
    /* Set up is enough: between instances the next one opens with an IDR anyway. */
    if (s_enc.out_frame.raw_data.buffer == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    atomic_store(&s_enc.idr_requested, true);
    return ESP_OK;
}

static void apply_pending(void)
{
    const uint32_t bitrate = atomic_exchange(&s_enc.pending_bitrate, 0);
    if (bitrate && esp_h264_enc_set_bitrate(s_enc.param, bitrate) == ESP_H264_ERR_OK) {
        s_enc.cfg.rc.bitrate = bitrate;
    }

    /* openh264 budgets each frame as bitrate / frame rate, so it has to know the rate it is
     * actually being offered, or it undershoots the bitrate in proportion. */
    const uint32_t fps = atomic_exchange(&s_enc.pending_fps, 0);
    if (fps && fps != s_enc.cfg.fps &&
        esp_h264_enc_set_fps(s_enc.param, (uint8_t)fps) == ESP_H264_ERR_OK) {
        s_enc.cfg.fps = (uint8_t)fps;
    }

    /* esp_h264 has no force-IDR for the software encoder, so the GOP is dropped to 1 for
     * as long as it takes an IDR to come out, the same trick the hardware wrapper uses. */
    if (atomic_exchange(&s_enc.idr_requested, false) && !s_enc.gop_overridden) {
        const int64_t now = esp_timer_get_time();
        if (now - s_enc.last_force_us >= IDR_FORCE_MIN_INTERVAL_US &&
            esp_h264_enc_set_gop(s_enc.param, 1) == ESP_H264_ERR_OK) {
            s_enc.last_force_us = now;
            s_enc.gop_overridden = true;
            s_enc.frames_since_force = 0;
            ESP_LOGW(TAG, "PLI: forcing keyframe (GOP=1 until IDR emitted)");
        }
    }
}

/* The override has not produced an IDR in time. A fresh encoder always opens with one. The
 * old instance goes first, so the new one has its memory; if the open still fails, the next
 * frame tries again. */
static void recreate_for_idr(void)
{
    ESP_LOGW(TAG, "GOP=1 did not yield an IDR in %d frames; recreating the encoder",
             IDR_FORCE_MAX_FRAMES);
    enc_close();
    s_enc.frame_count = 0;
    s_enc.gop_overridden = false;
    (void)enc_open();
}

esp_err_t h264_enc_encode_borrow(uint8_t *frame, size_t frame_len, uint64_t timestamp_us,
                                 esp_h264_out_buf_t *out)
{
    if (frame == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_enc.out_frame.raw_data.buffer == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_enc.enc == NULL && enc_open() != ESP_OK) {
        return ESP_FAIL;
    }
    apply_pending();

    s_enc.in_frame.raw_data.buffer = frame;
    s_enc.in_frame.raw_data.len    = frame_len;
    /* pts follows the capture clock, so it stays true while the frame rate moves. */
    if (timestamp_us != 0 && s_enc.first_ts_us == 0) {
        s_enc.first_ts_us = timestamp_us;
    }
    s_enc.in_frame.pts = (timestamp_us != 0 && timestamp_us >= s_enc.first_ts_us)
                         ? (uint32_t)((timestamp_us - s_enc.first_ts_us) / 1000)
                         : (uint32_t)((uint64_t)s_enc.frame_count * 1000 / s_enc.cfg.fps);
    s_enc.frame_count++;

    const int64_t t0 = esp_timer_get_time();
    esp_h264_err_t ret = esp_h264_enc_process(s_enc.enc, &s_enc.in_frame, &s_enc.out_frame);
    const uint32_t us = (uint32_t)(esp_timer_get_time() - t0);
    if (ret != ESP_H264_ERR_OK) {
        return ESP_FAIL;
    }

    out->buffer = s_enc.out_frame.raw_data.buffer;   /* borrowed */
    out->len    = s_enc.out_frame.length;
    out->type   = s_enc.out_frame.frame_type;

    if (s_enc.gop_overridden) {
        if (out->type == ESP_H264_FRAME_TYPE_IDR) {
            esp_h264_enc_set_gop(s_enc.param, s_enc.cfg.gop);
            s_enc.gop_overridden = false;
            ESP_LOGW(TAG, "PLI: IDR emitted, GOP restored to %u", s_enc.cfg.gop);
        } else if (++s_enc.frames_since_force >= IDR_FORCE_MAX_FRAMES) {
            recreate_for_idr();
        }
    }

    s_enc.us_sum += us;
    if (us > s_enc.us_max) {
        s_enc.us_max = us;
    }
    if (++s_enc.samples >= TIMING_EVERY) {
        ESP_LOGI(TAG, "encode avg %" PRIu32 " us (max %" PRIu32 ") over %" PRIu32 " frames, %"
                 PRIu32 " bps", s_enc.us_sum / s_enc.samples, s_enc.us_max, s_enc.samples,
                 s_enc.cfg.rc.bitrate);
        s_enc.us_sum = s_enc.us_max = s_enc.samples = 0;
    }
    return ESP_OK;
}

#endif /* MEDIA_STREAM_HAS_SW_H264_ENC */
