/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Header file for ICE server bridge client (streaming-only side)
 *
 * This component handles ICE server requests via bridge communication
 * from the streaming_only device to the signaling_only device.
 *
 * Async model:
 * 1. Request credentials from signaling (ice_bridge_client_request_ice_servers_async)
 * 2. Receive answers as and when signaling sends them (via callback)
 * 3. Use credentials when received (get cached, or callback delivers to app)
 */

#ifndef __ICE_BRIDGE_CLIENT_H__
#define __ICE_BRIDGE_CLIENT_H__

#include "app_webrtc_if.h"
#include "signaling_serializer.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Callback when ICE servers are updated (new servers received from signaling)
 *
 * @param count  Current number of ICE servers in cache
 * @param user_data  User context passed at registration
 */
typedef void (*ice_bridge_servers_updated_cb_t)(uint32_t count, void *user_data);

/**
 * @brief Request ICE servers from signaling asynchronously
 *
 * Sends a request to C6 via signaling channel; responses arrive as
 * SIGNALING_MSG_TYPE_ICE_SERVER_RESPONSE and are added to cache.
 * Each new server triggers the registered servers_updated callback.
 * Non-blocking - returns immediately.
 *
 * @return WEBRTC_STATUS_SUCCESS if request was sent, error otherwise
 */
WEBRTC_STATUS ice_bridge_client_request_ice_servers_async(void);

/**
 * @brief Get cached ICE servers (populated by async responses)
 *
 * @param pIceServers  Output array (ss_ice_server_t layout)
 * @param pServerNum   Output count
 * @return WEBRTC_STATUS_SUCCESS on success
 */
WEBRTC_STATUS ice_bridge_client_get_cached_servers(void *pIceServers, uint32_t *pServerNum);

/**
 * @brief Clear ICE server cache (e.g., before refresh to force fresh fetch)
 */
void ice_bridge_client_clear_cache(void);

/**
 * @brief Set callback for when new ICE servers arrive from signaling
 *
 * Called each time the cache is updated (new server added).
 *
 * @param callback   Callback to invoke (can be NULL to clear)
 * @param user_data  User context for callback
 */
void ice_bridge_client_set_servers_updated_callback(ice_bridge_servers_updated_cb_t callback, void *user_data);

/**
 * @brief Callback for ICE server response from signaling channel
 *
 * Called when SIGNALING_MSG_TYPE_ICE_SERVER_RESPONSE is received via webrtc_bridge.
 * Adds server to cache and invokes servers_updated callback.
 *
 * @param ice_server_response  Single ICE server response
 */
void ice_bridge_client_set_ice_server_response(const ss_ice_server_response_t *ice_server_response);

#if CONFIG_ESP_WEBRTC_BRIDGE_HOSTED
/**
 * @brief Initialize ICE bridge client (cache mutex, etc.)
 *
 * ICE exchange uses the signaling channel; no bridge_cmd.
 * Idempotent - safe to call multiple times.
 */
void ice_bridge_client_init(void);
#endif

/**
 * @brief Legacy sync get - returns cached servers (or STUN fallback) and triggers async fetch if empty
 *
 * For backward compatibility during transition. Prefer request_ice_servers_async + get_cached.
 */
WEBRTC_STATUS ice_bridge_client_get_servers(void *pAppSignaling, void *pIceServers, uint32_t *pServerNum);

#ifdef __cplusplus
}
#endif

#endif /* __ICE_BRIDGE_CLIENT_H__ */
