/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @brief The inference consumer: a raw video sink that runs person detection.
 *
 * Registers a VIDEO_RAW_MODE_CONVERTED sink, so the PPA hands it a small RGB565 frame at the
 * model's input size and the camera buffer goes straight back to the driver. That is what lets a
 * ~60 ms inference run without costing the H.264 encoder a single frame.
 *
 * Results are published into a snapshot the display reads whenever it likes. The display therefore
 * shows the most recent detection available, which generally belongs to an earlier frame than the
 * one on screen - that lag is inherent to running the two asynchronously and is the point of the
 * design, not a defect.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "person_detect.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief A copy of the most recent detection results
 */
typedef struct {
    person_box_t boxes[PERSON_DETECT_MAX_BOXES];
    int          count;
    uint16_t     src_width;   /* Coordinate space the boxes are in */
    uint16_t     src_height;
    uint32_t     seq;         /* Capture sequence of the frame they came from */
    uint32_t     infer_us;    /* How long that inference took */
    uint32_t     inferences;  /* Completed inferences since boot. Differencing THIS gives
                               * the detector's frame rate; differencing seq would give the
                               * camera's, since frames the detector skipped still advance
                               * it. */
} detect_results_t;

/**
 * @brief Load the model, register the sink and start the inference task
 *
 * Safe to call before the camera is running - the sink is registered either way and starts
 * receiving once capture begins.
 */
esp_err_t detect_sink_init(void);

/**
 * @brief Start or stop delivering frames to the inference sink
 */
esp_err_t detect_sink_set_enabled(bool enabled);

/**
 * @brief Whether the sink is currently enabled
 */
bool detect_sink_is_enabled(void);

/**
 * @brief Copy out the latest results
 *
 * Never blocks on inference; returns the last completed set.
 *
 * @param out Receives the snapshot
 * @return ESP_OK, or ESP_ERR_INVALID_STATE before the first inference completes
 */
esp_err_t detect_sink_get_results(detect_results_t *out);

#ifdef __cplusplus
}
#endif
