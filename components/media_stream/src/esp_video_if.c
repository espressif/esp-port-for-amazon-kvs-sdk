/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: ESPRESSIF MIT
 */

#include "sdkconfig.h"

/* esp_video (V4L2-style capture) here is wired specifically for
 * P4 MIPI-CSI + the bundled board configs in esp_video_if_cam_sel.h.
 * On other targets either the headers (<sys/mman.h>) don't exist
 * (plain ESP32, no xtensa-newlib mman) or the CSI types do not
 * (S3 uses DVP, not CSI). Compile to empty on non-P4 — callers
 * already gate their use of esp_video_if_* behind
 * CONFIG_IDF_TARGET_ESP32P4 in MJPEGFrameGrabber / video_capture_adapter. */
#include "media_stream_caps.h"
#if MEDIA_STREAM_HAS_ESP_VIDEO_CAPTURE

#if !CONFIG_BSP_SELECT_NONE
#include "bsp/esp-bsp.h"
#endif
#include <assert.h>

#include <inttypes.h>
#include <string.h>
#include <stdbool.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/param.h>
#include <sys/errno.h>
#include <sys/time.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <sys/select.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "driver/i2c_master.h"
#include "linux/videodev2.h"
#include "esp_video_if_cam_sel.h"

#include "esp_video_init.h"
#include "esp_video_device.h"
#include "esp_video_ioctl.h"
#include "esp_cam_sensor.h"
#include "esp_cam_sensor_xclk.h"
#include "esp_video_if.h"
#include "esp_h264_hw_enc.h"

/* Forward declaration of internal I2C init function */
extern esp_err_t media_stream_i2c_init_safe(void);

/* Kconfig-driven; the macro and its fallback live in esp_video_if.h because
 * video_raw_bus.c sizes its slot table off the same number. */
#define BUFFER_COUNT        MEDIA_STREAM_CAM_BUFFER_COUNT
/* UVC needs MMAP: esp_video hands uvc_host the element[] pointers as
 * advanced.user_frame_buffers at stream-open time, and in USERPTR mode those are still
 * NULL then, so the camera has nowhere to write and streams nothing. CSI/DVP keep
 * USERPTR, which lets this file own the buffer placement. */
#if MEDIA_STREAM_ENABLE_USB_UVC_CAM_SENSOR
#define USE_V4L2_USERPTR    0
#else
#define USE_V4L2_USERPTR    1
#endif
#define USERPTR_ALIGNMENT   64
#define USERPTR_HEAP_CAPS   (MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA)

/* Default capture size. Passthrough sends the camera's own H.264, whose bitrate the
 * camera fixes, so the practical ceiling is the uplink rather than USB bandwidth. */
#if MEDIA_STREAM_ENABLE_USB_UVC_CAM_SENSOR && CONFIG_MEDIA_STREAM_UVC_PASSTHROUGH_H264
#define MEDIA_STREAM_CAPTURE_WIDTH   CONFIG_MEDIA_STREAM_UVC_CAPTURE_WIDTH
#define MEDIA_STREAM_CAPTURE_HEIGHT  CONFIG_MEDIA_STREAM_UVC_CAPTURE_HEIGHT
#else
#define MEDIA_STREAM_CAPTURE_WIDTH   WIDTH
#define MEDIA_STREAM_CAPTURE_HEIGHT  HEIGHT
#endif

typedef struct v4l2 {
    int cap_fd;
    uint8_t             *cap_buffer[BUFFER_COUNT];
    size_t              buffer_size[BUFFER_COUNT];  /* Store sizes separately for USERPTR fd reopen */
    bool                fb_used[BUFFER_COUNT];
    struct v4l2_buffer  v4l2_buf[BUFFER_COUNT];
    bool                buffers_allocated;
    video_fb_t          fb[BUFFER_COUNT];
} v4l2_src_t;

static v4l2_src_t *g_v4l2 = NULL;
static uint32_t g_current_pixelformat = 0;
static video_resolution_t g_current_resolution = {.width = 0, .height = 0, .fps = 0};
static bool s_sensor_is_ov2710 = false;
/* esp_video_init() registers the ISP video device as a side-effect that
 * persists for the life of the process (there is no esp_video_deinit()). Track
 * that we've called it so we never call it twice — a second call faults with
 * "video name=ISP id=N has been registered". Set as soon as we attempt init,
 * because the ISP is registered even if sensor detection later fails. */
static bool s_esp_video_registered = false;

/* Global variable to pre-configure resolution before init (set by video_capture_adapter) */
video_resolution_t g_desired_resolution = {.width = 0, .height = 0, .fps = 0};

static const char *TAG = "esp_video_if";


static void print_video_device_info(const struct v4l2_capability *capability)
{
    ESP_LOGI(TAG, "version: %d.%d.%d", (uint16_t)(capability->version >> 16),
             (uint8_t)(capability->version >> 8),
             (uint8_t)capability->version);
    ESP_LOGI(TAG, "driver:  %s", capability->driver);
    ESP_LOGI(TAG, "card:    %s", capability->card);
    ESP_LOGI(TAG, "bus:     %s", capability->bus_info);
    ESP_LOGI(TAG, "capabilities:");
    if (capability->capabilities & V4L2_CAP_VIDEO_CAPTURE) {
        ESP_LOGI(TAG, "\tVIDEO_CAPTURE");
    }
    if (capability->capabilities & V4L2_CAP_READWRITE) {
        ESP_LOGI(TAG, "\tREADWRITE");
    }
    if (capability->capabilities & V4L2_CAP_ASYNCIO) {
        ESP_LOGI(TAG, "\tASYNCIO");
    }
    if (capability->capabilities & V4L2_CAP_STREAMING) {
        ESP_LOGI(TAG, "\tSTREAMING");
    }
    if (capability->capabilities & V4L2_CAP_META_OUTPUT) {
        ESP_LOGI(TAG, "\tMETA_OUTPUT");
    }
    if (capability->capabilities & V4L2_CAP_DEVICE_CAPS) {
        ESP_LOGI(TAG, "device capabilities:");
        if (capability->device_caps & V4L2_CAP_VIDEO_CAPTURE) {
            ESP_LOGI(TAG, "\tVIDEO_CAPTURE");
        }
        if (capability->device_caps & V4L2_CAP_READWRITE) {
            ESP_LOGI(TAG, "\tREADWRITE");
        }
        if (capability->device_caps & V4L2_CAP_ASYNCIO) {
            ESP_LOGI(TAG, "\tASYNCIO");
        }
        if (capability->device_caps & V4L2_CAP_STREAMING) {
            ESP_LOGI(TAG, "\tSTREAMING");
        }
        if (capability->device_caps & V4L2_CAP_META_OUTPUT) {
            ESP_LOGI(TAG, "\tMETA_OUTPUT");
        }
    }
}

/* Number of VIDIOC_ENUM_FMT indices to probe.
 *
 * Deliberately a fixed sweep rather than the conventional
 * enumerate-until-EINVAL loop: the USB-UVC device assigns V4L2 format indices
 * from a fixed table (MJPEG=0, YUY2=1, H264=2, H265=3) and advances the index
 * even for formats the camera does not advertise, so the index space is gapped.
 * A camera offering only H.264 returns EINVAL for index 0 and the usual loop
 * would report "no formats" on a perfectly working device. The CSI/DVP devices
 * enumerate densely, so sweeping a few extra indices costs nothing there. */
#define ENUM_FMT_PROBE_COUNT    8

/* Log what the device actually offers. Diagnostic only — a mismatch between
 * this and the format we request is the difference between "S_FMT failed" and
 * "streaming starts but no frame ever arrives". */
static void log_supported_formats(int fd)
{
    for (uint32_t i = 0; i < ENUM_FMT_PROBE_COUNT; i++) {
        struct v4l2_fmtdesc fmtdesc = {
            .index = i,
            .type  = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        };

        if (ioctl(fd, VIDIOC_ENUM_FMT, &fmtdesc) != 0) {
            continue;
        }

        ESP_LOGI(TAG, "fmt[%" PRIu32 "]: %.4s (%.32s)", i,
                 (const char *)&fmtdesc.pixelformat, (const char *)fmtdesc.description);

        for (uint32_t j = 0; ; j++) {
            struct v4l2_frmsizeenum frmsize = {
                .index         = j,
                .pixel_format  = fmtdesc.pixelformat,
                /* esp_video reads `type` as an *input* v4l2_buf_type to look up
                 * the stream (esp_video.c: esp_video_enum_framesizes), whereas
                 * mainline V4L2 treats it as output-only. Leaving it zero makes
                 * every call fail with EINVAL and no size is ever printed. The
                 * driver overwrites it with V4L2_FRMSIZE_TYPE_DISCRETE on
                 * success, so the branch below still reads correctly. */
                .type          = V4L2_BUF_TYPE_VIDEO_CAPTURE,
            };

            if (ioctl(fd, VIDIOC_ENUM_FRAMESIZES, &frmsize) != 0) {
                break;
            }

            if (frmsize.type == V4L2_FRMSIZE_TYPE_DISCRETE) {
                ESP_LOGI(TAG, "        %" PRIu32 "x%" PRIu32,
                         frmsize.discrete.width, frmsize.discrete.height);
            } else {
                ESP_LOGI(TAG, "        %" PRIu32 "x%" PRIu32 " .. %" PRIu32 "x%" PRIu32,
                         frmsize.stepwise.min_width, frmsize.stepwise.min_height,
                         frmsize.stepwise.max_width, frmsize.stepwise.max_height);
            }
        }
    }
}

static esp_err_t init_camera(v4l2_src_t *v4l2)
{
    int fd;
    struct v4l2_capability capability;

    fd = open(MEDIA_STREAM_CAM_DEV_PATH, O_RDWR);
    if (fd < 0) {
        ESP_LOGE(TAG, "Failed to open camera device %s, errno: %d", MEDIA_STREAM_CAM_DEV_PATH, errno);
        return ESP_FAIL;
    }

    if (ioctl(fd, VIDIOC_QUERYCAP, &capability) < 0) {
        ESP_LOGE(TAG, "Failed to query capabilities, errno: %d", errno);
        close(fd);  /* Close file descriptor on error */
        return ESP_FAIL;
    }
    print_video_device_info(&capability);

    /* Bound VIDIOC_DQBUF. esp_video opens every device with
     * dqbuf_timeout_ticks = portMAX_DELAY and its VFS layer registers no
     * start_select and ignores O_NONBLOCK, so select()/poll() cannot be used
     * to wait with a deadline — a camera that stops completing frames blocks
     * the caller forever. With a timeout DQBUF instead fails with EPERM,
     * esp_video_if_get_frame() returns NULL, and the encoder task warns and
     * retries. The ioctl is esp_video-private; treat its absence as benign.
     *
     * Every branch logs, including "disabled": whether a deadline is in force
     * must be readable from the boot log, not inferred from a missing line. */
    if (CONFIG_ESP_VIDEO_IF_DQBUF_TIMEOUT_MS > 0) {
        struct timeval dqbuf_timeout = {
            .tv_sec  = CONFIG_ESP_VIDEO_IF_DQBUF_TIMEOUT_MS / 1000,
            .tv_usec = (CONFIG_ESP_VIDEO_IF_DQBUF_TIMEOUT_MS % 1000) * 1000,
        };
        if (ioctl(fd, VIDIOC_S_DQBUF_TIMEOUT, &dqbuf_timeout) != 0) {
            ESP_LOGW(TAG, "VIDIOC_S_DQBUF_TIMEOUT unsupported (errno %d); DQBUF will block indefinitely", errno);
        } else {
            ESP_LOGI(TAG, "DQBUF timeout set to %d ms", CONFIG_ESP_VIDEO_IF_DQBUF_TIMEOUT_MS);
        }
    } else {
        ESP_LOGW(TAG, "DQBUF timeout disabled by configuration; DQBUF will block indefinitely");
    }

    log_supported_formats(fd);

    /* Query sensor chip ID via esp_cam_sensor ioctl to detect OV2710 (PID 0x2710) */
    esp_cam_sensor_id_t chip_id = {0};
    struct v4l2_ext_controls ext_ctrls = {0};
    struct v4l2_ext_control ext_ctrl = {0};
    ext_ctrls.ctrl_class = V4L2_CTRL_CLASS_ESP_CAM_IOCTL;
    ext_ctrls.count = 1;
    ext_ctrls.controls = &ext_ctrl;
    ext_ctrl.id = ESP_CAM_SENSOR_IOC_G_CHIP_ID;
    ext_ctrl.p_u8 = (uint8_t *)&chip_id;
    ext_ctrl.size = sizeof(chip_id);
    if (ioctl(fd, VIDIOC_G_EXT_CTRLS, &ext_ctrls) == 0) {
        ESP_LOGI(TAG, "Sensor PID=0x%04x", chip_id.pid);
        s_sensor_is_ov2710 = (chip_id.pid == 0x2710);
        if (s_sensor_is_ov2710) {
            ESP_LOGW(TAG, "OV2710 sensor detected, flip controls will be skipped");
        }
    }

    struct v4l2_format format;

    memset(&format, 0, sizeof(struct v4l2_format));
    format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(fd, VIDIOC_G_FMT, &format) != 0) {
        ESP_LOGE(TAG, "failed to get format");
        close(fd);  /* Close file descriptor on error */
        return ESP_FAIL;
    }

    /* The device's format before we touch it. Worth the fourcc: on the USB-UVC
     * device this is copied verbatim from the camera's frame_info[0]
     * (esp_video_usb_uvc_device.c, uvc_video_init), and frame_info[0] is also
     * the format uvc_host_stream_open() negotiates at STREAMON — which is what
     * fixes the alternate setting and the ISOC URB geometry for the whole
     * session. A later S_FMT to a different format does not revisit either, so
     * when this line disagrees with the format we go on to request, that
     * mismatch is a prime suspect for a stream that starts but never delivers. */
    ESP_LOGI(TAG, "Default: width=%" PRIu32 " height=%" PRIu32 " %.4s",
             format.fmt.pix.width, format.fmt.pix.height,
             (const char *)&format.fmt.pix.pixelformat);

    v4l2->cap_fd = fd;

    ESP_LOGI(TAG, "Camera capture initialized and streaming started");
    return ESP_OK;
}

static void video_stop_cb(void *cb_ctx)
{
    int type;
    v4l2_src_t *v4l2 = (v4l2_src_t *)cb_ctx;

    type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ioctl(v4l2->cap_fd, VIDIOC_STREAMOFF, &type);
}

static void requeue_used_buffers(v4l2_src_t *v4l2)
{
    if (!v4l2) {
        return;
    }

    for (int i = 0; i < BUFFER_COUNT; i++) {
        if (v4l2->fb_used[i]) {
            v4l2->fb_used[i] = false;
#if USE_V4L2_USERPTR
            /* For USERPTR, always set the pointer and length before requeueing */
            v4l2->v4l2_buf[i].m.userptr = (unsigned long)v4l2->cap_buffer[i];
            /* length should already be set from QUERYBUF */
#endif
            ioctl(v4l2->cap_fd, VIDIOC_QBUF, &v4l2->v4l2_buf[i]);
        }
    }
}

/* Capture-buffer size for a compressed format.
 *
 * esp_video_config_buffer() honours sizeimage for JPEG/H264 and otherwise falls back to
 * width * height * bpp / 8 with bpp = 8, i.e. one byte per pixel - far too small.
 */
#ifndef CONFIG_MEDIA_STREAM_H264_COMPRESSED_FRAME_MAX_KB
#define CONFIG_MEDIA_STREAM_H264_COMPRESSED_FRAME_MAX_KB 1024
#endif

static uint32_t compressed_buffer_size(uint32_t width, uint32_t height)
{
    /* uvc_host.c uses advanced.frame_size directly as the
     * allocation size (dwMaxVideoFrameSize is only the fallback when it is 0) and never
     * compares the two - the only check is frame_size > 0. The real requirement is that
     * the buffer hold the largest *actual* frame, which for H.264 is a keyframe.
     */
    uint64_t declared = (uint64_t) width * height * 2;
    uint64_t cap = (uint64_t) CONFIG_MEDIA_STREAM_H264_COMPRESSED_FRAME_MAX_KB * 1024;

    if (cap == 0 || declared <= cap) {
        return (uint32_t) declared;
    }
    return (uint32_t) cap;
}

static esp_err_t queue_all_buffers(v4l2_src_t *v4l2)
{
    if (!v4l2) {
        return ESP_ERR_INVALID_ARG;
    }

    for (int i = 0; i < BUFFER_COUNT; i++) {
        /* Rebuild the descriptor from scratch. Reusing the struct left over
         * from the previous session's last DQBUF would carry stale fields
         * (flags such as DONE/ERROR, bytesused, sequence, timestamp) into the
         * requeue. After a fresh REQBUFS on the reopened fd the driver expects
         * clean descriptors; stale flags make QBUF/DQBUF go out of sync so
         * DQBUF never returns a frame (black video after a few sessions).
         * We keep the backing memory (cap_buffer[]/buffer_size[]) to avoid
         * PSRAM fragmentation — only the bookkeeping is reset. */
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = USE_V4L2_USERPTR ? V4L2_MEMORY_USERPTR : V4L2_MEMORY_MMAP;
        buf.index  = i;

        v4l2->fb_used[i] = false;

        if (USE_V4L2_USERPTR) {
            if (v4l2->buffer_size[i] == 0) {
                ESP_LOGE(TAG, "USERPTR buffer size not set for buffer %d", i);
                return ESP_FAIL;
            }
            buf.m.userptr  = (unsigned long)v4l2->cap_buffer[i];
            buf.length     = v4l2->buffer_size[i];  /* Restore from saved size */
            buf.bytesused  = 0;
        }

        v4l2->v4l2_buf[i] = buf;

        if (ioctl(v4l2->cap_fd, VIDIOC_QBUF, &v4l2->v4l2_buf[i]) < 0) {
            ESP_LOGE(TAG, "Failed to requeue buffer %d, errno: %d", i, errno);
            /* Buffers 0..i-1 are already queued to the driver. Flush them with
             * STREAMOFF (legal before STREAMON; returns every queued buffer to
             * the dequeued state) so the driver is not left half-queued and a
             * later start attempt begins from a clean queue. */
            int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            if (i > 0 && ioctl(v4l2->cap_fd, VIDIOC_STREAMOFF, &type) < 0) {
                ESP_LOGE(TAG, "STREAMOFF after partial queue failed, errno: %d (full re-init required)", errno);
            }
            return ESP_FAIL;
        }
    }

    return ESP_OK;
}

static video_fb_t *video_fb_get_cb(void *cb_ctx)
{
    int64_t us;
    v4l2_src_t *v4l2 = (v4l2_src_t *)cb_ctx;

    for (int i = 0; i < BUFFER_COUNT; i++) {
        if (v4l2->fb_used[i] == false) {
            struct v4l2_buffer buf = {
                .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
                .memory = USE_V4L2_USERPTR ? V4L2_MEMORY_USERPTR : V4L2_MEMORY_MMAP,
            };
            int ret = ioctl(v4l2->cap_fd, VIDIOC_DQBUF, &buf);
            if (ret != 0) {
                /* Rate-limited: a camera that has stopped delivering fails here
                 * on every poll, and one line per DQBUF drowns the log.
                 * esp_video maps its DQBUF timeout onto EPERM — call that out,
                 * since "no frame within the deadline" and "the ioctl was
                 * rejected" want very different debugging. */
                static uint32_t dqbuf_fail_count;
                dqbuf_fail_count++;
                if (dqbuf_fail_count == 1 || (dqbuf_fail_count & 0x1F) == 0) {
                    ESP_LOGE(TAG, "failed to receive video frame: %s (errno %d, count=%" PRIu32 ")",
                             errno == EPERM ? "timed out waiting for the driver" : "ioctl error",
                             errno, dqbuf_fail_count);
                    if (dqbuf_fail_count == 1) {
                        ESP_LOGW(TAG, "if this persists with no frames at all, the capture buffer may be "
                                      "too small for a keyframe - raise "
                                      "CONFIG_MEDIA_STREAM_H264_COMPRESSED_FRAME_MAX_KB (currently %d KB) "
                                      "or enable DEBUG on tag \"usb_uvc_device\" to see "
                                      "\"Frame buffer overflow\"",
                                 CONFIG_MEDIA_STREAM_H264_COMPRESSED_FRAME_MAX_KB);
                    }
                }
                return NULL;
            }

            /* Guard the driver-supplied index before it touches fb_used[] /
             * cap_buffer[] / v4l2_buf[] (all sized BUFFER_COUNT): a buggy or
             * racing driver returning an unexpected index must not become an
             * out-of-bounds write. */
            if (buf.index >= BUFFER_COUNT) {
                ESP_LOGE(TAG, "DQBUF returned out-of-range buffer index %u", (unsigned) buf.index);
                return NULL;
            }

            video_fb_t *fb = &v4l2->fb[buf.index];

            v4l2->fb_used[buf.index] = true;
            fb->buf = v4l2->cap_buffer[buf.index];
            fb->len = buf.bytesused;
            fb->width = g_current_resolution.width;
            fb->height = g_current_resolution.height;
            fb->index = buf.index;
            v4l2->v4l2_buf[buf.index] = buf;

            /* Compressed frames have arbitrary lengths and esp_cache_msync() requires a
             * cache-line-aligned size, but the rounded-up length must not run past the
             * buffer or the sync covers memory that is not ours. */
            size_t sync_len = ((size_t)fb->len + 63U) & ~63U;
            if (sync_len > v4l2->buffer_size[buf.index]) {
                sync_len = v4l2->buffer_size[buf.index] & ~63U;
            }
#if MEDIA_STREAM_ENABLE_USB_UVC_CAM_SENSOR
            /* No invalidate on the USB-UVC path - it would destroy the frame. */
            (void) sync_len;
#else
            /* This branch is only compiled for CSI/DVP. */
            esp_cache_msync(fb->buf, sync_len, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
#endif

            us = esp_timer_get_time();
            fb->timestamp.tv_sec = us / 1000000UL;
            fb->timestamp.tv_usec = us % 1000000UL;

            // uint64_t end_time = us;
            // printf("Frame Grab FPS: %d\n", (int) (1000000 / (end_time - start_time)));

            return fb;
        }
    }
    return NULL;
}

static void video_fb_return_cb(video_fb_t *fb, void *cb_ctx)
{
    v4l2_src_t *v4l2 = (v4l2_src_t *)cb_ctx;

    ESP_LOGD(TAG, "Returning encoder buffer");

    /* Keyed on the index the descriptor carries rather than scanned by buffer pointer.
     * With several frames in flight a scan is both O(n) on a hot path and ambiguous the
     * moment two descriptors could name the same buffer. */
    const unsigned i = fb->index;
    if (i >= BUFFER_COUNT || v4l2->cap_buffer[i] != fb->buf) {
        ESP_LOGE(TAG, "release of a foreign frame (index %u, buf %p)", i, (void *)fb->buf);
        return;
    }
    if (!v4l2->fb_used[i]) {
        /* Double release. The caller's refcounting should have caught it; requeueing a
         * second time would hand the driver a buffer another consumer may still be
         * reading, so refuse and make the bug audible. */
        ESP_LOGE(TAG, "double release of capture buffer %u", i);
        return;
    }

    v4l2->fb_used[i] = false;
#if USE_V4L2_USERPTR
    /* For USERPTR, always set the pointer and length before requeueing (per ESP-BSP example) */
    v4l2->v4l2_buf[i].m.userptr = (unsigned long)v4l2->cap_buffer[i];
    /* length should already be set from QUERYBUF, but as fallback use fb length */
    if (v4l2->v4l2_buf[i].length == 0) {
        v4l2->v4l2_buf[i].length = fb->len;
    }
#endif
    ioctl(v4l2->cap_fd, VIDIOC_QBUF, &v4l2->v4l2_buf[i]);
}

static void free_mapped_buffers(v4l2_src_t *v4l2)
{
    if (!v4l2 || !v4l2->buffers_allocated) {
        return;
    }

    for (int i = 0; i < BUFFER_COUNT; i++) {
        if (v4l2->cap_buffer[i]) {
            if (USE_V4L2_USERPTR) {
                heap_caps_free(v4l2->cap_buffer[i]);
            } else {
                munmap(v4l2->cap_buffer[i], v4l2->buffer_size[i]);
            }
            v4l2->cap_buffer[i] = NULL;
        }
        v4l2->buffer_size[i] = 0;
        v4l2->fb_used[i] = false;
    }

    v4l2->buffers_allocated = false;
}

video_fb_t *esp_video_if_get_frame(void)
{
    video_fb_t *fb = NULL;
    if (g_v4l2) {
        fb = video_fb_get_cb(g_v4l2);
        if (!fb) {
            ESP_LOGE(TAG, "Failed to get frame");
            return NULL;
        }
        return fb;  // Return the raw frame without encoding
    }
    ESP_LOGE(TAG, "Camera not initialized");
    return NULL;
}

void esp_video_if_release_frame(video_fb_t *fb)
{
    if (g_v4l2 && fb) {
        video_fb_return_cb(fb, g_v4l2);
    }
}

esp_err_t esp_video_if_stop(void)
{
    if (g_v4l2) {
        requeue_used_buffers(g_v4l2);
        video_stop_cb(g_v4l2);
        /* Keep buffers allocated to avoid fragmentation - they'll be reused on next
         * start. UVC cannot reuse them at all, but that release happens in
         * esp_video_if_start(): doing it here races the USB host task, which may still
         * be delivering frames while the stream is being stopped. */
        ESP_LOGD(TAG, "Streaming stopped (buffers kept allocated for reuse)");
        return ESP_OK;
    }
    return ESP_FAIL;
}

esp_err_t esp_video_if_deinit(void)
{
    if (!g_v4l2) {
        ESP_LOGD(TAG, "Video interface not initialized, nothing to deinitialize");
        return ESP_OK;
    }

    ESP_LOGD(TAG, "Deinitializing camera hardware (keeping fd open and buffers for reuse)");

    /* Stop streaming only. We deliberately keep the fd OPEN and the USERPTR
     * buffers allocated across sessions:
     *
     *  - Keeping the buffers avoids re-allocating the large aligned PSRAM
     *    blocks every session (heap fragmentation).
     *  - Keeping the fd open avoids the close()/reopen()+REQBUFS dance, which
     *    tears down and rebuilds the driver's device + sensor state. That
     *    close/reopen cycle is what became unreliable after a few sessions:
     *    STREAMON would succeed but DQBUF never delivered a frame (black video).
     *    A persistent fd with a plain STREAMOFF -> (requeue) -> STREAMON is the
     *    canonical V4L2 stop/start and restarts cleanly every time.
     *
     * esp_video_if_stop() does requeue_used_buffers() + STREAMOFF; STREAMOFF
     * returns every buffer to the dequeued state so the next start can QBUF
     * them all again (see restart_streaming_with_existing_buffers). */
    esp_video_if_stop();

#if USE_V4L2_USERPTR
    /* Allow any concurrent DQBUF ioctl to finish unwinding after STREAMOFF. */
    vTaskDelay(pdMS_TO_TICKS(50));
#endif

    return ESP_OK;
}

/* Helper: Reopen camera for USERPTR after fd was closed */
static esp_err_t reopen_camera_for_userptr(v4l2_src_t *v4l2)
{
#if USE_V4L2_USERPTR
    if (v4l2->cap_fd >= 0) {
        return ESP_OK;  /* Already open */
    }

    ESP_LOGD(TAG, "USERPTR buffers allocated but fd closed, reopening camera");

    /* Reopen camera device */
    if (init_camera(v4l2) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to reopen camera for USERPTR");
        return ESP_FAIL;
    }

    /* Set format again */
    struct v4l2_format format = {0};
    format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    format.fmt.pix.width = g_current_resolution.width;
    format.fmt.pix.height = g_current_resolution.height;
    format.fmt.pix.pixelformat = g_current_pixelformat;
    /* Same reasoning as the initial S_FMT above: without sizeimage the capture buffer is
     * sized at one byte per pixel and every camera frame overflows it. */
    if (g_current_pixelformat == V4L2_PIX_FMT_H264 || g_current_pixelformat == V4L2_PIX_FMT_JPEG) {
        format.fmt.pix.sizeimage = compressed_buffer_size(format.fmt.pix.width, format.fmt.pix.height);
    }
    if (ioctl(v4l2->cap_fd, VIDIOC_S_FMT, &format) != 0) {
        ESP_LOGE(TAG, "Failed to set format on reopen");
        return ESP_FAIL;
    }

    /* Re-request USERPTR buffers */
    struct v4l2_requestbuffers req = {0};
    req.count = BUFFER_COUNT;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_USERPTR;
    if (ioctl(v4l2->cap_fd, VIDIOC_REQBUFS, &req) < 0) {
        ESP_LOGE(TAG, "Failed to re-request USERPTR buffers, errno: %d", errno);
        return ESP_FAIL;
    }

    ESP_LOGD(TAG, "Camera reopened, reusing %d USERPTR buffers", BUFFER_COUNT);
#endif
    return ESP_OK;
}

/* Helper: Apply flip controls and start streaming */
static esp_err_t start_streaming(v4l2_src_t *v4l2)
{
#if CONFIG_ESP_VIDEO_IF_HOR_FLIP || CONFIG_ESP_VIDEO_IF_VER_FLIP
    if (s_sensor_is_ov2710) {
        ESP_LOGW(TAG, "OV2710 sensor detected, skipping flip controls (not supported)");
    } else {
        struct v4l2_ext_controls ext_ctrls = {0};
        struct v4l2_ext_control ctrls[2] = {0};
        int ctrl_count = 0;

#if CONFIG_ESP_VIDEO_IF_HOR_FLIP
        ctrls[ctrl_count].id = V4L2_CID_HFLIP;
        ctrls[ctrl_count].value = 1;
        ctrl_count++;
#endif

#if CONFIG_ESP_VIDEO_IF_VER_FLIP
        ctrls[ctrl_count].id = V4L2_CID_VFLIP;
        ctrls[ctrl_count].value = 1;
        ctrl_count++;
#endif

        ext_ctrls.controls = ctrls;
        ext_ctrls.count = ctrl_count;

        if (ioctl(v4l2->cap_fd, VIDIOC_S_EXT_CTRLS, &ext_ctrls) < 0) {
            ESP_LOGW(TAG, "Failed to set flip controls, errno: %d", errno);
        }
    }
#endif

    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(v4l2->cap_fd, VIDIOC_STREAMON, &type) < 0) {
        ESP_LOGE(TAG, "Failed to stream on, errno: %d", errno);
        /* Cleanup all buffers on stream start failure */
        for (int i = 0; i < BUFFER_COUNT; i++) {
            if (v4l2->cap_buffer[i]) {
                if (USE_V4L2_USERPTR) {
                    heap_caps_free(v4l2->cap_buffer[i]);
                } else {
                    munmap(v4l2->cap_buffer[i], v4l2->buffer_size[i]);
                }
                v4l2->cap_buffer[i] = NULL;
            }
        }
        v4l2->buffers_allocated = false;
        return ESP_FAIL;
    }

    return ESP_OK;
}

/* Helper: Allocate and queue buffers for first-time setup */
static esp_err_t allocate_and_queue_buffers(v4l2_src_t *v4l2)
{
    struct v4l2_buffer buf;
    struct v4l2_requestbuffers req = {0};

    req.count = BUFFER_COUNT;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = USE_V4L2_USERPTR ? V4L2_MEMORY_USERPTR : V4L2_MEMORY_MMAP;

    if (ioctl(v4l2->cap_fd, VIDIOC_REQBUFS, &req) < 0) {
        ESP_LOGE(TAG, "Failed to require buffers, errno: %d", errno);
        return ESP_FAIL;
    }

    ESP_LOGD(TAG, "Allocating %d %s buffers (first time or after cleanup)",
             BUFFER_COUNT, USE_V4L2_USERPTR ? "USERPTR" : "MMAP");

    for (int i = 0; i < BUFFER_COUNT; i++) {
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = USE_V4L2_USERPTR ? V4L2_MEMORY_USERPTR : V4L2_MEMORY_MMAP;
        buf.index = i;

        /* Query buffer to get the required length from driver */
        if (ioctl(v4l2->cap_fd, VIDIOC_QUERYBUF, &buf) < 0) {
            ESP_LOGE(TAG, "Failed to query buffer, errno: %d", errno);
#if USE_V4L2_USERPTR
            for (int j = 0; j < i; j++) {
                if (v4l2->cap_buffer[j]) {
                    heap_caps_free(v4l2->cap_buffer[j]);
                    v4l2->cap_buffer[j] = NULL;
                }
            }
#else
            for (int j = 0; j < i; j++) {
                if (v4l2->cap_buffer[j]) {
                    munmap(v4l2->cap_buffer[j], v4l2->buffer_size[j]);
                    v4l2->cap_buffer[j] = NULL;
                }
            }
#endif
            return ESP_FAIL;
        }

        /* Store buffer info */
        v4l2->v4l2_buf[i] = buf;
        v4l2->buffer_size[i] = buf.length;

        if (i == 0) {
            /* The driver's required buffer size, once. Worth seeing: it is what
             * the capture device will refuse to overflow, so a compressed frame
             * larger than this is dropped rather than truncated. */
            ESP_LOGI(TAG, "Driver requires %u bytes per capture buffer (%d buffers)",
                     (unsigned)buf.length, BUFFER_COUNT);
        }

#if USE_V4L2_USERPTR
        /* Allocate user buffer */
        v4l2->cap_buffer[i] = heap_caps_aligned_alloc(USERPTR_ALIGNMENT, buf.length, USERPTR_HEAP_CAPS);
        if (!v4l2->cap_buffer[i]) {
            ESP_LOGE(TAG, "Failed to allocate USERPTR buffer %d size=%u (caps=0x%x)",
                     i, (unsigned)buf.length, USERPTR_HEAP_CAPS);
            for (int j = 0; j < i; j++) {
                if (v4l2->cap_buffer[j]) {
                    heap_caps_free(v4l2->cap_buffer[j]);
                    v4l2->cap_buffer[j] = NULL;
                }
            }
            return ESP_FAIL;
        }
        ESP_LOGD(TAG, "Allocated USERPTR buffer %d addr=%p size=%zu caps=0x%x",
                 i, v4l2->cap_buffer[i], v4l2->buffer_size[i], USERPTR_HEAP_CAPS);
        buf.m.userptr = (unsigned long)v4l2->cap_buffer[i];
#else
        /* Map buffer */
        v4l2->cap_buffer[i] = (uint8_t *)mmap(NULL, buf.length, PROT_READ | PROT_WRITE,
                                                MAP_SHARED, v4l2->cap_fd, buf.m.offset);
        if (!v4l2->cap_buffer[i]) {
            ESP_LOGE(TAG, "Failed to map buffer, errno: %d", errno);
            for (int j = 0; j < i; j++) {
                if (v4l2->cap_buffer[j]) {
                    munmap(v4l2->cap_buffer[j], v4l2->buffer_size[j]);
                    v4l2->cap_buffer[j] = NULL;
                }
            }
            return ESP_FAIL;
        }
#endif

        /* Queue buffer */
        if (ioctl(v4l2->cap_fd, VIDIOC_QBUF, &buf) < 0) {
            ESP_LOGE(TAG, "Failed to queue buffer %d, errno: %d userptr=%p len=%u bytesused=%u",
                     i, errno, (void *)buf.m.userptr, (unsigned)buf.length, (unsigned)buf.bytesused);
            for (int j = 0; j <= i; j++) {
                if (v4l2->cap_buffer[j]) {
                    if (USE_V4L2_USERPTR) {
                        heap_caps_free(v4l2->cap_buffer[j]);
                    } else {
                        munmap(v4l2->cap_buffer[j], v4l2->buffer_size[j]);
                    }
                    v4l2->cap_buffer[j] = NULL;
                }
            }
            return ESP_FAIL;
        }

        v4l2->v4l2_buf[i] = buf;
    }

    v4l2->buffers_allocated = true;
    return ESP_OK;
}

/* Helper: Configure camera format with fallback resolution support */
static esp_err_t configure_camera_format(v4l2_src_t *v4l2, uint32_t pixelformat)
{
    typedef struct {
        uint32_t width;
        uint32_t height;
    } resolution_t;

    resolution_t fallback_resolutions[] = {
#if MEDIA_STREAM_ENABLE_USB_UVC_CAM_SENSOR && !CONFIG_MEDIA_STREAM_UVC_PASSTHROUGH_H264
        /* Uncompressed capture is bounded by the USB isochronous budget, not by the
         * sensor: the endpoint carries 3060 B/microframe = ~24 MB/s, while YUY2 needs
         * w*h*2 per frame - 18.4 MB/s at 640x480@30 but 124 MB/s at 1080p@30, which the
         * camera simply cannot deliver. Ask for sizes that fit; the encoder scales the
         * bitrate afterwards. (Passthrough has no such limit - see above.) */
        {320, 240},
        {640, 360},
        {640, 480},
#else
        /* What the app/menuconfig asked for comes first; the rest are fallbacks in
         * descending order for cameras that reject it. */
        {g_desired_resolution.width ? g_desired_resolution.width : MEDIA_STREAM_CAPTURE_WIDTH,
         g_desired_resolution.height ? g_desired_resolution.height : MEDIA_STREAM_CAPTURE_HEIGHT},
        {1920, 1080},
        {1280, 720},
        {800, 600},
        {640, 480},
        {320, 240}
#endif
    };
    int num_fallbacks = sizeof(fallback_resolutions) / sizeof(fallback_resolutions[0]);

    for (int i = 0; i < num_fallbacks; i++) {
        /* Skip duplicate attempts */
        if (i > 0 && fallback_resolutions[i].width == fallback_resolutions[i - 1].width &&
            fallback_resolutions[i].height == fallback_resolutions[i - 1].height) {
            continue;
        }

        struct v4l2_format format = {0};
        format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        format.fmt.pix.width = fallback_resolutions[i].width;
        format.fmt.pix.height = fallback_resolutions[i].height;
        format.fmt.pix.pixelformat = pixelformat;

        if (pixelformat == V4L2_PIX_FMT_H264 || pixelformat == V4L2_PIX_FMT_JPEG) {
            format.fmt.pix.sizeimage = compressed_buffer_size(format.fmt.pix.width, format.fmt.pix.height);
        }

        if (i == 0) {
            ESP_LOGD(TAG, "Attempting to set format: %dx%d", (int)format.fmt.pix.width, (int)format.fmt.pix.height);
        } else {
            ESP_LOGW(TAG, "Retrying with fallback resolution: %dx%d", (int)format.fmt.pix.width, (int)format.fmt.pix.height);
        }

        if (ioctl(v4l2->cap_fd, VIDIOC_S_FMT, &format) == 0) {
            /* Report the fourcc and the driver's own sizing, not just what we
             * asked for: S_FMT is free to adjust every field, and the pixel
             * format it settled on decides both what the frame grabber must do
             * with the data and how esp_video sized the capture buffers. */
#if MEDIA_STREAM_ENABLE_USB_UVC_CAM_SENSOR && CONFIG_MEDIA_STREAM_UVC_TARGET_FPS > 0
            /* Ask for a lower frame interval. A UVC camera picks its own H.264 bitrate
             * and we have no encoding-unit control to change it, so frame rate is the
             * only lever we have over how much data lands on the uplink. */
            struct v4l2_streamparm parm = {0};
            parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            parm.parm.capture.timeperframe.numerator = 1;
            parm.parm.capture.timeperframe.denominator = CONFIG_MEDIA_STREAM_UVC_TARGET_FPS;
            if (ioctl(v4l2->cap_fd, VIDIOC_S_PARM, &parm) == 0) {
                ESP_LOGI(TAG, "Requested %d fps, driver granted %" PRIu32 "/%" PRIu32 " s",
                         CONFIG_MEDIA_STREAM_UVC_TARGET_FPS,
                         (uint32_t) parm.parm.capture.timeperframe.numerator,
                         (uint32_t) parm.parm.capture.timeperframe.denominator);
            } else {
                ESP_LOGW(TAG, "VIDIOC_S_PARM for %d fps rejected (errno %d); camera keeps its default",
                         CONFIG_MEDIA_STREAM_UVC_TARGET_FPS, errno);
            }
#endif
            ESP_LOGI(TAG, "Successfully set format: %dx%d %.4s (sizeimage=%" PRIu32 " bytesperline=%" PRIu32 ")",
                     (int)format.fmt.pix.width, (int)format.fmt.pix.height,
                     (const char *)&format.fmt.pix.pixelformat,
                     format.fmt.pix.sizeimage, format.fmt.pix.bytesperline);
            /* Track actual resolution that was set (V4L2 may adjust it) */
            g_current_pixelformat = format.fmt.pix.pixelformat;
            g_current_resolution.width = format.fmt.pix.width;
            g_current_resolution.height = format.fmt.pix.height;
            g_current_resolution.fps = g_desired_resolution.fps ? g_desired_resolution.fps : 30;

            /* Query actual frame rate from driver */
            struct v4l2_streamparm streamparm = {0};
            streamparm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            if (ioctl(v4l2->cap_fd, VIDIOC_G_PARM, &streamparm) == 0) {
                if (streamparm.parm.capture.timeperframe.numerator > 0) {
                    g_current_resolution.fps = streamparm.parm.capture.timeperframe.denominator /
                                               streamparm.parm.capture.timeperframe.numerator;
                    ESP_LOGI(TAG, "Actual FPS from driver: %d", (int)g_current_resolution.fps);
                }
            }

            return ESP_OK;
        }
        ESP_LOGW(TAG, "Failed to set format %dx%d, errno: %d",
                 (int)format.fmt.pix.width, (int)format.fmt.pix.height, errno);
    }

    /* None of the requested/fallback resolutions could be set. Rather than treating
     * this as fatal, fall back to whatever format the sensor is already in: query it
     * with VIDIOC_G_FMT, adopt it as the current resolution, warn, and continue. Some
     * sensors reject S_FMT but still deliver a usable current format. */
    struct v4l2_format current_format = {0};
    current_format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(v4l2->cap_fd, VIDIOC_G_FMT, &current_format) == 0) {
        g_current_pixelformat = current_format.fmt.pix.pixelformat;
        g_current_resolution.width = current_format.fmt.pix.width;
        g_current_resolution.height = current_format.fmt.pix.height;
        g_current_resolution.fps = g_desired_resolution.fps ? g_desired_resolution.fps : 30;
        ESP_LOGW(TAG,
                 "Could not set any requested resolution; using the sensor's current format %dx%d %.4s "
                 "(check the camera resolution in menuconfig if this is unexpected)",
                 (int) g_current_resolution.width, (int) g_current_resolution.height,
                 (const char *)&current_format.fmt.pix.pixelformat);
        return ESP_OK;
    }

    ESP_LOGE(TAG, "Failed to set any supported format and could not query the current one (errno %d)", errno);
    return ESP_FAIL;
}

/* Helper: Restart streaming with already-allocated buffers */
static esp_err_t restart_streaming_with_existing_buffers(v4l2_src_t *v4l2)
{
    ESP_LOGD(TAG, "Buffers already allocated (%d buffers in %s mode), skipping reallocation",
             BUFFER_COUNT, USE_V4L2_USERPTR ? "USERPTR" : "MMAP");

    /* For USERPTR, reopen fd if needed */
    if (reopen_camera_for_userptr(v4l2) != ESP_OK) {
        return ESP_FAIL;
    }

    /* Requeue all buffers */
    if (queue_all_buffers(v4l2) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to requeue buffers for restart");
        return ESP_FAIL;
    }

    /* Start streaming */
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(v4l2->cap_fd, VIDIOC_STREAMON, &type) < 0) {
        ESP_LOGE(TAG, "Failed to stream on, errno: %d", errno);
        return ESP_FAIL;
    }

    ESP_LOGD(TAG, "Video streaming restarted successfully (no reallocation)");
    return ESP_OK;
}

esp_err_t esp_video_if_start(void)
{
    if (!g_v4l2) {
        ESP_LOGE(TAG, "Camera not initialized");
        return ESP_FAIL;
    }

    v4l2_src_t *v4l2 = g_v4l2;

    ESP_LOGD(TAG, "Starting video (buffers_allocated=%d, fd=%d, mode=%s)",
             v4l2->buffers_allocated, v4l2->cap_fd, USE_V4L2_USERPTR ? "USERPTR" : "MMAP");

#if MEDIA_STREAM_ENABLE_USB_UVC_CAM_SENSOR
    /* UVC always re-inits. Stopping the stream makes esp_video close the underlying
     * uvc_host stream and tear its queues down, so buffers carried over from a previous
     * session reference a dead stream and the next QBUF asserts inside
     * xQueueGenericSend(). Released here rather than in stop() so it runs with the USB
     * host task idle. */
    if (v4l2->buffers_allocated) {
        ESP_LOGD(TAG, "UVC: releasing previous session's buffers before restart");
        free_mapped_buffers(v4l2);
        if (v4l2->cap_fd >= 0) {
            close(v4l2->cap_fd);
            v4l2->cap_fd = -1;
        }
        if (init_camera(v4l2) != ESP_OK) {
            ESP_LOGE(TAG, "UVC: failed to reopen the camera device");
            return ESP_FAIL;
        }
    }
#endif

    /* Fast path: buffers already allocated, just restart streaming */
    if (v4l2->buffers_allocated) {
        return restart_streaming_with_existing_buffers(v4l2);
    }

    /* Slow path: First-time setup or after cleanup */
#if MEDIA_STREAM_ENABLE_USB_UVC_CAM_SENSOR
#if CONFIG_MEDIA_STREAM_UVC_PASSTHROUGH_H264
    /* Pass the camera's own H.264 through untouched: no ISP, no hardware encoder, and
     * PSRAM traffic drops by orders of magnitude versus raw capture.
     *
     * Only usable when the uplink can carry whatever bitrate the camera decides on. A
     * UVC camera picks its own rate and the host driver exposes no encoding-unit control
     * to change it - the camera measured here holds ~3 Mbps at every resolution from
     * 320x240 to 1080p - so on a constrained link most frames lose a packet and the
     * viewer's jitter buffer discards them. Prefer the raw path below unless the link is
     * known to be fat.
     *
     * Against stock registry components this is the whole story on esp32p4: no camera-side
     * bitrate or key-frame period, no on-demand key frames, and a camera that never sets
     * the EoF payload flag delivers no frames at all. See the limitations section in the
     * MEDIA_STREAM_ENABLE_USB_UVC_CAM_SENSOR help text. */
    uint32_t capture_fmt = V4L2_PIX_FMT_H264;
#else
    /* Capture raw and encode locally, so bitrate, GOP length and PLI-driven keyframes
     * are ours to control - which is what makes the stream survive a lossy uplink.
     * YUY2 is what a UVC camera offers as its uncompressed format and the P4 encoder
     * takes it directly (ESP_H264_RAW_FMT_YUYV), so nothing has to convert. */
    uint32_t capture_fmt = V4L2_PIX_FMT_YUYV;
#endif
#else
    /* CSI/DVP/SPI sensors deliver raw frames for the hardware encoder. */
    uint32_t capture_fmt = V4L2_PIX_FMT_YUV420;
#endif

    /* Configure camera format with fallback resolution support */
    if (configure_camera_format(v4l2, capture_fmt) != ESP_OK) {
        return ESP_FAIL;
    }

    /* Allocate and queue buffers */
    if (allocate_and_queue_buffers(v4l2) != ESP_OK) {
        return ESP_FAIL;
    }

    /* Apply flip controls and start streaming */
    return start_streaming(v4l2);
}

/* -------------------------------------------------------------------------- */
/*  esp_video hardware bring-up                                               */
/* -------------------------------------------------------------------------- */

/* Which I2C bus the camera SCCB (sensor control) traffic rides on.
 *
 * CONFIG_MEDIA_STREAM_SCCB_I2C_INIT_BY_APP — default y whenever a BSP is selected —
 * means esp_video is handed an already-open bus instead of opening one itself.
 * With a BSP that bus is the BSP's shared one, which the ES8311 audio codec
 * also sits on, so esp_video must not own it. Without a BSP there is nobody
 * else to open it, so we do, from the pins the board header exports. */
#if CONFIG_MEDIA_STREAM_SCCB_I2C_INIT_BY_APP
#if !CONFIG_BSP_SELECT_NONE
#define SCCB_I2C_HANDLE()   bsp_i2c_get_handle()
#else
static i2c_master_bus_handle_t s_sccb_i2c_handle;
#define SCCB_I2C_HANDLE()   s_sccb_i2c_handle
#endif

static esp_err_t sccb_i2c_bus_init(void)
{
#if !CONFIG_BSP_SELECT_NONE
    /* media_stream_init() already brought the BSP I2C bus up; bsp_i2c_init()
     * itself is idempotent, so call it again defensively in case this path
     * is reached without media_stream_init() running first. */
    esp_err_t ret = bsp_i2c_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "bsp_i2c_init failed: %s", esp_err_to_name(ret));
    }
    return ret;
#else
    if (s_sccb_i2c_handle) {
        return ESP_OK;
    }

    const i2c_master_bus_config_t bus_config = {
        .i2c_port = MEDIA_STREAM_SCCB_I2C_PORT_INIT_BY_APP,
        .scl_io_num = MEDIA_STREAM_SCCB_I2C_SCL_PIN_INIT_BY_APP,
        .sda_io_num = MEDIA_STREAM_SCCB_I2C_SDA_PIN_INIT_BY_APP,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };

    esp_err_t ret = i2c_new_master_bus(&bus_config, &s_sccb_i2c_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "failed to open the SCCB I2C bus: %s", esp_err_to_name(ret));
    }
    return ret;
#endif
}
#endif /* CONFIG_MEDIA_STREAM_SCCB_I2C_INIT_BY_APP */

#if defined(MEDIA_STREAM_MIPI_CSI_XCLK_PIN) && MEDIA_STREAM_MIPI_CSI_XCLK_PIN > 0
#if !CONFIG_CAMERA_XCLK_USE_ESP_CLOCK_ROUTER
#error "This board drives the MIPI-CSI sensor clock from a GPIO — enable CONFIG_CAMERA_XCLK_USE_ESP_CLOCK_ROUTER"
#endif

static esp_cam_sensor_xclk_handle_t s_xclk_handle;

/* Boards that fit no crystal to the camera module (ESP32-P4-EYE: XCLK on GPIO
 * 11 at 24 MHz) need the SoC to drive the sensor's input clock *before*
 * esp_video probes it over SCCB — an unclocked sensor never answers, so
 * auto-detect fails and the session comes up audio-only. Boards with their own
 * oscillator define MEDIA_STREAM_MIPI_CSI_XCLK_PIN as -1 and this compiles out. */
static esp_err_t mipi_csi_xclk_start(void)
{
    const esp_cam_sensor_xclk_config_t xclk_config = {
        .esp_clock_router_cfg = {
            .xclk_pin     = MEDIA_STREAM_MIPI_CSI_XCLK_PIN,
            .xclk_freq_hz = MEDIA_STREAM_MIPI_CSI_XCLK_FREQ,
        },
    };

    ESP_LOGI(TAG, "MIPI-CSI xclk pin=%d, freq=%d",
             (int) MEDIA_STREAM_MIPI_CSI_XCLK_PIN, (int) MEDIA_STREAM_MIPI_CSI_XCLK_FREQ);

    esp_err_t ret = esp_cam_sensor_xclk_allocate(ESP_CAM_SENSOR_XCLK_ESP_CLOCK_ROUTER, &s_xclk_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "failed to allocate the sensor xclk: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_cam_sensor_xclk_start(s_xclk_handle, &xclk_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "failed to start the sensor xclk: %s", esp_err_to_name(ret));
        esp_cam_sensor_xclk_free(s_xclk_handle);
        s_xclk_handle = NULL;
        return ret;
    }

    return ESP_OK;
}
#endif /* MEDIA_STREAM_MIPI_CSI_XCLK_PIN > 0 */

/* Register the video devices esp_video exposes as /dev/videoN.
 *
 * The whole configuration comes out of esp_video_if_cam_sel.h, which picks a
 * header from src/boards/<board>/ off the bsp_selector choice — so supporting a
 * new board or camera interface is a header, not a change here. This mirrors
 * esp_video's own reference implementation
 * (examples/common_components/example_video_common/example_init_video.c).
 *
 * Each interface arm is compiled out unless the board and the esp_video Kconfig
 * both enable it. On every P4 board shipped in this repo only MIPI-CSI is on,
 * so this reduces to the CSI-only config it replaced, plus the sensor XCLK. */
static esp_err_t esp_video_hw_init(void)
{
#if CONFIG_MEDIA_STREAM_SCCB_I2C_INIT_BY_APP
    esp_err_t sccb_ret = sccb_i2c_bus_init();
    if (sccb_ret != ESP_OK) {
        return sccb_ret;
    }
#endif

/* The pin-based CSI config only compiles when Kconfig defines the pins, which it does
 * only under BSP_SELECT_NONE (Kconfig.projbuild: `if BSP_SELECT_NONE`). With a BSP
 * selected the board owns power, reset, XCLK and SCCB, and bsp_camera_start() below
 * performs the bring-up instead. */
#if MEDIA_STREAM_ENABLE_MIPI_CSI_CAM_SENSOR && CONFIG_BSP_SELECT_NONE
    const esp_video_init_csi_config_t csi_config = {
        .sccb_config = {
#if !CONFIG_MEDIA_STREAM_SCCB_I2C_INIT_BY_APP
            .init_sccb = true,
            .i2c_config = {
                .port    = MEDIA_STREAM_MIPI_CSI_SCCB_I2C_PORT,
                .scl_pin = MEDIA_STREAM_MIPI_CSI_SCCB_I2C_SCL_PIN,
                .sda_pin = MEDIA_STREAM_MIPI_CSI_SCCB_I2C_SDA_PIN,
            },
#else
            .init_sccb  = false,
            .i2c_handle = SCCB_I2C_HANDLE(),
#endif
            .freq = MEDIA_STREAM_MIPI_CSI_SCCB_I2C_FREQ,
        },
        .reset_pin = MEDIA_STREAM_MIPI_CSI_CAM_SENSOR_RESET_PIN,
        .pwdn_pin  = MEDIA_STREAM_MIPI_CSI_CAM_SENSOR_PWDN_PIN,
#if CONFIG_MEDIA_STREAM_MIPI_CSI_VIDEO_DEVICE_DONT_INIT_LDO
        .dont_init_ldo = true,
#endif
    };

#if MEDIA_STREAM_ENABLE_MIPI_CSI_CAM_MOTOR
    const esp_video_init_cam_motor_config_t cam_motor_config = {
        .sccb_config = {
#if !CONFIG_MEDIA_STREAM_SCCB_I2C_INIT_BY_APP
            .init_sccb = true,
            .i2c_config = {
                .port    = MEDIA_STREAM_MIPI_CSI_CAM_MOTOR_SCCB_I2C_PORT,
                .scl_pin = MEDIA_STREAM_MIPI_CSI_CAM_MOTOR_SCCB_I2C_SCL_PIN,
                .sda_pin = MEDIA_STREAM_MIPI_CSI_CAM_MOTOR_SCCB_I2C_SDA_PIN,
            },
#else
            .init_sccb  = false,
            .i2c_handle = SCCB_I2C_HANDLE(),
#endif
            .freq = MEDIA_STREAM_MIPI_CSI_CAM_MOTOR_SCCB_I2C_FREQ,
        },
        .reset_pin  = MEDIA_STREAM_MIPI_CSI_CAM_MOTOR_RESET_PIN,
        .pwdn_pin   = MEDIA_STREAM_MIPI_CSI_CAM_MOTOR_PWDN_PIN,
        .signal_pin = MEDIA_STREAM_MIPI_CSI_CAM_MOTOR_SIGNAL_PIN,
    };
#endif /* MEDIA_STREAM_ENABLE_MIPI_CSI_CAM_MOTOR */
#endif /* MEDIA_STREAM_ENABLE_MIPI_CSI_CAM_SENSOR */

#if MEDIA_STREAM_ENABLE_DVP_CAM_SENSOR
    const esp_video_init_dvp_config_t dvp_config = {
        .sccb_config = {
#if !CONFIG_MEDIA_STREAM_SCCB_I2C_INIT_BY_APP
            .init_sccb = true,
            .i2c_config = {
                .port    = MEDIA_STREAM_DVP_SCCB_I2C_PORT,
                .scl_pin = MEDIA_STREAM_DVP_SCCB_I2C_SCL_PIN,
                .sda_pin = MEDIA_STREAM_DVP_SCCB_I2C_SDA_PIN,
            },
#else
            .init_sccb  = false,
            .i2c_handle = SCCB_I2C_HANDLE(),
#endif
            .freq = MEDIA_STREAM_DVP_SCCB_I2C_FREQ,
        },
        .reset_pin = MEDIA_STREAM_DVP_CAM_SENSOR_RESET_PIN,
        .pwdn_pin  = MEDIA_STREAM_DVP_CAM_SENSOR_PWDN_PIN,
        .dvp_pin = {
            .data_width = CAM_CTLR_DATA_WIDTH_8,
            .data_io = {
                MEDIA_STREAM_DVP_D0_PIN, MEDIA_STREAM_DVP_D1_PIN, MEDIA_STREAM_DVP_D2_PIN, MEDIA_STREAM_DVP_D3_PIN,
                MEDIA_STREAM_DVP_D4_PIN, MEDIA_STREAM_DVP_D5_PIN, MEDIA_STREAM_DVP_D6_PIN, MEDIA_STREAM_DVP_D7_PIN,
            },
            .vsync_io = MEDIA_STREAM_DVP_VSYNC_PIN,
            .de_io    = MEDIA_STREAM_DVP_DE_PIN,
            .pclk_io  = MEDIA_STREAM_DVP_PCLK_PIN,
            .xclk_io  = MEDIA_STREAM_DVP_XCLK_PIN,
        },
        .xclk_freq = MEDIA_STREAM_DVP_XCLK_FREQ,
    };
#endif /* MEDIA_STREAM_ENABLE_DVP_CAM_SENSOR */

#if MEDIA_STREAM_ENABLE_SPI_CAM_SENSOR
    const esp_video_init_spi_config_t spi_config[] = {
        {
            .sccb_config = {
#if !CONFIG_MEDIA_STREAM_SCCB_I2C_INIT_BY_APP
                .init_sccb = true,
                .i2c_config = {
                    .port    = MEDIA_STREAM_SPI_CAM_0_SCCB_I2C_PORT,
                    .scl_pin = MEDIA_STREAM_SPI_CAM_0_SCCB_I2C_SCL_PIN,
                    .sda_pin = MEDIA_STREAM_SPI_CAM_0_SCCB_I2C_SDA_PIN,
                },
#else
                .init_sccb  = false,
                .i2c_handle = SCCB_I2C_HANDLE(),
#endif
                .freq = MEDIA_STREAM_SPI_CAM_0_SCCB_I2C_FREQ,
            },
            .intf    = MEDIA_STREAM_SPI_CAM_0_INTERFACE,
            .io_mode = MEDIA_STREAM_SPI_CAM_0_IO_MODE,

            .spi_port         = MEDIA_STREAM_SPI_CAM_0_SPI_PORT,
            .spi_cs_pin       = MEDIA_STREAM_SPI_CAM_0_CS_PIN,
            .spi_sclk_pin     = MEDIA_STREAM_SPI_CAM_0_SCLK_PIN,
            .spi_data0_io_pin = MEDIA_STREAM_SPI_CAM_0_DATA0_IO_PIN,
            .spi_data1_io_pin = MEDIA_STREAM_SPI_CAM_0_DATA1_IO_PIN,

            .reset_pin = MEDIA_STREAM_SPI_CAM_0_SENSOR_RESET_PIN,
            .pwdn_pin  = MEDIA_STREAM_SPI_CAM_0_SENSOR_PWDN_PIN,

            .xclk_source = MEDIA_STREAM_SPI_CAM_0_XCLK_RESOURCE,
            .xclk_freq   = MEDIA_STREAM_SPI_CAM_0_XCLK_FREQ,
            .xclk_pin    = MEDIA_STREAM_SPI_CAM_0_XCLK_PIN,
            /* Upstream's reference guards this on CONFIG_MEDIA_STREAM_SPI_CAM_XCLK_USE_LEDC,
             * which no Kconfig defines — the per-camera symbol is the _0_ one, and the
             * struct member only exists under CONFIG_CAMERA_XCLK_USE_LEDC (which the
             * Kconfig entry depends on), so both have to hold. */
#if CONFIG_MEDIA_STREAM_SPI_CAM_0_XCLK_USE_LEDC
            .xclk_ledc_cfg = {
                .timer   = MEDIA_STREAM_SPI_CAM_0_XCLK_TIMER,
                .clk_cfg = LEDC_AUTO_CLK,
                .channel = MEDIA_STREAM_SPI_CAM_0_XCLK_TIMER_CHANNEL,
            },
#endif
        },
#if MEDIA_STREAM_ENABLE_SPI_CAM_1_SENSOR
        {
            .sccb_config = {
#if !CONFIG_MEDIA_STREAM_SCCB_I2C_INIT_BY_APP
                .init_sccb = true,
                .i2c_config = {
                    .port    = MEDIA_STREAM_SPI_CAM_1_SCCB_I2C_PORT,
                    .scl_pin = MEDIA_STREAM_SPI_CAM_1_SCCB_I2C_SCL_PIN,
                    .sda_pin = MEDIA_STREAM_SPI_CAM_1_SCCB_I2C_SDA_PIN,
                },
#else
                .init_sccb  = false,
                .i2c_handle = SCCB_I2C_HANDLE(),
#endif
                .freq = MEDIA_STREAM_SPI_CAM_1_SCCB_I2C_FREQ,
            },
            .intf    = ESP_CAM_CTLR_SPI_CAM_INTF_SPI,
            .io_mode = ESP_CAM_CTLR_SPI_CAM_IO_MODE_1BIT,

            .spi_port         = MEDIA_STREAM_SPI_CAM_1_SPI_PORT,
            .spi_cs_pin       = MEDIA_STREAM_SPI_CAM_1_CS_PIN,
            .spi_sclk_pin     = MEDIA_STREAM_SPI_CAM_1_SCLK_PIN,
            .spi_data0_io_pin = MEDIA_STREAM_SPI_CAM_1_DATA0_IO_PIN,
            .spi_data1_io_pin = MEDIA_STREAM_SPI_CAM_1_DATA1_IO_PIN,

            .reset_pin = MEDIA_STREAM_SPI_CAM_1_SENSOR_RESET_PIN,
            .pwdn_pin  = MEDIA_STREAM_SPI_CAM_1_SENSOR_PWDN_PIN,

            .xclk_source = MEDIA_STREAM_SPI_CAM_1_XCLK_RESOURCE,
            .xclk_freq   = MEDIA_STREAM_SPI_CAM_1_XCLK_FREQ,
            .xclk_pin    = MEDIA_STREAM_SPI_CAM_1_XCLK_PIN,
#if CONFIG_MEDIA_STREAM_SPI_CAM_1_XCLK_USE_LEDC
            .xclk_ledc_cfg = {
                .timer   = MEDIA_STREAM_SPI_CAM_1_XCLK_TIMER,
                .clk_cfg = LEDC_AUTO_CLK,
                .channel = MEDIA_STREAM_SPI_CAM_1_XCLK_TIMER_CHANNEL,
            },
#endif
        },
#endif /* MEDIA_STREAM_ENABLE_SPI_CAM_1_SENSOR */
    };
#endif /* MEDIA_STREAM_ENABLE_SPI_CAM_SENSOR */

#if MEDIA_STREAM_ENABLE_USB_UVC_CAM_SENSOR
#if CONFIG_USB_HOST_CONTROL_TRANSFER_MAX_SIZE < 4096
#error "Set CONFIG_USB_HOST_CONTROL_TRANSFER_MAX_SIZE=4096: a UVC configuration descriptor \
enumerates every format, frame size and frame interval, so it runs past the default and \
enumeration fails with \"Configuration descriptor larger than control transfer max length\"."
#endif

    const esp_video_init_usb_uvc_config_t usb_uvc_config = {
        .uvc = {
            .uvc_dev_num   = CONFIG_MEDIA_STREAM_USB_UVC_DEVICES_NUM,
            .task_stack    = CONFIG_MEDIA_STREAM_USB_UVC_TASK_STACK_SIZE,
            .task_priority = CONFIG_MEDIA_STREAM_USB_UVC_TASK_PRIORITY,
            .task_affinity = CONFIG_MEDIA_STREAM_USB_UVC_TASK_AFFINITY,
        },
        .usb = {
            .init_usb_host_lib = true,
            .peripheral_map    = CONFIG_MEDIA_STREAM_USB_PERIPHERAL_MAP,
            .task_stack        = CONFIG_MEDIA_STREAM_USB_LIB_TASK_STACK_SIZE,
            .task_priority     = CONFIG_MEDIA_STREAM_USB_LIB_TASK_PRIORITY,
            .task_affinity     = CONFIG_MEDIA_STREAM_USB_LIB_TASK_AFFINITY,
        },
    };
#endif /* MEDIA_STREAM_ENABLE_USB_UVC_CAM_SENSOR */

    const esp_video_init_config_t cam_config = {
#if MEDIA_STREAM_ENABLE_MIPI_CSI_CAM_SENSOR && CONFIG_BSP_SELECT_NONE
        .csi = &csi_config,
#if MEDIA_STREAM_ENABLE_MIPI_CSI_CAM_MOTOR
        .cam_motor = &cam_motor_config,
#endif
#endif
#if MEDIA_STREAM_ENABLE_DVP_CAM_SENSOR
        .dvp = &dvp_config,
#endif
#if MEDIA_STREAM_ENABLE_SPI_CAM_SENSOR
        .spi = spi_config,
#endif
#if MEDIA_STREAM_ENABLE_USB_UVC_CAM_SENSOR
        .usb_uvc = &usb_uvc_config,
#endif
    };

#if defined(MEDIA_STREAM_MIPI_CSI_XCLK_PIN) && MEDIA_STREAM_MIPI_CSI_XCLK_PIN > 0
    esp_err_t xclk_ret = mipi_csi_xclk_start();
    if (xclk_ret != ESP_OK) {
        return xclk_ret;
    }
#endif

    /* Mark registered before the call: esp_video_init() registers the ISP
     * device even when it then fails to detect the sensor, and that can't
     * be undone — so a retry must never call it again. */
    s_esp_video_registered = true;

#if !CONFIG_BSP_SELECT_NONE && !MEDIA_STREAM_ENABLE_USB_UVC_CAM_SENSOR
    /* BSP path: the selected board's BSP owns the camera pins, power and XCLK and
     * registers the CSI sensor with esp_video itself. Boards whose camera enable sits
     * behind an I2C IO expander (M5Stack Tab5) or a power switch (P4-EYE) only come up
     * through that sequencing, so the generic pin-based init below cannot replace it.
     * UVC needs the generic path: there is no BSP camera to start. */
    esp_err_t ret = bsp_camera_start(NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "bsp_camera_start failed: %s", esp_err_to_name(ret));
    }
#else
    esp_err_t ret = esp_video_init(&cam_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize video: %s", esp_err_to_name(ret));
#if defined(MEDIA_STREAM_MIPI_CSI_XCLK_PIN) && MEDIA_STREAM_MIPI_CSI_XCLK_PIN > 0
        esp_cam_sensor_xclk_stop(s_xclk_handle);
        esp_cam_sensor_xclk_free(s_xclk_handle);
        s_xclk_handle = NULL;
#endif
        return ret;
    }
#endif /* BSP vs generic camera bring-up */

    return ESP_OK;
}

#if CONFIG_ESP_VIDEO_IF_VERBOSE_DRIVER_LOGS
/* Raise the capture stack's own logging.
 *
 * Almost every frame-drop path below us is ESP_LOGD or silent: esp_video
 * reports UVC frame-buffer overflow/underflow at DEBUG, and the USB-UVC host
 * stack rejects malformed ISOC payload headers at DEBUG and drops skipped or
 * timed-out packets with no log at all. A camera that delivers zero frames is
 * therefore indistinguishable in a default-level log from one that was never
 * started, so raise the tags rather than guess. */
static void enable_verbose_driver_logs(void)
{
    static const char *const tags[] = {
        "esp_video",        /* generic buffer/stream layer */
        "usb_uvc_device",   /* esp_video's UVC capture device: frame over/underflow */
        "uvc",              /* usb_host_uvc: stream open/negotiation, alt switches */
        "uvc-frame",
        "uvc-control",
    };

    for (size_t i = 0; i < sizeof(tags) / sizeof(tags[0]); i++) {
        esp_log_level_set(tags[i], ESP_LOG_DEBUG);
    }

    /* uvc-isoc logs once per isochronous packet at DEBUG — 8000 lines/second on
     * a high-speed endpoint, which buries every app-level message and is slow
     * enough to perturb the timing it is meant to observe. INFO keeps the
     * once-per-second packet/frame counter and the warnings ("usb err",
     * "frame error", "missed EoF") while dropping the per-packet spam. Raise it
     * to DEBUG by hand when individual payload headers are genuinely needed. */
    esp_log_level_set("uvc-isoc", ESP_LOG_INFO);

#if !defined(CONFIG_LOG_MAXIMUM_LEVEL) || CONFIG_LOG_MAXIMUM_LEVEL < 4
    ESP_LOGW(TAG, "verbose driver logs requested but DEBUG statements are compiled out; "
                  "set CONFIG_LOG_MAXIMUM_LEVEL_DEBUG=y");
#endif
}
#endif /* CONFIG_ESP_VIDEO_IF_VERBOSE_DRIVER_LOGS */

esp_err_t esp_video_if_init(void)
{
#if CONFIG_ESP_VIDEO_IF_VERBOSE_DRIVER_LOGS
    enable_verbose_driver_logs();
#endif

    if (g_v4l2) {
        ESP_LOGD(TAG, "video interface already initialized, restarting streaming");
        return esp_video_if_start();
    }

#if CONFIG_CODEC_I2C_BACKWARD_COMPATIBLE
    ESP_LOGE(TAG, "esp_video is not compatible with old I2C driver");
    return ESP_FAIL;
#endif

    v4l2_src_t *v4l2 = heap_caps_calloc(1, sizeof(v4l2_src_t), MALLOC_CAP_SPIRAM);
    if (!v4l2) {
        ESP_LOGE(TAG, "Failed to allocate memory for v4l2");
        return ESP_FAIL;
    }

    // Register esp_video only once per process (the ISP registration persists;
    // there is no esp_video_deinit()). A second call faults with "ISP id=N has
    // been registered". A flag is more reliable than probing /dev/video0, which
    // is absent when a prior attempt registered the ISP but failed sensor detect.
    if (!s_esp_video_registered) {
        if (esp_video_hw_init() != ESP_OK) {
            free(v4l2);
            return ESP_FAIL;
        }
    } else {
        ESP_LOGI(TAG, "esp_video ISP already registered; reusing it");
    }

    if (init_camera(v4l2) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize capture video");
        free(v4l2);
        return ESP_FAIL;
    }
    g_v4l2 = v4l2;

    if (esp_video_if_start() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start video");
        g_v4l2 = NULL;
        if (v4l2->cap_fd >= 0) {
            close(v4l2->cap_fd);  /* Close file descriptor before freeing */
        }
        free(v4l2);
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t esp_video_if_get_resolution(video_resolution_t *resolution)
{
    if (!resolution) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!g_v4l2 || g_current_resolution.width == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    *resolution = g_current_resolution;
    return ESP_OK;
}

esp_err_t esp_video_if_get_pixel_format(uint32_t *pixelformat)
{
    if (!pixelformat) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!g_v4l2 || g_current_resolution.width == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    *pixelformat = g_current_pixelformat;
    return ESP_OK;
}


esp_err_t esp_video_if_get_expected_resolution(video_resolution_t *resolution)
{
    if (!resolution) {
        return ESP_ERR_INVALID_ARG;
    }

    /* The first entry configure_camera_format() will try. */
#if MEDIA_STREAM_ENABLE_USB_UVC_CAM_SENSOR && !CONFIG_MEDIA_STREAM_UVC_PASSTHROUGH_H264
    resolution->width = 320;
    resolution->height = 240;
#else
    resolution->width = g_desired_resolution.width ? g_desired_resolution.width : MEDIA_STREAM_CAPTURE_WIDTH;
    resolution->height = g_desired_resolution.height ? g_desired_resolution.height : MEDIA_STREAM_CAPTURE_HEIGHT;
#endif
    resolution->fps = g_desired_resolution.fps ? g_desired_resolution.fps : 30;
    return ESP_OK;
}

esp_err_t esp_video_if_set_desired_resolution(const video_resolution_t *resolution)
{
    if (!resolution) {
        return ESP_ERR_INVALID_ARG;
    }
    g_desired_resolution = *resolution;
    return ESP_OK;
}

esp_err_t esp_video_if_cleanup(void)
{
    /* This function explicitly frees mapped buffers and closes the camera fd.
     * Call this when you want a full cleanup (releasing mmap memory).
     */

    if (!g_v4l2) {
        ESP_LOGD(TAG, "No buffers to clean up");
        return ESP_OK;
    }

    video_stop_cb(g_v4l2);
    free_mapped_buffers(g_v4l2);

    if (g_v4l2->cap_fd >= 0) {
        close(g_v4l2->cap_fd);
        g_v4l2->cap_fd = -1;
    }

    heap_caps_free(g_v4l2);
    g_v4l2 = NULL;

    g_current_pixelformat = 0;
    g_current_resolution.width = 0;
    g_current_resolution.height = 0;
    g_current_resolution.fps = 0;

    ESP_LOGI(TAG, "Buffers and camera fd cleaned up");
    return ESP_OK;
}

#endif /* MEDIA_STREAM_HAS_ESP_VIDEO_CAPTURE */
