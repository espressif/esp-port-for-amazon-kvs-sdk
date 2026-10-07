/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

int esp_cli_start();

/* Registers the diagnostic commands. esp_cli_start() calls it, and a console
 * that was torn down and restarted can call it again. */
int esp_cli_register_cmds();

#ifdef __cplusplus
}
#endif
