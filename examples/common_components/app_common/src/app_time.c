/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "app_time.h"

#if CONFIG_ESP_WEBRTC_BRIDGE_HOSTED
#include "freertos/semphr.h"
#include "webrtc_bridge.h"
#include "bridge_cmd_defs.h"
#endif

static const char *TAG = "app_time";
static bool time_sync_done = false;

static void time_sync_notification_cb(struct timeval *tv)
{
    ESP_LOGI(TAG, "Notification of a time synchronization event");
    time_sync_done = true;
}

static const char *server_list[] = {
    "stratum2.iad01.publicntp.org",
    "pool.ntp.org",
    "time.google.com",
    "time.cloudflare.com"
};
static const int num_servers = sizeof(server_list) / sizeof(server_list[0]);

/* Start SNTP with one server; esp_netif_sntp_init() replaces any earlier one. */
static void start_sntp(const char *server)
{
    esp_netif_sntp_deinit();

    esp_sntp_config_t config = {
        .smooth_sync = false,  /* faster initial sync */
        .server_from_dhcp = false,
        .wait_for_sync = true,
        .start = true,
        .sync_cb = time_sync_notification_cb,
        .renew_servers_after_new_IP = false,
        .ip_event_to_renew = IP_EVENT_STA_GOT_IP,
        .index_of_first_server = 0,
        .num_of_servers = 1,
        .servers = { server },
    };
    esp_netif_sntp_init(&config);
}

void app_time_sync_no_wait(void)
{
    if (time_sync_done) {
        ESP_LOGI(TAG, "Time sync already done");
        return;
    }
    ESP_LOGI(TAG, "Initializing SNTP");
    start_sntp(server_list[0]);
}

void app_time_sync_and_wait(void)
{
    if (time_sync_done) {
        ESP_LOGI(TAG, "Time sync already done");
        return;
    }
    ESP_LOGI(TAG, "Initializing SNTP");
    start_sntp(server_list[0]);

    const int retry_count = num_servers * 3;
    const int retry_delay_ms = 2000;
    int current_server = 0;

    for (int retry = 1; retry <= retry_count && !time_sync_done; retry++) {
        if (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(retry_delay_ms)) == ESP_OK) {
            time_sync_done = true;
            break;
        }
        ESP_LOGI(TAG, "Waiting for system time to be set... (%d/%d)", retry, retry_count);

        /* Try the next server after 3 failures */
        if (retry % 3 == 0) {
            current_server = (current_server + 1) % num_servers;
            ESP_LOGW(TAG, "Switching to NTP server: %s", server_list[current_server]);
            start_sntp(server_list[current_server]);
        }
    }

    if (time_sync_done) {
        ESP_LOGI(TAG, "Time sync completed successfully");
    } else {
        ESP_LOGE(TAG, "Failed to sync time after %d attempts", retry_count);
    }
}

#if CONFIG_ESP_WEBRTC_BRIDGE_HOSTED
/*
 * Time from the co-processor, for the streaming host of a split pair (the P4 in
 * streaming_only). The host has no SNTP of its own. The co-processor
 * (signaling_only) syncs over SNTP and answers BRIDGE_CMD_GET_TIME in
 * signaling_bridge_adapter. See app_time_sync_from_coprocessor() in app_time.h.
 */
#define TIME_SYNC_REF_SEC     (1722297600)  /* 1 July 2024 00:00:00 UTC */
#define TIME_SYNC_TIMEOUT_MS  5000
#define TIME_SYNC_RETRIES     5

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
    free(resp_data);
    if (s_time_sem) {
        xSemaphoreGive(s_time_sem);
    }
}

static bool ensure_time_handler_registered(void)
{
    if (s_time_sem) {
        return true;
    }
    s_time_sem = xSemaphoreCreateBinary();
    if (!s_time_sem) {
        return false;
    }
    if (bridge_cmd_register_response_handler(BRIDGE_CMD_GET_TIME, get_time_response_cb) != ESP_OK) {
        vSemaphoreDelete(s_time_sem);
        s_time_sem = NULL;
        return false;
    }
    return true;
}

/* Ask the co-processor for its time once; correct for half the round trip. */
static bool request_coprocessor_time(struct timeval *out_tv)
{
    s_time_response_valid = false;
    struct timeval before;
    gettimeofday(&before, NULL);

    if (bridge_cmd_send(BRIDGE_CMD_GET_TIME, NULL, 0) != ESP_OK) {
        return false;
    }
    if (xSemaphoreTake(s_time_sem, pdMS_TO_TICKS(TIME_SYNC_TIMEOUT_MS)) != pdTRUE ||
            !s_time_response_valid) {
        return false;
    }

    struct timeval after;
    gettimeofday(&after, NULL);
    int64_t rtt_us = (int64_t)(after.tv_sec - before.tv_sec) * 1000000 +
                     (after.tv_usec - before.tv_usec);
    if (rtt_us < 0) {
        rtt_us = 0;
    }
    int64_t us = (int64_t)s_coproc_timeval.tv_usec + rtt_us / 2;
    out_tv->tv_sec = s_coproc_timeval.tv_sec + us / 1000000;
    out_tv->tv_usec = us % 1000000;
    return true;
}

esp_err_t app_time_sync_from_coprocessor(void)
{
    if (!ensure_time_handler_registered()) {
        ESP_LOGE(TAG, "Failed to register the GET_TIME response handler");
        return ESP_FAIL;
    }

    for (int retry = 1; retry <= TIME_SYNC_RETRIES; retry++) {
        struct timeval tv;
        if (!request_coprocessor_time(&tv)) {
            ESP_LOGW(TAG, "Time sync from the co-processor failed, retry %d/%d", retry, TIME_SYNC_RETRIES);
        } else if (tv.tv_sec < TIME_SYNC_REF_SEC) {
            /* The co-processor has not synced over SNTP yet */
            ESP_LOGI(TAG, "Co-processor time not valid yet (%lld), retry %d/%d",
                     (long long)tv.tv_sec, retry, TIME_SYNC_RETRIES);
        } else {
            settimeofday(&tv, NULL);
            time_sync_done = true;
            ESP_LOGI(TAG, "Time synced from the co-processor");
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
    ESP_LOGE(TAG, "Failed to sync time from the co-processor after %d tries", TIME_SYNC_RETRIES);
    return ESP_ERR_TIMEOUT;
}
#endif /* CONFIG_ESP_WEBRTC_BRIDGE_HOSTED */
