/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Linux-target webrtc_classic (KVS master): credentials and channel from
 * environment variables, media from file_capture's sample frames. No NVS,
 * Wi-Fi provisioning or SNTP: the host provides networking and time.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

#include <sys/stat.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_webrtc.h"
#include "kvs_signaling.h"
#include "kvs_peer_connection.h"
#include "esp_log.h"
#include "file_capture.h"

static const char *TAG = "linux_test";

static volatile sig_atomic_t s_should_exit = 0;

static void on_signal(int sig)
{
    s_should_exit = 1;
    signal(sig, SIG_DFL);   /* a second Ctrl+C force-quits */
}

static const char *getenv_or_die(const char *name)
{
    const char *val = getenv(name);
    if (!val || !*val) {
        fprintf(stderr, "linux_test: required env var %s not set\n", name);
        exit(2);
    }
    return val;
}

static void event_handler(app_webrtc_event_data_t *event_data, void *user_ctx)
{
    (void) user_ctx;
    if (!event_data) {
        return;
    }
    switch (event_data->event_id) {
        case APP_WEBRTC_EVENT_INITIALIZED:
            ESP_LOGI(TAG, "[KVS Event] WebRTC initialized");
            break;
        case APP_WEBRTC_EVENT_SIGNALING_CONNECTING:
            ESP_LOGI(TAG, "[KVS Event] Signaling connecting");
            break;
        case APP_WEBRTC_EVENT_SIGNALING_CONNECTED:
            ESP_LOGI(TAG, "[KVS Event] Signaling connected");
            break;
        case APP_WEBRTC_EVENT_SIGNALING_DISCONNECTED:
            ESP_LOGI(TAG, "[KVS Event] Signaling disconnected");
            break;
        case APP_WEBRTC_EVENT_PEER_CONNECTION_REQUESTED:
            ESP_LOGI(TAG, "[KVS Event] Peer connection requested");
            break;
        case APP_WEBRTC_EVENT_PEER_CONNECTED:
            ESP_LOGI(TAG, "[KVS Event] Peer connected: %s", event_data->peer_id);
            break;
        case APP_WEBRTC_EVENT_PEER_DISCONNECTED:
            ESP_LOGI(TAG, "[KVS Event] Peer disconnected: %s", event_data->peer_id);
            break;
        case APP_WEBRTC_EVENT_RECEIVED_OFFER:
            ESP_LOGI(TAG, "[KVS Event] Received offer");
            break;
        case APP_WEBRTC_EVENT_SENT_ANSWER:
            ESP_LOGI(TAG, "[KVS Event] Sent answer");
            break;
        case APP_WEBRTC_EVENT_ERROR:
        case APP_WEBRTC_EVENT_SIGNALING_ERROR:
        case APP_WEBRTC_EVENT_PEER_CONNECTION_FAILED:
            ESP_LOGE(TAG, "[KVS Event] Error %d: %d %s",
                     (int) event_data->event_id,
                     (int) event_data->status_code,
                     event_data->message);
            break;
        default:
            ESP_LOGI(TAG, "[KVS Event] Other event %d", (int) event_data->event_id);
            break;
    }
}

#ifdef __cplusplus
extern "C" {
#endif
void app_main(void);
#ifdef __cplusplus
}
#endif

void app_main(void)
{
    /* SIGINT only: the POSIX port blocks every other signal in all threads */
    signal(SIGINT, on_signal);

    printf("linux_test: espressif-port KVS WebRTC SDK Linux-target master\n");

    const char *channel = getenv_or_die("KVS_CHANNEL_NAME");
    const char *region  = getenv("AWS_DEFAULT_REGION");
    if (!region || !*region) region = "us-west-2";

    const char *access_key   = getenv_or_die("AWS_ACCESS_KEY_ID");
    const char *secret_key   = getenv_or_die("AWS_SECRET_ACCESS_KEY");
    const char *session_tok  = getenv("AWS_SESSION_TOKEN");

    ESP_LOGI(TAG, "channel=%s region=%s", channel, region);

    if (app_webrtc_register_event_callback(event_handler, NULL) != 0) {
        ESP_LOGE(TAG, "register_event_callback failed");
        exit(1);
    }

    static kvs_signaling_config_t sig = {0};
    sig.pChannelName = (char *) channel;
    sig.useIotCredentials = false;
    sig.awsAccessKey = (char *) access_key;
    sig.awsSecretKey = (char *) secret_key;
    sig.awsSessionToken = session_tok ? (char *) session_tok : (char *) "";
    sig.awsRegion = (char *) region;
    /* KVS_CA_CERT_PATH, else the first system CA bundle found (Debian/Ubuntu, macOS, Fedora/RHEL) */
    const char *ca_path = getenv("KVS_CA_CERT_PATH");
    if (!ca_path || !*ca_path) {
        struct stat st;
        if (stat("/etc/ssl/certs/ca-certificates.crt", &st) == 0) {
            ca_path = "/etc/ssl/certs/ca-certificates.crt";
        } else if (stat("/etc/ssl/cert.pem", &st) == 0) {
            ca_path = "/etc/ssl/cert.pem";
        } else if (stat("/etc/pki/tls/certs/ca-bundle.crt", &st) == 0) {
            ca_path = "/etc/pki/tls/certs/ca-bundle.crt";
        }
    }
    sig.caCertPath = (char *) ca_path;
    if (ca_path) {
        ESP_LOGI(TAG, "Using CA bundle: %s", ca_path);
    } else {
        ESP_LOGW(TAG, "No CA bundle found — TLS handshake will likely fail");
    }

    app_webrtc_config_t cfg = APP_WEBRTC_CONFIG_DEFAULT();
    cfg.signaling_client_if = kvs_signaling_client_if_get();
    cfg.signaling_cfg = &sig;
    cfg.peer_connection_if = kvs_peer_connection_if_get();

    /* The SDK's H.264 + Opus sample frames; KVS_FRAMES_DIR overrides "samples" (relative to cwd) */
    const char *frames_dir = getenv("KVS_FRAMES_DIR");
    if (frames_dir && *frames_dir) {
        file_capture_set_frames_dir(frames_dir);
    }
    cfg.video_capture = file_capture_get_video_if();
    cfg.audio_capture = file_capture_get_audio_if();

    ESP_LOGI(TAG, "Initializing WebRTC (master role)");
    WEBRTC_STATUS rc = app_webrtc_init(&cfg);
    if (rc != WEBRTC_STATUS_SUCCESS) {
        ESP_LOGE(TAG, "app_webrtc_init failed: 0x%08x", (unsigned) rc);
        exit(1);
    }

    rc = app_webrtc_run();
    if (rc != WEBRTC_STATUS_SUCCESS) {
        ESP_LOGE(TAG, "app_webrtc_run failed: 0x%08x", (unsigned) rc);
        app_webrtc_terminate();
        exit(1);
    }
    ESP_LOGI(TAG, "linux_test: WebRTC running. Press Ctrl+C to exit.");

    /* Run for TEST_DURATION_SEC (default 60 s, 0 = until Ctrl+C) */
    long duration = 60;
    const char *dur_env = getenv("TEST_DURATION_SEC");
    if (dur_env && *dur_env) {
        char *end = NULL;
        duration = strtol(dur_env, &end, 10);
        if (*end != '\0' || duration < 0) {
            ESP_LOGW(TAG, "Ignoring invalid TEST_DURATION_SEC='%s'", dur_env);
            duration = 60;
        }
    }
    /* vTaskDelay, not sleep(): app_main is a task and the tick signal cuts a raw sleep short */
    for (long i = 0; (duration == 0 || i < duration) && !s_should_exit; ++i) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    ESP_LOGI(TAG, "linux_test: shutting down");
    app_webrtc_terminate();
    /* Linux IDF target keeps FreeRTOS spinning otherwise. */
    exit(0);
}
