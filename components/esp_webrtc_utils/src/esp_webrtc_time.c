/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esp_log.h"
#include "esp_idf_version.h"

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#else
#include "esp_sntp.h"
#endif

#if ENABLE_STREAMING_ONLY && CONFIG_IDF_TARGET_ESP32P4
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "webrtc_bridge.h"
#include "bridge_cmd_defs.h"
#endif

static const char *TAG = "esp_webrtc_time";
static bool time_sync_done = false;

void time_sync_notification_cb(struct timeval *tv)
{
    ESP_LOGI(TAG, "Notification of a time synchronization event");
    time_sync_done = true;
}

bool kvswebrtc_is_time_sync_done()
{
    return time_sync_done;
}

static const char *server_list[] = {
    "stratum2.iad01.publicntp.org",
    "pool.ntp.org",
    "time.google.com",
    "time.cloudflare.com"
};
static const int num_servers = sizeof(server_list) / sizeof(server_list[0]);

#if ENABLE_STREAMING_ONLY && CONFIG_IDF_TARGET_ESP32P4 && CONFIG_ESP_WEBRTC_BRIDGE_HOSTED
#define TIME_SYNC_REF_SEC  (1722297600)  /* 1 July 2024 00:00:00 UTC */
#define TIME_SYNC_TIMEOUT_MS 5000

static SemaphoreHandle_t s_time_sem = NULL;
static struct timeval s_coproc_timeval = {0};
static volatile bool s_time_response_valid = false;

static void get_time_response_cb(uint32_t cmd_id, uint32_t req_id,
                                 esp_err_t status, uint8_t *resp_data, size_t resp_len)
{
    (void)cmd_id;
    (void)req_id;
    s_time_response_valid = false;
    if (status == ESP_OK && resp_data != NULL && resp_len >= sizeof(struct timeval)) {
        memcpy(&s_coproc_timeval, resp_data, sizeof(struct timeval));
        s_time_response_valid = true;
    }
    if (resp_data) free(resp_data);
    if (s_time_sem) xSemaphoreGive(s_time_sem);
}

static bool s_time_handler_registered = false;

static bool ensure_time_handler_registered(void)
{
    if (s_time_handler_registered) {
        return (s_time_sem != NULL);
    }
    s_time_sem = xSemaphoreCreateBinary();
    if (!s_time_sem) return false;
    if (bridge_cmd_register_response_handler(BRIDGE_CMD_GET_TIME, get_time_response_cb) != ESP_OK) {
        vSemaphoreDelete(s_time_sem);
        s_time_sem = NULL;
        return false;
    }
    s_time_handler_registered = true;
    return true;
}

void esp_webrtc_time_register_bridge_cmd_handlers(void)
{
    if (s_time_handler_registered) {
        return;
    }
    ensure_time_handler_registered();
}

static bool sync_time_from_coprocessor(struct timeval *out_tv)
{
    if (!ensure_time_handler_registered()) {
        return false;
    }

    s_time_response_valid = false;
    struct timeval time_before_send;
    gettimeofday(&time_before_send, NULL);

    esp_err_t err = bridge_cmd_send(BRIDGE_CMD_GET_TIME, NULL, 0);
    if (err != ESP_OK) return false;

    if (xSemaphoreTake(s_time_sem, pdMS_TO_TICKS(TIME_SYNC_TIMEOUT_MS)) != pdTRUE) {
        return false;
    }
    if (!s_time_response_valid || out_tv == NULL) return s_time_response_valid;

    struct timeval time_after_recv;
    gettimeofday(&time_after_recv, NULL);
    int rtt_us = (int)((time_after_recv.tv_sec - time_before_send.tv_sec) * 1000000 +
                       (time_after_recv.tv_usec - time_before_send.tv_usec));
    if (rtt_us < 0) rtt_us = 0;
    int correction_usec = rtt_us / 2;
    out_tv->tv_sec = s_coproc_timeval.tv_sec + correction_usec / 1000000;
    out_tv->tv_usec = s_coproc_timeval.tv_usec + correction_usec % 1000000;
    return true;
}
#endif

static void initialize_sntp(void)
{
#if ENABLE_STREAMING_ONLY && CONFIG_IDF_TARGET_ESP32P4 && CONFIG_ESP_WEBRTC_BRIDGE_HOSTED
    /* P4 streaming: get time from C6 (signaling) via bridge_cmd */
    struct timeval tv;
    for (int retry = 0; retry < 5; retry++) {
        if (!sync_time_from_coprocessor(&tv)) {
            ESP_LOGW(TAG, "Time sync from coprocessor failed, retry %d/5", retry + 1);
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }
        if (tv.tv_sec < TIME_SYNC_REF_SEC) {
            ESP_LOGI(TAG, "C6 time not yet valid (%ld), retrying...", (long)tv.tv_sec);
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }
        settimeofday(&tv, NULL);
        time_sync_done = true;
        ESP_LOGI(TAG, "Time synced from coprocessor");
        return;
    }
    ESP_LOGE(TAG, "Failed to sync time from coprocessor after 5 retries");
#endif

    ESP_LOGI(TAG, "Initializing SNTP");
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
    // First deinit in case of reinitialization
    esp_netif_sntp_deinit();

    esp_sntp_config_t config = {
        .smooth_sync = false,  // Changed to false for faster initial sync
        .server_from_dhcp = false,
        .wait_for_sync = true,
        .start = true,
        .sync_cb = time_sync_notification_cb,
        .renew_servers_after_new_IP = false,  // Don't change servers after IP
        .ip_event_to_renew = IP_EVENT_STA_GOT_IP,
        .index_of_first_server = 0,
        .num_of_servers = 1,  // Try one server at a time
        .servers = {
            server_list[0],  // Start with most reliable
        }
    };
    esp_netif_sntp_init(&config);
#else
    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    sntp_setservername(0, server_list[0]);
    sntp_set_time_sync_notification_cb(time_sync_notification_cb);
#ifdef CONFIG_SNTP_TIME_SYNC_METHOD_SMOOTH
    sntp_set_sync_mode(SNTP_SYNC_MODE_SMOOTH);
#endif
    sntp_init();
#endif
}

void esp_webrtc_time_sntp_time_sync_no_wait()
{
    if (time_sync_done){
        ESP_LOGI(TAG, "Time sync already done");
        return;
    }

    initialize_sntp();
}

void esp_webrtc_time_sntp_time_sync_and_wait()
{
    if (time_sync_done){
        ESP_LOGI(TAG, "Time sync already done");
        return;
    }

    initialize_sntp();

    // wait for time to be set
    int retry = 0;
    const int retry_count = num_servers * 3;
    const int retry_delay_ms = 2000;
    int current_server = 0;

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
    if (time_sync_done) {
        ESP_LOGI(TAG, "Time sync completed successfully");
        return;
    }
    while (retry < retry_count) {
        esp_err_t ret = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(retry_delay_ms));
        if (ret == ESP_OK) {
            time_sync_done = true;
            ESP_LOGI(TAG, "Time sync completed successfully");
            break;
        }
        retry++;
        ESP_LOGI(TAG, "Waiting for system time to be set... (%d/%d)", retry, retry_count);

        // Try next server after 3 failures
        if (retry % 3 == 0) {
            current_server = (current_server + 1) % num_servers;
            ESP_LOGW(TAG, "Switching to NTP server: %s", server_list[current_server]);

            esp_netif_sntp_deinit();
            esp_sntp_config_t new_config = {
                .smooth_sync = false,
                .server_from_dhcp = false,
                .wait_for_sync = true,
                .start = true,
                .sync_cb = time_sync_notification_cb,
                .renew_servers_after_new_IP = false,
                .ip_event_to_renew = IP_EVENT_STA_GOT_IP,
                .index_of_first_server = 0,
                .num_of_servers = 1,
                .servers = {
                    server_list[current_server],
                }
            };
            esp_netif_sntp_init(&new_config);
        }
    }
#else
    while (retry < retry_count) {
        if (sntp_get_sync_status() != SNTP_SYNC_STATUS_RESET) {
            time_sync_done = true;
            ESP_LOGI(TAG, "Time sync completed successfully");
            break;
        }
        retry++;
        ESP_LOGI(TAG, "Waiting for system time to be set... (%d/%d)", retry, retry_count);
        if (retry % 3 == 0) {
            current_server = (current_server + 1) % num_servers;
            ESP_LOGW(TAG, "Switching to NTP server: %s", server_list[current_server]);
            sntp_stop();
            vTaskDelay(pdMS_TO_TICKS(100));
            sntp_setservername(0, server_list[current_server]);
            sntp_set_time_sync_notification_cb(time_sync_notification_cb);
            sntp_init();
        }
        vTaskDelay(pdMS_TO_TICKS(retry_delay_ms));
    }
#endif
    if (!time_sync_done) {
        ESP_LOGE(TAG, "Failed to sync time after %d attempts", retry_count);
    }
}
