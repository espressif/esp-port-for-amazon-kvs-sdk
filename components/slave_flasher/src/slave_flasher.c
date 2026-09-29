/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esp_loader.h"
#include "esp32_port.h"
#include "slave_flasher.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "loader_utils.h"

#if CONFIG_SLAVE_FLASHER_IMAGE_SPIFFS
#include "esp_spiffs.h"
#endif
#include "esp_heap_caps.h"
#include "mbedtls/md.h"


static const char *TAG = "slave_flasher";

#define HIGHER_BAUDRATE 2000000
#define PARTITION_TABLE_ADDRESS  CONFIG_SLAVE_PARTITION_TABLE_ADDRESS
#define APPLICATION_ADDRESS      CONFIG_SLAVE_APPLICATION_ADDRESS




#define MD5_MAX_LEN     16
#define MD5_CHUNK_SIZE  4096
#define FLASH_BLOCK_SIZE 1024

typedef struct {
    const char *name;
    FILE *file;             /* NULL for a memory source */
    const uint8_t *data;
    size_t size;
    char path[48];
} image_src_t;

/* Return len bytes at off: in place for a memory source, else read into buf.
 * Callers read in order from off 0. */
static const uint8_t *image_read(image_src_t *src, size_t off, size_t len, uint8_t *buf)
{
    if (!src->file) {
        return src->data + off;
    }
    if (off == 0) {
        rewind(src->file);
    }
    size_t read = fread(buf, 1, len, src->file);
    if (read != len) {
        ESP_LOGE(TAG, "Failed to read %s (read %zu of %zu)", src->name, read, len);
        return NULL;
    }
    return buf;
}

static esp_err_t image_md5_hex(image_src_t *src, char digest_str[MD5_MAX_LEN * 2 + 1])
{
    uint8_t *chunk = NULL;
    if (src->file) {
        chunk = heap_caps_malloc(MD5_CHUNK_SIZE, MALLOC_CAP_SPIRAM);
        if (!chunk) {
            ESP_LOGE(TAG, "Failed to allocate MD5 buffer for %s", src->name);
            return ESP_ERR_NO_MEM;
        }
    }

    mbedtls_md_context_t ctx;
    unsigned char digest[MD5_MAX_LEN];
    esp_err_t ret = ESP_OK;
    mbedtls_md_init(&ctx);
    if (mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(MBEDTLS_MD_MD5), 0) != 0) {
        ESP_LOGE(TAG, "MD5 unavailable in mbedTLS config");
        ret = ESP_FAIL;
        goto out;
    }
    mbedtls_md_starts(&ctx);

    for (size_t off = 0; off < src->size; off += MD5_CHUNK_SIZE) {
        size_t len = MIN(src->size - off, MD5_CHUNK_SIZE);
        const uint8_t *p = image_read(src, off, len, chunk);
        if (!p) {
            ret = ESP_FAIL;
            goto out;
        }
        mbedtls_md_update(&ctx, p, len);
    }
    mbedtls_md_finish(&ctx, digest);

    for (int i = 0; i < MD5_MAX_LEN; i++) {
        sprintf(&digest_str[i * 2], "%02x", (unsigned int)digest[i]);
    }
    digest_str[MD5_MAX_LEN * 2] = '\0';

out:
    mbedtls_md_free(&ctx);
    free(chunk);
    return ret;
}

static esp_err_t image_flash(image_src_t *src, uint32_t addr)
{
    uint8_t *block = heap_caps_malloc(FLASH_BLOCK_SIZE, MALLOC_CAP_SPIRAM);
    if (!block) {
        ESP_LOGE(TAG, "Failed to allocate flash buffer for %s", src->name);
        return ESP_ERR_NO_MEM;
    }

    esp_loader_error_t err = esp_loader_flash_start(addr, src->size, FLASH_BLOCK_SIZE);
    if (err != ESP_LOADER_SUCCESS) {
        ESP_LOGE(TAG, "Failed to start flash operation: %d", err);
        free(block);
        return ESP_FAIL;
    }

    for (size_t off = 0; off < src->size; off += FLASH_BLOCK_SIZE) {
        size_t len = MIN(src->size - off, FLASH_BLOCK_SIZE);
        const uint8_t *p = image_read(src, off, len, block);
        if (!p) {
            free(block);
            return ESP_FAIL;
        }
        // esp_loader_flash_write() pads the buffer in place, so rodata needs a copy.
        if (p != block) {
            memcpy(block, p, len);
        }
        err = esp_loader_flash_write(block, len);
        if (err != ESP_LOADER_SUCCESS) {
            ESP_LOGE(TAG, "Failed to write flash chunk: %d", err);
            free(block);
            return ESP_FAIL;
        }
        printf("\rFlashing progress: %d%%", (int)(((float)(off + len) / src->size) * 100));
    }
    printf("\n");
    free(block);

    err = esp_loader_flash_verify();
    if (err != ESP_LOADER_SUCCESS) {
        ESP_LOGE(TAG, "Failed to finish flash operation: %d", err);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Successfully flashed %s", src->name);
    return ESP_OK;
}

static esp_err_t check_and_flash_image(image_src_t *src, uint32_t addr)
{
    if (src->size == 0) {
        ESP_LOGE(TAG, "File %s is empty", src->name);
        return ESP_ERR_INVALID_SIZE;
    }

    char digest_str[MD5_MAX_LEN * 2 + 1];
    esp_err_t ret = image_md5_hex(src, digest_str);
    if (ret != ESP_OK) {
        return ret;
    }
    ESP_LOGI(TAG, "Computed MD5 hash of %s: %s", src->name, digest_str);

    if (esp_loader_flash_verify_known_md5(addr, src->size, (uint8_t *)digest_str) == ESP_LOADER_SUCCESS) {
        ESP_LOGI(TAG, "%s MD5 match, skipping...", src->name);
        return ESP_OK;
    }
    ESP_LOGI(TAG, "%s MD5 mismatch, flashing...", src->name);
    return image_flash(src, addr);
}

#if CONFIG_SLAVE_FLASHER_IMAGE_EMBEDDED
#define EMBEDDED_IMAGE(sym) \
    extern const uint8_t _binary_##sym##_start[] asm("_binary_" #sym "_start"); \
    extern const uint8_t _binary_##sym##_end[] asm("_binary_" #sym "_end")
EMBEDDED_IMAGE(slave_bootloader_bin);
EMBEDDED_IMAGE(slave_partition_table_bin);
EMBEDDED_IMAGE(slave_app_bin);

static const struct {
    const char *name;
    const uint8_t *start;
    const uint8_t *end;
} s_embedded_images[] = {
    { "bootloader.bin",      _binary_slave_bootloader_bin_start,      _binary_slave_bootloader_bin_end },
    { "partition-table.bin", _binary_slave_partition_table_bin_start, _binary_slave_partition_table_bin_end },
    { "app.bin",             _binary_slave_app_bin_start,             _binary_slave_app_bin_end },
};

static esp_err_t image_open(const char *name, image_src_t *src)
{
    for (size_t i = 0; i < sizeof(s_embedded_images) / sizeof(s_embedded_images[0]); i++) {
        if (strcmp(s_embedded_images[i].name, name) == 0) {
            *src = (image_src_t) {
                .name = s_embedded_images[i].name,
                .data = s_embedded_images[i].start,
                .size = (size_t)(s_embedded_images[i].end - s_embedded_images[i].start),
            };
            return ESP_OK;
        }
    }
    ESP_LOGE(TAG, "No embedded image %s", name);
    return ESP_ERR_NOT_FOUND;
}

static void image_close(image_src_t *src)
{
}

static esp_err_t image_source_init(void)
{
    ESP_LOGI(TAG, "Using embedded slave image (app %u bytes)",
             (unsigned)(_binary_slave_app_bin_end - _binary_slave_app_bin_start));
    return ESP_OK;
}
#else
static esp_err_t image_open(const char *name, image_src_t *src)
{
    *src = (image_src_t) { 0 };
    snprintf(src->path, sizeof(src->path), "/target_fw/%s", name);
    src->name = src->path;
    src->file = fopen(src->path, "rb");
    if (src->file == NULL) {
        ESP_LOGE(TAG, "Failed to open %s", src->path);
        return ESP_FAIL;
    }
    fseek(src->file, 0, SEEK_END);
    src->size = ftell(src->file);
    return ESP_OK;
}

static void image_close(image_src_t *src)
{
    fclose(src->file);
}

static esp_err_t image_source_init(void)
{
    ESP_LOGI(TAG, "Initializing SPIFFS");

    esp_vfs_spiffs_conf_t conf = {
      .base_path = "/target_fw",
      .partition_label = "slave",
      .max_files = 5,
      .format_if_mount_failed = false
    };

    // Use settings defined above to initialize and mount SPIFFS filesystem.
    // Note: esp_vfs_spiffs_register is an all-in-one convenience function.
    esp_err_t ret = esp_vfs_spiffs_register(&conf);

    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount or format filesystem");
        } else if (ret == ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG, "Failed to find SPIFFS partition");
        } else {
            ESP_LOGE(TAG, "Failed to initialize SPIFFS (%s)", esp_err_to_name(ret));
        }
        return ESP_FAIL;
    }

    size_t total = 0, used = 0;
    ret = esp_spiffs_info(conf.partition_label, &total, &used);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get SPIFFS partition information (%s)", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "Partition size: total: %d, used: %d", total, used);
    }
    return ESP_OK;
}
#endif

/* Flashing is done: give back the UART and the boot pin, so the host drives
 * nothing on them while the co-processor runs, sleeps or wakes. The reset pin
 * stays with esp_hosted, which configures it itself. */
static void release_loader(void)
{
    loader_port_esp32_deinit();
    gpio_reset_pin(CONFIG_SLAVE_GPIO0_TRIGGER_PIN);
}

esp_err_t flash_slave()
{
    esp_err_t ret = image_source_init();
    if (ret != ESP_OK) {
        return ret;
    }

/* esp_hosted 3.x names the co-processor reset pin ESP_HOSTED_HOST_RESET_GPIO.
 * webrtc_person_detect still builds against the in-tree esp_hosted 2.x, which
 * names it ESP_HOSTED_GPIO_SLAVE_RESET_SLAVE. */
#if defined(CONFIG_ESP_HOSTED_HOST_RESET_GPIO)
#define SLAVE_RESET_GPIO CONFIG_ESP_HOSTED_HOST_RESET_GPIO
#else
#define SLAVE_RESET_GPIO CONFIG_ESP_HOSTED_GPIO_SLAVE_RESET_SLAVE
#endif

    const loader_esp32_config_t config = {
        .baud_rate = 115200,
        .uart_port = UART_NUM_1,
        .uart_rx_pin = CONFIG_SLAVE_UART_RX_PIN,
        .uart_tx_pin = CONFIG_SLAVE_UART_TX_PIN,
        .reset_trigger_pin = SLAVE_RESET_GPIO,
        .gpio0_trigger_pin = CONFIG_SLAVE_GPIO0_TRIGGER_PIN,
    };

    if (loader_port_esp32_init(&config) != ESP_LOADER_SUCCESS) {
        ESP_LOGE(TAG, "serial initialization failed.");
        return ESP_FAIL;
    }

    if (connect_to_target(HIGHER_BAUDRATE) == ESP_LOADER_SUCCESS) {

        const struct {
            const char *name;
            uint32_t addr;
        } images[] = {
            { "bootloader.bin",      get_bootloader_address(esp_loader_get_target()) },
            { "partition-table.bin", PARTITION_TABLE_ADDRESS },
            { "app.bin",             APPLICATION_ADDRESS },
        };

        for (size_t i = 0; i < sizeof(images) / sizeof(images[0]); i++) {
            image_src_t src;
            ret = image_open(images[i].name, &src);
            if (ret == ESP_OK) {
                ret = check_and_flash_image(&src, images[i].addr);
                image_close(&src);
            }
            if (ret != ESP_OK) {
                release_loader();
                return ret;
            }
        }
        ESP_LOGI(TAG, "Done!");
        esp_loader_reset_target();

        // Delay for skipping the boot message of the targets
        vTaskDelay(500 / portTICK_PERIOD_MS);

    } else {
        /* connect_to_target() drives reset together with GPIO0 to force serial
         * download mode. A co-processor that does not answer -- it is in light
         * sleep with its UART powered down, say -- is then left sitting in the
         * bootloader, so it never runs its app and SDIO never enumerates. Reset
         * it without GPIO0 so it boots normally. */
        ESP_LOGW(TAG, "co-processor did not answer the loader; resetting it to run its app");
        esp_loader_reset_target();
        vTaskDelay(500 / portTICK_PERIOD_MS);
    }
    release_loader();

#if CONFIG_SLAVE_CONSOLE_ENABLE
    slave_console_attach();
#endif
    return ESP_OK;
}