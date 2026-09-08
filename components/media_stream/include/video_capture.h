/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @brief Generic video capture interface for camera input and encoding
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Video codec type enumeration
 */
typedef enum {
    VIDEO_CODEC_H264,
    VIDEO_CODEC_MJPEG,
    VIDEO_CODEC_RAW,
    /* Add more codecs as needed */
} video_codec_type_t;

/**
 * @brief Video resolution configuration
 */
typedef struct {
    uint16_t width;
    uint16_t height;
    uint8_t fps;
} video_resolution_t;

/**
 * @brief Frame type for encoded video
 */
typedef enum {
    VIDEO_FRAME_TYPE_I,    /* I-frame */
    VIDEO_FRAME_TYPE_P,    /* P-frame */
    VIDEO_FRAME_TYPE_B,    /* B-frame */
    VIDEO_FRAME_TYPE_OTHER /* Other frame types */
} video_frame_type_t;

/**
 * @brief FourCC pixel-format codes
 *
 * Values match the V4L2 codes of the same name, so a fourcc read straight out of the
 * driver compares equal to the constant here. Asserted, not assumed: see the
 * _Static_asserts in esp32p4_frame_grabber.c, which is where a V4L2 header is in scope.
 *
 * DO NOT "simplify" this away by forwarding the driver's fourcc verbatim. On the P4 that
 * fourcc is wrong: esp_video asks for V4L2_PIX_FMT_YUV420 on the CSI/DVP path, but the ISP
 * emits the semi-packed O_UYY_E_VYY layout, which V4L2 has no code for and esp_video adds
 * none. 'YU12' therefore arrives on a buffer that is not I420 - and VIDEO_FOURCC_I420 is
 * deliberately that same 32-bit value, so the two are indistinguishable downstream. The
 * capture pump is the only layer that knows which it really is, and it re-labels rather
 * than forwards (esp32p4_frame_grabber.c, video_pump_task). That relabel is the whole
 * reason this namespace exists instead of linux/videodev2.h - which a public header may
 * not include anyway, since esp_video is not a dependency on every target.
 */
#define VIDEO_FOURCC(a, b, c, d) ((uint32_t)(a) | ((uint32_t)(b) << 8) | \
                                  ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))

/* Semi-packed YUV420: odd rows U Y Y, even rows V Y Y. What the P4 ISP emits and the
 * only layout the P4 hardware H.264 encoder accepts */
#define VIDEO_FOURCC_O_UYY_E_VYY  VIDEO_FOURCC('O', 'U', 'E', 'V')

#define VIDEO_FOURCC_I420         VIDEO_FOURCC('Y', 'U', '1', '2')  /* planar YUV 4:2:0 */
#define VIDEO_FOURCC_YUYV         VIDEO_FOURCC('Y', 'U', 'Y', 'V')  /* packed YUV 4:2:2 */
#define VIDEO_FOURCC_UYVY         VIDEO_FOURCC('U', 'Y', 'V', 'Y')
#define VIDEO_FOURCC_RGB565       VIDEO_FOURCC('R', 'G', 'B', 'P')  /* little-endian */
#define VIDEO_FOURCC_RGB24        VIDEO_FOURCC('R', 'G', 'B', '3')
#define VIDEO_FOURCC_GREY         VIDEO_FOURCC('G', 'R', 'E', 'Y')  /* 8-bit luma only */
#define VIDEO_FOURCC_SBGGR8       VIDEO_FOURCC('B', 'A', '8', '1')  /* raw Bayer, ISP bypassed */
#define VIDEO_FOURCC_H264         VIDEO_FOURCC('H', '2', '6', '4')

/**
 * @brief A raw (uncompressed) frame handed to a raw sink
 *
 * Deliberately small and free of pointers-to-owned-things: it is passed BY VALUE through
 * queues, so a frame costs no allocation to deliver.
 *
 * @c slot and @c generation are the frame's identity for release purposes. Do not modify
 * them, and pass the descriptor back unchanged - releasing a mutated or stale descriptor
 * is detected and refused rather than corrupting the buffer pool.
 */
typedef struct {
    uint8_t *buffer;        /* Pixel data */
    size_t   len;           /* Bytes valid in @c buffer */
    uint16_t width;
    uint16_t height;
    uint32_t fourcc;        /* VIDEO_FOURCC_*: what @c buffer actually contains */
    uint64_t timestamp_us;  /* Capture time, microseconds since boot */
    uint32_t seq;           /* Monotonic capture counter; a gap means frames were dropped */
    uint8_t  slot;          /* Opaque: which capture buffer this is */
    uint8_t  generation;    /* Opaque: stale-release detector */
    bool     owned;         /* true when this is a private converted buffer, not a camera one */
} video_raw_frame_t;

/**
 * @brief Video capture configuration
 */
typedef struct {
    video_codec_type_t codec;
    video_resolution_t resolution;
    uint8_t quality;            /* 0-100, higher is better quality */
    uint32_t bitrate;           /* Target bitrate in kbps */
    void *codec_specific;       /* Codec-specific parameters if needed */
} video_capture_config_t;

/**
 * @brief Get the resolution the capture pipeline is currently running at
 *
 * Reflects what the camera actually negotiated, which can differ from the requested
 * resolution when the sensor or UVC camera rejects it. Before capture starts it reports
 * the resolution capture will ask for. Needed by consumers that must describe the stream
 * to a peer (for example choosing an H.264 level for SDP).
 *
 * @param[out] resolution Filled in on success
 * @return ESP_OK, or ESP_ERR_NOT_SUPPORTED where the capture layer cannot tell
 */
esp_err_t video_capture_get_active_resolution(video_resolution_t *resolution);

/**
 * @brief What the capture pipeline MEASURED over its most recent one-second window
 *
 * These are achieved rates, not targets - the same numbers the grabber logs as its
 * `cap_fps:` and `enc_fps:` lines, kept instead of only printed so a UI can show them.
 * For what the rate controller is ASKING for, see video_rate_ctrl_get_stats(); the two
 * differing is the interesting case (a target the hardware cannot meet).
 *
 * Sampled without a lock: each field is published independently at the end of its window,
 * so a reader can in principle catch @c cap_fps from one window and @c enc_fps from the
 * next. Harmless at a one-second cadence, and cheaper than serialising the pump against a
 * display task.
 */
typedef struct {
    uint32_t cap_fps;              /* Camera frames published to the raw bus */
    uint32_t enc_fps;              /* Encoded frames the encoder produced */
    uint32_t enc_kbps;             /* Encoded bitrate produced. NOT uplink load: each
                                    * transport adds framing, and with two sinks live the
                                    * same video leaves the device twice. */
    uint32_t enc_max_frame_bytes;  /* Largest encoded frame in the window, i.e. a keyframe */
} video_capture_live_stats_t;

/**
 * @brief Snapshot the measured capture/encode rates
 *
 * Zeroes any field whose first window has not closed yet, so a caller may display it
 * immediately after boot.
 *
 * Implemented by the ESP32-P4 CSI grabber. Other capture backends do not define it.
 *
 * @param[out] out Filled in on success
 * @return ESP_OK or ESP_ERR_INVALID_ARG
 */
esp_err_t video_capture_get_live_stats(video_capture_live_stats_t *out);

/**
 * @brief Video frame buffer structure
 */
typedef struct {
    uint8_t *buffer;        /* Data buffer */
    uint32_t len;           /* Buffer length in bytes */
    uint64_t timestamp;     /* Timestamp in microseconds */
    video_frame_type_t type; /* Frame type */
} video_frame_t;

/**
 * @brief Video capture handle
 */
typedef void* video_capture_handle_t;

/**
 * @brief Initialize video capture with specified configuration
 *
 * @param config Video capture configuration
 * @param ret_handle Pointer to store the created handle
 * @return esp_err_t ESP_OK on success, otherwise an error code
 */
esp_err_t video_capture_init(video_capture_config_t *config, video_capture_handle_t *ret_handle);

/**
 * @brief Start video capture
 *
 * @param handle Video capture handle
 * @return esp_err_t ESP_OK on success, otherwise an error code
 */
esp_err_t video_capture_start(video_capture_handle_t handle);

/**
 * @brief Stop video capture
 *
 * @param handle Video capture handle
 * @return esp_err_t ESP_OK on success, otherwise an error code
 */
esp_err_t video_capture_stop(video_capture_handle_t handle);

/**
 * @brief Get the next captured video frame (MJPEG only - deprecated)
 *
 * @deprecated For H.264 this returns ESP_ERR_NOT_SUPPORTED. Encoded frames are
 *             delivered through the sink registry instead.
 *
 *             Still functional for MJPEG, which is a separate grabber with its
 *             own queue. That path is expected to move to sinks too, at which
 *             point this function goes away.
 *
 * @param handle Video capture handle
 * @param frame Pointer to store the video frame
 * @param wait_ms Time to wait for a frame in milliseconds (0 for non-blocking)
 * @return esp_err_t ESP_OK on success, ESP_ERR_TIMEOUT if no frame available,
 *                   ESP_ERR_NOT_SUPPORTED for an H.264 handle
 */
esp_err_t video_capture_get_frame(video_capture_handle_t handle, video_frame_t **frame, uint32_t wait_ms);

/**
 * @brief Set the video bitrate dynamically
 *
 * @param handle Video capture handle
 * @param bitrate_kbps New bitrate in kbps
 * @return esp_err_t ESP_OK on success, otherwise an error code
 */
esp_err_t video_capture_set_bitrate(video_capture_handle_t handle, uint32_t bitrate_kbps);

/**
 * @brief Get the current video bitrate
 *
 * @param handle Video capture handle
 * @param bitrate_kbps Pointer to store current bitrate in kbps
 * @return esp_err_t ESP_OK on success, otherwise an error code
 */
esp_err_t video_capture_get_bitrate(video_capture_handle_t handle, uint32_t *bitrate_kbps);

/**
 * @brief Request the encoder emit a keyframe (IDR) as soon as possible
 *
 * One-shot action, typically driven by a Picture Loss Indication (PLI) from the
 * remote peer so a stalled decoder can resync without waiting for the periodic GOP.
 *
 * @return esp_err_t ESP_OK on success, ESP_ERR_NOT_SUPPORTED when the source's GOP
 *         cannot be forced (UVC H.264 passthrough; wait for its own keyframe),
 *         otherwise an error code
 */
esp_err_t video_capture_request_keyframe(void);

/**
 * @brief Release a video frame when no longer needed (MJPEG only - deprecated)
 *
 * @deprecated Counterpart to video_capture_get_frame(); see its note.
 *
 * @param handle Video capture handle
 * @param frame Frame to release
 * @return esp_err_t ESP_OK on success, otherwise an error code
 */
esp_err_t video_capture_release_frame(video_capture_handle_t handle, video_frame_t *frame);

/**
 * @brief Deinitialize video capture and free resources
 *
 * @param handle Video capture handle
 * @return esp_err_t ESP_OK on success, otherwise an error code
 */
esp_err_t video_capture_deinit(video_capture_handle_t handle);

/**
 * @brief Capture a single JPEG snapshot
 *
 * When H264 streaming is active, intercepts the next raw frame from the
 * encoder pipeline and JPEG-encodes it (no camera sharing issues).
 * When streaming is NOT active but camera is initialized, directly grabs
 * a raw frame from the camera and JPEG-encodes it.
 * When camera is not initialized, temporarily initializes it, captures
 * one frame, encodes, and cleans up.
 *
 * Caller must free the returned buffer with video_capture_snapshot_free().
 *
 * @param[out] jpeg_buf  Pointer to store the allocated JPEG buffer
 * @param[out] jpeg_len  Pointer to store the JPEG data length
 * @param quality        JPEG quality (1-100, higher is better)
 * @param timeout_ms     Maximum time to wait for a frame in milliseconds
 * @return esp_err_t ESP_OK on success, otherwise an error code
 */
esp_err_t video_capture_get_snapshot(uint8_t **jpeg_buf, size_t *jpeg_len,
                                     uint8_t quality, uint32_t timeout_ms);

/**
 * @brief Capture a JPEG snapshot, optionally downscaled to a target resolution
 *
 * Same as video_capture_get_snapshot() but scales the captured frame down to
 * out_width x out_height before JPEG encoding. Downscale-only: if the request
 * is 0 or does not fit within the raw frame (would upscale), the native frame
 * resolution is used unchanged.
 *
 * @param[out] jpeg_buf   Pointer to store the allocated JPEG buffer
 * @param[out] jpeg_len   Pointer to store the JPEG data length
 * @param quality         JPEG quality (1-100, higher is better)
 * @param out_width       Target width in px, or 0 for native
 * @param out_height      Target height in px, or 0 for native
 * @param timeout_ms      Maximum time to wait for a frame in milliseconds
 * @return esp_err_t ESP_OK on success, otherwise an error code
 */
esp_err_t video_capture_get_snapshot_scaled(uint8_t **jpeg_buf, size_t *jpeg_len,
                                            uint8_t quality, uint16_t out_width,
                                            uint16_t out_height, uint32_t timeout_ms);

/**
 * @brief Free a JPEG snapshot buffer allocated by video_capture_get_snapshot()
 *
 * @param jpeg_buf Pointer to the JPEG buffer to free
 */
void video_capture_snapshot_free(uint8_t *jpeg_buf);

/**
 * @brief Check if the H264 encoder is currently running (actively encoding)
 *
 * @return true if the encoder task is running, false otherwise
 */
bool h264_encoder_is_running(void);

/**
 * @brief Check if the H264 encoder has been initialized
 *
 * @return true if initialized, false otherwise
 */
bool h264_encoder_is_initialized(void);

#ifdef __cplusplus
}
#endif
