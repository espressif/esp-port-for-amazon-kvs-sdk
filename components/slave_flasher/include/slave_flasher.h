/* Flash multiple partitions if MD5 mismatch example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <sys/param.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include "esp_err.h"
#include "esp_log.h"
#include "driver/uart.h"
#include "driver/gpio.h"

esp_err_t flash_slave(void);

/* Forward the co-processor console ([SLAVE] log, write_slave) without touching its
 * reset or GPIO0 lines. Idempotent; flash_slave() calls it when done. */
esp_err_t slave_console_attach(void);

/* Write a line to the co-processor console. */
int slave_write_string(const char *str);