/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @brief LCD preview: the LOCAL camera stream with detection boxes drawn over it.
 *
 * Not the WebRTC receive path. media_stream's video player is off in this example; what reaches the
 * panel is a raw sink on the camera bus, hardware-converted to the panel's size by the PPA.
 *
 * Display bring-up goes through the BSP (bsp_display_start / bsp_display_lock), so this works on
 * any board bsp_selector resolves without knowing anything about the panel.
 */

#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Bring up the display and register the preview sink
 *
 * Safe to call before the camera is running.
 */
esp_err_t preview_display_init(void);

/**
 * @brief Start or stop delivering frames to the preview sink
 */
esp_err_t preview_display_set_enabled(bool enabled);

/**
 * @brief Show or hide the live pipeline-rate overlay
 *
 * Capture fps, encoder fps and kbps, detector fps and display fps, refreshed once a
 * second in the top-left corner. On by default: on a device with four consumers of one
 * camera, which one is falling behind is the first question, and the answer should not
 * require a serial console.
 */
esp_err_t preview_display_show_stats(bool show);

/**
 * @brief The detector and display rates the overlay is currently showing
 *
 * In TENTHS of a frame per second, because both run near 5 fps where whole frames per
 * second is too coarse to see a problem. Measured here rather than by the caller: these
 * two are the only rates in the pipeline that nothing else counts.
 *
 * Lets the console report the same numbers the LCD does, so a run without eyes on the
 * panel can still say what the panel said.
 */
esp_err_t preview_display_get_rates(uint16_t *det_dfps, uint16_t *lcd_dfps);

#ifdef __cplusplus
}
#endif
