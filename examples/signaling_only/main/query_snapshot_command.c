/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * CLI command to capture a JPEG snapshot from the streaming device (P4)
 * using the bridge command framework.
 *
 * Usage:  query-snapshot [quality]
 *   quality: JPEG quality 1-100 (default: 80)
 *
 * Fire-and-forget send; response is handled by the registered response handler.
 * Snapshot size is printed when the response is received.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#include <esp_console.h>
#include <esp_log.h>

#include "webrtc_bridge.h"
#include "bridge_cmd_defs.h"

static const char *TAG = "query_snapshot_cmd";

/* Last quality used (for response handler output; single in-flight snapshot) */
static uint8_t s_last_quality = 80;

/**
 * Chunked response handler for snapshot data from P4.
 * Called once per chunk as it arrives; data is valid only during callback.
 */
static void snapshot_response_cb(uint32_t cmd_id, uint32_t req_id,
                                 esp_err_t status,
                                 const uint8_t *data, size_t len,
                                 uint32_t seq_num, uint32_t total_size,
                                 bool is_final)
{
    (void)cmd_id;
    (void)req_id;

    if (status != ESP_OK && is_final) {
        ESP_LOGE(TAG, "Snapshot request failed: %s", esp_err_to_name(status));
        return;
    }

    ESP_LOGI(TAG, "Snapshot chunk: seq=%" PRIu32 " len=%zu total=%" PRIu32 " is_final=%d",
             seq_num, len, total_size, is_final);

    if (is_final) {
        printf("Snapshot complete: %" PRIu32 " bytes (quality=%u)\n", total_size, s_last_quality);
    }
}

static int query_snapshot_cli_handler(int argc, char *argv[])
{
    uint8_t quality = 80;

    if (argc >= 2) {
        int q = atoi(argv[1]);
        if (q >= 1 && q <= 100) {
            quality = (uint8_t)q;
        } else {
            ESP_LOGW(TAG, "Invalid quality %d, using default %u", q, quality);
        }
    }

    s_last_quality = quality;
    ESP_LOGI(TAG, "Requesting JPEG snapshot from streaming device (quality=%u)...", quality);

    bridge_cmd_snapshot_req_t req = {
        .quality = quality,
    };

    esp_err_t err = bridge_cmd_send(BRIDGE_CMD_GET_SNAPSHOT,
                                    (const uint8_t *)&req, sizeof(req));

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send snapshot request: %s", esp_err_to_name(err));
        return -1;
    }

    printf("Request sent. Snapshot will be printed when the response is received.\n");
    return 0;
}

static esp_console_cmd_t query_snapshot_cmds[] = {
    {
        .command = "query-snapshot",
        .help = "Capture JPEG snapshot from streaming device (P4). Usage: query-snapshot [quality]",
        .hint = NULL,
        .func = query_snapshot_cli_handler,
    }
};

int query_snapshot_command_register_response_handler(void)
{
    return bridge_cmd_register_chunked_response_handler(BRIDGE_CMD_GET_SNAPSHOT,
                                                        snapshot_response_cb) == ESP_OK ? 0 : -1;
}

int query_snapshot_command_register_cli(void)
{
    int cmds_num = sizeof(query_snapshot_cmds) / sizeof(esp_console_cmd_t);
    for (int i = 0; i < cmds_num; i++) {
        ESP_LOGI(TAG, "Registering command: %s", query_snapshot_cmds[i].command);
        esp_err_t result = esp_console_cmd_register(&query_snapshot_cmds[i]);
        if (result != ESP_OK) {
            ESP_LOGE(TAG, "Failed to register command %s: %d",
                     query_snapshot_cmds[i].command, result);
            return -1;
        }
    }
    return 0;
}
