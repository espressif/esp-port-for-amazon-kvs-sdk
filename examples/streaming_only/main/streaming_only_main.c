/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include <inttypes.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"

#include "esp_cli.h"
#include "app_storage.h"

#include "media_stream.h"
#include "video_capture.h"
#include "signaling_serializer.h"
#include "webrtc_bridge.h"
#include "webrtc_bridge_signaling.h"
// ice_bridge_client and esp_webrtc_time are initialized internally by bridge_signaling
#include "bridge_cmd_defs.h"
#include "esp_work_queue.h"
#include "app_webrtc.h"
#include "kvs_peer_connection.h"
#include "esp_hosted.h"
#include "power_save_handler.h"
#ifdef CONFIG_SLAVE_FLASHER_ENABLE
#include "slave_flasher.h"
#endif
#ifdef CONFIG_SLAVE_FLASHER_ENABLE
static vprintf_like_t s_original_vprintf = NULL;

static int custom_vprintf(const char* fmt, va_list args)
{
    // Print the [HOST] prefix in bright cyan color
    printf("\033[1;36m[HOST]\033[0m ");

    if (s_original_vprintf) {
        return s_original_vprintf(fmt, args);
    } else {
        return vprintf(fmt, args);
    }
}
#endif

extern esp_err_t esp_hosted_wait_for_slave(void);

#define HOST_USES_STATIC_NETIF (0)
static esp_netif_t *sta_netif;

static const char *TAG = "streaming_only";

// Data channel callback context
typedef struct {
    char peer_id[64];
} data_channel_context_t;

// Global data channel context for active peer
static data_channel_context_t g_data_channel_ctx;

// WiFi event group
static EventGroupHandle_t s_wifi_event_group;
static char wifi_ip[72];

// Data channel callbacks
static void on_data_channel_open(uint64_t custom_data, void *p_data_channel, const char *peer_id)
{
    data_channel_context_t *ctx = (data_channel_context_t *)(uintptr_t)custom_data;

    ESP_LOGI(TAG, "on_data_channel_open called with custom_data: 0x%llx", (unsigned long long)custom_data);
    ESP_LOGI(TAG, "p_data_channel: %p, peer_id: %s", p_data_channel, peer_id);

    if (ctx == NULL) {
        ESP_LOGE(TAG, "Invalid data channel context (NULL) in open callback");
        return;
    }

    // Store the peer_id in our context
    if (peer_id != NULL) {
        strncpy(ctx->peer_id, peer_id, sizeof(ctx->peer_id) - 1);
        ctx->peer_id[sizeof(ctx->peer_id) - 1] = '\0';
    }

    ESP_LOGI(TAG, "Data channel opened for peer: %s", ctx->peer_id);

    // Send a welcome message when the channel opens
    const char *welcome_msg = "Hello from ESP32! Data channel is now open. Send a message and I'll echo it back.";
    ESP_LOGI(TAG, "Sending welcome message to peer: %s", ctx->peer_id);

    WEBRTC_STATUS status = app_webrtc_send_data_channel_message(
        ctx->peer_id,
        p_data_channel,
        false,
        (const uint8_t *)welcome_msg,
        strlen(welcome_msg)
    );

    if (status != WEBRTC_STATUS_SUCCESS) {
        ESP_LOGE(TAG, "Failed to send welcome message: 0x%08x", status);
    } else {
        ESP_LOGI(TAG, "Successfully sent welcome message");
    }
}

static void on_data_channel_message(uint64_t custom_data, void *p_data_channel, const char *peer_id,
                                    bool is_binary, uint8_t *p_message, uint32_t message_len)
{
    data_channel_context_t *ctx = (data_channel_context_t *)(uintptr_t)custom_data;

    ESP_LOGI(TAG, "on_data_channel_message called with custom_data: 0x%llx", (unsigned long long)custom_data);
    ESP_LOGI(TAG, "p_data_channel: %p, peer_id: %s, is_binary: %d, p_message: %p, message_len: %" PRIu32,
             p_data_channel, peer_id, (int) is_binary, p_message, message_len);

    if (ctx == NULL) {
        ESP_LOGE(TAG, "Invalid data channel context (NULL) in message callback");
        return;
    }

    if (p_message == NULL) {
        ESP_LOGE(TAG, "Invalid message pointer (NULL) in message callback");
        return;
    }

    // Store or update the peer_id in our context
    if (peer_id != NULL) {
        strncpy(ctx->peer_id, peer_id, sizeof(ctx->peer_id) - 1);
        ctx->peer_id[sizeof(ctx->peer_id) - 1] = '\0';
    }

    ESP_LOGI(TAG, "Data channel message received from peer %s: %.*s (binary: %s)",
             peer_id, (int) message_len, p_message, is_binary ? "yes" : "no");

    // Echo the message back
    ESP_LOGI(TAG, "Echoing message back to peer: %s", peer_id);
    WEBRTC_STATUS status = app_webrtc_send_data_channel_message(peer_id, p_data_channel, is_binary, p_message, message_len);

    if (status != WEBRTC_STATUS_SUCCESS) {
        ESP_LOGE(TAG, "Failed to echo message back: 0x%08x", status);
    } else {
        ESP_LOGI(TAG, "Successfully echoed message back");
    }
}

#define GOT_IP_BIT BIT0

// WiFi event handler
static void event_handler(void* arg, esp_event_base_t event_base,
                          int32_t event_id, void* event_data)
{
    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        memset(wifi_ip, 0, sizeof(wifi_ip)/sizeof(wifi_ip[0]));
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        memcpy(wifi_ip, &event->ip_info.ip, 4);
        xEventGroupSetBits(s_wifi_event_group, GOT_IP_BIT);
    }
}

#if HOST_USES_STATIC_NETIF
esp_netif_t *create_slave_sta_netif_with_static_ip(void)
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
    sta_netif = esp_netif_new(&cfg_sta);
    ESP_LOGI(TAG, "Created slave sta netif with static IP %p", sta_netif);
    assert(sta_netif);

    ESP_LOGI(TAG, "Creating slave sta netif with static IP");

    /* stop dhcpc */
    ESP_ERROR_CHECK(esp_netif_dhcpc_stop(sta_netif));

    // esp_netif_action_start(sta_netif, NULL, 0, NULL);
    // esp_netif_action_connected(sta_netif, 0, 0, 0);

    return sta_netif;
}
#endif

static void wifi_init_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

	ESP_ERROR_CHECK(esp_hosted_init());
	ESP_ERROR_CHECK(esp_hosted_connect_to_slave());

#if HOST_USES_STATIC_NETIF
    sta_netif = create_slave_sta_netif_with_static_ip();
#else
    sta_netif = esp_netif_create_default_wifi_sta();
    if (sta_netif == NULL) {
        ESP_LOGE(TAG, "Failed to create default WiFi STA netif");
        return;
    }
    ESP_LOGI(TAG, "Created default WiFi STA netif: %p", sta_netif);
#endif

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL));

    ESP_ERROR_CHECK(esp_wifi_start());
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_wifi_connect();
    // ESP_ERROR_CHECK(esp_hosted_wait_for_slave());

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
            GOT_IP_BIT,
            pdFALSE,
            pdFALSE,
            pdMS_TO_TICKS(20000));

    if (bits & GOT_IP_BIT) {
        ESP_LOGI(TAG, "Connected to WiFi");
    } else {
        ESP_LOGE(TAG, "Failed to connect to WiFi");
    }
}

/* -------------------------------------------------------------------------- */
/*  Bridge command handlers                                                   */
/* -------------------------------------------------------------------------- */

/**
 * Handle BRIDGE_CMD_GET_RESOLUTION from the signaling device (C6).
 * Returns the current camera resolution.
 *
 * TODO: Retrieve actual resolution from media_stream / camera driver
 *       once a public getter API is available.
 */
static esp_err_t handle_get_resolution(uint32_t cmd_id,
                                       const uint8_t *req_data, size_t req_len,
                                       uint8_t **resp_data, size_t *resp_len)
{
    bridge_cmd_resolution_t *res = malloc(sizeof(bridge_cmd_resolution_t));
    if (!res) {
        return ESP_ERR_NO_MEM;
    }

    /* Default resolution matching media_stream defaults.
     * Replace with actual camera resolution query when available. */
    res->width  = 1280;
    res->height = 720;

    ESP_LOGI("bridge_cmd", "GET_RESOLUTION -> %"PRIu32"x%"PRIu32, res->width, res->height);

    *resp_data = (uint8_t *)res;
    *resp_len  = sizeof(*res);
    return ESP_OK;
}

/**
 * Handle BRIDGE_CMD_GET_SNAPSHOT from the signaling device (C6).
 * Captures a JPEG snapshot and returns it as the response payload.
 * The bridge_cmd framework handles chunking automatically.
 */
static esp_err_t handle_get_snapshot(uint32_t cmd_id,
                                     const uint8_t *req_data, size_t req_len,
                                     uint8_t **resp_data, size_t *resp_len)
{
    uint8_t quality = 80;  /* Default JPEG quality */

    /* Parse optional request payload for quality setting */
    if (req_data && req_len >= sizeof(bridge_cmd_snapshot_req_t)) {
        const bridge_cmd_snapshot_req_t *req = (const bridge_cmd_snapshot_req_t *)req_data;
        if (req->quality > 0 && req->quality <= 100) {
            quality = req->quality;
        }
    }

    ESP_LOGI(TAG, "GET_SNAPSHOT requested (quality=%u)", quality);

    uint8_t *jpeg_buf = NULL;
    size_t jpeg_len = 0;

    esp_err_t ret = video_capture_get_snapshot(&jpeg_buf, &jpeg_len, quality, 5000);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to capture snapshot: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "GET_SNAPSHOT -> JPEG %zu bytes (quality=%u)", jpeg_len, quality);

    *resp_data = jpeg_buf;
    *resp_len = jpeg_len;
    return ESP_OK;
}

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
        case APP_WEBRTC_EVENT_PEER_CONNECTION_REQUESTED:
            ESP_LOGI(TAG, "[KVS Event] Peer Connection Requested.");
            break;
        case APP_WEBRTC_EVENT_PEER_CONNECTED:
            ESP_LOGI(TAG, "[KVS Event] Peer Connected: %s", event_data->peer_id);

            // Update data channel context with the connected peer ID
            strncpy(g_data_channel_ctx.peer_id, event_data->peer_id, sizeof(g_data_channel_ctx.peer_id) - 1);
            g_data_channel_ctx.peer_id[sizeof(g_data_channel_ctx.peer_id) - 1] = '\0';
            ESP_LOGI(TAG, "Updated data channel context with peer ID: %s", g_data_channel_ctx.peer_id);
            break;
        case APP_WEBRTC_EVENT_PEER_DISCONNECTED:
            ESP_LOGI(TAG, "[KVS Event] Peer Disconnected: %s", event_data->peer_id);

            // Clear data channel context if this is our active peer
            if (strcmp(g_data_channel_ctx.peer_id, event_data->peer_id) == 0) {
                memset(&g_data_channel_ctx, 0, sizeof(g_data_channel_ctx));
            }
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
        case APP_WEBRTC_EVENT_PEER_CONNECTION_FAILED:
            ESP_LOGE(TAG, "[KVS Event] Error Event %d: Code %d, Message: %s",
                     (int) event_data->event_id, (int) event_data->status_code, event_data->message);
            break;
        default:
            ESP_LOGI(TAG, "[KVS Event] Unhandled Event ID: %d", (int) event_data->event_id);
            break;
    }
}

void app_main(void)
{
    esp_err_t ret;

    WEBRTC_STATUS status;

    // Initialize NVS
    ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

#ifdef CONFIG_SLAVE_FLASHER_ENABLE
    s_original_vprintf = esp_log_set_vprintf(custom_vprintf);

    ret = flash_slave();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to flash slave: %s", esp_err_to_name(ret));
        return;
    }
#endif

    ESP_LOGI(TAG, "ESP32 WebRTC Streaming Example");

    esp_cli_start();

    /* Register deep sleep command */
    power_save_cli_register();

    esp_hosted_init();
    esp_hosted_connect_to_slave();

    // Initialize WiFi
    wifi_init_sta();

    app_storage_init();

    // Initialize signaling serializer
    signaling_serializer_init();

    // Enable automatic power save
    if (power_save_enable() != 0) {
        ESP_LOGE(TAG, "Failed to enable power save.");
    }

    // Register application event handler
    // Multiple handlers can be registered - both will receive all events
    if (app_webrtc_register_event_callback(app_webrtc_event_handler, NULL) != 0) {
        ESP_LOGE(TAG, "Failed to register app event handler.");
    }

    // Get media interfaces for streaming (capture for sending, player for receiving)
    // Optionally use file-based capture instead of camera/microphone
    // Set USE_FILE_STREAM to 1 to use file-based streaming, 0 for hardware capture
#define USE_FILE_STREAM 0

    media_stream_video_capture_t *video_capture = NULL;
    media_stream_audio_capture_t *audio_capture = NULL;
    media_stream_video_player_t *video_player = NULL;
    media_stream_audio_player_t *audio_player = NULL;

#if USE_FILE_STREAM
    video_capture = media_stream_get_file_video_capture_if();
#else
#ifdef CONFIG_ESP_HOSTED_P4_C5_CORE_BOARD
    video_capture = media_stream_get_video_capture_if();
#else
    video_capture = media_stream_get_video_capture_if();
    audio_capture = media_stream_get_audio_capture_if();
    video_player = media_stream_get_video_player_if();
    audio_player = media_stream_get_audio_player_if();
#endif
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

    // Initialize data channel context
    memset(&g_data_channel_ctx, 0, sizeof(g_data_channel_ctx));
    strcpy(g_data_channel_ctx.peer_id, "default");  // Will be updated when peer connects

    // Define data channel config
    static webrtc_data_channel_config_t dc_config;
    dc_config.onOpen = on_data_channel_open;
    dc_config.onMessage = on_data_channel_message;
    dc_config.onClose = NULL;  // Reserved for future use (not yet supported by KVS SDK)
    dc_config.customData = (uint64_t)(uintptr_t)&g_data_channel_ctx;

    // Configure WebRTC with our new simplified API - streaming-only mode
    app_webrtc_config_t app_webrtc_config = APP_WEBRTC_CONFIG_DEFAULT();

    // Set up bridge signaling interface and config
    bridge_signaling_config_t bridge_config = {
        .client_id = "streaming_client",
        .log_level = 2
    };

    // Essential configuration - signaling interface
    app_webrtc_config.signaling_client_if = bridge_signaling_client_if_get();
    app_webrtc_config.signaling_cfg = &bridge_config;

    // Enable pluggable peer connection using KVS implementation
    app_webrtc_config.peer_connection_if = kvs_peer_connection_if_get();

    // Media interfaces for bi-directional streaming
    app_webrtc_config.video_capture = video_capture;
    app_webrtc_config.audio_capture = audio_capture;
    app_webrtc_config.video_player = video_player;
    app_webrtc_config.audio_player = audio_player;

    // Data channel configuration
    app_webrtc_config.data_channel_config = &dc_config;

    ESP_LOGI(TAG, "Initializing WebRTC streaming-only device with simplified API:");
    ESP_LOGI(TAG, "  - Mode: streaming-only (no signaling, receives via bridge)");
    ESP_LOGI(TAG, "  - Media type: auto-detected (audio+video from interfaces)");
    ESP_LOGI(TAG, "  - Streaming: bi-directional (can send and receive media)");
    ESP_LOGI(TAG, "  - Bridge: communicates with signaling device");
    ESP_LOGI(TAG, "  - Role: MASTER (default - optimized for streaming)");

    // Initialize WebRTC application with simplified API
    status = app_webrtc_init(&app_webrtc_config);
    if (status != WEBRTC_STATUS_SUCCESS) {
        ESP_LOGE(TAG, "Failed to initialize WebRTC application: 0x%08" PRIx32, (uint32_t) status);
        goto CleanUp;
    }

    // app_webrtc_set_log_level(2); // DEBUG level

    // Enable media reception for bi-directional streaming
    app_webrtc_enable_media_reception(true);

    ESP_LOGI(TAG, "Data channel callbacks configured via config structure");

    // Start webrtc bridge
    webrtc_bridge_start();

    /* ice_bridge_client_init() is handled internally by bridge_signaling
     * during app_webrtc_init(). Time sync response handler (esp_webrtc_time)
     * self-registers on first use.
     * Initialize bridge command framework and register example-specific handlers. */
    if (bridge_cmd_init() == ESP_OK) {
        bridge_cmd_register_handler(BRIDGE_CMD_GET_RESOLUTION, handle_get_resolution);
        bridge_cmd_register_handler(BRIDGE_CMD_GET_SNAPSHOT, handle_get_snapshot);
    } else {
        ESP_LOGE(TAG, "Failed to initialize bridge command subsystem");
    }

    ESP_LOGI(TAG, "Streaming example initialized, waiting for signaling messages");

    // Run WebRTC application
    status = app_webrtc_run();
    if (status != WEBRTC_STATUS_SUCCESS) {
        ESP_LOGE(TAG, "WebRTC application failed: 0x%08" PRIx32, (uint32_t) status);
        goto CleanUp;
    }

CleanUp:
    // Do not terminate the WebRTC application in streaming-only mode
    // Only streaming sessions are created and destroyed internally
    // app_webrtc_terminate();
}
