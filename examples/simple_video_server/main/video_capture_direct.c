/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Direct V4L2 MJPEG capture - implementation.
 */

#include <inttypes.h>
#include <string.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/errno.h>
#include "linux/videodev2.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "video_capture_direct.h"
#include "example_video_common.h"
#include "esp_video_device.h"

#define BUFFER_COUNT           3
#define JPEG_QUALITY           80
#define PART_BOUNDARY          "123456789000000000000987654321"
#define STREAM_BOUNDARY         "\r\n--" PART_BOUNDARY "\r\n"
#define STREAM_PART             "Content-Type: image/jpeg\r\nContent-Length: %" PRIu32 "\r\nX-Timestamp: %d.%06d\r\n\r\n"

static const char *TAG = "capture_direct";

typedef struct web_cam_video {
    int fd;
    example_encoder_handle_t encoder_handle;
    uint8_t *jpeg_out_buf;
    uint32_t jpeg_out_size;
    uint8_t *buffer[BUFFER_COUNT];
    uint32_t buffer_size;
    uint32_t width;
    uint32_t height;
    uint32_t pixel_format;
    uint8_t jpeg_quality;
    uint32_t frame_rate;
    SemaphoreHandle_t sem;
} web_cam_video_t;

struct web_cam_direct {
    web_cam_video_t video;
};

static esp_err_t capture_frame_to_http(httpd_req_t *req, web_cam_video_t *video, bool as_jpeg)
{
    struct v4l2_buffer buf;
    uint32_t jpeg_encoded_size;

    memset(&buf, 0, sizeof(buf));
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    if (ioctl(video->fd, VIDIOC_DQBUF, &buf) != 0) {
        ESP_LOGE(TAG, "VIDIOC_DQBUF failed, errno=%d", errno);
        return ESP_FAIL;
    }
    if (!(buf.flags & V4L2_BUF_FLAG_DONE)) {
        ioctl(video->fd, VIDIOC_QBUF, &buf);
        return ESP_ERR_INVALID_RESPONSE;
    }

    if (video->pixel_format == V4L2_PIX_FMT_JPEG) {
        jpeg_encoded_size = buf.bytesused;
        if (as_jpeg) {
            if (httpd_resp_send(req, (char *)video->buffer[buf.index], buf.bytesused) != ESP_OK) {
                ioctl(video->fd, VIDIOC_QBUF, &buf);
                return ESP_FAIL;
            }
        } else {
            if (httpd_resp_send_chunk(req, (char *)video->buffer[buf.index], buf.bytesused) != ESP_OK) {
                ioctl(video->fd, VIDIOC_QBUF, &buf);
                return ESP_FAIL;
            }
        }
    } else {
        if (xSemaphoreTake(video->sem, portMAX_DELAY) != pdPASS) {
            ioctl(video->fd, VIDIOC_QBUF, &buf);
            return ESP_FAIL;
        }
        esp_err_t ret = example_encoder_process(video->encoder_handle, video->buffer[buf.index],
                                                video->buffer_size, video->jpeg_out_buf,
                                                video->jpeg_out_size, &jpeg_encoded_size);
        xSemaphoreGive(video->sem);
        if (ret != ESP_OK) {
            ioctl(video->fd, VIDIOC_QBUF, &buf);
            return ret;
        }
        if (as_jpeg) {
            if (httpd_resp_send(req, (char *)video->jpeg_out_buf, jpeg_encoded_size) != ESP_OK) {
                ioctl(video->fd, VIDIOC_QBUF, &buf);
                return ESP_FAIL;
            }
        } else {
            if (httpd_resp_send_chunk(req, (char *)video->jpeg_out_buf, jpeg_encoded_size) != ESP_OK) {
                ioctl(video->fd, VIDIOC_QBUF, &buf);
                return ESP_FAIL;
            }
        }
    }

    if (ioctl(video->fd, VIDIOC_QBUF, &buf) != 0) {
        ESP_LOGE(TAG, "VIDIOC_QBUF failed");
        return ESP_FAIL;
    }
    if (!as_jpeg) {
        httpd_resp_send_chunk(req, NULL, 0);
    }
    return ESP_OK;
}

static esp_err_t init_web_cam_video(web_cam_video_t *video, const char *dev_name)
{
    int fd = open(dev_name, O_RDWR);
    if (fd < 0) {
        ESP_LOGE(TAG, "Open %s failed, errno=%d", dev_name, errno);
        return ESP_ERR_NOT_FOUND;
    }

    struct v4l2_format format;
    memset(&format, 0, sizeof(format));
    format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(fd, VIDIOC_G_FMT, &format) != 0) {
        ESP_LOGE(TAG, "VIDIOC_G_FMT failed");
        close(fd);
        return ESP_FAIL;
    }

    format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    format.fmt.pix.pixelformat = V4L2_PIX_FMT_YUV420;
    if (ioctl(fd, VIDIOC_S_FMT, &format) != 0) {
        ESP_LOGE(TAG, "VIDIOC_S_FMT YUV420 failed");
        close(fd);
        return ESP_FAIL;
    }

    struct v4l2_streamparm sparm;
    memset(&sparm, 0, sizeof(sparm));
    sparm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(fd, VIDIOC_G_PARM, &sparm) == 0) {
        video->frame_rate = sparm.parm.capture.timeperframe.denominator /
                           sparm.parm.capture.timeperframe.numerator;
    } else {
        video->frame_rate = 25;
    }

    struct v4l2_requestbuffers req = {0};
    req.count = BUFFER_COUNT;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (ioctl(fd, VIDIOC_REQBUFS, &req) != 0) {
        ESP_LOGE(TAG, "VIDIOC_REQBUFS failed");
        close(fd);
        return ESP_FAIL;
    }

    for (int i = 0; i < BUFFER_COUNT; i++) {
        struct v4l2_buffer buf = {0};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        if (ioctl(fd, VIDIOC_QUERYBUF, &buf) != 0) {
            ESP_LOGE(TAG, "VIDIOC_QUERYBUF failed");
            for (int j = 0; j < i; j++) {
                munmap(video->buffer[j], video->buffer_size);
            }
            close(fd);
            return ESP_FAIL;
        }

        video->buffer[i] = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, buf.m.offset);
        if (video->buffer[i] == MAP_FAILED) {
            ESP_LOGE(TAG, "mmap failed");
            for (int j = 0; j < i; j++) {
                munmap(video->buffer[j], video->buffer_size);
            }
            close(fd);
            return ESP_FAIL;
        }
        video->buffer_size = buf.length;

        if (ioctl(fd, VIDIOC_QBUF, &buf) != 0) {
            ESP_LOGE(TAG, "VIDIOC_QBUF failed");
            for (int j = 0; j <= i; j++) {
                munmap(video->buffer[j], video->buffer_size);
            }
            close(fd);
            return ESP_FAIL;
        }
    }

    video->fd = fd;
    video->width = format.fmt.pix.width;
    video->height = format.fmt.pix.height;
    video->pixel_format = format.fmt.pix.pixelformat;
    video->jpeg_quality = JPEG_QUALITY;

    example_encoder_config_t enc_cfg = {
        .width = video->width,
        .height = video->height,
        .pixel_format = video->pixel_format,
        .quality = video->jpeg_quality,
    };
    if (example_encoder_init(&enc_cfg, &video->encoder_handle) != ESP_OK) {
        ESP_LOGE(TAG, "example_encoder_init failed");
        for (int i = 0; i < BUFFER_COUNT; i++) {
            munmap(video->buffer[i], video->buffer_size);
        }
        close(fd);
        return ESP_FAIL;
    }
    if (example_encoder_alloc_output_buffer(video->encoder_handle, &video->jpeg_out_buf, &video->jpeg_out_size) != ESP_OK) {
        ESP_LOGE(TAG, "example_encoder_alloc_output_buffer failed");
        example_encoder_deinit(video->encoder_handle);
        for (int i = 0; i < BUFFER_COUNT; i++) {
            munmap(video->buffer[i], video->buffer_size);
        }
        close(fd);
        return ESP_FAIL;
    }

    video->sem = xSemaphoreCreateBinary();
    if (!video->sem) {
        example_encoder_free_output_buffer(video->encoder_handle, video->jpeg_out_buf);
        example_encoder_deinit(video->encoder_handle);
        for (int i = 0; i < BUFFER_COUNT; i++) {
            munmap(video->buffer[i], video->buffer_size);
        }
        close(fd);
        return ESP_FAIL;
    }
    xSemaphoreGive(video->sem);

    ESP_LOGI(TAG, "Camera ready: %lux%lu format=%s", (unsigned long)video->width, (unsigned long)video->height,
             video->pixel_format == V4L2_PIX_FMT_JPEG ? "JPEG" : "YUV420");
    return ESP_OK;
}

static esp_err_t start_streaming(web_cam_video_t *video)
{
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(video->fd, VIDIOC_STREAMON, &type) != 0) {
        ESP_LOGE(TAG, "VIDIOC_STREAMON failed");
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t stream_loop(web_cam_video_t *video, httpd_req_t *req)
{
    struct v4l2_buffer buf;
    char http_string[128];
    bool locked = false;

    while (1) {
        uint32_t jpeg_encoded_size;
        struct timespec ts;

        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        if (ioctl(video->fd, VIDIOC_DQBUF, &buf) != 0) {
            ESP_LOGE(TAG, "VIDIOC_DQBUF failed");
            break;
        }
        if (!(buf.flags & V4L2_BUF_FLAG_DONE)) {
            ioctl(video->fd, VIDIOC_QBUF, &buf);
            continue;
        }

        if (httpd_resp_send_chunk(req, STREAM_BOUNDARY, strlen(STREAM_BOUNDARY)) != ESP_OK) {
            ioctl(video->fd, VIDIOC_QBUF, &buf);
            break;
        }

        if (video->pixel_format == V4L2_PIX_FMT_JPEG) {
            video->jpeg_out_buf = video->buffer[buf.index];
            jpeg_encoded_size = buf.bytesused;
        } else {
            locked = (xSemaphoreTake(video->sem, portMAX_DELAY) == pdPASS);
            if (example_encoder_process(video->encoder_handle, video->buffer[buf.index],
                                       video->buffer_size, video->jpeg_out_buf,
                                       video->jpeg_out_size, &jpeg_encoded_size) != ESP_OK) {
                if (locked) xSemaphoreGive(video->sem);
                ioctl(video->fd, VIDIOC_QBUF, &buf);
                break;
            }
        }

        clock_gettime(CLOCK_MONOTONIC, &ts);
        int hlen = snprintf(http_string, sizeof(http_string), STREAM_PART,
                            jpeg_encoded_size, (int)ts.tv_sec, (int)(ts.tv_nsec / 1000));
        if (hlen <= 0 || httpd_resp_send_chunk(req, http_string, hlen) != ESP_OK) {
            if (locked) xSemaphoreGive(video->sem);
            ioctl(video->fd, VIDIOC_QBUF, &buf);
            break;
        }

        if (httpd_resp_send_chunk(req, (char *)video->jpeg_out_buf, jpeg_encoded_size) != ESP_OK) {
            if (locked) xSemaphoreGive(video->sem);
            ioctl(video->fd, VIDIOC_QBUF, &buf);
            break;
        }
        if (locked) {
            xSemaphoreGive(video->sem);
            locked = false;
        }

        if (ioctl(video->fd, VIDIOC_QBUF, &buf) != 0) {
            ESP_LOGE(TAG, "VIDIOC_QBUF failed");
            break;
        }
    }
    return ESP_OK;
}

esp_err_t web_cam_direct_init(const char *dev_name, web_cam_direct_t **out_cam)
{
    if (!dev_name || !out_cam) {
        return ESP_ERR_INVALID_ARG;
    }
    web_cam_direct_t *cam = calloc(1, sizeof(web_cam_direct_t));
    if (!cam) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t ret = init_web_cam_video(&cam->video, dev_name);
    if (ret != ESP_OK) {
        free(cam);
        return ret;
    }
    ret = start_streaming(&cam->video);
    if (ret != ESP_OK) {
        free(cam);
        return ret;
    }
    *out_cam = cam;
    return ESP_OK;
}

esp_err_t web_cam_direct_capture_to_http(web_cam_direct_t *cam, httpd_req_t *req, bool as_jpeg)
{
    if (!cam || !req) {
        return ESP_ERR_INVALID_ARG;
    }
    return capture_frame_to_http(req, &cam->video, as_jpeg);
}

esp_err_t web_cam_direct_stream(web_cam_direct_t *cam, httpd_req_t *req)
{
    if (!cam || !req) {
        return ESP_ERR_INVALID_ARG;
    }
    return stream_loop(&cam->video, req);
}

void web_cam_direct_deinit(web_cam_direct_t *cam)
{
    if (!cam) {
        return;
    }
    web_cam_video_t *v = &cam->video;
    if (v->fd >= 0) {
        int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        ioctl(v->fd, VIDIOC_STREAMOFF, &type);
        if (v->encoder_handle) {
            if (v->sem) {
                xSemaphoreTake(v->sem, 0);
            }
            example_encoder_free_output_buffer(v->encoder_handle, v->jpeg_out_buf);
            example_encoder_deinit(v->encoder_handle);
        }
        for (int i = 0; i < BUFFER_COUNT; i++) {
            if (v->buffer[i]) {
                munmap(v->buffer[i], v->buffer_size);
                v->buffer[i] = NULL;
            }
        }
        if (v->sem) {
            vSemaphoreDelete(v->sem);
        }
        close(v->fd);
        v->fd = -1;
    }
    free(cam);
}
