/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Direct V4L2 MJPEG capture backend - same approach as esp-video simple_video_server.
 * No media_stream; uses example_video_init + V4L2 + example_encoder.
 */

#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct web_cam_direct web_cam_direct_t;

/**
 * @brief Initialize direct V4L2 capture.
 * Call example_video_init() before this.
 *
 * @param dev_name Device path (e.g. ESP_VIDEO_MIPI_CSI_DEVICE_NAME)
 * @param out_cam  Output camera handle for HTTP handlers
 * @return ESP_OK on success
 */
esp_err_t web_cam_direct_init(const char *dev_name, web_cam_direct_t **out_cam);

/**
 * @brief Capture and send a single frame (JPEG or raw) to HTTP response.
 *
 * @param cam    Camera handle
 * @param req    HTTP request
 * @param as_jpeg true for image/jpeg, false for application/octet-stream
 * @return ESP_OK on success
 */
esp_err_t web_cam_direct_capture_to_http(web_cam_direct_t *cam, httpd_req_t *req, bool as_jpeg);

/**
 * @brief Stream MJPEG loop - blocks until client disconnects.
 *
 * @param cam Camera handle
 * @param req HTTP request (multipart response)
 * @return ESP_OK when stream ends normally
 */
esp_err_t web_cam_direct_stream(web_cam_direct_t *cam, httpd_req_t *req);

/**
 * @brief Deinit and free resources.
 *
 * @param cam Camera handle
 */
void web_cam_direct_deinit(web_cam_direct_t *cam);

#ifdef __cplusplus
}
#endif
