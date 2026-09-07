// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
// SPDX-License-Identifier: Apache-2.0
//
// IDF linux target: no camera. Stubs for the capture entry points kvs_webrtc
// references; the rate controller and the sink bus build for real.
#include "video_capture.h"

esp_err_t video_capture_request_keyframe(void)
{
    return ESP_OK;
}

/* No camera: the SDP falls back to the default H.264 level */
esp_err_t video_capture_get_active_resolution(video_resolution_t *resolution)
{
    (void) resolution;
    return ESP_ERR_NOT_SUPPORTED;
}
