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
#include "eh_cp.h"
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

/* The host boots, flashes this image and only then brings Wi-Fi up, so the
 * co-processor waits far longer than a plain association would take. */
#define CP_WAIT_FOR_IP_MS 120000

#if IS_VALID_SLAVE_CHIPSET
#include "esp_wifi.h"
#include "esp_netif.h"

static EventGroupHandle_t s_ip_event_group;
#define CP_GOT_IP_BIT BIT0

static void on_got_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    xEventGroupSetBits(s_ip_event_group, CP_GOT_IP_BIT);
}

static esp_err_t signaling_pre_wifi_init(void *ctx)
{
    /* esp_netif_init must be done before eh_cp_init() */
    ESP_ERROR_CHECK(eh_cp_init());

    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                              &on_got_ip, NULL));

    /* Same order as esp_hosted's power-save example: the host_ps callbacks are
     * registered right after eh_cp_init(), before anything can ask the
     * co-processor to sleep. CP_FEAT_HOST_PS_AUTO_INIT is off for this reason. */
    power_save_init();
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
    trigger_offer_command_register_cli();
    query_resolution_command_register_cli();
    query_snapshot_command_register_cli();

#if CONFIG_ESP_HOSTED_CP_FEAT_NW_SPLIT_READY
    /* Under network split the host owns the Wi-Fi connection. It calls
     * esp_wifi_init(), esp_wifi_start() and esp_wifi_connect() over RPC, the same
     * division of work as esp_hosted's own network_split/station example, whose
     * co-processor calls eh_cp_init() and nothing else. A second esp_wifi_init()
     * here makes eh_cp_feat_rpc_ext_v2 compare the host's configuration with this
     * one, find a difference and initialize the driver again, which drops the
     * connection. The co-processor brings up its own IP stack and waits. */
    s_ip_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(signaling_pre_wifi_init(NULL));

    ESP_LOGI(TAG, "network split: waiting for the host to bring Wi-Fi up");
    ret = (xEventGroupWaitBits(s_ip_event_group, CP_GOT_IP_BIT, pdFALSE, pdFALSE,
                               pdMS_TO_TICKS(CP_WAIT_FOR_IP_MS)) & CP_GOT_IP_BIT)
          ? ESP_OK : ESP_ERR_TIMEOUT;
#else
    // Initialize WiFi (with BLE provisioning if enabled)
    app_wifi_prov_config_t net_cfg = APP_NETWORK_CONFIG_DEFAULT();
#if IS_VALID_SLAVE_CHIPSET
    net_cfg.pre_wifi_init_cb = signaling_pre_wifi_init;
    net_cfg.skip_default_sta_netif = true;
#endif
    ret = app_wifi_prov_init(&net_cfg);
#endif
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

    /* Init early (before app_webrtc's 32 KB default) to keep this shared task
     * small on the C6. Measured peak on the signaling path is ~4 KB. */
    esp_work_queue_config_t work_queue_config = ESP_WORK_QUEUE_CONFIG_DEFAULT();
    work_queue_config.stack_size = 8 * 1024;
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
    power_save_register_webrtc_events();

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
