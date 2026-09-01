/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Called in the recovery task just before the ESP-Hosted transport is torn
 * down, so the app can stop whatever rides on it (Wi-Fi / netif).
 * Return anything but ESP_OK to mark the recovery cycle failed. */
typedef esp_err_t (*hosted_recovery_teardown_cb_t)(void);

/* Called after the transport is re-established, so the app can bring its
 * network back up and re-announce readiness to the co-processor.
 * Return anything but ESP_OK (e.g. the network never came back) to mark the
 * cycle failed, which schedules a backed-off retry. */
typedef esp_err_t (*hosted_recovery_restore_cb_t)(void);

/* Start co-processor error detection + recovery.
 *
 * Detects a C6 reboot (a second ESP_HOSTED_EVENT_CP_INIT), a hang (heartbeat
 * timeout) or a transport failure, and — per CONFIG_STREAMING_HOSTED_RECOVERY_* —
 * re-establishes the ESP-Hosted transport (default), resets the host, or does
 * nothing. On the default path the transport bounce re-fires TRANSPORT_UP, which
 * re-arms the webrtc_bridge RX; teardown()/restore() handle the app's network.
 *
 * A cycle that fails (any step or callback returning an error) is retried with
 * backoff, so a co-processor that stays dead through one attempt does not leave
 * the link down for good.
 *
 * Call AFTER the initial esp_hosted_init()/connect and Wi-Fi bring-up. */
esp_err_t hosted_recovery_start(hosted_recovery_teardown_cb_t teardown,
                                hosted_recovery_restore_cb_t restore);

#ifdef __cplusplus
}
#endif
