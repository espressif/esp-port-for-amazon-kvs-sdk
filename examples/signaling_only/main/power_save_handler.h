/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __POWER_SAVE_HANDLER_H__
#define __POWER_SAVE_HANDLER_H__

#include "esp_err.h"

/**
 * @brief Initialize power save handling for split mode operation
 *
 * This function:
 * - Initializes host power save monitoring with callbacks
 * - Initializes slave light sleep component
 * - Registers WebRTC event handler for waking host on offer/streaming events
 *
 * Should be called after esp_hosted_coprocessor_init() and before app_webrtc_init()
 *
 * @return
 *     - ESP_OK: Success
 *     - ESP_FAIL: Initialization failed
 */
esp_err_t power_save_init(void);

/**
 * @brief Register sleep-related CLI commands
 *
 * Registers the "wake-up" command to manually wake the host from deep sleep.
 *
 * @return 0 on success
 */
int power_save_register_cli(void);

#endif /* __POWER_SAVE_HANDLER_H__ */
