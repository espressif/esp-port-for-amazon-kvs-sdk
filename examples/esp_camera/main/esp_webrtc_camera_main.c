/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include <inttypes.h>
#include "sdkconfig.h"
#include "esp_system.h"
#include "esp_idf_version.h"
#include "esp_log.h"

#include "app_webrtc.h"
#include "media_stream.h"
#include "apprtc_signaling.h"
#include "app_webrtc_if.h"
#include "kvs_peer_connection.h"

#if CONFIG_IDF_TARGET_LINUX
/* Host build: host networking, sample frames from disk, config from the environment */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <termios.h>
#include <sys/stat.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_event.h"
#include "file_capture.h"
#include "webrtc_cli.h"
#else
#include "nvs_flash.h"
#include "esp_cli.h"
#include "app_wifi_prov.h"
#include "wifi_cli.h"
#include "webrtc_cli.h"
#endif

#ifdef CONFIG_SLAVE_FLASHER_ENABLE
#include "slave_flasher.h"
#endif

static const char *TAG = "esp_webrtc_camera";

#if CONFIG_ESP_HOSTED_ENABLE_BT_NIMBLE
#include "esp_check.h"
#include "esp_hosted_misc.h"

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

/**
 * @brief Handle WebRTC events from the WebRTC SDK
 *
 * This callback is registered with the WebRTC SDK and receives events
 * such as peer connection state changes, ICE gathering completion, etc.
 */
static void app_webrtc_event_handler(app_webrtc_event_data_t *event_data, void *user_ctx)
{
    if (event_data == NULL) {
        return;
    }

    switch (event_data->event_id) {
        case APP_WEBRTC_EVENT_INITIALIZED:
            ESP_LOGI(TAG, "[WebRTC Event] WebRTC Initialized.");
            break;
        case APP_WEBRTC_EVENT_DEINITIALIZING:
            ESP_LOGI(TAG, "[WebRTC Event] WebRTC Deinitialized.");
            break;
        case APP_WEBRTC_EVENT_SIGNALING_CONNECTING:
            ESP_LOGI(TAG, "[WebRTC Event] Signaling Connecting.");
            break;
        case APP_WEBRTC_EVENT_SIGNALING_CONNECTED:
            ESP_LOGI(TAG, "[WebRTC Event] Signaling Connected.");
            break;
        case APP_WEBRTC_EVENT_SIGNALING_DISCONNECTED:
            ESP_LOGI(TAG, "[WebRTC Event] Signaling Disconnected.");
            break;
        case APP_WEBRTC_EVENT_PEER_CONNECTION_REQUESTED:
            ESP_LOGI(TAG, "[WebRTC Event] Peer Connection Requested.");
            break;
        case APP_WEBRTC_EVENT_RECEIVED_OFFER:
            ESP_LOGI(TAG, "[WebRTC Event] Received Offer.");
            break;
        case APP_WEBRTC_EVENT_SENT_ANSWER:
            ESP_LOGI(TAG, "[WebRTC Event] Sent Answer.");
            break;
        case APP_WEBRTC_EVENT_ICE_GATHERING_COMPLETE:
            ESP_LOGI(TAG, "[WebRTC Event] ICE Gathering Complete.");
            break;
        case APP_WEBRTC_EVENT_PEER_CONNECTED:
            ESP_LOGI(TAG, "[WebRTC Event] Peer Connected: %s", event_data->peer_id);
            break;
        case APP_WEBRTC_EVENT_PEER_DISCONNECTED:
            ESP_LOGI(TAG, "[WebRTC Event] Peer Disconnected: %s", event_data->peer_id);
            break;
        case APP_WEBRTC_EVENT_STREAMING_STARTED:
            ESP_LOGI(TAG, "[WebRTC Event] Streaming Started for Peer: %s", event_data->peer_id);
            break;
        case APP_WEBRTC_EVENT_STREAMING_STOPPED:
            ESP_LOGI(TAG, "[WebRTC Event] Streaming Stopped for Peer: %s", event_data->peer_id);
            break;
        case APP_WEBRTC_EVENT_ERROR:
        case APP_WEBRTC_EVENT_SIGNALING_ERROR:
        case APP_WEBRTC_EVENT_PEER_CONNECTION_FAILED:
            ESP_LOGE(TAG, "[WebRTC Event] Error Event %d: Code %d, Message: %s",
                     (int) event_data->event_id, (int) event_data->status_code, event_data->message);
            break;
        default:
            ESP_LOGI(TAG, "[WebRTC Event] Unhandled Event ID: %d", (int) event_data->event_id);
            break;
    }
}

#if CONFIG_IDF_TARGET_LINUX
static volatile sig_atomic_t s_should_exit = 0;
static struct termios s_term;          /* the terminal as we found it, for a forced quit */
static bool s_term_saved;

/* First Ctrl+C stops the run cleanly (exit() also restores the terminal the console
 * set up). A second one quits at once: tcsetattr() is async-signal-safe, atexit is not. */
static void on_signal(int sig)
{
    (void) sig;
    if (s_should_exit) {
        if (s_term_saved) {
            tcsetattr(STDIN_FILENO, TCSANOW, &s_term);
        }
        _exit(130);
    }
    s_should_exit = 1;
}

#include "esp_console.h"

static int host_exit_cmd(int argc, char **argv)
{
    (void) argc;
    (void) argv;
    s_should_exit = 1;
    return 0;
}

/* The same console as on target (join-room, get-room, ...) on stdin/stdout, plus exit */
static void host_console_start(void)
{
    /* Only with something to read from: at end of input (stdin on /dev/null or a file)
     * the REPL spins on empty reads and starves the main task */
    struct stat in;
    if (!isatty(STDIN_FILENO) && !(fstat(STDIN_FILENO, &in) == 0 && S_ISFIFO(in.st_mode))) {
        ESP_LOGI(TAG, "stdin is not a terminal or a pipe: no console");
        return;
    }
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = "esp_camera> ";
    const esp_console_cmd_t exit_cmd = {
        .command = "exit",
        .help = "Stop the example and exit",
        .func = host_exit_cmd,
    };

    s_term_saved = isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO, &s_term) == 0;
    esp_console_register_help_command();
    esp_console_cmd_register(&exit_cmd);
    webrtc_register_cli();
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 1, 0)
    /* IDF 6.1 replaced the UART REPL on the Linux target with a stdio one */
    esp_err_t err = esp_console_new_repl_stdio(&repl_config, &repl);
#else
    esp_console_dev_uart_config_t dev_config = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    esp_err_t err = esp_console_new_repl_uart(&dev_config, &repl_config, &repl);
#endif
    if (err != ESP_OK || esp_console_start_repl(repl) != ESP_OK) {
        ESP_LOGW(TAG, "Console not available; the example runs without it");
    }
}

/* Returning from app_main leaves the POSIX port's scheduler running: exit instead */
#define EXIT_ON_HOST(code) exit(code)
#else
#define EXIT_ON_HOST(code) ((void) 0)
#endif

void app_main(void)
{
    WEBRTC_STATUS status = WEBRTC_STATUS_SUCCESS;

#if CONFIG_IDF_TARGET_LINUX
    /* SIGINT only: the POSIX port blocks every other signal in all threads */
    signal(SIGINT, on_signal);
    ESP_LOGI(TAG, "ESP WebRTC Camera Example (linux host, sample frames)");
    /* esp_http_client posts its events to the default loop; on ESP targets the
     * Wi-Fi bring-up creates it, on the host nothing else does */
    esp_err_t loop_err = esp_event_loop_create_default();
    if (loop_err != ESP_OK && loop_err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Failed to create the default event loop: %s", esp_err_to_name(loop_err));
        EXIT_ON_HOST(1);
    }

    const char *frames_dir = getenv("KVS_FRAMES_DIR");
    if (frames_dir && *frames_dir) {
        file_capture_set_frames_dir(frames_dir);
    }
    media_stream_video_capture_t *video_capture = file_capture_get_video_if();
    media_stream_audio_capture_t *audio_capture = file_capture_get_audio_if();
#else
    esp_err_t ret;

    // Initialize NVS
    ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_LOGI(TAG, "ESP32 WebRTC Camera Example");

#ifdef CONFIG_SLAVE_FLASHER_ENABLE
    /* P4-EYE: flash the on-board C6 (network_adapter) from the slave SPIFFS
     * partition before bringing up Wi-Fi, which runs over that co-processor. */
    ret = flash_slave();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to flash slave co-processor: %s", esp_err_to_name(ret));
        return;
    }
#endif

    esp_cli_start();
    wifi_register_cli();
    webrtc_register_cli();

    // Initialize WiFi (with BLE provisioning if enabled)
    app_wifi_prov_config_t net_cfg = APP_NETWORK_CONFIG_DEFAULT();
    net_cfg.wifi_connect_timeout_ms = 10000;
#if CONFIG_ESP_HOSTED_ENABLE_BT_NIMBLE
    net_cfg.prov_start_cb = hosted_bt_prov_start;
    net_cfg.prov_end_cb = hosted_bt_prov_end;
#endif
    ret = app_wifi_prov_init(&net_cfg);
    if (ret == ESP_ERR_TIMEOUT) {
        /* Wi-Fi not connected within wifi_connect_timeout_ms — common on the
         * P4 + C6 hosted path, where co-processor association takes longer than
         * the timeout. The connection continues in the background; WebRTC must
         * still initialize regardless (the session is started on demand via the
         * `join-room` console command), so don't abort here. */
        ESP_LOGW(TAG, "Wi-Fi not connected yet; continuing WebRTC init (will connect in background)");
    } else if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WiFi provisioning init failed: %s", esp_err_to_name(ret));
        return;
    }

    // This example is a lean camera *sender*: it captures the camera (and mic)
    // and streams to the browser. It deliberately does NOT render received
    // media locally — no video player (no H.264 decoder, no LVGL/LCD draw
    // buffers, no SPI-DMA bounce) and no audio player. That keeps internal RAM
    // usage minimal and the example board-agnostic (esp32s3 launchpad, P4, …)
    // with no display dependency. Enable a player here only if a specific board
    // build wants local playback.
    media_stream_video_capture_t *video_capture = media_stream_get_video_capture_if();
    media_stream_audio_capture_t *audio_capture = media_stream_get_audio_capture_if();
#endif /* CONFIG_IDF_TARGET_LINUX */

    // Register the WebRTC event callback to receive events from the WebRTC SDK
    if (app_webrtc_register_event_callback(app_webrtc_event_handler, NULL) != 0) {
        ESP_LOGE(TAG, "Failed to register WebRTC event callback");
    }

    media_stream_video_player_t *video_player = NULL;
    media_stream_audio_player_t *audio_player = NULL;

    if (video_capture == NULL) {
        ESP_LOGW(TAG, "Video capture not available - continuing without video capture");
    }
    if (audio_capture == NULL) {
        ESP_LOGW(TAG, "Audio capture not available - continuing without audio capture");
    }

    // Configure AppRTC signaling (must outlive app_main since app_webrtc stores the pointer)
    static apprtc_signaling_config_t apprtc_config = {
        .serverUrl = NULL,  // Use default AppRTC server
        .roomId = NULL,     // NULL: the signaling client picks the room
        .autoConnect = false,
        .connectionTimeout = 30000,
        .logLevel = 3
    };

#if CONFIG_IDF_TARGET_LINUX
    /* APPRTC_ROOM_ID joins that room; unset creates a new one (its URL is logged) */
    const char *room = getenv("APPRTC_ROOM_ID");
    if (room && *room) {
        apprtc_config.roomId = (char *) room;
    }
#endif

    // Configure WebRTC app with our new simplified API
    app_webrtc_config_t app_webrtc_config = APP_WEBRTC_CONFIG_DEFAULT();

    // Essential configuration - signaling interface
    app_webrtc_config.signaling_client_if = apprtc_signaling_client_if_get();
    app_webrtc_config.signaling_cfg = &apprtc_config;

    // Peer connection interface
    app_webrtc_config.peer_connection_if = kvs_peer_connection_if_get();
    /* In a room the device creates, its offer waits for someone to join */
    kvs_peer_connection_set_answer_timeout(10 * 60);
    app_webrtc_config.implementation_config = NULL;

    // Media interfaces for bi-directional streaming
    app_webrtc_config.video_capture = video_capture;
    app_webrtc_config.audio_capture = audio_capture;
    app_webrtc_config.video_player = video_player;
    app_webrtc_config.audio_player = audio_player;

    ESP_LOGI(TAG, "Initializing WebRTC application with simplified API:");
    ESP_LOGI(TAG, "  - Media type: auto-detected (audio+video from interfaces)");
    ESP_LOGI(TAG, "  - AppRTC signaling: browser-compatible");
    ESP_LOGI(TAG, "  - Streaming: bi-directional (can send and receive)");

    // Initialize WebRTC application with simplified API
    status = app_webrtc_init(&app_webrtc_config);
    if (status != WEBRTC_STATUS_SUCCESS) {
        ESP_LOGE(TAG, "Failed to initialize WebRTC application: 0x%08" PRIx32, (uint32_t) status);
        EXIT_ON_HOST(1);
        return;
    }

    // Role type from the configuration. With AppRTC it does not decide who offers:
    // the server does (the first in the room offers, a later joiner answers).
#if CONFIG_APPRTC_ROLE_TYPE == 0
    app_webrtc_set_role(WEBRTC_CHANNEL_ROLE_TYPE_MASTER);
    ESP_LOGI(TAG, "Configured as MASTER role using advanced API");
#else
    app_webrtc_set_role(WEBRTC_CHANNEL_ROLE_TYPE_VIEWER);
    ESP_LOGI(TAG, "Configured as VIEWER role using advanced API");
#endif

    // Keep bi-directional negotiation enabled so the peer can still send media.
    // Received media is not rendered locally (no players wired above).
    app_webrtc_enable_media_reception(true);

    // Start the WebRTC application (this will handle signaling connection)
    status = app_webrtc_run();
    if (status != WEBRTC_STATUS_SUCCESS) {
        ESP_LOGE(TAG, "Failed to start WebRTC application: 0x%08" PRIx32, (uint32_t) status);
        app_webrtc_terminate();
        EXIT_ON_HOST(1);
        return;
    }

#if CONFIG_APPRTC_AUTO_CONNECT
    // The boot connect has started. Without a room id the signaling client picks
    // the room: CONFIG_APPRTC_ROOM_ID with APPRTC_USE_FIXED_ROOM, otherwise a new one
    if (apprtc_config.roomId != NULL) {
        ESP_LOGI(TAG, "Joining room: %s", apprtc_config.roomId);
    } else {
#if CONFIG_APPRTC_USE_FIXED_ROOM
        ESP_LOGI(TAG, "Joining fixed room: %s", CONFIG_APPRTC_ROOM_ID);
#else
        ESP_LOGI(TAG, "Creating a new room");
#endif
    }
#else
    // Manual connection mode - user must use CLI commands
    ESP_LOGI(TAG, "------------------------------------------------------------");
    ESP_LOGI(TAG, "| WebRTC connection ready. Use CLI commands to connect:    |");
    ESP_LOGI(TAG, "| - To create a new room:  join-room new                   |");
    ESP_LOGI(TAG, "| - To join existing room: join-room <room_id>             |");
    ESP_LOGI(TAG, "| - To check current room: get-room                        |");
    ESP_LOGI(TAG, "| - To disconnect:         disconnect                      |");
    ESP_LOGI(TAG, "------------------------------------------------------------");
#endif

    ESP_LOGI(TAG, "------------------------------------------------------------");
    ESP_LOGI(TAG, "| WebRTC application initialized and ready!                |");
    ESP_LOGI(TAG, "| The signaling will connect automatically                 |");
    ESP_LOGI(TAG, "| Room URL will be displayed once connection is established|");
    ESP_LOGI(TAG, "------------------------------------------------------------");

#if CONFIG_IDF_TARGET_LINUX
    host_console_start();

    /* Run for TEST_DURATION_SEC (0 = until `exit` or Ctrl+C), then exit: the POSIX
     * port keeps the scheduler running otherwise. */
    long duration = 0;
    const char *dur_env = getenv("TEST_DURATION_SEC");
    if (dur_env && *dur_env) {
        char *end = NULL;
        duration = strtol(dur_env, &end, 10);
        if (*end != '\0' || duration < 0) {
            ESP_LOGW(TAG, "Ignoring invalid TEST_DURATION_SEC='%s', running until Ctrl+C", dur_env);
            duration = 0;
        }
    }
    /* app_main is a FreeRTOS task: a raw sleep() is cut short by the port's SIGALRM tick */
    for (long i = 0; (duration == 0 || i < duration) && !s_should_exit; ++i) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    ESP_LOGI(TAG, "shutting down");
    app_webrtc_terminate();
    exit(0);
#endif
}
