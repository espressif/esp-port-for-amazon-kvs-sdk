/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

/* Role-neutral peer-data calls: esp_hosted names them differently per role.
 * send blocks until the peer answers; the receive callback borrows data.
 * Host receive runs on the event loop task, so it must not call send. */

#include <stdint.h>
#include <stddef.h>

#include "esp_err.h"
#include "sdkconfig.h"

/* TEST_STUB: test_app supplies both functions. The feature headers stay on the
 * include path even when unbuilt, so check the symbol here, not at link time. */
#if defined(BRIDGE_PEER_DATA_TEST_STUB)
#elif CONFIG_ESP_HOSTED_CP
#if !CONFIG_ESP_HOSTED_CP_FEAT_PEER_DATA
#error "the bridge carries signaling over hosted peer data; enable CONFIG_ESP_HOSTED_CP_FEAT_PEER_DATA."
#endif
#include "eh_cp_feat_peer_data.h"
#elif CONFIG_ESP_HOSTED_HOST
#if !CONFIG_ESP_HOSTED_HOST_FEAT_PEER_DATA
#error "the bridge carries signaling over hosted peer data; enable CONFIG_ESP_HOSTED_HOST_FEAT_PEER_DATA."
#endif
#include "eh_host_feat_peer_data.h"
#else
#error "ESP_WEBRTC_BRIDGE_HOSTED needs a hosted role: set CONFIG_ESP_HOSTED=y and pick CONFIG_ESP_HOSTED_HOST or CONFIG_ESP_HOSTED_CP, or select the MQTT bridge instead."
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*bridge_peer_data_cb_t)(uint32_t msg_id, const uint8_t *data,
                                      size_t len, void *ctx);

#if defined(BRIDGE_PEER_DATA_TEST_STUB)
esp_err_t bridge_peer_data_send(uint32_t msg_id, const uint8_t *data, size_t len);
esp_err_t bridge_peer_data_register(uint32_t msg_id, bridge_peer_data_cb_t cb, void *ctx);
#else

static inline esp_err_t bridge_peer_data_send(uint32_t msg_id,
                                              const uint8_t *data, size_t len)
{
#if CONFIG_ESP_HOSTED_CP
    return eh_cp_feat_peer_data_send(msg_id, data, len);
#else
    return eh_host_peer_data_send(msg_id, data, len);
#endif
}

static inline esp_err_t bridge_peer_data_register(uint32_t msg_id,
                                                  bridge_peer_data_cb_t cb,
                                                  void *ctx)
{
#if CONFIG_ESP_HOSTED_CP
    return eh_cp_feat_peer_data_register_callback(msg_id, cb, ctx);
#else
    return eh_host_peer_data_register(msg_id, (eh_host_peer_data_cb_t)cb, ctx);
#endif
}

#endif /* BRIDGE_PEER_DATA_TEST_STUB */

#ifdef __cplusplus
}
#endif
