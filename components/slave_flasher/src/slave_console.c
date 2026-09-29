/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* Co-processor console over the host's UART: forwards its log behind a [SLAVE]
 * prefix and offers write_slave. Independent of flashing, so a board that
 * programs its co-processor some other way keeps the console. */

#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_console.h"
#include "argtable3/argtable3.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "slave_flasher.h"

static const char *TAG = "slave_console";

#define BUF_LEN 128
#define SLAVE_BUFFER_SIZE 512
static uint8_t buf[BUF_LEN] = {0};
static char slave_line_buffer[SLAVE_BUFFER_SIZE] = {0};
static size_t slave_line_pos = 0;
static bool s_attached;

/**
 * CLI command argument structure for write_slave command
 */
static struct {
    struct arg_str *data;
    struct arg_end *end;
} write_slave_args;

/**
 * Write data to slave console via UART
 * @param data Pointer to data buffer
 * @param len Length of data to write
 * @return Number of bytes written, or negative on error
 */
static int slave_write(const uint8_t *data, size_t len)
{
    if (!data || len == 0) {
        return -1;
    }

    /* TX is connected only while writing: a co-processor with its own USB-UART
     * bridge drives the same line, and two drivers must not fight over it. */
    uart_set_pin(UART_NUM_1, CONFIG_SLAVE_UART_TX_PIN, UART_PIN_NO_CHANGE,
                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    int written = uart_write_bytes(UART_NUM_1, (const char *)data, len);
    if (written > 0) {
        uart_wait_tx_done(UART_NUM_1, portMAX_DELAY);
    }
    gpio_reset_pin(CONFIG_SLAVE_UART_TX_PIN);
    return written;
}

/**
 * Write a string to slave console via UART
 * @param str Null-terminated string to write
 * @return Number of bytes written, or negative on error
 */
int slave_write_string(const char *str)
{
    if (!str) {
        return -1;
    }
    return slave_write((const uint8_t *)str, strlen(str));
}

/**
 * CLI command handler: write_slave
 * Writes data to the slave device console
 */
static int cmd_write_slave(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&write_slave_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, write_slave_args.end, argv[0]);
        return 1;
    }

    const char *data = write_slave_args.data->sval[0];
    if (!data) {
        printf("Error: No data provided\n");
        return 1;
    }

    int written = slave_write_string(data);
    if (written < 0) {
        printf("Error: Failed to write to slave\n");
        return 1;
    }

    // Optionally add newline if not present
    size_t len = strlen(data);
    if (len == 0 || data[len - 1] != '\n') {
        slave_write_string("\n");
    }

    /* Print the count only: the data can carry a Wi-Fi password (wifi-set). */
    printf("Sent %d bytes to slave\n", written);
    return 0;
}

static void slave_print_line(const char* line)
{
    if (!line) return;

    // Skip leading whitespace
    while (*line == ' ' || *line == '\t') line++;

    if (*line != '\0') {
        printf("\033[1;33m[SLAVE]\033[0m %s\n", line);
        fflush(stdout);
    }
}

static void slave_monitor(void *arg)
{
    slave_line_pos = 0;

    while (1) {
        int rxBytes = uart_read_bytes(UART_NUM_1, buf, BUF_LEN - 1, 100 / portTICK_PERIOD_MS);
        if (rxBytes <= 0) continue;

        buf[rxBytes] = '\0';

        // Process received data line by line
        for (char *p = (char *)buf; *p; ) {
            char *line_end = strpbrk(p, "\n\r");

            if (line_end) {
                // Complete line found - append to buffer and print
                size_t chunk_len = line_end - p;
                if (slave_line_pos + chunk_len < SLAVE_BUFFER_SIZE) {
                    memcpy(&slave_line_buffer[slave_line_pos], p, chunk_len);
                    slave_line_buffer[slave_line_pos + chunk_len] = '\0';
                    slave_print_line(slave_line_buffer);  // slave_print_line now handles newline
                    slave_line_pos = 0;
                }
                p = line_end + 1;  // Skip line break
            } else {
                // Partial line - append remainder to buffer
                size_t remaining = strlen(p);
                if (slave_line_pos + remaining >= SLAVE_BUFFER_SIZE - 1) {
                    // Buffer overflow - print and reset
                    slave_line_buffer[SLAVE_BUFFER_SIZE - 1] = '\0';
                    slave_print_line(slave_line_buffer);
                    slave_line_pos = 0;
                }
                memcpy(&slave_line_buffer[slave_line_pos], p, remaining);
                slave_line_pos += remaining;
                slave_line_buffer[slave_line_pos] = '\0';
                break;
            }
        }
    }
}

/**
 * Register CLI commands for slave console interaction
 */
static void register_slave_commands(void)
{
    // Register write_slave command
    write_slave_args.data = arg_str1(NULL, NULL, "<data>", "Data string to write to slave device");
    write_slave_args.end = arg_end(1);

    const esp_console_cmd_t write_slave_cmd = {
        .command = "write_slave",
        .help = "Write data string to slave device console",
        .hint = NULL,
        .func = &cmd_write_slave,
        .argtable = &write_slave_args
    };

    ESP_ERROR_CHECK(esp_console_cmd_register(&write_slave_cmd));
    ESP_LOGI(TAG, "Registered CLI command: write_slave");
}

esp_err_t slave_console_attach(void)
{
    if (s_attached) {
        return ESP_OK;
    }
    if (!uart_is_driver_installed(UART_NUM_1)) {
        const uart_config_t cfg = {
            .baud_rate = 115200,
            .data_bits = UART_DATA_8_BITS,
            .parity = UART_PARITY_DISABLE,
            .stop_bits = UART_STOP_BITS_1,
            .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
            .source_clk = UART_SCLK_DEFAULT,
        };
        esp_err_t err = uart_driver_install(UART_NUM_1, BUF_LEN * 2, 0, 0, NULL, 0);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "uart install failed (%s)", esp_err_to_name(err));
            return err;
        }
        uart_param_config(UART_NUM_1, &cfg);
    } else {
        /* The flasher left it at its loader baud rate. */
        uart_flush_input(UART_NUM_1);
        uart_set_baudrate(UART_NUM_1, 115200);
    }
    /* RX only; TX is connected per write (see slave_write). */
    uart_set_pin(UART_NUM_1, UART_PIN_NO_CHANGE, CONFIG_SLAVE_UART_RX_PIN,
                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    gpio_reset_pin(CONFIG_SLAVE_UART_TX_PIN);
    xTaskCreate(slave_monitor, "slave_monitor", 2048, NULL, configMAX_PRIORITIES - 1, NULL);
    register_slave_commands();
    s_attached = true;
    return ESP_OK;
}
