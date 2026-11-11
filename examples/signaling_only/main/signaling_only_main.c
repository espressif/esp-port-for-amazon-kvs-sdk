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
#include "host_ps_integration.h"

static const char *TAG = "signaling_only";

extern int trigger_offer_command_register_cli(void);
extern int query_resolution_command_register_cli(void);
extern int query_resolution_command_register_response_handler(void);
extern int query_snapshot_command_register_cli(void);
extern int query_snapshot_command_register_response_handler(void);

static void register_app_cli(void)
{
    wifi_register_cli(); // for wifi-set command
    trigger_offer_command_register_cli();
    query_resolution_command_register_cli();
    query_snapshot_command_register_cli();
}

static void register_cli_after_wake(void)
{
    esp_cli_register_cmds();
    register_app_cli();
}

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
#include "esp_efuse.h"
#include "esp_efuse_table.h"
#include "esp_heap_caps.h"


static void on_got_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    esp_netif_t *netif = ((ip_event_got_ip_t *)data)->esp_netif;
    esp_netif_dns_info_t dns = { 0 };

    /* One unanswered query fails a signaling call. DHCP's server first. */
    if (esp_netif_get_dns_info(netif, ESP_NETIF_DNS_BACKUP, &dns) != ESP_OK ||
        dns.ip.u_addr.ip4.addr == 0) {
        dns.ip.type = ESP_IPADDR_TYPE_V4;
        dns.ip.u_addr.ip4.addr = ESP_IP4TOADDR(8, 8, 8, 8);
        esp_netif_set_dns_info(netif, ESP_NETIF_DNS_BACKUP, &dns);
    }
}


static esp_err_t signaling_pre_wifi_init(void *ctx)
{
    /* esp_netif_init must be done before eh_cp_init() */
    ESP_ERROR_CHECK(eh_cp_init());

    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                              &on_got_ip, NULL));

    /* Before anything can ask the CP to sleep. AUTO_INIT is off for this. */
    power_save_init();
    vTaskDelay(pdMS_TO_TICKS(5000));
    return ESP_OK;
}
#endif /* IS_VALID_SLAVE_CHIPSET */


#if CONFIG_EXAMPLE_MEMPROBE
static void memprobe_task(void *arg)
{
    for (int i = 0; i < 20; i++) {
        ESP_LOGW(TAG, "MEM t=%3ds int_free=%7u int_largest=%7u int_min=%7u "
                 "dma_free=%7u psram_free=%8u", i * 15,
                 (unsigned) heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned) heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                 (unsigned) heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned) heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL),
                 (unsigned) heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        vTaskDelay(pdMS_TO_TICKS(15000));
    }
    vTaskDelete(NULL);
}
#endif /* CONFIG_EXAMPLE_MEMPROBE */

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

    /* ESP_EFUSE_TEMP and PSRAM_CAP are C5-only; the emulated CP is a C6. */
#if CONFIG_IDF_TARGET_ESP32C5 && CONFIG_EXAMPLE_BOOT_DIAG
    {
        /* An unburned block also reads zero. MAC and wafer version share BLK1. */
        uint8_t mac[6] = {0}, cap = 0, vendor = 0, pkg = 0, wmaj = 0, wmin = 0, blkv = 0;
        esp_efuse_read_field_blob(ESP_EFUSE_MAC, mac, 48);
        esp_efuse_read_field_blob(ESP_EFUSE_WAFER_VERSION_MAJOR, &wmaj, 2);
        esp_efuse_read_field_blob(ESP_EFUSE_WAFER_VERSION_MINOR, &wmin, 4);
        esp_efuse_read_field_blob(ESP_EFUSE_BLK_VERSION_MAJOR, &blkv, 2);
        uint8_t fcap = 0, fvendor = 0, temp = 0;
        esp_efuse_read_field_blob(ESP_EFUSE_FLASH_CAP, &fcap, 3);
        esp_efuse_read_field_blob(ESP_EFUSE_FLASH_VENDOR, &fvendor, 3);
        esp_efuse_read_field_blob(ESP_EFUSE_TEMP, &temp, 2);
        esp_efuse_read_field_blob(ESP_EFUSE_PSRAM_CAP, &cap, 3);
        esp_efuse_read_field_blob(ESP_EFUSE_PSRAM_VENDOR, &vendor, 2);
        esp_efuse_read_field_blob(ESP_EFUSE_PKG_VERSION, &pkg, 3);
        ESP_LOGW(TAG, "efuse BLK1 control: MAC=%02x:%02x:%02x:%02x:%02x:%02x "
                 "wafer=v%u.%u blk_ver=%u pkg=%u",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], wmaj, wmin, blkv, pkg);
        /* FLASH_CAP (77) sits beside PSRAM_CAP (83): a matching flash size
         * proves this part of BLK1 is burned. */
        ESP_LOGW(TAG, "efuse neighbours: FLASH_CAP=%u FLASH_VENDOR=%u TEMP=%u", fcap, fvendor, temp);
        ESP_LOGW(TAG, "efuse PSRAM_CAP=%u PSRAM_VENDOR=%u", cap, vendor);
    }
#endif

    esp_cli_start();
#if CONFIG_EXAMPLE_MEMPROBE
    xTaskCreate(memprobe_task, "memprobe", 3072, NULL, 1, NULL);
#endif
    register_app_cli();
    host_ps_integration_set_cli_register_cb(register_cli_after_wake);

    /* Whoever calls esp_wifi_init() second must pass the same WIFI_INIT_CONFIG,
     * or eh_cp_feat_rpc_ext_v2 reinitialises the driver and drops the link. */
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

    /* Init early (before app_webrtc's 32 KB default) to keep this shared task
     * small on the C6. Measured peak on the signaling path is ~4 KB; 6 KB
     * leaves room for larger SDP or ICE messages and error-path logging. */
    esp_work_queue_config_t work_queue_config = ESP_WORK_QUEUE_CONFIG_DEFAULT();
    work_queue_config.stack_size = 6 * 1024;
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
