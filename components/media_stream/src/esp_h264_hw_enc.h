/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "H264FrameGrabber.h"
#include "media_stream_caps.h"
#if MEDIA_STREAM_HAS_HW_H264_ENC
#include "esp_h264_types.h"
#else
/* No hardware encoder on this target (see media_stream_caps.h). The camera
 * delivers H.264 itself, so only the config types the grabber names are needed -
 * esp_h264_frame_type_t and esp_h264_out_buf_t already come from
 * H264FrameGrabber.h, which defines them locally off-target. */
typedef enum {
    ESP_H264_RAW_FMT_YUYV,
    ESP_H264_RAW_FMT_I420,
    ESP_H264_RAW_FMT_O_UYY_E_VYY,
} esp_h264_raw_format_t;

typedef struct {
    uint16_t width;
    uint16_t height;
} esp_h264_resolution_t;

typedef struct {
    uint32_t bitrate;
    uint8_t  qp_min;
    uint8_t  qp_max;
} esp_h264_enc_rc_t;

typedef struct {
    esp_h264_raw_format_t pic_type;
    uint8_t               gop;
    uint8_t               fps;
    esp_h264_resolution_t res;
    esp_h264_enc_rc_t     rc;
} esp_h264_enc_cfg_t;
#endif /* MEDIA_STREAM_HAS_HW_H264_ENC */
#include "esp_video_if_cam_sel.h"   /* MEDIA_STREAM_ENABLE_*_CAM_SENSOR */
#include "esp_err.h"

#define WIDTH               (1920)
#define HEIGHT              (1080)

typedef struct {
    uint8_t *buffer; /*<! Data buffer */
    uint32_t len;    /*<! It is buffer length in byte */
} esp_h264_buf_t;

// Data read callback to read raw data
typedef void data_read_cb_t(void *ctx, esp_h264_buf_t *in_data);

// Data write callback to output encoded frames
typedef void data_write_cb_t(void *ctx, esp_h264_out_buf_t *out_data);

/* The P4 hardware encoder accepts exactly one raw layout - O_UYY_E_VYY - per
 * esp_h264_types.h (YUYV and I420 are software-encoder only). The ISP produces it
 * natively; a UVC camera's YUY2 is repacked into it in the grabber. */
#define MEDIA_STREAM_H264_ENC_PIC_TYPE  ESP_H264_RAW_FMT_O_UYY_E_VYY

#define DEFAULT_ENCODER_CFG() { \
    .gop = 10, \
    .fps = 22, \
    .res = { \
        .width = WIDTH, \
        .height = HEIGHT, \
    }, \
    .rc.bitrate = (512 * 1024), \
    .rc.qp_min = 30, \
    .rc.qp_max = 40, \
    .pic_type = MEDIA_STREAM_H264_ENC_PIC_TYPE, \
}

typedef struct {
    data_read_cb_t *read_cb;
    data_write_cb_t *write_cb;
    esp_h264_enc_cfg_t enc_cfg;
} h264_enc_user_cfg_t;

#if MEDIA_STREAM_HAS_HW_H264_ENC
/* setup encoder with given parameters */
esp_err_t esp_h264_setup_encoder(h264_enc_user_cfg_t *cfg);
esp_err_t esp_h264_hw_enc_process_one_frame();
/**
 * @brief Encode one frame, returning a BORROWED view of the encoder's own output.
 *
 * No allocation and no copy. @p out points into the encoder's output buffer and
 * stays valid only until the next encode call, i.e. roughly one frame period.
 * Callers needing it longer must copy it out - the grabber does that into a
 * small rotating pool rather than allocating per frame.
 *
 * @param frame      Raw input frame
 * @param frame_len  Length of the raw frame
 * @param out        Filled with buffer/len/type on success
 * @return ESP_OK, ESP_ERR_INVALID_ARG, or ESP_FAIL if the encoder rejected it
 */
esp_err_t esp_h264_hw_enc_encode_frame_borrow(uint8_t *frame, size_t frame_len,
                                              esp_h264_out_buf_t *out);

esp_err_t esp_h264_hw_enc_set_bitrate(uint32_t bitrate);
uint32_t esp_h264_hw_enc_get_bitrate(void);
/* Request the next emitted frame be an IDR keyframe (PLI response). */
esp_err_t esp_h264_hw_enc_request_idr(void);
void esp_h264_destroy_encoder();

#else /* !MEDIA_STREAM_HAS_HW_H264_ENC */

/* esp_h264_hw_enc.c compiles to an empty object on these targets, so these are
 * inline rather than declarations: the encode calls sit in `if (!is_passthrough)`
 * branches that are unreachable when the camera supplies H.264, but they still
 * have to compile and link. Failing here degrades to "encoder produced nothing",
 * which the grabber already handles by dropping the frame. */
static inline esp_err_t esp_h264_setup_encoder(h264_enc_user_cfg_t *cfg)
{
    (void) cfg;
    return ESP_ERR_NOT_SUPPORTED;
}

static inline esp_err_t esp_h264_hw_enc_process_one_frame(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

static inline esp_err_t esp_h264_hw_enc_encode_frame_borrow(uint8_t *frame, size_t frame_len,
                                                            esp_h264_out_buf_t *out)
{
    (void) frame;
    (void) frame_len;
    (void) out;
    return ESP_ERR_NOT_SUPPORTED;
}

static inline esp_err_t esp_h264_hw_enc_set_bitrate(uint32_t bitrate)
{
    (void) bitrate;
    return ESP_ERR_NOT_SUPPORTED;
}

static inline uint32_t esp_h264_hw_enc_get_bitrate(void)
{
    return 0;
}

static inline esp_err_t esp_h264_hw_enc_request_idr(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

static inline esp_err_t esp_h264_hw_enc_set_reset_request(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

static inline void esp_h264_destroy_encoder(void)
{
}

#endif /* MEDIA_STREAM_HAS_HW_H264_ENC */
