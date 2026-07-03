/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include <stdlib.h>
#include <pthread.h>
#include <inttypes.h>
#include "esp_system.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "esp_cli.h"
#include "wifi_cli.h"
#include "app_storage.h"
#include "app_wifi_prov.h"

#include "app_webrtc.h"
#include "kvs_signaling.h"
#include "kvs_peer_connection.h"
#include "esp_webrtc_time.h"
#include "esp_work_queue.h"
#include "media_stream.h"

#include "esp_console.h"

static const char *TAG = "webrtc_main";

#ifdef CONFIG_HOST_USES_STATIC_NETIF
#include "esp_wifi.h"
#include "esp_netif.h"

static esp_err_t create_static_netif_cb(void *ctx)
{
    ESP_LOGI(TAG, "Create netif with static IP");
    /* Create "almost" default station, but with un-flagged DHCP client */
    esp_netif_inherent_config_t netif_cfg;
    memcpy(&netif_cfg, ESP_NETIF_BASE_DEFAULT_WIFI_STA, sizeof(netif_cfg));
    netif_cfg.flags &= ~ESP_NETIF_DHCP_CLIENT;
    esp_netif_config_t cfg_sta = {
        .base = &netif_cfg,
        .stack = ESP_NETIF_NETSTACK_DEFAULT_WIFI_STA,
    };
    esp_netif_t *sta_netif = esp_netif_new(&cfg_sta);
    assert(sta_netif);

    ESP_ERROR_CHECK(esp_netif_attach_wifi_station(sta_netif));
    ESP_ERROR_CHECK(esp_wifi_set_default_wifi_sta_handlers());

    /* stop dhcpc */
    ESP_ERROR_CHECK(esp_netif_dhcpc_stop(sta_netif));

    return ESP_OK;
}
#endif

#if CONFIG_ESP_HOSTED_ENABLE_BT_NIMBLE
#include "esp_check.h"
#include "esp_hosted_misc.h"
#ifdef CONFIG_SLAVE_FLASHER_ENABLE
#include "slave_flasher.h"
#endif

static esp_err_t hosted_bt_prov_start(void *ctx)
{
    ESP_LOGI(TAG, "Initializing BT controller on coprocessor for provisioning");
    ESP_RETURN_ON_ERROR(esp_hosted_bt_controller_init(), TAG, "BT controller init failed");
    ESP_RETURN_ON_ERROR(esp_hosted_bt_controller_enable(), TAG, "BT controller enable failed");
    return ESP_OK;
}

static void hosted_bt_prov_end(void *ctx)
{
    ESP_LOGI(TAG, "Deinitializing BT controller on coprocessor");
    esp_hosted_bt_controller_disable();
    esp_hosted_bt_controller_deinit(true);
}
#endif

static void app_webrtc_event_handler(app_webrtc_event_data_t *event_data, void *user_ctx)
{
    if (event_data == NULL) {
        return;
    }

    switch (event_data->event_id) {
        case APP_WEBRTC_EVENT_INITIALIZED:
            ESP_LOGI(TAG, "[KVS Event] WebRTC Initialized.");
            break;
        case APP_WEBRTC_EVENT_DEINITIALIZING:
            ESP_LOGI(TAG, "[KVS Event] WebRTC Deinitialized.");
            break;
        case APP_WEBRTC_EVENT_SIGNALING_CONNECTING:
            ESP_LOGI(TAG, "[KVS Event] Signaling Connecting.");
            break;
        case APP_WEBRTC_EVENT_SIGNALING_CONNECTED:
            ESP_LOGI(TAG, "[KVS Event] Signaling Connected.");
            break;
        case APP_WEBRTC_EVENT_SIGNALING_DISCONNECTED:
            ESP_LOGI(TAG, "[KVS Event] Signaling Disconnected.");
            break;
        case APP_WEBRTC_EVENT_SIGNALING_DESCRIBE:
            ESP_LOGI(TAG, "[KVS Event] Signaling Describe.");
            break;
        case APP_WEBRTC_EVENT_SIGNALING_GET_ENDPOINT:
            ESP_LOGI(TAG, "[KVS Event] Signaling Get Endpoint.");
            break;
        case APP_WEBRTC_EVENT_SIGNALING_GET_ICE:
            ESP_LOGI(TAG, "[KVS Event] Signaling Get ICE.");
            break;
        case APP_WEBRTC_EVENT_PEER_CONNECTION_REQUESTED:
            ESP_LOGI(TAG, "[KVS Event] Peer Connection Requested.");
            break;
        case APP_WEBRTC_EVENT_PEER_CONNECTED:
            ESP_LOGI(TAG, "[KVS Event] Peer Connected: %s", event_data->peer_id);
            break;
        case APP_WEBRTC_EVENT_PEER_DISCONNECTED:
            ESP_LOGI(TAG, "[KVS Event] Peer Disconnected: %s", event_data->peer_id);
            break;
        case APP_WEBRTC_EVENT_STREAMING_STARTED:
            ESP_LOGI(TAG, "[KVS Event] Streaming Started for Peer: %s", event_data->peer_id);
            break;
        case APP_WEBRTC_EVENT_STREAMING_STOPPED:
            ESP_LOGI(TAG, "[KVS Event] Streaming Stopped for Peer: %s", event_data->peer_id);
            break;
        case APP_WEBRTC_EVENT_RECEIVED_OFFER:
            ESP_LOGI(TAG, "[KVS Event] Received Offer.");
            break;
        case APP_WEBRTC_EVENT_SENT_ANSWER:
            ESP_LOGI(TAG, "[KVS Event] Sent Answer.");
            break;
        case APP_WEBRTC_EVENT_ERROR:
            /* fall-through */
        case APP_WEBRTC_EVENT_SIGNALING_ERROR:
            /* fall-through */
        case APP_WEBRTC_EVENT_PEER_CONNECTION_FAILED:
            ESP_LOGE(TAG, "[KVS Event] Error Event %d: Code %d, Message: %s",
                     (int) event_data->event_id, (int) event_data->status_code, event_data->message);
            break;
        // Add cases for other events if needed
        default:
            ESP_LOGI(TAG, "[KVS Event] Unhandled Event ID: %d", (int) event_data->event_id);
            break;
    }
}

/* Structure to pass peer_id to trigger offer work function */
typedef struct {
    char peer_id[256];
} trigger_offer_work_params_t;

/* Work function to trigger offer in esp_work_queue context (avoids console reentrancy) */
static void trigger_offer_work_fn(void *priv_data)
{
    trigger_offer_work_params_t *params = (trigger_offer_work_params_t *)priv_data;

    if (params == NULL) {
        ESP_LOGE(TAG, "Invalid parameters for trigger offer work function");
        return;
    }

    ESP_LOGI(TAG, "Triggering offer for peer: %s", params->peer_id);

    int result = app_webrtc_trigger_offer(params->peer_id);

    if (result == 0) {
        ESP_LOGI(TAG, "Trigger offer command completed successfully");
    } else {
        ESP_LOGE(TAG, "Trigger offer command failed with code: %d", result);
    }

    /* Free the parameters */
    free(params);
}

/* CLI handler for trigger offer command */
static int trigger_offer_cli_handler(int argc, char *argv[])
{
    if (argc != 2) {
        ESP_LOGE(TAG, "Usage: trigger-offer <peer_id>");
        return -1;
    }

    const char *peer_id = argv[1];

    /* Validate peer_id length */
    if (strlen(peer_id) >= 256) {
        ESP_LOGE(TAG, "Peer ID too long (max 255 characters)");
        return -1;
    }

    /* Allocate parameters for the work function */
    trigger_offer_work_params_t *params = (trigger_offer_work_params_t *)malloc(sizeof(trigger_offer_work_params_t));
    if (params == NULL) {
        ESP_LOGE(TAG, "Failed to allocate memory for trigger offer work function");
        return -1;
    }

    /* Copy peer_id to work parameters */
    strncpy(params->peer_id, peer_id, sizeof(params->peer_id) - 1);
    params->peer_id[sizeof(params->peer_id) - 1] = '\0';

    /* Enqueue work to esp_work_queue (avoids console reentrancy issues) */
    esp_err_t err = esp_work_queue_add_task(trigger_offer_work_fn, params);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enqueue trigger offer work: %d", err);
        free(params);
        return -1;
    }

    ESP_LOGI(TAG, "Trigger offer work queued for peer: %s", peer_id);
    return 0;
}

/* Register trigger offer CLI command */
static void register_trigger_offer_cli(void)
{
    esp_console_cmd_t cmd = {
        .command = "trigger-offer",
        .help = "Trigger WebRTC offer to a peer. Usage: trigger-offer <peer_id>",
        .hint = "<peer_id>",
        .func = trigger_offer_cli_handler,
    };

    esp_err_t ret = esp_console_cmd_register(&cmd);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register trigger-offer command: %d", ret);
    } else {
        ESP_LOGI(TAG, "Registered trigger-offer command");
    }
}

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

#ifdef CONFIG_SLAVE_FLASHER_ENABLE
    // Program the attached C6/C5 co-processor before bringing up esp_hosted /
    // Wi-Fi, so the network path is available once the slave is running.
    ret = flash_slave();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to flash slave co-processor: %s", esp_err_to_name(ret));
        return;
    }
#endif

    ESP_LOGI(TAG, "ESP32 WebRTC Example");

    esp_cli_start();
    wifi_register_cli();
    register_trigger_offer_cli();

    // Initialize WiFi (with BLE provisioning if enabled)
    app_wifi_prov_config_t net_cfg = APP_NETWORK_CONFIG_DEFAULT();
#ifdef CONFIG_HOST_USES_STATIC_NETIF
    net_cfg.pre_wifi_init_cb = create_static_netif_cb;
    net_cfg.skip_default_sta_netif = true;
#endif
#if CONFIG_ESP_HOSTED_ENABLE_BT_NIMBLE
    net_cfg.prov_start_cb = hosted_bt_prov_start;
    net_cfg.prov_end_cb = hosted_bt_prov_end;
#endif
    ret = app_wifi_prov_init(&net_cfg);
    if (ret == ESP_ERR_TIMEOUT) {
        /* Wi-Fi not connected within wifi_connect_timeout_ms — common on the
         * P4 + C6 hosted path, where co-processor association takes longer than
         * the timeout. The connection continues in the background; WebRTC must
         * still initialize regardless (signaling connects once Wi-Fi is up), so
         * don't abort here. */
        ESP_LOGW(TAG, "Wi-Fi not connected yet; continuing WebRTC init (will connect in background)");
    } else if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WiFi provisioning init failed: %s", esp_err_to_name(ret));
        return;
    }

    app_storage_init();

    // Perform the time sync
    esp_webrtc_time_sntp_time_sync_and_wait();

    // Register the event callback *before* init to catch all events
    if (app_webrtc_register_event_callback(app_webrtc_event_handler, NULL) != 0) {
        ESP_LOGE(TAG, "Failed to register KVS event callback.");
    }

    // Get the media capture interfaces for sending audio/video
    media_stream_video_capture_t *video_capture = media_stream_get_video_capture_if();
    media_stream_audio_capture_t *audio_capture = media_stream_get_audio_capture_if();
    media_stream_video_player_t *video_player = media_stream_get_video_player_if();
    media_stream_audio_player_t *audio_player = media_stream_get_audio_player_if();

#ifdef CONFIG_ESP_P4_CORE_BOARD
    audio_capture = NULL;
    audio_player = NULL;
#endif

#ifndef CONFIG_MEDIA_STREAM_ENABLE_VIDEO_PLAYER
    /* Receive-path player is opt-in. When disabled, the media_stream
     * interface is a logging stub - do not wire it into app_webrtc so
     * received video is simply dropped. */
    video_player = NULL;
#endif

    if (video_capture == NULL) {
        ESP_LOGW(TAG, "Video capture not available - continuing without video capture");
    }
    if (video_player == NULL) {
        ESP_LOGW(TAG, "Video player not available - continuing without video player");
    }

    if (audio_capture == NULL) {
        ESP_LOGW(TAG, "Audio capture not available - continuing without audio capture");
    }

    if (audio_player == NULL) {
        ESP_LOGW(TAG, "Audio player not available - continuing without audio player");
    }

    // Set up KVS signaling configuration (as opaque config)
    static kvs_signaling_config_t kvs_signaling_cfg = {0};

    // Channel configuration
    kvs_signaling_cfg.pChannelName = CONFIG_AWS_KVS_CHANNEL_NAME;

#ifdef CONFIG_IOT_CORE_ENABLE_CREDENTIALS
    // Configure IoT Core credentials
    kvs_signaling_cfg.useIotCredentials = true;
    kvs_signaling_cfg.iotCoreCredentialEndpoint = CONFIG_AWS_IOT_CORE_CREDENTIAL_ENDPOINT;
    kvs_signaling_cfg.iotCoreCert = CONFIG_AWS_IOT_CORE_CERT;
    kvs_signaling_cfg.iotCorePrivateKey = CONFIG_AWS_IOT_CORE_PRIVATE_KEY;
    kvs_signaling_cfg.iotCoreRoleAlias = CONFIG_AWS_IOT_CORE_ROLE_ALIAS;
    kvs_signaling_cfg.iotCoreThingName = CONFIG_AWS_IOT_CORE_THING_NAME;
#else
    // Configure direct AWS credentials
    kvs_signaling_cfg.useIotCredentials = false;
    kvs_signaling_cfg.awsAccessKey = CONFIG_AWS_ACCESS_KEY_ID;
    kvs_signaling_cfg.awsSecretKey = CONFIG_AWS_SECRET_ACCESS_KEY;
    kvs_signaling_cfg.awsSessionToken = CONFIG_AWS_SESSION_TOKEN;
#endif

    // Set common AWS options
    kvs_signaling_cfg.awsRegion = CONFIG_AWS_DEFAULT_REGION;
    kvs_signaling_cfg.caCertPath = "/spiffs/certs/cacert.pem";

    // Configure WebRTC app with our new simplified API
    app_webrtc_config_t app_webrtc_config = APP_WEBRTC_CONFIG_DEFAULT();

    // Essential configuration - what you MUST provide
    app_webrtc_config.signaling_client_if = kvs_signaling_client_if_get();
    app_webrtc_config.signaling_cfg = &kvs_signaling_cfg;

    // Peer connection configuration - for full WebRTC functionality
    app_webrtc_config.peer_connection_if = kvs_peer_connection_if_get();

    // Media interfaces (NULL for signaling-only builds)
    app_webrtc_config.video_capture = video_capture;
    app_webrtc_config.audio_capture = audio_capture;
    app_webrtc_config.video_player  = video_player;   /* NULL when CONFIG_MEDIA_STREAM_ENABLE_VIDEO_PLAYER=n */
    app_webrtc_config.audio_player  = audio_player;

    ESP_LOGI(TAG, "Initializing WebRTC Classic Example - Full KVS Integration");
    ESP_LOGI(TAG, "  - Architecture: app_webrtc + kvs_webrtc + kvs_signaling");
    ESP_LOGI(TAG, "  - Signaling: AWS KVS WebSocket signaling");
    ESP_LOGI(TAG, "  - Peer Connections: Full KVS WebRTC peer connections");
    ESP_LOGI(TAG, "  - Role: MASTER (initiates connections)");
    ESP_LOGI(TAG, "  - Trickle ICE: enabled (faster connection setup)");
    ESP_LOGI(TAG, "  - TURN servers: enabled (better NAT traversal)");
    ESP_LOGI(TAG, "  - Audio codec: OPUS, Video codec: H264");
    ESP_LOGI(TAG, "  - Media reception: enabled for bidirectional streaming");

    // Initialize WebRTC application with simplified API
    status = app_webrtc_init(&app_webrtc_config);
    if (status != WEBRTC_STATUS_SUCCESS) {
        ESP_LOGE(TAG, "Failed to initialize WebRTC application: 0x%08" PRIx32, (uint32_t) status);
        return;
    }

    // Example: Advanced configuration APIs (optional)
    // Uncomment any of these if you need to change defaults:
    //
    // app_webrtc_set_role(WEBRTC_CHANNEL_ROLE_TYPE_VIEWER);  // Change to viewer role
    // app_webrtc_set_log_level(2);                                     // Enable DEBUG logging
    app_webrtc_enable_media_reception(true);                         // Enable receiving media
    // app_webrtc_set_ice_config(false, false);                         // Disable trickle ICE and TURN

    ESP_LOGI(TAG, "Running WebRTC application");

    // Run WebRTC application
    status = app_webrtc_run();
    if (status != WEBRTC_STATUS_SUCCESS) {
        ESP_LOGE(TAG, "WebRTC application failed: 0x%08" PRIx32, (uint32_t) status);
        app_webrtc_terminate();
    } else {
        ESP_LOGI(TAG, "WebRTC application started successfully");
    }
}
