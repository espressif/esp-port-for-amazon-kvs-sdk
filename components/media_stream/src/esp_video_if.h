/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: ESPRESSIF MIT
 */

#pragma once

#include "sdkconfig.h"
#include "esp_err.h"
#include "H264FrameGrabber.h"
#include "video_capture.h"
#include <stdbool.h>
#include <sys/time.h>

/* Size of the V4L2 capture pool. Consumers that key a table off the buffer index
 * (video_raw_bus.c) size their arrays with this, so it has to be visible here rather
 * than staying private to esp_video_if.c. The fallback covers an sdkconfig predating
 * the option. */
#ifndef CONFIG_MEDIA_STREAM_CAM_BUFFER_COUNT
#define CONFIG_MEDIA_STREAM_CAM_BUFFER_COUNT 4
#endif
#define MEDIA_STREAM_CAM_BUFFER_COUNT   CONFIG_MEDIA_STREAM_CAM_BUFFER_COUNT

/* Fallback when the Kconfig symbol is absent — a project whose sdkconfig
 * predates this option simply will not define it. */
#ifndef CONFIG_ESP_VIDEO_IF_DQBUF_TIMEOUT_MS
#define CONFIG_ESP_VIDEO_IF_DQBUF_TIMEOUT_MS    1000
#endif

/**
 * @brief Frame buffer structure
 *
 * Descriptors are per capture buffer and owned by this layer, so several may be
 * checked out at once - one per index. The pointer stays valid until the matching
 * esp_video_if_release_frame().
 */
typedef struct {
    uint8_t *buf;               /*!< Pointer to the frame data */
    size_t len;                 /*!< Length of the buffer in bytes */
    size_t width;               /*!< Width of the image frame in pixels */
    size_t height;              /*!< Height of the image frame in pixels */
    struct timeval timestamp;   /*!< Timestamp since boot of the frame */
    uint8_t index;              /*!< V4L2 buffer index; stable key for the frame's slot */
} video_fb_t;

/**
 * @brief Initialize the video interface and start the camera
 *
 * Counts as one esp_video_if_start(): pair it with esp_video_if_stop() or
 * esp_video_if_deinit(). On an initialized interface this is the same as
 * esp_video_if_start().
 *
 * @return esp_err_t ESP_OK on success, otherwise an error code
 */
esp_err_t esp_video_if_init(void);

/**
 * @brief Release the camera
 *
 * The camera stops streaming once every consumer that started it has stopped
 * it; until then the call only drops this consumer's hold. Safe to call from
 * any task.
 *
 * @return esp_err_t ESP_OK on success, ESP_FAIL when the interface is not initialized
 */
esp_err_t esp_video_if_stop(void);

/**
 * @brief Deinitialize the video interface while retaining mmap buffers
 *
 * Releases the camera like esp_video_if_stop() (it stops streaming once no other
 * consumer holds it) but keeps the mmap buffers and fd open so the next
 * init/start cycle can reuse them without reallocation. The camera hardware may
 * remain powered because the fd stays open.
 *
 * For a full cleanup (free buffers, close fd, release resources), call
 * esp_video_if_cleanup().
 *
 * @return esp_err_t ESP_OK on success, otherwise an error code
 */
esp_err_t esp_video_if_deinit(void);

/**
 * @brief Start the camera, or join a stream another consumer already started
 *
 * Each successful call must be balanced by esp_video_if_stop(). A joining
 * consumer gets the running format, not a resolution it set meanwhile; read
 * it back with esp_video_if_get_resolution(). Safe to call from any task.
 *
 * @return esp_err_t ESP_OK on success, otherwise an error code
 */
esp_err_t esp_video_if_start(void);

/**
 * @brief Stop and restart the stream for every consumer holding the camera
 *
 * For settings the camera only reads at STREAMON. Unlike esp_video_if_stop()
 * followed by esp_video_if_start(), this restarts the stream even when several
 * consumers hold the camera, and leaves their holds untouched. On UVC the
 * capture buffers are freed and reallocated, so no consumer may be holding a
 * frame across the call.
 *
 * @return esp_err_t
 *  - ESP_OK: Stream restarted
 *  - ESP_ERR_INVALID_STATE: No consumer holds the camera
 *  - Other: STREAMON failed; the camera is off, the holds remain and a later
 *    restart or start turns it on again
 */
esp_err_t esp_video_if_restart(void);

/**
 * @brief Whether the camera is held but not delivering frames
 *
 * True when a consumer holds the camera and it is either off (a restart failed)
 * or streaming with its last few dequeues all failed. Starting the camera joins
 * a stalled stream like any other: only a holder that knows no other task is
 * dequeuing (none blocked in DQBUF, no frame checked out) may restart it, with
 * esp_video_if_restart(), since on UVC the restart frees the capture buffers.
 */
bool esp_video_if_is_stalled(void);

/**
 * @brief Get a raw video frame
 *
 * @return video_fb_t* Pointer to the raw frame, or NULL if no frame is available
 */
video_fb_t *esp_video_if_get_frame(void);

/**
 * @brief Release a video frame when done with it
 *
 * @param fb Pointer to the frame to release
 */
void esp_video_if_release_frame(video_fb_t *fb);

/**
 * @brief Get the current video resolution
 *
 * @param resolution Pointer to store the current resolution
 * @return esp_err_t
 *  - ESP_OK: Successfully retrieved resolution
 *  - ESP_ERR_INVALID_ARG: resolution pointer is NULL
 *  - ESP_ERR_INVALID_STATE: Video interface not initialized
 */
esp_err_t esp_video_if_get_resolution(video_resolution_t *resolution);

/**
 * @brief Get the current video pixelformat
 *
 * @param pixelformat Pointer to store the current pixelformat
 * @return esp_err_t
 *  - ESP_OK: Successfully retrieved pixelformat
 *  - ESP_ERR_INVALID_ARG: pixelformat pointer is NULL
 *  - ESP_ERR_INVALID_STATE: Video interface not initialized
 */
esp_err_t esp_video_if_get_pixel_format(uint32_t *pixelformat);

/**
 * @brief Get the size of one capture buffer, i.e. the largest frame the camera can deliver
 *
 * @param size Pointer to store the size in bytes
 * @return esp_err_t
 *  - ESP_OK: Successfully retrieved the size
 *  - ESP_ERR_INVALID_ARG: size pointer is NULL
 *  - ESP_ERR_INVALID_STATE: Capture is not streaming
 */
esp_err_t esp_video_if_get_frame_size(size_t *size);

/**
 * @brief Get the resolution capture will first ask the camera for
 *
 * Usable before capture starts. The camera may still settle on a fallback, so prefer
 * esp_video_if_get_resolution() once it is running.
 *
 * @param resolution Pointer to store the expected resolution
 * @return ESP_OK, or ESP_ERR_INVALID_ARG if resolution is NULL
 */
esp_err_t esp_video_if_get_expected_resolution(video_resolution_t *resolution);

/**
 * @brief Set desired resolution before init (called by video_capture_adapter)
 *
 * @param resolution Desired resolution (width, height, fps). Use 0 for width/height to use defaults.
 * @return esp_err_t ESP_OK on success
 */
esp_err_t esp_video_if_set_desired_resolution(const video_resolution_t *resolution);

/**
 * @brief Cleanup mapped buffers and close camera fd
 *
 * This function explicitly frees mapped buffers and closes the camera file
 * descriptor. After this, a subsequent esp_video_if_init/start cycle will
 * reallocate buffers.
 *
 * @return esp_err_t ESP_OK on success, ESP_ERR_INVALID_STATE while a consumer
 *         still holds the camera (every start must be stopped first)
 */
esp_err_t esp_video_if_cleanup(void);
