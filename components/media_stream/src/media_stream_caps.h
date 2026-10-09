/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @brief Target capability macros for the capture pipeline.
 */

#pragma once

#include "sdkconfig.h"

/* Targets with the esp_video V4L2 capture stack (esp_video_if.c, the capture pump and the
 * raw-frame bus): MIPI-CSI/UVC on the P4, DVP on the S3. */
#if CONFIG_IDF_TARGET_ESP32P4 || CONFIG_IDF_TARGET_ESP32S3
#define MEDIA_STREAM_HAS_ESP_VIDEO_CAPTURE   1
#else
#define MEDIA_STREAM_HAS_ESP_VIDEO_CAPTURE   0
#endif

/* Targets where espressif/esp_image_effects is a dependency: the JPEG snapshot on the
 * P4, the frame converter on the S3. Keep in lockstep with the esp_image_effects rule
 * in ../idf_component.yml. */
#if CONFIG_IDF_TARGET_ESP32P4 || CONFIG_IDF_TARGET_ESP32S3
#define MEDIA_STREAM_HAS_IMG_EFFECTS         1
#else
#define MEDIA_STREAM_HAS_IMG_EFFECTS         0
#endif

/* The JPEG snapshot: esp_image_effects for the colour conversion and scaling, the
 * hardware JPEG encoder for the rest. */
#if MEDIA_STREAM_HAS_IMG_EFFECTS && CONFIG_SOC_JPEG_CODEC_SUPPORTED
#define MEDIA_STREAM_HAS_JPEG_SNAPSHOT       1
#else
#define MEDIA_STREAM_HAS_JPEG_SNAPSHOT       0
#endif

/* Targets with the H.264 hardware encoder. */
#if CONFIG_IDF_TARGET_ESP32P4
#define MEDIA_STREAM_HAS_HW_H264_ENC         1
#else
#define MEDIA_STREAM_HAS_HW_H264_ENC         0
#endif

/* Targets where espressif/esp_h264 is a dependency at all, hardware encoder or
 * not. Keep in lockstep with the esp_h264 rule in ../idf_component.yml. */
#if CONFIG_IDF_TARGET_ESP32P4 || CONFIG_IDF_TARGET_ESP32S3
#define MEDIA_STREAM_HAS_ESP_H264            1
#else
#define MEDIA_STREAM_HAS_ESP_H264            0
#endif

/* esp_h264 without the hardware block: the capture pump's encoder sink runs the
 * software (openh264) encoder instead. */
#define MEDIA_STREAM_HAS_SW_H264_ENC         (MEDIA_STREAM_HAS_ESP_H264 && !MEDIA_STREAM_HAS_HW_H264_ENC)

/* Targets with the PPA (2D pixel-processing accelerator). It backs the hardware
 * colour-convert/scale behind VIDEO_RAW_MODE_CONVERTED. Unlike the target lists above,
 * this one is a real SoC feature test rather than a target list. */
#if CONFIG_SOC_PPA_SUPPORTED
#define MEDIA_STREAM_HAS_PPA                 1
#else
#define MEDIA_STREAM_HAS_PPA                 0
#endif
