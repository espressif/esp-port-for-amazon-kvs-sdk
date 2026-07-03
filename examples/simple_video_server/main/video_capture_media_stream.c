/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Media stream MJPEG capture - implementation.
 * Uses video_capture API (MJPEGFrameGrabber) from media_stream component.
 *
 * The MJPEG pipeline is started on-demand when /stream or /capture is
 * accessed and stopped when all users are done.  This keeps the camera
 * free for standalone snapshot capture (video_capture_get_snapshot)
 * when nobody is streaming.
 */

#include <inttypes.h>
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include "video_capture.h"
#include "video_capture_media_stream.h"

#define PART_BOUNDARY          "123456789000000000000987654321"
#define STREAM_BOUNDARY         "\r\n--" PART_BOUNDARY "\r\n"
#define STREAM_PART             "Content-Type: image/jpeg\r\nContent-Length: %" PRIu32 "\r\nX-Timestamp: %d.%06d\r\n\r\n"
#define FRAME_WAIT_MS           100
#define FRAME_RETRY_MAX         50

static const char *TAG = "capture_media";

struct web_cam_media_stream {
    uint16_t width;
    uint16_t height;
    uint8_t quality;
    video_capture_handle_t handle;   /* NULL when pipeline not running */
    SemaphoreHandle_t lock;          /* Protects handle and active_users */
    int active_users;                /* Refcount of stream/capture users */
};

/* -------------------------------------------------------------------------- */
/*  Pipeline lifecycle (refcounted)                                            */
/* -------------------------------------------------------------------------- */

static esp_err_t pipeline_acquire(web_cam_media_stream_t *cam)
{
    xSemaphoreTake(cam->lock, portMAX_DELAY);

    if (cam->active_users == 0) {
        video_capture_config_t config = {
            .codec = VIDEO_CODEC_MJPEG,
            .resolution = { .width = cam->width, .height = cam->height, .fps = 25 },
            .quality = cam->quality,
        };

        esp_err_t ret = video_capture_init(&config, &cam->handle);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "video_capture_init failed: %s", esp_err_to_name(ret));
            cam->handle = NULL;
            xSemaphoreGive(cam->lock);
            return ret;
        }

        ret = video_capture_start(cam->handle);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "video_capture_start failed: %s", esp_err_to_name(ret));
            video_capture_deinit(cam->handle);
            cam->handle = NULL;
            xSemaphoreGive(cam->lock);
            return ret;
        }
        ESP_LOGI(TAG, "MJPEG pipeline started");
    }

    cam->active_users++;
    xSemaphoreGive(cam->lock);
    return ESP_OK;
}

static void pipeline_release(web_cam_media_stream_t *cam)
{
    xSemaphoreTake(cam->lock, portMAX_DELAY);

    if (cam->active_users > 0) {
        cam->active_users--;
    }

    if (cam->active_users == 0 && cam->handle) {
        video_capture_stop(cam->handle);
        video_capture_deinit(cam->handle);
        cam->handle = NULL;
        ESP_LOGI(TAG, "MJPEG pipeline stopped");
    }

    xSemaphoreGive(cam->lock);
}

/* -------------------------------------------------------------------------- */
/*  Frame helpers                                                              */
/* -------------------------------------------------------------------------- */

static esp_err_t get_one_frame(video_capture_handle_t handle, video_frame_t **frame, uint32_t timeout_ms)
{
    if (!handle || !frame) {
        return ESP_ERR_INVALID_ARG;
    }
    *frame = NULL;
    uint32_t elapsed = 0;
    while (elapsed < timeout_ms) {
        esp_err_t ret = video_capture_get_frame(handle, frame, FRAME_WAIT_MS);
        if (ret == ESP_OK && *frame != NULL) {
            return ESP_OK;
        }
        if (ret == ESP_ERR_TIMEOUT) {
            elapsed += FRAME_WAIT_MS;
            vTaskDelay(pdMS_TO_TICKS(FRAME_WAIT_MS));
            continue;
        }
        return ret;
    }
    return ESP_ERR_TIMEOUT;
}

static esp_err_t capture_frame_to_http(httpd_req_t *req, web_cam_media_stream_t *cam, bool as_jpeg)
{
    video_frame_t *frame = NULL;
    esp_err_t ret = get_one_frame(cam->handle, &frame, 3000);
    if (ret != ESP_OK || !frame || !frame->buffer) {
        ESP_LOGE(TAG, "get_one_frame failed: %s", esp_err_to_name(ret));
        return ret != ESP_OK ? ret : ESP_FAIL;
    }

    esp_err_t send_ret;
    if (as_jpeg) {
        send_ret = httpd_resp_send(req, (const char *)frame->buffer, frame->len);
    } else {
        send_ret = httpd_resp_send_chunk(req, (const char *)frame->buffer, frame->len);
        if (send_ret == ESP_OK) {
            httpd_resp_send_chunk(req, NULL, 0);
        }
    }

    video_capture_release_frame(cam->handle, frame);

    return send_ret;
}

static esp_err_t stream_loop(web_cam_media_stream_t *cam, httpd_req_t *req)
{
    uint32_t retries = 0;

    while (1) {
        video_frame_t *frame = NULL;
        esp_err_t ret = get_one_frame(cam->handle, &frame, 500);
        if (ret != ESP_OK || !frame || !frame->buffer) {
            retries++;
            if (retries >= FRAME_RETRY_MAX) {
                ESP_LOGW(TAG, "Stream: no frame after %" PRIu32 " retries", retries);
                return ESP_OK;
            }
            vTaskDelay(pdMS_TO_TICKS(33));
            continue;
        }
        retries = 0;

        if (httpd_resp_send_chunk(req, STREAM_BOUNDARY, strlen(STREAM_BOUNDARY)) != ESP_OK) {
            video_capture_release_frame(cam->handle, frame);
            break;
        }

        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        char http_string[128];
        int hlen = snprintf(http_string, sizeof(http_string), STREAM_PART,
                            (uint32_t)frame->len, (int)ts.tv_sec, (int)(ts.tv_nsec / 1000));
        if (hlen <= 0 || httpd_resp_send_chunk(req, http_string, (size_t)hlen) != ESP_OK) {
            video_capture_release_frame(cam->handle, frame);
            break;
        }

        if (httpd_resp_send_chunk(req, (const char *)frame->buffer, frame->len) != ESP_OK) {
            video_capture_release_frame(cam->handle, frame);
            break;
        }

        video_capture_release_frame(cam->handle, frame);
    }
    return ESP_OK;
}

/* -------------------------------------------------------------------------- */
/*  Public API                                                                 */
/* -------------------------------------------------------------------------- */

esp_err_t web_cam_media_stream_init(uint16_t width, uint16_t height, uint8_t quality,
                                    web_cam_media_stream_t **out_cam)
{
    if (!out_cam || width == 0 || height == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (quality == 0 || quality > 100) {
        quality = 80;
    }

    web_cam_media_stream_t *cam = calloc(1, sizeof(web_cam_media_stream_t));
    if (!cam) {
        return ESP_ERR_NO_MEM;
    }

    cam->width = width;
    cam->height = height;
    cam->quality = quality;
    cam->lock = xSemaphoreCreateMutex();
    if (!cam->lock) {
        free(cam);
        return ESP_ERR_NO_MEM;
    }

    *out_cam = cam;
    ESP_LOGI(TAG, "Media stream configured: %ux%u quality=%u (pipeline starts on demand)",
             width, height, quality);
    return ESP_OK;
}

esp_err_t web_cam_media_stream_capture_to_http(web_cam_media_stream_t *cam, httpd_req_t *req,
                                               bool as_jpeg)
{
    if (!cam || !req) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = pipeline_acquire(cam);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = capture_frame_to_http(req, cam, as_jpeg);

    pipeline_release(cam);
    return ret;
}

esp_err_t web_cam_media_stream_stream(web_cam_media_stream_t *cam, httpd_req_t *req)
{
    if (!cam || !req) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = pipeline_acquire(cam);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = stream_loop(cam, req);

    pipeline_release(cam);
    return ret;
}

void web_cam_media_stream_deinit(web_cam_media_stream_t *cam)
{
    if (!cam) {
        return;
    }
    if (cam->handle) {
        video_capture_stop(cam->handle);
        video_capture_deinit(cam->handle);
        cam->handle = NULL;
    }
    if (cam->lock) {
        vSemaphoreDelete(cam->lock);
    }
    free(cam);
}
