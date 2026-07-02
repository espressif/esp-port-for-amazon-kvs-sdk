/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Implementation of ICE server bridge client for streaming-only mode
 *
 * Async model:
 * 1. Request credentials from signaling (ice_bridge_client_request_ice_servers_async)
 * 2. Receive answers via signaling channel (SIGNALING_MSG_TYPE_ICE_SERVER_RESPONSE over webrtc_bridge)
 * 3. Use credentials when received (get_cached_servers, servers_updated callback)
 *
 * ICE exchange uses the signaling channel, not bridge_cmd.
 */

#include "ice_bridge_client.h"
#include "signaling_serializer.h"
#include "webrtc_bridge.h"
#include "app_webrtc_if.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>

#define LOG_CLASS "ice_bridge_client"
static const char *TAG = "ice_bridge_client";

#define ICE_CACHE_MAX APP_WEBRTC_MAX_ICE_SERVERS_COUNT

/* Constants from signaling_serializer (matching ss_ice_server_t layout) */
#define MAX_ICE_CONFIG_URI_LEN SS_MAX_ICE_CONFIG_URI_LEN
#define MAX_ICE_CONFIG_USER_NAME_LEN SS_MAX_ICE_CONFIG_USER_NAME_LEN
#define MAX_ICE_CONFIG_CREDENTIAL_LEN SS_MAX_ICE_CONFIG_CREDENTIAL_LEN

#if CONFIG_ESP_WEBRTC_BRIDGE_HOSTED
static void send_next_ice_request(void);

/* ICE server cache (populated asynchronously) */
static ss_ice_server_t s_cache[ICE_CACHE_MAX];
static uint32_t s_cache_count = 0;
static SemaphoreHandle_t s_cache_mutex = NULL;

/* Callback when new servers arrive */
static ice_bridge_servers_updated_cb_t s_servers_updated_cb = NULL;
static void *s_servers_updated_user_data = NULL;

/* Async request state: chain of index-based requests when have_more */
static volatile bool s_async_in_progress = false;
static volatile uint32_t s_next_index = 0;
static volatile bool s_initialized = false;
#endif

/**
 * @brief Add a single ICE server to cache (mutex must be held by caller)
 *
 * Does NOT invoke callback - caller must release mutex before invoking callback
 * so the callback can call get_cached_servers without deadlock.
 */
static void add_server_to_cache(const ss_ice_server_response_t *resp)
{
#if CONFIG_ESP_WEBRTC_BRIDGE_HOSTED
    if (resp == NULL || resp->urls[0] == '\0') {
        return;
    }
    if (s_cache_count >= ICE_CACHE_MAX) {
        ESP_LOGW(TAG, "ICE cache full, dropping server: %s", resp->urls);
        return;
    }

    ss_ice_server_t *entry = &s_cache[s_cache_count];
    strncpy(entry->urls, resp->urls, MAX_ICE_CONFIG_URI_LEN);
    entry->urls[MAX_ICE_CONFIG_URI_LEN] = '\0';
    strncpy(entry->username, resp->username, MAX_ICE_CONFIG_USER_NAME_LEN);
    entry->username[MAX_ICE_CONFIG_USER_NAME_LEN] = '\0';
    strncpy(entry->credential, resp->credential, MAX_ICE_CONFIG_CREDENTIAL_LEN);
    entry->credential[MAX_ICE_CONFIG_CREDENTIAL_LEN] = '\0';

    s_cache_count++;
    ESP_LOGI(TAG, "Added ICE server %" PRIu32 ": %s", s_cache_count - 1, entry->urls);
#else
    (void)resp;
#endif
}

/**
 * @brief Callback for ICE server response from signaling channel
 *
 * Called when SIGNALING_MSG_TYPE_ICE_SERVER_RESPONSE is received via webrtc_bridge.
 */
void ice_bridge_client_set_ice_server_response(const ss_ice_server_response_t *ice_server_response)
{
    if (ice_server_response == NULL) {
        ESP_LOGE(TAG, "ice_bridge_client_set_ice_server_response called with NULL");
        return;
    }

    ESP_LOGI(TAG, "ICE server response from signaling: %s (have_more: %s)",
             ice_server_response->urls, ice_server_response->have_more ? "true" : "false");

#if CONFIG_ESP_WEBRTC_BRIDGE_HOSTED
    if (s_cache_mutex && xSemaphoreTake(s_cache_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        add_server_to_cache(ice_server_response);
        uint32_t count_after = s_cache_count;
        ice_bridge_servers_updated_cb_t cb = s_servers_updated_cb;
        void *ud = s_servers_updated_user_data;
        xSemaphoreGive(s_cache_mutex);
        if (cb && count_after > 0) {
            cb(count_after, ud);
        }
    }

    if (ice_server_response->have_more && s_cache_count < ICE_CACHE_MAX && s_async_in_progress) {
        s_next_index++;
        send_next_ice_request();
    } else if (s_async_in_progress) {
        s_async_in_progress = false;
    }
#endif
}

#if CONFIG_ESP_WEBRTC_BRIDGE_HOSTED
/**
 * @brief Send next ICE server request via signaling channel (SIGNALING_MSG_TYPE_ICE_REQUEST)
 */
static void send_next_ice_request(void)
{
    signaling_msg_t req_msg = {0};
    if (create_ice_request_message(s_next_index, true, &req_msg) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create ICE request for index %" PRIu32, s_next_index);
        s_async_in_progress = false;
        return;
    }

    size_t serialized_len = 0;
    char *serialized = serialize_signaling_message(&req_msg, &serialized_len);
    if (req_msg.payload) {
        free(req_msg.payload);
    }
    if (!serialized) {
        ESP_LOGE(TAG, "Failed to serialize ICE request for index %" PRIu32, s_next_index);
        s_async_in_progress = false;
        return;
    }

    webrtc_bridge_send_message(serialized, serialized_len);
    ESP_LOGI(TAG, "Requested ICE server index %" PRIu32 " via signaling channel", s_next_index);
}

void ice_bridge_client_init(void)
{
    if (s_initialized) {
        return;
    }

    s_cache_mutex = xSemaphoreCreateMutex();
    if (s_cache_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create cache mutex");
        return;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "ICE bridge client initialized (signaling channel)");
}
#endif

WEBRTC_STATUS ice_bridge_client_request_ice_servers_async(void)
{
#if CONFIG_ESP_WEBRTC_BRIDGE_HOSTED
    if (!s_initialized || s_cache_mutex == NULL) {
        ESP_LOGE(TAG, "ICE bridge client not initialized - call ice_bridge_client_init()");
        return WEBRTC_STATUS_INVALID_OPERATION;
    }

    if (s_async_in_progress) {
        ESP_LOGD(TAG, "ICE async request already in progress");
        return WEBRTC_STATUS_SUCCESS;
    }

    s_async_in_progress = true;
    s_next_index = 0;
    send_next_ice_request();
    return WEBRTC_STATUS_SUCCESS;
#else
    return WEBRTC_STATUS_NOT_IMPLEMENTED;
#endif
}

WEBRTC_STATUS ice_bridge_client_get_cached_servers(void *pIceServers, uint32_t *pServerNum)
{
    if (pIceServers == NULL || pServerNum == NULL) {
        return WEBRTC_STATUS_NULL_ARG;
    }

#if CONFIG_ESP_WEBRTC_BRIDGE_HOSTED
    if (s_cache_mutex == NULL) {
        *pServerNum = 0;
        return WEBRTC_STATUS_SUCCESS;
    }

    if (xSemaphoreTake(s_cache_mutex, pdMS_TO_TICKS(500)) != pdTRUE) {
        *pServerNum = 0;
        return WEBRTC_STATUS_INTERNAL_ERROR;
    }

    uint32_t n = (s_cache_count < ICE_CACHE_MAX) ? s_cache_count : ICE_CACHE_MAX;
    memcpy(pIceServers, s_cache, n * sizeof(ss_ice_server_t));
    *pServerNum = n;
    xSemaphoreGive(s_cache_mutex);
    return WEBRTC_STATUS_SUCCESS;
#else
    *pServerNum = 0;
    return WEBRTC_STATUS_SUCCESS;
#endif
}

void ice_bridge_client_set_servers_updated_callback(ice_bridge_servers_updated_cb_t callback, void *user_data)
{
#if CONFIG_ESP_WEBRTC_BRIDGE_HOSTED
    s_servers_updated_cb = callback;
    s_servers_updated_user_data = user_data;
#else
    (void)callback;
    (void)user_data;
#endif
}

void ice_bridge_client_clear_cache(void)
{
#if CONFIG_ESP_WEBRTC_BRIDGE_HOSTED
    if (s_cache_mutex && xSemaphoreTake(s_cache_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
        s_cache_count = 0;
        xSemaphoreGive(s_cache_mutex);
    }
#endif
}

WEBRTC_STATUS ice_bridge_client_get_servers(void *pAppSignaling, void *pIceServers, uint32_t *pServerNum)
{
    (void)pAppSignaling;

    if (pIceServers == NULL || pServerNum == NULL) {
        return WEBRTC_STATUS_NULL_ARG;
    }

    ss_ice_server_t *ice_servers_array = (ss_ice_server_t *)pIceServers;

#if CONFIG_ESP_WEBRTC_BRIDGE_HOSTED
    WEBRTC_STATUS st = ice_bridge_client_get_cached_servers(pIceServers, pServerNum);
    if (st != WEBRTC_STATUS_SUCCESS) {
        *pServerNum = 0;
    }

    if (*pServerNum == 0) {
        ESP_LOGW(TAG, "No cached ICE servers - using STUN fallback and triggering async request");
        snprintf(ice_servers_array[0].urls, MAX_ICE_CONFIG_URI_LEN, "%s", APP_WEBRTC_DEFAULT_STUN_SERVER);
        ice_servers_array[0].urls[MAX_ICE_CONFIG_URI_LEN] = '\0';
        ice_servers_array[0].username[0] = '\0';
        ice_servers_array[0].credential[0] = '\0';
        *pServerNum = 1;

        ice_bridge_client_request_ice_servers_async();
    }

    ESP_LOGI(TAG, "Returning %" PRIu32 " ICE servers", *pServerNum);
    return WEBRTC_STATUS_SUCCESS;
#else
    ESP_LOGW(TAG, "Hosted bridge not enabled - using STUN fallback");
    snprintf(ice_servers_array[0].urls, MAX_ICE_CONFIG_URI_LEN, "%s", APP_WEBRTC_DEFAULT_STUN_SERVER);
    ice_servers_array[0].urls[MAX_ICE_CONFIG_URI_LEN] = '\0';
    ice_servers_array[0].username[0] = '\0';
    ice_servers_array[0].credential[0] = '\0';
    *pServerNum = 1;
    return WEBRTC_STATUS_SUCCESS;
#endif
}
