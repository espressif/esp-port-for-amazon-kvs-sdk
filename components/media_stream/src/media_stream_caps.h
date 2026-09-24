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

/* Targets with the esp_video V4L2 capture stack (esp_video_if.c and the grabber). */
#if CONFIG_IDF_TARGET_ESP32P4
#define MEDIA_STREAM_HAS_ESP_VIDEO_CAPTURE   1
#else
#define MEDIA_STREAM_HAS_ESP_VIDEO_CAPTURE   0
#endif

/* Targets where espressif/esp_image_effects is available - it backs the JPEG
 * snapshot and scaling path. Same rule as the esp_image_effects dependency in
 * ../idf_component.yml. */
#if CONFIG_IDF_TARGET_ESP32P4
#define MEDIA_STREAM_HAS_IMG_EFFECTS         1
#else
#define MEDIA_STREAM_HAS_IMG_EFFECTS         0
#endif

/* Targets with the H.264 hardware encoder, i.e. where espressif/esp_h264 is
 * available. Keep in lockstep with the esp_h264 rule in ../idf_component.yml. */
#if CONFIG_IDF_TARGET_ESP32P4
#define MEDIA_STREAM_HAS_HW_H264_ENC         1
#else
#define MEDIA_STREAM_HAS_HW_H264_ENC         0
#endif

/* Targets with the PPA (2D pixel-processing accelerator). It backs the hardware
 * colour-convert/scale behind VIDEO_RAW_MODE_CONVERTED. Unlike the three above,
 * this one is a real SoC feature test rather than a target list. */
#if CONFIG_SOC_PPA_SUPPORTED
#define MEDIA_STREAM_HAS_PPA                 1
#else
#define MEDIA_STREAM_HAS_PPA                 0
#endif
