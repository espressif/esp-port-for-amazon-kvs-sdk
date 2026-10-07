/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

/**
 * @file bridge_peer_data.h
 * @brief Role-neutral peer-data calls for the bridge.
 *
 * esp_hosted names the peer-data entry points differently per role, so the
 * bridge — which compiles for both — goes through these two wrappers. Argument
 * order, types and return values are identical on both sides; only the symbol
 * names differ.
 *
 * Semantics that hold for both roles:
 *  - send blocks until the peer answers, and returns the peer's status.
 *  - the receive callback borrows @c data. Copy anything you need to keep;
 *    do not free it.
 *  - msg_id 0xFFFFFFFF is reserved.
 *
 * Host receive runs on the default event loop task, so a callback must stay
 * short and must not call bridge_peer_data_send() — that would block the loop
 * until the peer replies. Hand work to a queue instead.
 */

#include <stdint.h>
#include <stddef.h>

#include "esp_err.h"
#include "sdkconfig.h"

/* BRIDGE_PEER_DATA_TEST_STUB lets the host test build exercise the chunking and
 * reassembly logic without a hosted role: the test supplies the two functions
 * below and captures what would have gone on the wire. Only test_app defines it. */
#if defined(BRIDGE_PEER_DATA_TEST_STUB)
/* provided by the test */
#elif CONFIG_ESP_HOSTED_CP
#include "eh_cp_feat_peer_data.h"
#elif CONFIG_ESP_HOSTED_HOST
#include "eh_host_feat_peer_data.h"
#else
#error "ESP_WEBRTC_BRIDGE_HOSTED needs a hosted role: set CONFIG_ESP_HOSTED=y and pick CONFIG_ESP_HOSTED_HOST or CONFIG_ESP_HOSTED_CP, or select the MQTT bridge instead."
#endif

#ifdef __cplusplus
extern "C" {
#endif

/** Receive callback. Same shape on both roles. */
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
