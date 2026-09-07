/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 *
 * Linux IDF-target implementation of `flash_wrapper.h` and a host
 * `esp_timer_get_time()`.
 *
 * The real `flash_wrapper.c` (esp32 / s3 / p4 / etc.) spawns an
 * internal-RAM dispatcher task, gates flash + NVS access through
 * it, and supports both regular files (SPIFFS / FAT) and NVS keys.
 * On the Linux IDF target there's no SPIFFS / NVS / cache-disable
 * concern — ordinary libc fopen/fread/fseek do exactly what the
 * dispatcher does on real hardware. Same shape, plain libc.
 *
 * The NVS path (paths starting with `/nvs/...`) is rejected — Linux
 * IDF target doesn't have nvs_flash, and our examples that hit
 * Linux only use regular file paths anyway. If a future use case
 * needs NVS-on-Linux, back it with a small JSON or sqlite store.
 *
 * Before ESP-IDF 6.2 the linux target's esp_timer is headers-only, so
 * `esp_timer_get_time()` is provided here too.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <time.h>
#include <errno.h>
#include "esp_idf_version.h"
#include <sys/stat.h>

#include "esp_err.h"
#include "esp_log.h"
#include "flash_wrapper.h"

#define TAG "flash_wrapper_linux"

static bool path_is_nvs(const char *path)
{
    return path && strncmp(path, "/nvs/", 5) == 0;
}

esp_err_t flash_wrapper_init(void)
{
    return ESP_OK;
}

esp_err_t flash_wrapper_deinit(void)
{
    return ESP_OK;
}

esp_err_t flash_wrapper_get_size(const char *path, size_t *out_size)
{
    if (!path || !out_size) {
        return ESP_ERR_INVALID_ARG;
    }
    if (path_is_nvs(path)) {
        ESP_LOGW(TAG, "NVS paths not supported on linux target: %s", path);
        return ESP_ERR_NOT_SUPPORTED;
    }
    struct stat st;
    if (stat(path, &st) != 0) {
        return ESP_ERR_NOT_FOUND;
    }
    *out_size = (size_t) st.st_size;
    return ESP_OK;
}

esp_err_t flash_wrapper_read(const char *path, void *buf, size_t size, size_t offset)
{
    if (!path || !buf) {
        return ESP_ERR_INVALID_ARG;
    }
    if (path_is_nvs(path)) {
        ESP_LOGW(TAG, "NVS paths not supported on linux target: %s", path);
        return ESP_ERR_NOT_SUPPORTED;
    }
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        return ESP_ERR_NOT_FOUND;
    }
    if (offset > 0 && fseek(fp, (long) offset, SEEK_SET) != 0) {
        fclose(fp);
        return ESP_FAIL;
    }
    size_t n = fread(buf, 1, size, fp);
    fclose(fp);
    return (n == size) ? ESP_OK : ESP_FAIL;
}

esp_err_t flash_wrapper_write(const char *path, const void *buf, size_t size)
{
    if (!path || !buf) {
        return ESP_ERR_INVALID_ARG;
    }
    if (path_is_nvs(path)) {
        ESP_LOGW(TAG, "NVS paths not supported on linux target: %s", path);
        return ESP_ERR_NOT_SUPPORTED;
    }
    FILE *fp = fopen(path, "wb");
    if (!fp) {
        return ESP_FAIL;
    }
    size_t n = fwrite(buf, 1, size, fp);
    fclose(fp);
    return (n == size) ? ESP_OK : ESP_FAIL;
}

esp_err_t flash_wrapper_exists(const char *path, bool *exists)
{
    if (!path || !exists) {
        return ESP_ERR_INVALID_ARG;
    }
    if (path_is_nvs(path)) {
        *exists = false;
        return ESP_OK;
    }
    struct stat st;
    *exists = (stat(path, &st) == 0);
    return ESP_OK;
}

esp_err_t flash_wrapper_stat(const char *path, struct stat *st)
{
    if (!path || !st) {
        return ESP_ERR_INVALID_ARG;
    }
    if (path_is_nvs(path)) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    return (stat(path, st) == 0) ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t flash_wrapper_read_cert(const char *cert_path, uint8_t *cert_buf, size_t cert_len, size_t *bytes_read)
{
    if (!cert_path || !cert_buf || !bytes_read || cert_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    *bytes_read = 0;
    FILE *fp = fopen(cert_path, "rb");
    if (!fp) {
        return ESP_ERR_NOT_FOUND;
    }
    size_t n = fread(cert_buf, 1, cert_len, fp);
    bool failed = (n < cert_len && ferror(fp));
    fclose(fp);
    if (failed) {
        return ESP_FAIL;
    }
    /* Like the target version: report the file size, so a caller can spot truncation */
    struct stat st;
    if (stat(cert_path, &st) != 0) {
        return ESP_FAIL;
    }
    *bytes_read = (size_t) st.st_size;
    return ESP_OK;
}

#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(6, 2, 0)
/* Monotonic microseconds; IDF 6.2+ provides the real one on linux */
int64_t esp_timer_get_time(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t) ts.tv_sec * 1000000LL + (int64_t) ts.tv_nsec / 1000LL;
}
#endif
