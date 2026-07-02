/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Media stream MJPEG capture backend.
 * Uses video_capture (MJPEGFrameGrabber) from media_stream component.
 */

#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct web_cam_media_stream web_cam_media_stream_t;

/**
 * @brief Initialize media_stream MJPEG capture.
 * Does NOT use example_video_init - media_stream initializes camera via esp_video_if.
 *
 * @param width   Frame width (e.g. 640)
 * @param height  Frame height (e.g. 480)
 * @param quality JPEG quality (1-100)
 * @param out_cam Output camera handle for HTTP handlers
 * @return ESP_OK on success
 */
esp_err_t web_cam_media_stream_init(uint16_t width, uint16_t height, uint8_t quality,
                                    web_cam_media_stream_t **out_cam);

/**
 * @brief Capture and send a single frame (JPEG or raw) to HTTP response.
 *
 * @param cam    Camera handle
 * @param req    HTTP request
 * @param as_jpeg true for image/jpeg, false for application/octet-stream
 * @return ESP_OK on success
 */
esp_err_t web_cam_media_stream_capture_to_http(web_cam_media_stream_t *cam, httpd_req_t *req,
                                                bool as_jpeg);

/**
 * @brief Stream MJPEG loop - blocks until client disconnects.
 *
 * @param cam Camera handle
 * @param req HTTP request (multipart response)
 * @return ESP_OK when stream ends normally
 */
esp_err_t web_cam_media_stream_stream(web_cam_media_stream_t *cam, httpd_req_t *req);

/**
 * @brief Deinit and free resources.
 *
 * @param cam Camera handle
 */
void web_cam_media_stream_deinit(web_cam_media_stream_t *cam);

#ifdef __cplusplus
}
#endif
