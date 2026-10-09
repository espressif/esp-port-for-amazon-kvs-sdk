/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @brief The H.264 encoder behind the capture pump's encoder sink.
 *
 * One interface over two backends: the P4's hardware encoder (esp_h264_hw_enc.c, wrapped
 * inline here so nothing about it changes) and, on targets with esp_h264 but no hardware
 * block, the software encoder in esp_h264_sw_enc.c.
 *
 * Threading: setup/destroy run while the pump is parked; encode_borrow runs on the encoder
 * sink's task; set_bitrate and request_idr may be called from any task.
 */

#pragma once

#include "media_stream_caps.h"
#include "esp_h264_hw_enc.h"
#include "video_capture.h"

#if MEDIA_STREAM_HAS_SW_H264_ENC

esp_err_t h264_enc_setup(h264_enc_user_cfg_t *cfg);

/**
 * @brief Encode one frame; @p out borrows the encoder's output until the next call.
 *
 * @p timestamp_us is the frame's capture time (0 if unknown), which the encoder's pts follows.
 */
esp_err_t h264_enc_encode_borrow(uint8_t *frame, size_t frame_len, uint64_t timestamp_us,
                                 esp_h264_out_buf_t *out);
esp_err_t h264_enc_set_bitrate(uint32_t bitrate);
/** @brief The frame rate the encoder is offered; its per-frame bit budget follows it. */
esp_err_t h264_enc_set_fps(uint32_t fps);
uint32_t  h264_enc_get_bitrate(void);
esp_err_t h264_enc_request_idr(void);
void      h264_enc_destroy(void);

/**
 * @brief The layout the encoder takes when the camera delivers @p camera_fourcc.
 *
 * The software encoder reads YUYV and I420, so a camera producing either is encoded as it
 * arrives. Anything else is converted to I420 on the way in (the encoder sink is then a
 * converted sink), which is the smaller of the two and a conversion target for every raw
 * format the converter reads.
 */
static inline uint32_t h264_enc_input_for(uint32_t camera_fourcc)
{
    return (camera_fourcc == VIDEO_FOURCC_YUYV || camera_fourcc == VIDEO_FOURCC_I420)
           ? camera_fourcc : VIDEO_FOURCC_I420;
}

/** @brief The layout the encoder was set up for, from its pic_type. */
uint32_t h264_enc_input_fourcc(void);

#else /* hardware encoder, or the stubs in esp_h264_hw_enc.h */

static inline esp_err_t h264_enc_setup(h264_enc_user_cfg_t *cfg)
{
    return esp_h264_setup_encoder(cfg);
}

static inline esp_err_t h264_enc_encode_borrow(uint8_t *frame, size_t frame_len,
                                               uint64_t timestamp_us, esp_h264_out_buf_t *out)
{
    (void)timestamp_us;
    return esp_h264_hw_enc_encode_frame_borrow(frame, frame_len, out);
}

static inline esp_err_t h264_enc_set_bitrate(uint32_t bitrate)
{
    return esp_h264_hw_enc_set_bitrate(bitrate);
}

static inline uint32_t h264_enc_get_bitrate(void)
{
    return esp_h264_hw_enc_get_bitrate();
}

/* The hardware encoder keeps the rate it was set up with. */
static inline esp_err_t h264_enc_set_fps(uint32_t fps)
{
    (void)fps;
    return ESP_ERR_NOT_SUPPORTED;
}

static inline esp_err_t h264_enc_request_idr(void)
{
    return esp_h264_hw_enc_request_idr();
}

static inline void h264_enc_destroy(void)
{
    esp_h264_destroy_encoder();
}

/* The hardware encoder takes O_UYY_E_VYY and nothing else; the encoder sink repacks a UVC
 * camera's YUY2 into it itself. */
static inline uint32_t h264_enc_input_for(uint32_t camera_fourcc)
{
    (void)camera_fourcc;
    return VIDEO_FOURCC_O_UYY_E_VYY;
}

static inline uint32_t h264_enc_input_fourcc(void)
{
    return VIDEO_FOURCC_O_UYY_E_VYY;
}

#endif /* MEDIA_STREAM_HAS_SW_H264_ENC */
