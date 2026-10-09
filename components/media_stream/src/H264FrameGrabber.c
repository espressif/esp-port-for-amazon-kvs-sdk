/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <inttypes.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"

#include "esp_heap_caps.h"
#include "webrtc_mem_utils.h"

#include "sdkconfig.h"

static const char *TAG __attribute__((unused)) = "H264FrameGrabber";

#include "H264FrameGrabber.h"
#include "video_sink_priv.h"

#if MEDIA_STREAM_HAS_ESP_VIDEO_CAPTURE
/* The capture pump in esp32p4_frame_grabber.c serves every target with esp_video
 * capture: the P4 (MIPI-CSI or UVC, hardware encoder) and the S3 (DVP, software
 * encoder). */
#include "h264_enc.h"
#if MEDIA_STREAM_HAS_HW_H264_ENC
#include "esp_h264_enc_single.h"
#endif
#include "esp_video_if_cam_sel.h"   /* MEDIA_STREAM_ENABLE_*_CAM_SENSOR selection */
#include "esp_video_if.h"           /* esp_video_if_set_desired_resolution() */
extern esp_err_t esp32p4_frame_grabber_init(void);
extern esp_err_t esp32p4_frame_grabber_start(void);
extern esp_err_t esp32p4_frame_grabber_stop(void);
extern esp_err_t esp32p4_frame_grabber_deinit(void);
extern bool esp32p4_is_encoder_running(void);
extern bool esp32p4_is_encoder_initialized(void);
extern esp_err_t esp32p4_snapshot_intercept_frame(uint8_t *buf, size_t buf_size,
                                                   size_t *len, uint16_t *width,
                                                   uint16_t *height,
                                                   uint32_t timeout_ms);
extern esp_err_t esp32p4_snapshot_direct_grab(uint8_t *buf, size_t buf_size,
                                               size_t *len, uint16_t *width,
                                               uint16_t *height,
                                               uint32_t timeout_ms);

/* File-scope variable to track camera initialization state (reset on deinit) */
static bool camera_enc_init_done = false;

esp_err_t camera_and_encoder_init(video_capture_config_t *config)
{
    if (camera_enc_init_done == true) {
        ESP_LOGI(TAG, "camera_and_encoder_init already done!");
        return ESP_OK;
    }

    /* Hand the requested resolution to esp_video_if BEFORE the grabber initialises the
     * camera, which is the only window in which it can act on it: configure_camera_format()
     * reads g_desired_resolution when it builds the first VIDIOC_S_FMT.
     *
     * This used to be `(void) config;`, and the whole video_capture_config_t was dropped on
     * the floor here. Nothing else on the P4 CSI path set the desired resolution - only
     * MJPEGFrameGrabber.c did - so g_desired_resolution stayed {0,0} and the format request
     * fell back to MEDIA_STREAM_CAPTURE_WIDTH/HEIGHT, which on this path resolve to the
     * WIDTH/HEIGHT macros in esp_h264_hw_enc.h: a hardcoded 1920x1080. Every P4 CSI
     * example therefore asked the sensor for 1080p regardless of what it had configured.
     *
     * It went unnoticed because the fallback list rescued it: with only the 720p sensor
     * format compiled in, S_FMT for 1080p returned EINVAL, esp_video_if logged a warning
     * and retried down the list, and the second entry happened to be the 720p the app
     * wanted all along. Compile the 1080p sensor format in as well and the first request
     * succeeds - so enabling a format silently changes the capture resolution of an
     * example that never asked for it.
     *
     * fps is passed through too: esp_video_if defaults it to 30 when unset, which is a
     * frame rate no sensor here actually delivers. */
    if (config != NULL && config->resolution.width != 0 && config->resolution.height != 0) {
        const video_resolution_t res = {
            .width  = config->resolution.width,
            .height = config->resolution.height,
            .fps    = config->resolution.fps,
        };
        esp_video_if_set_desired_resolution(&res);
    }

    /* Propagate the failure and do not latch camera_enc_init_done. Swallowing it let
       the caller go straight on to start the capture */
    esp_err_t ret = esp32p4_frame_grabber_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "frame grabber init failed: %s", esp_err_to_name(ret));
        return ret;
    }
    camera_enc_init_done = true;
    return ESP_OK;
}

/* The H.264 pull path is replaced by the video_sink registry. */
esp_h264_out_buf_t *get_h264_encoded_frame(void)
{
    static bool warned;
    if (!warned) {
        warned = true;
        ESP_LOGE(TAG, "H.264 pull API removed - register a sink with video_sink_register()");
    }
    return NULL;
}

esp_err_t video_capture_set_bitrate(video_capture_handle_t handle, uint32_t bitrate_kbps)
{
    (void) handle;
#if MEDIA_STREAM_HAS_ESP_H264
    return h264_enc_set_bitrate(bitrate_kbps * 1000);
#else
    /* The camera encodes and owns its own bitrate; the UVC host driver exposes no
     * encoding-unit control to change it. */
    (void) bitrate_kbps;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t video_capture_get_bitrate(video_capture_handle_t handle, uint32_t *bitrate_kbps)
{
    (void) handle;
    if (bitrate_kbps == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
#if MEDIA_STREAM_HAS_ESP_H264
    *bitrate_kbps = h264_enc_get_bitrate() / 1000;
    return ESP_OK;
#else
    *bitrate_kbps = 0;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t h264_encoder_start(void)
{
    return esp32p4_frame_grabber_start();
}

esp_err_t h264_encoder_stop(void)
{
    return esp32p4_frame_grabber_stop();
}

esp_err_t h264_encoder_deinit(void)
{
    esp_err_t ret = esp32p4_frame_grabber_deinit();
    /* Reset the camera initialization flag so it can be reinitialized later */
    camera_enc_init_done = false;
    return ret;
}

bool h264_encoder_is_running(void)
{
    return esp32p4_is_encoder_running();
}

bool h264_encoder_is_initialized(void)
{
    return esp32p4_is_encoder_initialized();
}
#else /* all other targets */
esp_err_t camera_and_encoder_init(video_capture_config_t *config)
{
    (void) config;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_h264_out_buf_t *get_h264_encoded_frame(void)
{
    /* Dummy function which does nothing */
    return NULL;
}

esp_err_t video_capture_set_bitrate(video_capture_handle_t handle, uint32_t bitrate_kbps)
{
    (void) handle;
    (void) bitrate_kbps;
    return ESP_OK;
}

esp_err_t video_capture_get_bitrate(video_capture_handle_t handle, uint32_t *bitrate_kbps)
{
    (void) handle;
    if (bitrate_kbps != NULL) {
        *bitrate_kbps = 500; /* Default bitrate for unsupported targets */
    }
    return ESP_OK;
}

esp_err_t h264_encoder_start(void)
{
    // No-op for unsupported targets
    return ESP_OK;
}

esp_err_t h264_encoder_stop(void)
{
    // No-op for unsupported targets
    return ESP_OK;
}

esp_err_t h264_encoder_deinit(void)
{
    // No-op for unsupported targets
    return ESP_OK;
}

bool h264_encoder_is_running(void)
{
    return false;
}

bool h264_encoder_is_initialized(void)
{
    return false;
}
#endif
