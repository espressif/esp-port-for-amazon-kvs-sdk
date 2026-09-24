/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "H264FrameGrabber.h"
#include "esp_h264_types.h"
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

/**
 * @brief Encode one frame into a newly allocated buffer the caller owns.
 *
 * Costs a malloc + copy per frame. Prefer esp_h264_hw_enc_encode_frame_borrow()
 * where the lifetime allows it. Caller frees ->buffer and the struct.
 */
esp_h264_out_buf_t *esp_h264_hw_enc_encode_frame(uint8_t *frame, size_t frame_len);
esp_err_t esp_h264_hw_enc_set_bitrate(uint32_t bitrate);
uint32_t esp_h264_hw_enc_get_bitrate(void);
/* Request the next emitted frame be an IDR keyframe (PLI response). */
esp_err_t esp_h264_hw_enc_request_idr(void);
void esp_h264_destroy_encoder();
