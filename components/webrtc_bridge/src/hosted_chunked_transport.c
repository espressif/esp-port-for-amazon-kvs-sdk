/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <inttypes.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "message_utils.h"
#include "hosted_chunked_transport.h"

#if CONFIG_ESP_WEBRTC_BRIDGE_HOSTED
/* Coprocessor (slave) uses peer_data API; host uses esp_hosted */
#if CONFIG_ESP_HOSTED_COPROCESSOR
#include "esp_hosted_peer_data.h"
#elif CONFIG_ESP_HOSTED_ENABLED
#include "esp_hosted.h"
#include "esp_hosted_misc.h"
#endif
#endif

static const char *TAG = "hosted_chunked";

#define HOSTED_CHUNK_MAGIC  0x43484E4B  /* "CHNK" - new format marker */

#define MAX_REGISTRATIONS  32

/* New format: magic + total_len + offset + chunk_len */
typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t total_len;
    uint32_t offset;
    uint32_t chunk_len;
} hosted_chunk_new_hdr_t;

/* Legacy format: seq_num + total_len + is_fin */
typedef struct __attribute__((packed)) {
    uint32_t seq_num;
    uint32_t total_len;
    uint8_t is_fin;
} hosted_chunk_legacy_hdr_t;

typedef struct {
    uint32_t msg_id;
    hosted_chunked_on_msg_t callback;
    hosted_chunked_on_chunk_t chunk_callback;
    void *chunk_ctx;
    received_msg_t *reassembly_msg;
    uint32_t expected_seq;  /* for legacy format */
} hosted_chunk_reg_t;

static hosted_chunk_reg_t s_regs[MAX_REGISTRATIONS];
static int s_reg_count = 0;

static hosted_chunk_reg_t *find_reg(uint32_t msg_id)
{
    for (int i = 0; i < s_reg_count; i++) {
        if (s_regs[i].msg_id == msg_id) {
            return &s_regs[i];
        }
    }
    return NULL;
}

void hosted_chunked_register(uint32_t msg_id, hosted_chunked_on_msg_t callback)
{
    hosted_chunk_reg_t *reg = find_reg(msg_id);
    if (reg) {
        reg->callback = callback;
        return;
    }
    if (s_reg_count >= MAX_REGISTRATIONS) {
        ESP_LOGE(TAG, "Registration table full");
        return;
    }
    s_regs[s_reg_count].msg_id = msg_id;
    s_regs[s_reg_count].callback = callback;
    s_regs[s_reg_count].chunk_callback = NULL;
    s_regs[s_reg_count].chunk_ctx = NULL;
    s_regs[s_reg_count].reassembly_msg = NULL;
    s_regs[s_reg_count].expected_seq = 0;
    s_reg_count++;
}

void hosted_chunked_register_chunk_cb(uint32_t msg_id,
                                      hosted_chunked_on_chunk_t chunk_cb,
                                      void *ctx)
{
    hosted_chunk_reg_t *reg = find_reg(msg_id);
    if (!reg) {
        ESP_LOGW(TAG, "msg_id 0x%" PRIx32 " not registered, call hosted_chunked_register first", msg_id);
        return;
    }
    reg->chunk_callback = chunk_cb;
    reg->chunk_ctx = ctx;
}

#if CONFIG_ESP_WEBRTC_BRIDGE_HOSTED

static void *alloc_payload(size_t size)
{
    return heap_caps_calloc_prefer(1, size, 2, MALLOC_CAP_SPIRAM, MALLOC_CAP_INTERNAL);
}

/* Process chunk in new (offset-based) format */
static void process_new_format_chunk(hosted_chunk_reg_t *reg,
                                     const uint8_t *data, size_t data_len)
{
    if (data_len < sizeof(hosted_chunk_new_hdr_t)) {
        return;
    }

    const hosted_chunk_new_hdr_t *hdr = (const hosted_chunk_new_hdr_t *)data;
    const uint8_t *chunk_data = data + sizeof(hosted_chunk_new_hdr_t);
    int chunk_data_len = (int)(data_len - sizeof(hosted_chunk_new_hdr_t));

    if (chunk_data_len > (int)hdr->chunk_len) {
        chunk_data_len = (int)hdr->chunk_len;
    }

    /* Per-chunk delivery: bypass reassembly if chunk callback is registered */
    if (reg->chunk_callback) {
        reg->chunk_callback(chunk_data, (size_t)chunk_data_len,
                            hdr->offset, hdr->total_len, reg->chunk_ctx);
        return;
    }

    if (hdr->offset == 0) {
        if (reg->reassembly_msg) {
            free(reg->reassembly_msg->buf);
            free(reg->reassembly_msg);
            reg->reassembly_msg = NULL;
        }
        if (hdr->total_len > INT_MAX) {
            ESP_LOGE(TAG, "total_len %" PRIu32 " exceeds int max", hdr->total_len);
            return;
        }
        reg->reassembly_msg = esp_webrtc_create_buffer_for_msg((int)hdr->total_len);
        if (!reg->reassembly_msg) {
            ESP_LOGE(TAG, "Failed to create reassembly buffer (%" PRIu32 " bytes)", hdr->total_len);
            return;
        }
    }

    if (!reg->reassembly_msg) {
        ESP_LOGE(TAG, "Chunk reassembly state error (missing first chunk)");
        return;
    }

    bool is_fin = (hdr->offset + (size_t)chunk_data_len >= hdr->total_len);
    esp_err_t ret = esp_webrtc_append_msg_to_existing(reg->reassembly_msg,
                                                      (void *)(uintptr_t)chunk_data,
                                                      chunk_data_len, is_fin);

    if (ret == ESP_OK) {
        hosted_chunked_on_msg_t cb = reg->callback;
        uint8_t *buf = (uint8_t *)reg->reassembly_msg->buf;
        int len = reg->reassembly_msg->data_size;

        free(reg->reassembly_msg);
        reg->reassembly_msg = NULL;

        if (cb) {
            cb(buf, (size_t)len);  /* callback owns buf, must free it */
        } else {
            free(buf);
        }
    } else if (ret == ESP_FAIL) {
        free(reg->reassembly_msg->buf);
        free(reg->reassembly_msg);
        reg->reassembly_msg = NULL;
    }
}

/* Process chunk in legacy (seq_num) format */
static void process_legacy_format_chunk(hosted_chunk_reg_t *reg,
                                        const uint8_t *data, size_t data_len)
{
    if (data_len < sizeof(hosted_chunk_legacy_hdr_t)) {
        ESP_LOGE(TAG, "Legacy chunk too small");
        return;
    }

    const hosted_chunk_legacy_hdr_t *hdr = (const hosted_chunk_legacy_hdr_t *)data;
    const uint8_t *chunk_data = data + sizeof(hosted_chunk_legacy_hdr_t);
    size_t chunk_data_len = data_len - sizeof(hosted_chunk_legacy_hdr_t);

    if (hdr->seq_num == 0) {
        if (reg->reassembly_msg) {
            free(reg->reassembly_msg->buf);
            free(reg->reassembly_msg);
            reg->reassembly_msg = NULL;
        }
        reg->reassembly_msg = esp_webrtc_create_buffer_for_msg((int)hdr->total_len);
        if (!reg->reassembly_msg) {
            ESP_LOGE(TAG, "Failed to create reassembly buffer (%" PRIu32 " bytes)", hdr->total_len);
            reg->expected_seq = 0;
            return;
        }
        reg->expected_seq = 0;
    }

    if (hdr->seq_num != reg->expected_seq) {
        ESP_LOGW(TAG, "Out of order chunk: expected %" PRIu32 " got %" PRIu32, reg->expected_seq, hdr->seq_num);
        if (reg->reassembly_msg) {
            free(reg->reassembly_msg->buf);
            free(reg->reassembly_msg);
            reg->reassembly_msg = NULL;
        }
        reg->expected_seq = 0;
        return;
    }

    if (!reg->reassembly_msg) {
        return;
    }

    bool is_fin = (hdr->is_fin != 0);
    esp_err_t ret = esp_webrtc_append_msg_to_existing(reg->reassembly_msg,
                                                      (void *)(uintptr_t)chunk_data,
                                                      (int)chunk_data_len, is_fin);

    if (ret == ESP_OK) {
        hosted_chunked_on_msg_t cb = reg->callback;
        uint8_t *buf = (uint8_t *)reg->reassembly_msg->buf;
        int len = reg->reassembly_msg->data_size;

        free(reg->reassembly_msg);
        reg->reassembly_msg = NULL;
        reg->expected_seq = 0;

        if (cb) {
            cb(buf, (size_t)len);  /* callback owns buf, must free it */
        } else {
            free(buf);
        }
    } else if (ret == ESP_FAIL) {
        free(reg->reassembly_msg->buf);
        free(reg->reassembly_msg);
        reg->reassembly_msg = NULL;
        reg->expected_seq = 0;
    } else {
        reg->expected_seq++;
    }
}

void hosted_chunked_process_chunk(uint32_t msg_id, const uint8_t *data, size_t data_len)
{
    hosted_chunk_reg_t *reg = find_reg(msg_id);
    if (!reg) {
        ESP_LOGW(TAG, "No handler for msg_id 0x%" PRIx32, msg_id);
        return;
    }

    /* Auto-detect: new format has magic at start */
    if (data_len >= 4) {
        uint32_t magic;
        memcpy(&magic, data, 4);
        if (magic == HOSTED_CHUNK_MAGIC) {
            process_new_format_chunk(reg, data, data_len);
            return;
        }
    }

    /* Legacy format: seq_num 0 (first chunk) or we have active reassembly (continuation) */
    if (data_len >= sizeof(hosted_chunk_legacy_hdr_t)) {
        const hosted_chunk_legacy_hdr_t *hdr = (const hosted_chunk_legacy_hdr_t *)data;
        if (hdr->seq_num == 0 || reg->reassembly_msg) {
            process_legacy_format_chunk(reg, data, data_len);
            return;
        }
    }

    /* Raw message (no chunk header), e.g. bridge_cmd small protobuf - copy and pass through */
    if (reg->callback && data_len > 0) {
        uint8_t *copy = alloc_payload(data_len);
        if (copy) {
            memcpy(copy, data, data_len);
            reg->callback(copy, data_len);  /* callback owns copy, must free it */
        }
    }
}

esp_err_t hosted_chunked_send(uint32_t msg_id,
                              const uint8_t *data, size_t len,
                              SemaphoreHandle_t send_mutex,
                              hosted_chunk_wire_format_t wire_format)
{
    if (!data || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (send_mutex && xSemaphoreTake(send_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to acquire send mutex");
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t ret = ESP_OK;

    if (wire_format == HOSTED_CHUNK_WIRE_NEW) {
        /* New format: send raw if fits, else chunk with magic header */
        if (len <= HOSTED_CHUNK_MAX_DATA) {
            ret = esp_hosted_send_custom_data(msg_id, data, len);
        } else {
            size_t offset = 0;
            size_t frame_buf_size = sizeof(hosted_chunk_new_hdr_t) + HOSTED_CHUNK_MAX_DATA;
            uint8_t *frame = alloc_payload(frame_buf_size);
            if (!frame) {
                ret = ESP_ERR_NO_MEM;
            } else {
                hosted_chunk_new_hdr_t *hdr = (hosted_chunk_new_hdr_t *)frame;
                hdr->magic = HOSTED_CHUNK_MAGIC;
                hdr->total_len = (uint32_t)len;

                while (offset < len && ret == ESP_OK) {
                    size_t chunk_data_len = len - offset;
                    if (chunk_data_len > HOSTED_CHUNK_MAX_DATA) {
                        chunk_data_len = HOSTED_CHUNK_MAX_DATA;
                    }

                    hdr->offset = (uint32_t)offset;
                    hdr->chunk_len = (uint32_t)chunk_data_len;
                    memcpy(frame + sizeof(hosted_chunk_new_hdr_t), data + offset, chunk_data_len);

                    ret = esp_hosted_send_custom_data(msg_id, frame,
                                                      sizeof(hosted_chunk_new_hdr_t) + chunk_data_len);
                    offset += chunk_data_len;
                }
                free(frame);
            }
        }
    } else {
        /* Legacy format: always chunk */
        uint32_t seq_num = 0;
        size_t offset = 0;

        uint8_t *chunk_buf = alloc_payload(sizeof(hosted_chunk_legacy_hdr_t) + HOSTED_CHUNK_MAX_DATA);
        if (!chunk_buf) {
            ret = ESP_ERR_NO_MEM;
        } else {
            while (offset < len && ret == ESP_OK) {
                size_t chunk_data_len = len - offset;
                if (chunk_data_len > HOSTED_CHUNK_MAX_DATA) {
                    chunk_data_len = HOSTED_CHUNK_MAX_DATA;
                }

                hosted_chunk_legacy_hdr_t *hdr = (hosted_chunk_legacy_hdr_t *)chunk_buf;
                hdr->seq_num = seq_num;
                hdr->total_len = (uint32_t)len;
                hdr->is_fin = (offset + chunk_data_len >= len) ? 1 : 0;

                memcpy(chunk_buf + sizeof(hosted_chunk_legacy_hdr_t), data + offset, chunk_data_len);

                size_t total_size = sizeof(hosted_chunk_legacy_hdr_t) + chunk_data_len;
                ret = esp_hosted_send_custom_data(msg_id, chunk_buf, total_size);

                offset += chunk_data_len;
                seq_num++;
            }
            free(chunk_buf);
        }
    }

    if (send_mutex) {
        xSemaphoreGive(send_mutex);
    }

    return ret;
}

#else

void hosted_chunked_process_chunk(uint32_t msg_id, const uint8_t *data, size_t data_len)
{
    (void)msg_id;
    (void)data;
    (void)data_len;
}

esp_err_t hosted_chunked_send(uint32_t msg_id,
                              const uint8_t *data, size_t len,
                              SemaphoreHandle_t send_mutex,
                              hosted_chunk_wire_format_t wire_format)
{
    (void)msg_id;
    (void)data;
    (void)len;
    (void)send_mutex;
    (void)wire_format;
    return ESP_ERR_NOT_SUPPORTED;
}

#endif /* CONFIG_ESP_WEBRTC_BRIDGE_HOSTED */
