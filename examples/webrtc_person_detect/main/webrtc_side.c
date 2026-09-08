/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @brief WebRTC side of this example: the live stream.
 *
 * The sink itself lives in components/kvs_webrtc/src/kvs_media.c, registered under the
 * name "webrtc" when transmission starts: its callback copies each frame onto a short
 * queue and returns, and the RTP send runs on the kvsGlobalVideo thread. This file only
 * drives the start/stop lifecycle.
 *
 * WEBRTC DOES NOT START AT BOOT. It used to, inline in app_main(), which meant the example
 * could not be run without talking to AWS - the camera, the preview and the detector all
 * came up behind a signaling connection that had to succeed first. Now the local pipeline
 * is independent and this is one console command: `start-webrtc`.
 *
 * Modelled on examples/kvs_combined/main/webrtc_side.c, minus its camera lease. That lease
 * exists there because nothing else holds the camera between peers, and on a UVC camera
 * the per-session teardown cannot re-acquire its buffers. Here the preview holds a
 * reference from boot for its own reasons, which covers the same ground - so a session
 * joining a running camera is the normal case rather than something to arrange.
 */

#include <inttypes.h>
#include <stdbool.h>
#include "esp_log.h"

#include "app_webrtc.h"
#include "kvs_signaling.h"
#include "kvs_peer_connection.h"
#include "media_stream.h"
#include "video_capture.h"

#include "app_ctrl.h"

static const char *TAG = "webrtc_side";

static bool s_running;
static bool s_event_cb_registered;


bool webrtc_side_is_running(void)
{
    return s_running;
}

static void app_webrtc_event_handler(app_webrtc_event_data_t *event_data, void *user_ctx)
{
    (void)user_ctx;

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
            ESP_LOGW(TAG, "[KVS Event] Signaling Disconnected.");
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
        case APP_WEBRTC_EVENT_ERROR:
        /* fall-through */
        case APP_WEBRTC_EVENT_SIGNALING_ERROR:
            ESP_LOGE(TAG, "[KVS Event] Error: %s",
                     event_data->peer_id ? event_data->peer_id : "(no peer)");
            break;
        default:
            ESP_LOGD(TAG, "[KVS Event] event %d", (int)event_data->event_id);
            break;
    }
}

esp_err_t webrtc_side_start(void)
{
    if (s_running) {
        ESP_LOGW(TAG, "WebRTC already running");
        return ESP_OK;
    }

    /* Registered before init so no event is missed, and only once - the callback outlives
     * a stop/start cycle. */
    if (!s_event_cb_registered) {
        if (app_webrtc_register_event_callback(app_webrtc_event_handler, NULL) != 0) {
            ESP_LOGE(TAG, "Failed to register KVS event callback");
        } else {
            s_event_cb_registered = true;
        }
    }

    /* kvs_media calls init/start on these itself and they are reference counted inside
     * media_stream, so this joins the camera the preview already has up rather than
     * bringing a second one online. kvs_media also registers its encoded sink there. */
    media_stream_video_capture_t *video_capture = media_stream_get_video_capture_if();
    media_stream_audio_capture_t *audio_capture = media_stream_get_audio_capture_if();
    media_stream_audio_player_t  *audio_player  = media_stream_get_audio_player_if();

    if (video_capture == NULL) {
        ESP_LOGE(TAG, "No video capture interface - this example needs a camera");
        return ESP_ERR_NOT_FOUND;
    }

    static kvs_signaling_config_t kvs_signaling_cfg = {0};
    kvs_signaling_cfg.pChannelName = CONFIG_AWS_KVS_CHANNEL_NAME;

#ifdef CONFIG_IOT_CORE_ENABLE_CREDENTIALS
    kvs_signaling_cfg.useIotCredentials         = true;
    kvs_signaling_cfg.iotCoreCredentialEndpoint = CONFIG_AWS_IOT_CORE_CREDENTIAL_ENDPOINT;
    kvs_signaling_cfg.iotCoreCert               = CONFIG_AWS_IOT_CORE_CERT;
    kvs_signaling_cfg.iotCorePrivateKey         = CONFIG_AWS_IOT_CORE_PRIVATE_KEY;
    kvs_signaling_cfg.iotCoreRoleAlias          = CONFIG_AWS_IOT_CORE_ROLE_ALIAS;
    kvs_signaling_cfg.iotCoreThingName          = CONFIG_AWS_IOT_CORE_THING_NAME;
#else
    /* From Kconfig, never hardcoded here. This block carried a literal access key, secret
     * and session token for a while: they went stale, and because they shadowed the
     * Kconfig values entirely, every request signed with them came back 403 while
     * menuconfig showed the right credentials. This file is git-tracked - a key pasted
     * here is a key committed. */
    kvs_signaling_cfg.useIotCredentials = false;
    kvs_signaling_cfg.awsAccessKey      = CONFIG_AWS_ACCESS_KEY_ID;
    kvs_signaling_cfg.awsSecretKey      = CONFIG_AWS_SECRET_ACCESS_KEY;
    kvs_signaling_cfg.awsSessionToken   = CONFIG_AWS_SESSION_TOKEN;
#endif
    kvs_signaling_cfg.awsRegion  = CONFIG_AWS_DEFAULT_REGION;
    kvs_signaling_cfg.caCertPath = "/spiffs/certs/cacert.pem";

    app_webrtc_config_t app_webrtc_config = APP_WEBRTC_CONFIG_DEFAULT();
    app_webrtc_config.signaling_client_if = kvs_signaling_client_if_get();
    app_webrtc_config.signaling_cfg       = &kvs_signaling_cfg;
    app_webrtc_config.peer_connection_if  = kvs_peer_connection_if_get();
    app_webrtc_config.video_capture       = video_capture;
    app_webrtc_config.audio_capture       = audio_capture;
    /* The LCD shows the local camera, so the inbound decode path is not wired up at all. */
    app_webrtc_config.video_player        = NULL;
    app_webrtc_config.audio_player        = audio_player;

    ESP_LOGI(TAG, "Starting WebRTC on channel '%s' (%s)",
             CONFIG_AWS_KVS_CHANNEL_NAME, CONFIG_AWS_DEFAULT_REGION);

    WEBRTC_STATUS status = app_webrtc_init(&app_webrtc_config);
    if (status != WEBRTC_STATUS_SUCCESS) {
        ESP_LOGE(TAG, "app_webrtc_init failed: 0x%08" PRIx32, (uint32_t)status);
        return ESP_FAIL;
    }
    app_webrtc_enable_media_reception(true);

    /* Non-blocking: app_webrtc_run() spawns the signaling and session threads and returns.
     * It must be, because this runs on the shared work queue that also serves
     * trigger-offer and the peer-connection setup app_webrtc dispatches onto it. */
    status = app_webrtc_run();
    if (status != WEBRTC_STATUS_SUCCESS) {
        ESP_LOGE(TAG, "app_webrtc_run failed: 0x%08" PRIx32, (uint32_t)status);
        app_webrtc_terminate();
        return ESP_FAIL;
    }

    s_running = true;
    ESP_LOGI(TAG, "WebRTC started");
    return ESP_OK;
}

esp_err_t webrtc_side_stop(void)
{
    if (!s_running) {
        ESP_LOGW(TAG, "WebRTC not running");
        return ESP_OK;
    }

    /* Tears down sessions and the signaling link and drops kvs_media's reference on the
     * capture pipeline. The camera keeps running: the preview still holds one. */
    WEBRTC_STATUS status = app_webrtc_terminate();
    if (status != WEBRTC_STATUS_SUCCESS) {
        ESP_LOGE(TAG, "app_webrtc_terminate failed: 0x%08" PRIx32, (uint32_t)status);
    }

    s_running = false;
    ESP_LOGI(TAG, "WebRTC stopped");
    return ESP_OK;
}
