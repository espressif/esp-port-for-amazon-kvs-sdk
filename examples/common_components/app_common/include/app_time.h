/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "sdkconfig.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

/**
 * @brief Sync time with SNTP server and wait for the time to be set.
 *
 * This function will initialize the SNTP client and start the time synchronization process.
 * It will wait for the time to be set before returning.
 */
void app_time_sync_and_wait(void);

/**
 * @brief Sync time with SNTP server without waiting for the time to be set.
 *
 * This function will initialize the SNTP client and start the time synchronization process.
 * However, it will not wait for the time to be set.
 */
void app_time_sync_no_wait(void);

#if CONFIG_ESP_WEBRTC_BRIDGE_HOSTED
/**
 * @brief Set the system time from the co-processor, over the webrtc_bridge.
 *
 * For the streaming host of a split pair (the P4 in streaming_only), which has
 * no SNTP of its own. The co-processor syncs over SNTP and answers
 * BRIDGE_CMD_GET_TIME. The result is corrected for half the round trip.
 *
 * Not called by default: WebRTC on the host does not need wall-clock time.
 * Call it when the host does, for example to timestamp recordings:
 *
 * @code
 * bridge_cmd_init();
 * app_time_sync_from_coprocessor();
 * @endcode
 *
 * Built only when the project includes webrtc_bridge with the hosted bridge.
 * Blocks for up to about 35 s: 5 tries, each a 5 s timeout plus 2 s apart.
 *
 * @return ESP_OK when the time is set, ESP_ERR_TIMEOUT when the co-processor
 *         gives no valid time, ESP_FAIL when the response handler cannot be
 *         registered.
 */
esp_err_t app_time_sync_from_coprocessor(void);
#endif

#ifdef __cplusplus
}
#endif
