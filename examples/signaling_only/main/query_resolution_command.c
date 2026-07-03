/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * CLI command to query the camera resolution from the streaming device (P4)
 * using the bridge command framework.
 *
 * Usage:  query-resolution
 * Fire-and-forget send; response is handled by the registered response handler.
 * Resolution is printed when the response is received.
 */

#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>

#include <esp_console.h>
#include <esp_log.h>

#include "webrtc_bridge.h"
#include "bridge_cmd_defs.h"

static const char *TAG = "query_resolution_cmd";

static void resolution_response_cb(uint32_t cmd_id, uint32_t req_id,
                                    esp_err_t status, uint8_t *resp_data, size_t resp_len)
{
    (void)cmd_id;
    (void)req_id;

    if (status != ESP_OK) {
        ESP_LOGE(TAG, "Resolution request failed: %s", esp_err_to_name(status));
        return;
    }

    if (resp_len != sizeof(bridge_cmd_resolution_t)) {
        ESP_LOGE(TAG, "Unexpected response size: %zu (expected %zu)",
                 resp_len, sizeof(bridge_cmd_resolution_t));
        free(resp_data);
        return;
    }

    bridge_cmd_resolution_t *res = (bridge_cmd_resolution_t *)resp_data;
    ESP_LOGI(TAG, "Camera resolution: %" PRIu32 "x%" PRIu32, res->width, res->height);
    printf("Resolution: %" PRIu32 "x%" PRIu32 "\n", res->width, res->height);
    free(resp_data);
}

static int query_resolution_cli_handler(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    ESP_LOGI(TAG, "Querying camera resolution from streaming device...");

    esp_err_t err = bridge_cmd_send(BRIDGE_CMD_GET_RESOLUTION, NULL, 0);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send resolution request: %s", esp_err_to_name(err));
        return -1;
    }

    printf("Request sent. Resolution will be printed when the response is received.\n");
    return 0;
}

static esp_console_cmd_t query_resolution_cmds[] = {
    {
        .command = "query-resolution",
        .help = "Query camera resolution from the streaming device (P4)",
        .hint = NULL,
        .func = query_resolution_cli_handler,
    }
};

int query_resolution_command_register_response_handler(void)
{
    return bridge_cmd_register_response_handler(BRIDGE_CMD_GET_RESOLUTION,
                                                resolution_response_cb) == ESP_OK ? 0 : -1;
}

int query_resolution_command_register_cli(void)
{
    int cmds_num = sizeof(query_resolution_cmds) / sizeof(esp_console_cmd_t);
    for (int i = 0; i < cmds_num; i++) {
        ESP_LOGI(TAG, "Registering command: %s", query_resolution_cmds[i].command);
        esp_err_t result = esp_console_cmd_register(&query_resolution_cmds[i]);
        if (result != ESP_OK) {
            ESP_LOGE(TAG, "Failed to register command %s: %d",
                     query_resolution_cmds[i].command, result);
            return -1;
        }
    }
    return 0;
}
