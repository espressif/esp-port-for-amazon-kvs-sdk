/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Configuration for app_wifi_prov_init()
 */
typedef struct {
    /** Timeout in ms to wait for WiFi connection. 0 = don't wait. Default: 15000 */
    uint32_t wifi_connect_timeout_ms;

    /**
     * Optional callback invoked after esp_netif_init() + esp_event_loop_create_default()
     * but BEFORE esp_wifi_init(). Use this to set up custom netif or coprocessor.
     * Return ESP_OK to continue, or an error to abort.
     */
    esp_err_t (*pre_wifi_init_cb)(void *user_ctx);
    void *pre_wifi_init_user_ctx;

    /** If true, skip creating default WiFi STA netif (caller creates their own via callback) */
    bool skip_default_sta_netif;

    /**
     * Optional callbacks invoked before/after BLE provisioning.
     * Use these on platforms where BLE runs on a coprocessor (e.g. ESP32-P4 + C6)
     * to init/deinit the remote BT controller via esp_hosted.
     * Only called when CONFIG_APP_NETWORK_PROV_BLE is enabled and provisioning runs.
     */
    esp_err_t (*prov_start_cb)(void *user_ctx);  /**< Called before BLE provisioning starts */
    void (*prov_end_cb)(void *user_ctx);          /**< Called after BLE provisioning ends */
    void *prov_cb_user_ctx;
} app_wifi_prov_config_t;

#define APP_NETWORK_CONFIG_DEFAULT() {  \
    .wifi_connect_timeout_ms = 15000,   \
    .pre_wifi_init_cb = NULL,           \
    .pre_wifi_init_user_ctx = NULL,     \
    .skip_default_sta_netif = false,    \
    .prov_start_cb = NULL,              \
    .prov_end_cb = NULL,                \
    .prov_cb_user_ctx = NULL,           \
}

/**
 * @brief Initialize WiFi with optional BLE provisioning
 *
 * If CONFIG_APP_NETWORK_PROV_BLE is enabled and device is not yet provisioned,
 * starts BLE provisioning and blocks until credentials are received.
 * Otherwise, uses Kconfig SSID/password or previously stored credentials.
 *
 * Prerequisites: NVS must be initialized before calling this function.
 *
 * @param config Network configuration (use APP_NETWORK_CONFIG_DEFAULT() for defaults)
 * @return ESP_OK on success
 */
esp_err_t app_wifi_prov_init(const app_wifi_prov_config_t *config);

/**
 * @brief Wait until WiFi is connected (or timeout)
 *
 * @param timeout_ms Maximum time to wait in milliseconds. 0 = wait indefinitely.
 * @return ESP_OK if connected, ESP_ERR_TIMEOUT on timeout
 */
esp_err_t app_wifi_prov_wait_for_connection(uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
