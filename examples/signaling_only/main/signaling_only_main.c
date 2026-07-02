/*
 * SPDX-FileCopyrightText: 2025-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "esp_cli.h"
#include "wifi_cli.h"

#include "app_storage.h"
#include "app_wifi_prov.h"
#include "kvs_signaling.h"

#if defined(CONFIG_IDF_TARGET_ESP32C6) || defined(CONFIG_IDF_TARGET_ESP32C5)
#define IS_VALID_SLAVE_CHIPSET 1
#else
#define IS_VALID_SLAVE_CHIPSET 0
#endif

#if IS_VALID_SLAVE_CHIPSET
#include "esp_hosted_coprocessor.h"
#endif

#include "esp_webrtc_time.h"
#include "app_webrtc.h"
#include "esp_work_queue.h"
#include "bridge_peer_connection.h"
#include "webrtc_bridge.h"
#include "power_save_handler.h"

static const char *TAG = "signaling_only";

extern int trigger_offer_command_register_cli(void);
extern int query_resolution_command_register_cli(void);
extern int query_resolution_command_register_response_handler(void);
extern int query_snapshot_command_register_cli(void);
extern int query_snapshot_command_register_response_handler(void);

// Global configuration - keep same structure as before for IoT Core compatibility
static kvs_signaling_config_t g_kvsSignalingConfig = {0};

/**
 * @brief Handle WebRTC application events
 */
static void app_webrtc_event_handler(app_webrtc_event_data_t *event_data, void *user_ctx)
{
    ESP_LOGI(TAG, "WebRTC Event: %d, Status: 0x%08" PRIx32 ", Peer: %s, Message: %s",
             event_data->event_id, event_data->status_code,
             event_data->peer_id, event_data->message);

    switch (event_data->event_id) {
        case APP_WEBRTC_EVENT_SIGNALING_CONNECTED:
            ESP_LOGI(TAG, "Signaling connected successfully");
            break;
        case APP_WEBRTC_EVENT_SIGNALING_GET_ICE:
            ESP_LOGI(TAG, "ICE servers fetched from AWS and ready for requests");
            break;
        case APP_WEBRTC_EVENT_SIGNALING_DISCONNECTED:
            ESP_LOGI(TAG, "Signaling disconnected");
            break;
        case APP_WEBRTC_EVENT_RECEIVED_OFFER:
            ESP_LOGI(TAG, "Received offer from peer %s", event_data->peer_id);
            break;
        case APP_WEBRTC_EVENT_SENT_ANSWER:
            ESP_LOGI(TAG, "Sent answer to peer %s", event_data->peer_id);
            break;
        case APP_WEBRTC_EVENT_RECEIVED_ICE_CANDIDATE:
            ESP_LOGI(TAG, "Received ICE candidate from peer %s", event_data->peer_id);
            break;
        case APP_WEBRTC_EVENT_SENT_ICE_CANDIDATE:
            ESP_LOGI(TAG, "Sent ICE candidate to peer %s", event_data->peer_id);
            break;
        case APP_WEBRTC_EVENT_ERROR:
        case APP_WEBRTC_EVENT_SIGNALING_ERROR:
            ESP_LOGE(TAG, "WebRTC error occurred: %s", event_data->message);
            break;
        default:
            ESP_LOGI(TAG, "Unhandled WebRTC event: %d", event_data->event_id);
            break;
    }
}

#if IS_VALID_SLAVE_CHIPSET
#include "esp_wifi.h"
#include "esp_netif.h"

#ifdef CONFIG_ESP_HOSTED_NETWORK_SPLIT_ENABLED
static esp_netif_t *sta_netif = NULL;

static esp_netif_t *create_slave_sta_netif(void)
{
    /* Create "almost" default station, but with un-flagged DHCP client */
    /* Use static to ensure the config persists after function returns */
    static esp_netif_inherent_config_t netif_cfg;
    memcpy(&netif_cfg, ESP_NETIF_BASE_DEFAULT_WIFI_STA, sizeof(netif_cfg));

    esp_netif_config_t cfg_sta = {
        .base = &netif_cfg,
        .stack = ESP_NETIF_NETSTACK_DEFAULT_WIFI_STA,
    };
    esp_netif_t *netif_sta = esp_netif_new(&cfg_sta);
    ESP_LOGI(TAG, "Created netif_sta: %p (if_key: %s)", netif_sta, netif_cfg.if_key);
    assert(netif_sta);

    ESP_ERROR_CHECK(esp_netif_attach_wifi_station(netif_sta));
    ESP_ERROR_CHECK(esp_wifi_set_default_wifi_sta_handlers());

    return netif_sta;
}
#endif

static esp_err_t signaling_pre_wifi_init(void *ctx)
{
#ifdef CONFIG_ESP_HOSTED_NETWORK_SPLIT_ENABLED
    /* Create and register the netif with "WIFI_STA_DEF" key */
    sta_netif = create_slave_sta_netif();
#endif
    /* esp_netif_init and netif creation must be done before network_coprocessor_init */
    esp_hosted_coprocessor_init();
    vTaskDelay(pdMS_TO_TICKS(5000));
    return ESP_OK;
}
#endif /* IS_VALID_SLAVE_CHIPSET */

void app_main(void)
{
    esp_err_t ret;
    WEBRTC_STATUS status = WEBRTC_STATUS_SUCCESS;

    // Initialize NVS
    ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_LOGI(TAG, "ESP32 WebRTC Signaling-Only Example");

    esp_cli_start();
    wifi_register_cli(); // for wifi-set command
    // power_save_register_cli();
    trigger_offer_command_register_cli();
    query_resolution_command_register_cli();
    query_snapshot_command_register_cli();

    // Initialize WiFi (with BLE provisioning if enabled)
    app_wifi_prov_config_t net_cfg = APP_NETWORK_CONFIG_DEFAULT();
#if IS_VALID_SLAVE_CHIPSET
    net_cfg.pre_wifi_init_cb = signaling_pre_wifi_init;
    net_cfg.skip_default_sta_netif = true;
#endif
    ret = app_wifi_prov_init(&net_cfg);
    if (ret == ESP_ERR_TIMEOUT) {
        /* Wi-Fi not connected within wifi_connect_timeout_ms — common on the
         * P4 + C6 hosted path, where co-processor association takes longer than
         * the timeout. The connection continues in the background; the app must
         * still initialize regardless (signaling connects once Wi-Fi is up), so
         * don't abort here. */
        ESP_LOGW(TAG, "Wi-Fi not connected yet; continuing init (will connect in background)");
    } else if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WiFi provisioning init failed: %s", esp_err_to_name(ret));
        return;
    }

    // Initialize storage
    app_storage_init();

    // Perform the time sync
    esp_webrtc_time_sntp_time_sync_and_wait();

    // Initialize work queue in advance with lower (than default) stack size
    esp_work_queue_config_t work_queue_config = ESP_WORK_QUEUE_CONFIG_DEFAULT();
    work_queue_config.stack_size = 12 * 1024;
    if (esp_work_queue_init_with_config(&work_queue_config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize work queue");
        return;
    }

    if (esp_work_queue_start() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start work queue");
        return;
    }

    ESP_LOGI(TAG, "Setting up WebRTC application with KVS signaling");

    g_kvsSignalingConfig.pChannelName = CONFIG_AWS_KVS_CHANNEL_NAME;
    g_kvsSignalingConfig.useIotCredentials = false; // Use static credentials for now
    g_kvsSignalingConfig.awsRegion = CONFIG_AWS_DEFAULT_REGION;
    g_kvsSignalingConfig.caCertPath = "/spiffs/certs/cacert.pem";

#ifdef CONFIG_IOT_CORE_ENABLE_CREDENTIALS
    ESP_LOGI(TAG, "Using IoT Core credentials");
    g_kvsSignalingConfig.useIotCredentials = true;
    g_kvsSignalingConfig.iotCoreCredentialEndpoint = CONFIG_AWS_IOT_CORE_CREDENTIAL_ENDPOINT;
    g_kvsSignalingConfig.iotCoreCert = CONFIG_AWS_IOT_CORE_CERT;
    g_kvsSignalingConfig.iotCorePrivateKey = CONFIG_AWS_IOT_CORE_PRIVATE_KEY;
    g_kvsSignalingConfig.iotCoreRoleAlias = CONFIG_AWS_IOT_CORE_ROLE_ALIAS;
    g_kvsSignalingConfig.iotCoreThingName = CONFIG_AWS_IOT_CORE_THING_NAME;
#else
    ESP_LOGI(TAG, "Using static AWS credentials");
    g_kvsSignalingConfig.awsAccessKey = CONFIG_AWS_ACCESS_KEY_ID;
    g_kvsSignalingConfig.awsSecretKey = CONFIG_AWS_SECRET_ACCESS_KEY;
    g_kvsSignalingConfig.awsSessionToken = CONFIG_AWS_SESSION_TOKEN;
#endif

    // Register WebRTC event callback
    app_webrtc_register_event_callback(app_webrtc_event_handler, NULL);

    /* Initialize power save handler (must be after esp_hosted_coprocessor_init) */
    power_save_init();

    // Configure WebRTC with our new simplified API - signaling-only mode
    app_webrtc_config_t app_webrtc_config = APP_WEBRTC_CONFIG_DEFAULT();

    // Essential configuration - what you MUST provide
    app_webrtc_config.signaling_client_if = kvs_signaling_client_if_get();
    app_webrtc_config.signaling_cfg = &g_kvsSignalingConfig;

    // Peer connection interface - use bridge-only implementation
    app_webrtc_config.peer_connection_if = bridge_peer_connection_if_get();

    // Initialize bridge command framework (must be before app_webrtc_init which
    // registers internal handlers like GET_TIME via signaling_bridge_adapter_init)
    ret = bridge_cmd_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize bridge command framework: %s", esp_err_to_name(ret));
    }

    ESP_LOGI(TAG, "Initializing WebRTC with bridge peer connection interface:");
    ESP_LOGI(TAG, "  - Interface: bridge-only (no WebRTC SDK initialization)");
    ESP_LOGI(TAG, "  - Role: MASTER (default - manages connections)");
    ESP_LOGI(TAG, "  - Channel: %s", g_kvsSignalingConfig.pChannelName);

    status = app_webrtc_init(&app_webrtc_config);
    if (status != WEBRTC_STATUS_SUCCESS) {
        ESP_LOGE(TAG, "Failed to initialize WebRTC application: 0x%08" PRIx32, (uint32_t) status);
        return;
    }

    // Set this to VIEWER when using trigger offer command
    // app_webrtc_set_role(WEBRTC_CHANNEL_ROLE_TYPE_VIEWER);

    ESP_LOGI(TAG, "Starting signaling-only WebRTC application");

    status = app_webrtc_run();
    if (status != WEBRTC_STATUS_SUCCESS) {
        ESP_LOGE(TAG, "WebRTC application failed: 0x%08" PRIx32, (uint32_t) status);
        app_webrtc_terminate();
    } else {
        ESP_LOGI(TAG, "WebRTC application started successfully");
    }

    /* bridge_cmd handlers (GET_TIME) are registered internally by
     * signaling_bridge_adapter_init. Register CLI response handlers here. */
    query_resolution_command_register_response_handler();
    query_snapshot_command_register_response_handler();

    ESP_LOGI(TAG, "Signaling-only example finished initialization");
    ESP_LOGI(TAG, "Use 'query-resolution' CLI command to query camera resolution from P4");
    ESP_LOGI(TAG, "Use 'query-snapshot [quality]' CLI command to capture JPEG snapshot from P4");
}
