/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @brief C-callable wrapper over esp-dl's PedestrianDetect.
 *
 * esp-dl is C++20 and its result type is a std::list of objects holding std::vector. None of that
 * can cross into a C translation unit, so this header exposes plain PODs and the C++ stays behind
 * person_detect.cpp. app_main and the sink code remain C.
 */

#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Most boxes we will ever hand back in one call. The model's own top_k is 10. */
#define PERSON_DETECT_MAX_BOXES 10

/**
 * @brief One detection, in the coordinate space of the frame that was passed in
 */
typedef struct {
    int   x1, y1, x2, y2;
    float score;
} person_box_t;

/**
 * @brief Load the model and get ready to run
 *
 * Loads eagerly rather than on the first inference: esp-dl defaults to lazy loading, and deferring
 * a ~435 KB load until the pipeline is already running fragments PSRAM at the worst moment.
 *
 * @param score_thr Detection score threshold, 0..1. 0 keeps the model default (0.7)
 * @return ESP_OK or ESP_FAIL
 */
esp_err_t person_detect_init(float score_thr);

/**
 * @brief Run one inference
 *
 * The model does its OWN preprocessing - resize, colour convert and quantize - so pass the frame at
 * whatever size it arrives. Note it stretches rather than letterboxes, so a non-square frame is
 * distorted; the converter upstream crops to square precisely to avoid that.
 *
 * @param rgb565     Tightly packed RGB565 little-endian pixels
 * @param width      Frame width
 * @param height     Frame height
 * @param out        Receives the detections
 * @param max_boxes  Capacity of @p out
 * @return Number of boxes written, or -1 on error
 */
int person_detect_run(const void *rgb565, int width, int height,
                      person_box_t *out, int max_boxes);

/**
 * @brief Free the model
 */
void person_detect_deinit(void);

#ifdef __cplusplus
}
#endif
