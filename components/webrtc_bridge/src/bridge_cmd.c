/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Bridge Command Framework
 *
 * Provides a generic request/response command channel between two processors
 * (e.g., C6 signaling_only and P4 streaming_only) using a dedicated esp_hosted
 * custom msg_id, independent of the signaling message flow.
 *
 * Transport: esp_hosted custom data with msg_id BRIDGE_CMD_MSG_ID (0x1001)
 * Serialization: protobuf-c (BridgeCommand message)
 *
 * Chunking is built into the protobuf format: each chunk is a self-contained
 * BridgeCommand with seq_num/is_final/total_size fields, fitting in one
 * esp_hosted frame (~8KB).  No CHNK wire-format needed for bridge_cmd.
 */

#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <stdbool.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_work_queue.h"
#include "message_utils.h"
#include "webrtc_bridge.h"
#include "bridge_cmd.pb-c.h"

#if CONFIG_ESP_WEBRTC_BRIDGE_HOSTED
/* Coprocessor (slave) uses peer_data API; host uses esp_hosted */
#if CONFIG_ESP_HOSTED_COPROCESSOR
#include "esp_hosted_peer_data.h"
#elif CONFIG_ESP_HOSTED_ENABLED
#include "esp_hosted.h"
#include "esp_hosted_misc.h"
#endif
#endif

static const char *TAG = "bridge_cmd";

/* -------------------------------------------------------------------------- */
/*  Constants                                                                 */
/* -------------------------------------------------------------------------- */

/** esp_hosted custom msg_id for the command channel (separate from signaling 0x1000) */
#define BRIDGE_CMD_MSG_ID  ((uint32_t)0x1001)

/** Max payload per chunk: 8100 (esp_hosted limit) minus ~100 bytes protobuf overhead */
#define BRIDGE_CMD_CHUNK_PAYLOAD_MAX  8000

/* -------------------------------------------------------------------------- */
/*  Handler table                                                             */
/* -------------------------------------------------------------------------- */

typedef enum {
    BRIDGE_CMD_HANDLER_SYNC,      /**< Regular request/response handler */
    BRIDGE_CMD_HANDLER_STREAMING, /**< Streaming handler with write callback */
} bridge_cmd_handler_type_t;

typedef struct {
    uint32_t cmd_id;
    bridge_cmd_handler_type_t type;
    union {
        bridge_cmd_handler_t          sync;
        bridge_cmd_handler_with_cb_t  streaming;
    } handler;
} bridge_cmd_entry_t;

static bridge_cmd_entry_t s_handlers[BRIDGE_CMD_MAX_HANDLERS];
static int s_handler_count = 0;
static SemaphoreHandle_t s_handler_mutex = NULL;

/* -------------------------------------------------------------------------- */
/*  Response handler table (by cmd_id)                                       */
/* -------------------------------------------------------------------------- */

typedef enum {
    BRIDGE_CMD_RSP_SIMPLE,
    BRIDGE_CMD_RSP_CHUNKED,
} bridge_cmd_rsp_type_t;

typedef struct {
    uint32_t cmd_id;
    bridge_cmd_rsp_type_t type;
    union {
        bridge_cmd_response_cb_t simple;
        bridge_cmd_chunked_response_cb_t chunked;
    } cb;
} response_handler_entry_t;

static response_handler_entry_t s_response_handlers[BRIDGE_CMD_MAX_HANDLERS];
static int s_response_handler_count = 0;
static SemaphoreHandle_t s_response_handler_mutex = NULL;

/* -------------------------------------------------------------------------- */
/*  Event handler table (by cmd_id)                                           */
/* -------------------------------------------------------------------------- */

typedef enum {
    BRIDGE_CMD_EVT_SIMPLE,
    BRIDGE_CMD_EVT_CHUNKED,
} bridge_cmd_evt_type_t;

typedef struct {
    uint32_t cmd_id;
    bridge_cmd_evt_type_t type;
    union {
        bridge_cmd_event_cb_t simple;
        bridge_cmd_chunked_event_cb_t chunked;
    } cb;
} event_handler_entry_t;

static event_handler_entry_t s_event_handlers[BRIDGE_CMD_MAX_HANDLERS];
static int s_event_handler_count = 0;
static SemaphoreHandle_t s_event_handler_mutex = NULL;

static SemaphoreHandle_t s_send_mutex = NULL;  /* Serialize outgoing sends (chunk ordering) */
static uint32_t s_seq_counter = 0;

static bool s_initialized = false;

/* -------------------------------------------------------------------------- */
/*  Work-queue context for handling incoming requests                          */
/* -------------------------------------------------------------------------- */

typedef struct {
    uint32_t cmd_id;
    uint32_t req_id;
    uint8_t *req_data;   /* Heap-allocated copy of request payload */
    size_t req_len;
} bridge_cmd_work_item_t;

/** Work-queue item for invoking response callback */
typedef struct {
    uint32_t cmd_id;
    uint32_t req_id;
    esp_err_t status;
    uint8_t *resp_data;
    size_t resp_len;
    bridge_cmd_response_cb_t response_cb;
} bridge_cmd_response_work_t;

/** Work-queue item for invoking event callback */
typedef struct {
    uint32_t cmd_id;
    esp_err_t status;
    uint8_t *data;
    size_t len;
    bridge_cmd_event_cb_t event_cb;
} bridge_cmd_event_work_t;

/* -------------------------------------------------------------------------- */
/*  Reassembly state for simple response handlers                              */
/* -------------------------------------------------------------------------- */

/**
 * When a simple (non-chunked) response handler is registered but the sender
 * transmits a multi-chunk response (payload > 8KB, auto-chunked by
 * bridge_cmd_send_response()), the framework aggregates the chunks here and
 * delivers the complete buffer to the simple handler once is_final is set.
 *
 * Uses the same received_msg_t / esp_webrtc_create_buffer_for_msg /
 * esp_webrtc_append_msg_to_existing infrastructure from message_utils.
 *
 * One slot per (cmd_id, msg_type) pair.  No extra mutex:
 * bridge_cmd_process_message() is called serially from the esp_hosted RX
 * callback.
 */
typedef struct {
    uint32_t cmd_id;
    int msg_type;        /**< BRIDGE__MSG_TYPE__RSP or BRIDGE__MSG_TYPE__EVT */
    uint32_t req_id;     /**< Only meaningful for RSP */
    esp_err_t status;
    received_msg_t *msg; /**< Reassembly buffer from message_utils */
    bool active;
} reassembly_slot_t;

static reassembly_slot_t s_reassembly[BRIDGE_CMD_MAX_HANDLERS];

/**
 * Find an active reassembly slot for (cmd_id, msg_type), or return a free slot.
 * @param cmd_id    Command to look up
 * @param msg_type  BRIDGE__MSG_TYPE__RSP or BRIDGE__MSG_TYPE__EVT
 * @param exact     If true, only return a slot with matching key (for continuation chunks).
 *                  If false, return matching slot or first free slot (for first chunk).
 */
static reassembly_slot_t *find_reassembly_slot(uint32_t cmd_id, int msg_type, bool exact)
{
    reassembly_slot_t *free_slot = NULL;
    for (int i = 0; i < BRIDGE_CMD_MAX_HANDLERS; i++) {
        if (s_reassembly[i].active &&
            s_reassembly[i].cmd_id == cmd_id &&
            s_reassembly[i].msg_type == msg_type) {
            return &s_reassembly[i];
        }
        if (!exact && !s_reassembly[i].active && !free_slot) {
            free_slot = &s_reassembly[i];
        }
    }
    return exact ? NULL : free_slot;
}

static void reset_reassembly_slot(reassembly_slot_t *slot)
{
    if (slot) {
        if (slot->msg) {
            free(slot->msg->buf);
            free(slot->msg);
        }
        memset(slot, 0, sizeof(*slot));
    }
}

/* Allocate with SPIRAM preferred (fallback to internal if SPIRAM unavailable or full) */
static void *bridge_cmd_alloc_payload(size_t size)
{
    return heap_caps_calloc_prefer(1, size, 2, MALLOC_CAP_SPIRAM, MALLOC_CAP_INTERNAL);
}

/* Forward declarations */
static void bridge_cmd_process_message(const uint8_t *data, size_t data_len);
static void bridge_cmd_receive_cb(uint32_t msg_id, const uint8_t *data, size_t data_len, void *local_context);
static void bridge_cmd_handle_request(void *priv_data);
static void bridge_cmd_handle_response(void *priv_data);
static void bridge_cmd_handle_event(void *priv_data);

/* -------------------------------------------------------------------------- */
/*  Lookup helpers                                                             */
/* -------------------------------------------------------------------------- */

/**
 * Look up a handler entry by cmd_id.
 * Returns pointer to the entry (valid while s_handler_mutex is held by caller),
 * or NULL if not found. Caller must hold s_handler_mutex.
 */
static bridge_cmd_entry_t *find_handler_entry_locked(uint32_t cmd_id)
{
    for (int i = 0; i < s_handler_count; i++) {
        if (s_handlers[i].cmd_id == cmd_id) {
            return &s_handlers[i];
        }
    }
    return NULL;
}

static bool get_response_handler(uint32_t cmd_id, response_handler_entry_t *out_entry)
{
    bool found = false;
    xSemaphoreTake(s_response_handler_mutex, portMAX_DELAY);
    for (int i = 0; i < s_response_handler_count; i++) {
        if (s_response_handlers[i].cmd_id == cmd_id) {
            *out_entry = s_response_handlers[i];
            found = true;
            break;
        }
    }
    xSemaphoreGive(s_response_handler_mutex);
    return found;
}

static bool get_event_handler(uint32_t cmd_id, event_handler_entry_t *out_entry)
{
    bool found = false;
    xSemaphoreTake(s_event_handler_mutex, portMAX_DELAY);
    for (int i = 0; i < s_event_handler_count; i++) {
        if (s_event_handlers[i].cmd_id == cmd_id) {
            *out_entry = s_event_handlers[i];
            found = true;
            break;
        }
    }
    xSemaphoreGive(s_event_handler_mutex);
    return found;
}

/* -------------------------------------------------------------------------- */
/*  Internal: send a single protobuf message via esp_hosted                    */
/* -------------------------------------------------------------------------- */

/**
 * Pack and send a single BridgeCommand protobuf via esp_hosted.
 *
 * Each protobuf chunk is sized to fit in one esp_hosted frame (payload <=
 * BRIDGE_CMD_CHUNK_PAYLOAD_MAX + ~30 bytes overhead < 8100 limit), so no
 * transport-level chunking (CHNK) is needed.  The protobuf's own seq_num /
 * is_final / total_size fields handle application-level chunking.
 *
 * Caller must hold s_send_mutex if serialization with other sends is needed.
 */
static esp_err_t bridge_cmd_send_one(Bridge__BridgeCommand *msg)
{
    size_t packed_size = bridge__bridge_command__get_packed_size(msg);
    uint8_t *buf = bridge_cmd_alloc_payload(packed_size);
    if (!buf) {
        ESP_LOGE(TAG, "Failed to allocate %zu bytes for message", packed_size);
        return ESP_ERR_NO_MEM;
    }
    bridge__bridge_command__pack(msg, buf);

#if CONFIG_ESP_WEBRTC_BRIDGE_HOSTED
    esp_err_t ret = esp_hosted_send_custom_data(BRIDGE_CMD_MSG_ID, buf, packed_size);
#else
    esp_err_t ret = ESP_ERR_NOT_SUPPORTED;
#endif
    free(buf);
    return ret;
}

/* -------------------------------------------------------------------------- */
/*  Internal: send typed message (auto-chunking)                               */
/* -------------------------------------------------------------------------- */

/**
 * Send a RSP or EVT message, automatically chunking if data exceeds
 * BRIDGE_CMD_CHUNK_PAYLOAD_MAX.  Holds s_send_mutex across all chunks.
 */
static esp_err_t bridge_cmd_send_typed(Bridge__MsgType msg_type,
                                        uint32_t cmd_id, uint32_t req_id,
                                        esp_err_t status,
                                        const uint8_t *data, size_t len)
{
    xSemaphoreTake(s_send_mutex, portMAX_DELAY);

    if (len <= BRIDGE_CMD_CHUNK_PAYLOAD_MAX) {
        /* Single message — no chunking needed */
        Bridge__BridgeCommand msg = BRIDGE__BRIDGE_COMMAND__INIT;
        msg.cmd_id = cmd_id;
        msg.msg_type = msg_type;
        msg.req_id = req_id;
        msg.status = (int32_t)status;
        msg.seq_num = 0;
        msg.is_final = 1;
        msg.total_size = (uint32_t)len;
        if (data && len > 0) {
            msg.payload.data = (uint8_t *)data;
            msg.payload.len = len;
        }

        ESP_LOGD(TAG, "Sending %s cmd_id=0x%04" PRIx32 " req=%" PRIu32 " status=%s (%zu bytes)",
                 msg_type == BRIDGE__MSG_TYPE__EVT ? "event" : "response",
                 cmd_id, req_id, esp_err_to_name(status), len);

        esp_err_t ret = bridge_cmd_send_one(&msg);
        xSemaphoreGive(s_send_mutex);
        return ret;
    }

    /* Multi-chunk */
    uint32_t seq_num = 0;
    size_t offset = 0;
    esp_err_t ret = ESP_OK;

    ESP_LOGI(TAG, "Sending chunked %s cmd_id=0x%04" PRIx32 " req=%" PRIu32 " total=%zu",
             msg_type == BRIDGE__MSG_TYPE__EVT ? "event" : "response",
             cmd_id, req_id, len);

    while (offset < len) {
        size_t chunk_len = len - offset;
        if (chunk_len > BRIDGE_CMD_CHUNK_PAYLOAD_MAX) {
            chunk_len = BRIDGE_CMD_CHUNK_PAYLOAD_MAX;
        }
        bool is_final = (offset + chunk_len >= len);

        Bridge__BridgeCommand msg = BRIDGE__BRIDGE_COMMAND__INIT;
        msg.cmd_id = cmd_id;
        msg.msg_type = msg_type;
        msg.req_id = req_id;
        msg.status = (int32_t)status;
        msg.seq_num = seq_num;
        msg.is_final = is_final ? 1 : 0;
        msg.total_size = (uint32_t)len;
        msg.payload.data = (uint8_t *)(data + offset);
        msg.payload.len = chunk_len;

        ret = bridge_cmd_send_one(&msg);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to send chunk %" PRIu32 ": %s", seq_num, esp_err_to_name(ret));
            break;
        }

        offset += chunk_len;
        seq_num++;
    }

    xSemaphoreGive(s_send_mutex);
    return ret;
}

/** Convenience wrapper for sending responses (existing callers unchanged) */
static esp_err_t bridge_cmd_send_response(uint32_t cmd_id, uint32_t req_id,
                                            esp_err_t status,
                                            const uint8_t *data, size_t len)
{
    return bridge_cmd_send_typed(BRIDGE__MSG_TYPE__RSP, cmd_id, req_id, status, data, len);
}

/* -------------------------------------------------------------------------- */
/*  Public API                                                                 */
/* -------------------------------------------------------------------------- */

esp_err_t bridge_cmd_init(void)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_OK;
    }

    s_handler_mutex = xSemaphoreCreateMutex();
    if (!s_handler_mutex) {
        ESP_LOGE(TAG, "Failed to create handler mutex");
        return ESP_ERR_NO_MEM;
    }

    s_send_mutex = xSemaphoreCreateMutex();
    if (!s_send_mutex) {
        ESP_LOGE(TAG, "Failed to create send mutex");
        vSemaphoreDelete(s_handler_mutex);
        s_handler_mutex = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_response_handler_mutex = xSemaphoreCreateMutex();
    if (!s_response_handler_mutex) {
        ESP_LOGE(TAG, "Failed to create response handler mutex");
        vSemaphoreDelete(s_send_mutex);
        vSemaphoreDelete(s_handler_mutex);
        s_send_mutex = NULL;
        s_handler_mutex = NULL;
        return ESP_ERR_NO_MEM;
    }
    memset(s_response_handlers, 0, sizeof(s_response_handlers));

    s_event_handler_mutex = xSemaphoreCreateMutex();
    if (!s_event_handler_mutex) {
        ESP_LOGE(TAG, "Failed to create event handler mutex");
        vSemaphoreDelete(s_response_handler_mutex);
        vSemaphoreDelete(s_send_mutex);
        vSemaphoreDelete(s_handler_mutex);
        s_response_handler_mutex = NULL;
        s_send_mutex = NULL;
        s_handler_mutex = NULL;
        return ESP_ERR_NO_MEM;
    }
    memset(s_event_handlers, 0, sizeof(s_event_handlers));

#if CONFIG_ESP_WEBRTC_BRIDGE_HOSTED
    ESP_LOGI(TAG, "Registering bridge_cmd callback with msg_id: 0x%" PRIx32, BRIDGE_CMD_MSG_ID);
    esp_err_t ret = esp_hosted_register_custom_callback(BRIDGE_CMD_MSG_ID, bridge_cmd_receive_cb, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register callback (msg_id 0x%" PRIx32 "): %s", BRIDGE_CMD_MSG_ID, esp_err_to_name(ret));
        vSemaphoreDelete(s_event_handler_mutex);
        vSemaphoreDelete(s_response_handler_mutex);
        vSemaphoreDelete(s_send_mutex);
        vSemaphoreDelete(s_handler_mutex);
        s_event_handler_mutex = NULL;
        s_response_handler_mutex = NULL;
        s_send_mutex = NULL;
        s_handler_mutex = NULL;
        return ret;
    }
#else
    ESP_LOGW(TAG, "bridge_cmd: hosted mode not enabled, command channel unavailable");
#endif

    s_initialized = true;
    ESP_LOGI(TAG, "Bridge command subsystem initialized");
    return ESP_OK;
}

esp_err_t bridge_cmd_register_handler(uint32_t cmd_id, bridge_cmd_handler_t handler)
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "Not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    if (!handler) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_handler_mutex, portMAX_DELAY);

    /* Check for existing registration (allow re-registration) */
    bridge_cmd_entry_t *entry = find_handler_entry_locked(cmd_id);
    if (entry) {
        entry->type = BRIDGE_CMD_HANDLER_SYNC;
        entry->handler.sync = handler;
        xSemaphoreGive(s_handler_mutex);
        ESP_LOGI(TAG, "Updated handler for cmd_id 0x%04" PRIx32, cmd_id);
        return ESP_OK;
    }

    /* Add new entry */
    if (s_handler_count >= BRIDGE_CMD_MAX_HANDLERS) {
        xSemaphoreGive(s_handler_mutex);
        ESP_LOGE(TAG, "Handler table full (%d)", BRIDGE_CMD_MAX_HANDLERS);
        return ESP_ERR_NO_MEM;
    }

    s_handlers[s_handler_count].cmd_id = cmd_id;
    s_handlers[s_handler_count].type = BRIDGE_CMD_HANDLER_SYNC;
    s_handlers[s_handler_count].handler.sync = handler;
    s_handler_count++;

    xSemaphoreGive(s_handler_mutex);
    ESP_LOGI(TAG, "Registered handler for cmd_id 0x%04" PRIx32, cmd_id);
    return ESP_OK;
}

esp_err_t bridge_cmd_register_with_callback(uint32_t cmd_id,
                                             bridge_cmd_handler_with_cb_t handler)
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "Not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    if (!handler) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_handler_mutex, portMAX_DELAY);

    /* Check for existing registration (allow re-registration) */
    bridge_cmd_entry_t *entry = find_handler_entry_locked(cmd_id);
    if (entry) {
        entry->type = BRIDGE_CMD_HANDLER_STREAMING;
        entry->handler.streaming = handler;
        xSemaphoreGive(s_handler_mutex);
        ESP_LOGI(TAG, "Updated streaming handler for cmd_id 0x%04" PRIx32, cmd_id);
        return ESP_OK;
    }

    /* Add new entry */
    if (s_handler_count >= BRIDGE_CMD_MAX_HANDLERS) {
        xSemaphoreGive(s_handler_mutex);
        ESP_LOGE(TAG, "Handler table full (%d)", BRIDGE_CMD_MAX_HANDLERS);
        return ESP_ERR_NO_MEM;
    }

    s_handlers[s_handler_count].cmd_id = cmd_id;
    s_handlers[s_handler_count].type = BRIDGE_CMD_HANDLER_STREAMING;
    s_handlers[s_handler_count].handler.streaming = handler;
    s_handler_count++;

    xSemaphoreGive(s_handler_mutex);
    ESP_LOGI(TAG, "Registered streaming handler for cmd_id 0x%04" PRIx32, cmd_id);
    return ESP_OK;
}

esp_err_t bridge_cmd_register_response_handler(uint32_t cmd_id,
                                                bridge_cmd_response_cb_t handler)
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "Not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    if (!handler) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_response_handler_mutex, portMAX_DELAY);

    for (int i = 0; i < s_response_handler_count; i++) {
        if (s_response_handlers[i].cmd_id == cmd_id) {
            s_response_handlers[i].type = BRIDGE_CMD_RSP_SIMPLE;
            s_response_handlers[i].cb.simple = handler;
            xSemaphoreGive(s_response_handler_mutex);
            ESP_LOGI(TAG, "Updated response handler for cmd_id 0x%04" PRIx32, cmd_id);
            return ESP_OK;
        }
    }

    if (s_response_handler_count >= BRIDGE_CMD_MAX_HANDLERS) {
        xSemaphoreGive(s_response_handler_mutex);
        ESP_LOGE(TAG, "Response handler table full (%d)", BRIDGE_CMD_MAX_HANDLERS);
        return ESP_ERR_NO_MEM;
    }

    s_response_handlers[s_response_handler_count].cmd_id = cmd_id;
    s_response_handlers[s_response_handler_count].type = BRIDGE_CMD_RSP_SIMPLE;
    s_response_handlers[s_response_handler_count].cb.simple = handler;
    s_response_handler_count++;

    xSemaphoreGive(s_response_handler_mutex);
    ESP_LOGI(TAG, "Registered response handler for cmd_id 0x%04" PRIx32, cmd_id);
    return ESP_OK;
}

esp_err_t bridge_cmd_register_chunked_response_handler(uint32_t cmd_id,
                                                         bridge_cmd_chunked_response_cb_t handler)
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "Not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    if (!handler) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_response_handler_mutex, portMAX_DELAY);

    for (int i = 0; i < s_response_handler_count; i++) {
        if (s_response_handlers[i].cmd_id == cmd_id) {
            s_response_handlers[i].type = BRIDGE_CMD_RSP_CHUNKED;
            s_response_handlers[i].cb.chunked = handler;
            xSemaphoreGive(s_response_handler_mutex);
            ESP_LOGI(TAG, "Updated chunked response handler for cmd_id 0x%04" PRIx32, cmd_id);
            return ESP_OK;
        }
    }

    if (s_response_handler_count >= BRIDGE_CMD_MAX_HANDLERS) {
        xSemaphoreGive(s_response_handler_mutex);
        ESP_LOGE(TAG, "Response handler table full (%d)", BRIDGE_CMD_MAX_HANDLERS);
        return ESP_ERR_NO_MEM;
    }

    s_response_handlers[s_response_handler_count].cmd_id = cmd_id;
    s_response_handlers[s_response_handler_count].type = BRIDGE_CMD_RSP_CHUNKED;
    s_response_handlers[s_response_handler_count].cb.chunked = handler;
    s_response_handler_count++;

    xSemaphoreGive(s_response_handler_mutex);
    ESP_LOGI(TAG, "Registered chunked response handler for cmd_id 0x%04" PRIx32, cmd_id);
    return ESP_OK;
}

esp_err_t bridge_cmd_send(uint32_t cmd_id,
                          const uint8_t *req_data, size_t req_len)
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "Not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_send_mutex, portMAX_DELAY);
    uint32_t seq = ++s_seq_counter;

    Bridge__BridgeCommand msg = BRIDGE__BRIDGE_COMMAND__INIT;
    msg.cmd_id = cmd_id;
    msg.msg_type = BRIDGE__MSG_TYPE__CMD;
    msg.req_id = seq;
    msg.status = 0;
    msg.seq_num = 0;
    msg.is_final = 1;
    if (req_data && req_len > 0) {
        msg.payload.data = (uint8_t *)req_data;
        msg.payload.len = req_len;
    }

    ESP_LOGI(TAG, "Sending cmd_id=0x%04" PRIx32 " req=%" PRIu32 " (%zu bytes payload)",
             cmd_id, seq, req_len);

    esp_err_t ret = bridge_cmd_send_one(&msg);
    xSemaphoreGive(s_send_mutex);

    return ret;
}

esp_err_t bridge_cmd_register_event_handler(uint32_t cmd_id,
                                             bridge_cmd_event_cb_t handler)
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "Not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    if (!handler) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_event_handler_mutex, portMAX_DELAY);

    for (int i = 0; i < s_event_handler_count; i++) {
        if (s_event_handlers[i].cmd_id == cmd_id) {
            s_event_handlers[i].type = BRIDGE_CMD_EVT_SIMPLE;
            s_event_handlers[i].cb.simple = handler;
            xSemaphoreGive(s_event_handler_mutex);
            ESP_LOGI(TAG, "Updated event handler for cmd_id 0x%04" PRIx32, cmd_id);
            return ESP_OK;
        }
    }

    if (s_event_handler_count >= BRIDGE_CMD_MAX_HANDLERS) {
        xSemaphoreGive(s_event_handler_mutex);
        ESP_LOGE(TAG, "Event handler table full (%d)", BRIDGE_CMD_MAX_HANDLERS);
        return ESP_ERR_NO_MEM;
    }

    s_event_handlers[s_event_handler_count].cmd_id = cmd_id;
    s_event_handlers[s_event_handler_count].type = BRIDGE_CMD_EVT_SIMPLE;
    s_event_handlers[s_event_handler_count].cb.simple = handler;
    s_event_handler_count++;

    xSemaphoreGive(s_event_handler_mutex);
    ESP_LOGI(TAG, "Registered event handler for cmd_id 0x%04" PRIx32, cmd_id);
    return ESP_OK;
}

esp_err_t bridge_cmd_register_chunked_event_handler(uint32_t cmd_id,
                                                      bridge_cmd_chunked_event_cb_t handler)
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "Not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    if (!handler) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_event_handler_mutex, portMAX_DELAY);

    for (int i = 0; i < s_event_handler_count; i++) {
        if (s_event_handlers[i].cmd_id == cmd_id) {
            s_event_handlers[i].type = BRIDGE_CMD_EVT_CHUNKED;
            s_event_handlers[i].cb.chunked = handler;
            xSemaphoreGive(s_event_handler_mutex);
            ESP_LOGI(TAG, "Updated chunked event handler for cmd_id 0x%04" PRIx32, cmd_id);
            return ESP_OK;
        }
    }

    if (s_event_handler_count >= BRIDGE_CMD_MAX_HANDLERS) {
        xSemaphoreGive(s_event_handler_mutex);
        ESP_LOGE(TAG, "Event handler table full (%d)", BRIDGE_CMD_MAX_HANDLERS);
        return ESP_ERR_NO_MEM;
    }

    s_event_handlers[s_event_handler_count].cmd_id = cmd_id;
    s_event_handlers[s_event_handler_count].type = BRIDGE_CMD_EVT_CHUNKED;
    s_event_handlers[s_event_handler_count].cb.chunked = handler;
    s_event_handler_count++;

    xSemaphoreGive(s_event_handler_mutex);
    ESP_LOGI(TAG, "Registered chunked event handler for cmd_id 0x%04" PRIx32, cmd_id);
    return ESP_OK;
}

esp_err_t bridge_cmd_send_event(uint32_t cmd_id,
                                 const uint8_t *data, size_t len)
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "Not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    return bridge_cmd_send_typed(BRIDGE__MSG_TYPE__EVT, cmd_id, 0, ESP_OK, data, len);
}

/* -------------------------------------------------------------------------- */
/*  Receive callback (called from RPC RX thread)                               */
/* -------------------------------------------------------------------------- */

/**
 * Re-register the RX callback after the peer chip has rebooted and the esp_hosted
 * transport has been re-established.
 */
esp_err_t bridge_cmd_rearm_rx(void)
{
#if CONFIG_ESP_WEBRTC_BRIDGE_HOSTED
    if (s_handler_mutex == NULL) {
        return ESP_ERR_INVALID_STATE; /* bridge_cmd_init() has not run on this device */
    }
    return esp_hosted_register_custom_callback(BRIDGE_CMD_MSG_ID, bridge_cmd_receive_cb, NULL);
#else
    return ESP_OK;
#endif
}

/**
 * esp_hosted RX callback.  Each incoming message is a self-contained
 * BridgeCommand protobuf (no transport-level CHNK reassembly needed).
 * protobuf-c unpack copies all data it needs, so the transient esp_hosted
 * buffer is safe to use directly.
 *
 * Both call sites are inside CONFIG_ESP_WEBRTC_BRIDGE_HOSTED while this file is
 * compiled unconditionally, so the MQTT-bridge build needs the unused attribute.
 */
__attribute__((unused))
static void bridge_cmd_receive_cb(uint32_t msg_id, const uint8_t *data, size_t data_len, void *local_context)
{
    if (msg_id != BRIDGE_CMD_MSG_ID) {
        ESP_LOGW(TAG, "Unexpected msg_id: 0x%" PRIx32, msg_id);
        return;
    }
    bridge_cmd_process_message(data, data_len);
}

static void bridge_cmd_process_message(const uint8_t *data, size_t data_len)
{
    /* Deserialize */
    Bridge__BridgeCommand *cmd = bridge__bridge_command__unpack(NULL, data_len, data);
    if (!cmd) {
        ESP_LOGE(TAG, "Failed to deserialize BridgeCommand (%zu bytes)", data_len);
        return;
    }

    switch (cmd->msg_type) {
    case BRIDGE__MSG_TYPE__RSP: {
        /* ---------- RESPONSE: dispatch to registered handler by cmd_id ---------- */
        ESP_LOGD(TAG, "Received response: cmd_id=0x%04" PRIx32 " req=%" PRIu32
                 " status=%" PRId32 " seq_num=%" PRIu32 " is_final=%d",
                 cmd->cmd_id, cmd->req_id, cmd->status, cmd->seq_num, cmd->is_final);

        response_handler_entry_t rsp_entry;
        bool found = get_response_handler(cmd->cmd_id, &rsp_entry);

        if (!found) {
            ESP_LOGW(TAG, "No handler for cmd_id 0x%04" PRIx32 " response, dropping", cmd->cmd_id);
            bridge__bridge_command__free_unpacked(cmd, NULL);
            return;
        }

        if (rsp_entry.type == BRIDGE_CMD_RSP_CHUNKED) {
            /* Chunked handler: deliver directly, no copy, no work-queue hop */
            rsp_entry.cb.chunked(cmd->cmd_id, cmd->req_id,
                                 (esp_err_t)cmd->status,
                                 cmd->payload.data, cmd->payload.len,
                                 cmd->seq_num, cmd->total_size,
                                 cmd->is_final);
            bridge__bridge_command__free_unpacked(cmd, NULL);
            return;
        }

        /* Simple handler: aggregate chunks if needed, deliver complete buffer */
        if (cmd->seq_num == 0 && cmd->is_final) {
            /* Single-message response — fast path, no reassembly needed */
            uint8_t *resp_data = NULL;
            size_t resp_len = 0;
            esp_err_t status = (esp_err_t)cmd->status;

            if (cmd->payload.len > 0 && cmd->payload.data) {
                resp_data = bridge_cmd_alloc_payload(cmd->payload.len);
                if (resp_data) {
                    memcpy(resp_data, cmd->payload.data, cmd->payload.len);
                    resp_len = cmd->payload.len;
                } else {
                    ESP_LOGE(TAG, "Failed to allocate response buffer (%zu bytes)", cmd->payload.len);
                    status = ESP_ERR_NO_MEM;
                }
            }

            bridge_cmd_response_work_t *work = malloc(sizeof(bridge_cmd_response_work_t));
            if (work) {
                work->cmd_id = cmd->cmd_id;
                work->req_id = cmd->req_id;
                work->status = status;
                work->resp_data = resp_data;
                work->resp_len = resp_len;
                work->response_cb = rsp_entry.cb.simple;
                bridge__bridge_command__free_unpacked(cmd, NULL);
                esp_err_t q_ret = esp_work_queue_add_task(bridge_cmd_handle_response, work);
                if (q_ret != ESP_OK) {
                    ESP_LOGE(TAG, "Failed to queue response callback: %s", esp_err_to_name(q_ret));
                    free(resp_data);
                    free(work);
                }
            } else {
                ESP_LOGE(TAG, "Failed to allocate response work item");
                free(resp_data);
                bridge__bridge_command__free_unpacked(cmd, NULL);
            }
            break;
        }

        /* Multi-chunk response: use message_utils reassembly */
        {
            bool is_first = (cmd->seq_num == 0);
            reassembly_slot_t *slot;

            if (is_first) {
                slot = find_reassembly_slot(cmd->cmd_id, BRIDGE__MSG_TYPE__RSP, false);
                if (!slot) {
                    ESP_LOGE(TAG, "No free reassembly slot for cmd_id 0x%04" PRIx32, cmd->cmd_id);
                    bridge__bridge_command__free_unpacked(cmd, NULL);
                    break;
                }
                /* If slot was active for this cmd_id (stale), reset it */
                if (slot->active) {
                    ESP_LOGW(TAG, "Resetting stale reassembly for cmd_id 0x%04" PRIx32, cmd->cmd_id);
                    reset_reassembly_slot(slot);
                }

                slot->cmd_id = cmd->cmd_id;
                slot->msg_type = BRIDGE__MSG_TYPE__RSP;
                slot->req_id = cmd->req_id;
                slot->status = (esp_err_t)cmd->status;
                slot->active = true;
                slot->msg = esp_webrtc_create_buffer_for_msg((int)cmd->total_size);
                if (!slot->msg) {
                    ESP_LOGE(TAG, "Failed to create reassembly buffer (%" PRIu32 " bytes)",
                             cmd->total_size);
                    reset_reassembly_slot(slot);
                    bridge__bridge_command__free_unpacked(cmd, NULL);
                    break;
                }
            } else {
                slot = find_reassembly_slot(cmd->cmd_id, BRIDGE__MSG_TYPE__RSP, true);
                if (!slot) {
                    ESP_LOGW(TAG, "No active reassembly for cmd_id 0x%04" PRIx32
                             " seq=%" PRIu32 ", dropping", cmd->cmd_id, cmd->seq_num);
                    bridge__bridge_command__free_unpacked(cmd, NULL);
                    break;
                }
                /* Validate req_id matches */
                if (slot->req_id != cmd->req_id) {
                    ESP_LOGW(TAG, "req_id mismatch in reassembly for cmd_id 0x%04" PRIx32
                             " (expected %" PRIu32 ", got %" PRIu32 "), resetting",
                             cmd->cmd_id, slot->req_id, cmd->req_id);
                    reset_reassembly_slot(slot);
                    bridge__bridge_command__free_unpacked(cmd, NULL);
                    break;
                }
            }

            /* Append chunk using message_utils */
            esp_err_t append_ret = esp_webrtc_append_msg_to_existing(
                slot->msg, (void *)cmd->payload.data, (int)cmd->payload.len,
                cmd->is_final);

            ESP_LOGD(TAG, "Reassembly seq=%" PRIu32 " cmd_id=0x%04" PRIx32
                     " appended=%zu final=%d ret=%s",
                     cmd->seq_num, cmd->cmd_id, cmd->payload.len,
                     cmd->is_final, esp_err_to_name(append_ret));

            if (append_ret == ESP_OK) {
                /* Reassembly complete — dispatch to simple handler via work queue */
                bridge_cmd_response_work_t *work = malloc(sizeof(bridge_cmd_response_work_t));
                if (work) {
                    work->cmd_id = slot->cmd_id;
                    work->req_id = slot->req_id;
                    work->status = slot->status;
                    work->resp_data = (uint8_t *)slot->msg->buf;
                    work->resp_len = (size_t)slot->msg->data_size;
                    work->response_cb = rsp_entry.cb.simple;

                    /* Transfer buffer ownership to work item */
                    free(slot->msg);  /* Free the received_msg_t struct, not buf */
                    slot->msg = NULL;
                    reset_reassembly_slot(slot);
                    bridge__bridge_command__free_unpacked(cmd, NULL);

                    esp_err_t q_ret = esp_work_queue_add_task(bridge_cmd_handle_response, work);
                    if (q_ret != ESP_OK) {
                        ESP_LOGE(TAG, "Failed to queue reassembled response: %s",
                                 esp_err_to_name(q_ret));
                        free(work->resp_data);
                        free(work);
                    }
                } else {
                    ESP_LOGE(TAG, "Failed to allocate work item for reassembled response");
                    reset_reassembly_slot(slot);
                    bridge__bridge_command__free_unpacked(cmd, NULL);
                }
            } else if (append_ret == ESP_FAIL) {
                /* Overflow or error — discard */
                ESP_LOGE(TAG, "Reassembly append failed for cmd_id 0x%04" PRIx32, cmd->cmd_id);
                reset_reassembly_slot(slot);
                bridge__bridge_command__free_unpacked(cmd, NULL);
            } else {
                /* ESP_ERR_NOT_FINISHED — more chunks expected */
                bridge__bridge_command__free_unpacked(cmd, NULL);
            }
        }
        break;
    }

    case BRIDGE__MSG_TYPE__CMD: {
        /* ---------- REQUEST: dispatch to handler via work queue ---------- */
        ESP_LOGD(TAG, "Received request: cmd_id=0x%04" PRIx32 " req=%" PRIu32,
                 cmd->cmd_id, cmd->req_id);

        /* Allocate a work item and copy the request data (RPC buffer is transient) */
        bridge_cmd_work_item_t *item = malloc(sizeof(bridge_cmd_work_item_t));
        if (!item) {
            ESP_LOGE(TAG, "Failed to allocate work item");
            bridge__bridge_command__free_unpacked(cmd, NULL);
            return;
        }

        item->cmd_id = cmd->cmd_id;
        item->req_id = cmd->req_id;
        item->req_data = NULL;
        item->req_len = 0;

        if (cmd->payload.len > 0 && cmd->payload.data) {
            item->req_data = bridge_cmd_alloc_payload(cmd->payload.len);
            if (item->req_data) {
                memcpy(item->req_data, cmd->payload.data, cmd->payload.len);
                item->req_len = cmd->payload.len;
            }
        }

        bridge__bridge_command__free_unpacked(cmd, NULL);

        /* Queue for processing outside the RPC RX thread */
        esp_err_t q_ret = esp_work_queue_add_task(bridge_cmd_handle_request, item);
        if (q_ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to queue command handler: %s", esp_err_to_name(q_ret));
            free(item->req_data);
            free(item);
        }
        break;
    }

    case BRIDGE__MSG_TYPE__EVT: {
        /* ---------- EVENT: dispatch to registered handler by cmd_id ---------- */
        ESP_LOGD(TAG, "Received event: cmd_id=0x%04" PRIx32
                 " status=%" PRId32 " seq_num=%" PRIu32 " is_final=%d",
                 cmd->cmd_id, cmd->status, cmd->seq_num, cmd->is_final);

        event_handler_entry_t evt_entry;
        bool found = get_event_handler(cmd->cmd_id, &evt_entry);

        if (!found) {
            ESP_LOGW(TAG, "No handler for cmd_id 0x%04" PRIx32 " event, dropping", cmd->cmd_id);
            bridge__bridge_command__free_unpacked(cmd, NULL);
            break;
        }

        if (evt_entry.type == BRIDGE_CMD_EVT_CHUNKED) {
            /* Chunked handler: deliver directly, no copy, no work-queue hop */
            evt_entry.cb.chunked(cmd->cmd_id,
                                 (esp_err_t)cmd->status,
                                 cmd->payload.data, cmd->payload.len,
                                 cmd->seq_num, cmd->total_size,
                                 cmd->is_final);
            bridge__bridge_command__free_unpacked(cmd, NULL);
            break;
        }

        /* Simple handler: aggregate chunks if needed, deliver complete buffer */
        if (cmd->seq_num == 0 && cmd->is_final) {
            /* Single-message event — fast path */
            uint8_t *evt_data = NULL;
            size_t evt_len = 0;
            esp_err_t status = (esp_err_t)cmd->status;

            if (cmd->payload.len > 0 && cmd->payload.data) {
                evt_data = bridge_cmd_alloc_payload(cmd->payload.len);
                if (evt_data) {
                    memcpy(evt_data, cmd->payload.data, cmd->payload.len);
                    evt_len = cmd->payload.len;
                } else {
                    ESP_LOGE(TAG, "Failed to allocate event buffer (%zu bytes)", cmd->payload.len);
                    status = ESP_ERR_NO_MEM;
                }
            }

            bridge_cmd_event_work_t *work = malloc(sizeof(bridge_cmd_event_work_t));
            if (work) {
                work->cmd_id = cmd->cmd_id;
                work->status = status;
                work->data = evt_data;
                work->len = evt_len;
                work->event_cb = evt_entry.cb.simple;
                bridge__bridge_command__free_unpacked(cmd, NULL);
                esp_err_t q_ret = esp_work_queue_add_task(bridge_cmd_handle_event, work);
                if (q_ret != ESP_OK) {
                    ESP_LOGE(TAG, "Failed to queue event callback: %s", esp_err_to_name(q_ret));
                    free(evt_data);
                    free(work);
                }
            } else {
                ESP_LOGE(TAG, "Failed to allocate event work item");
                free(evt_data);
                bridge__bridge_command__free_unpacked(cmd, NULL);
            }
            break;
        }

        /* Multi-chunk event: use message_utils reassembly */
        {
            bool is_first = (cmd->seq_num == 0);
            reassembly_slot_t *slot;

            if (is_first) {
                slot = find_reassembly_slot(cmd->cmd_id, BRIDGE__MSG_TYPE__EVT, false);
                if (!slot) {
                    ESP_LOGE(TAG, "No free reassembly slot for event cmd_id 0x%04" PRIx32,
                             cmd->cmd_id);
                    bridge__bridge_command__free_unpacked(cmd, NULL);
                    break;
                }
                if (slot->active) {
                    ESP_LOGW(TAG, "Resetting stale event reassembly for cmd_id 0x%04" PRIx32,
                             cmd->cmd_id);
                    reset_reassembly_slot(slot);
                }

                slot->cmd_id = cmd->cmd_id;
                slot->msg_type = BRIDGE__MSG_TYPE__EVT;
                slot->req_id = 0;
                slot->status = (esp_err_t)cmd->status;
                slot->active = true;
                slot->msg = esp_webrtc_create_buffer_for_msg((int)cmd->total_size);
                if (!slot->msg) {
                    ESP_LOGE(TAG, "Failed to create event reassembly buffer (%" PRIu32 " bytes)",
                             cmd->total_size);
                    reset_reassembly_slot(slot);
                    bridge__bridge_command__free_unpacked(cmd, NULL);
                    break;
                }
            } else {
                slot = find_reassembly_slot(cmd->cmd_id, BRIDGE__MSG_TYPE__EVT, true);
                if (!slot) {
                    ESP_LOGW(TAG, "No active event reassembly for cmd_id 0x%04" PRIx32
                             " seq=%" PRIu32 ", dropping", cmd->cmd_id, cmd->seq_num);
                    bridge__bridge_command__free_unpacked(cmd, NULL);
                    break;
                }
            }

            esp_err_t append_ret = esp_webrtc_append_msg_to_existing(
                slot->msg, (void *)cmd->payload.data, (int)cmd->payload.len,
                cmd->is_final);

            if (append_ret == ESP_OK) {
                bridge_cmd_event_work_t *work = malloc(sizeof(bridge_cmd_event_work_t));
                if (work) {
                    work->cmd_id = slot->cmd_id;
                    work->status = slot->status;
                    work->data = (uint8_t *)slot->msg->buf;
                    work->len = (size_t)slot->msg->data_size;
                    work->event_cb = evt_entry.cb.simple;

                    free(slot->msg);
                    slot->msg = NULL;
                    reset_reassembly_slot(slot);
                    bridge__bridge_command__free_unpacked(cmd, NULL);

                    esp_err_t q_ret = esp_work_queue_add_task(bridge_cmd_handle_event, work);
                    if (q_ret != ESP_OK) {
                        ESP_LOGE(TAG, "Failed to queue reassembled event: %s",
                                 esp_err_to_name(q_ret));
                        free(work->data);
                        free(work);
                    }
                } else {
                    ESP_LOGE(TAG, "Failed to allocate work item for reassembled event");
                    reset_reassembly_slot(slot);
                    bridge__bridge_command__free_unpacked(cmd, NULL);
                }
            } else if (append_ret == ESP_FAIL) {
                ESP_LOGE(TAG, "Event reassembly append failed for cmd_id 0x%04" PRIx32,
                         cmd->cmd_id);
                reset_reassembly_slot(slot);
                bridge__bridge_command__free_unpacked(cmd, NULL);
            } else {
                bridge__bridge_command__free_unpacked(cmd, NULL);
            }
        }
        break;
    }

    default:
        ESP_LOGW(TAG, "Unknown msg_type %d, dropping", cmd->msg_type);
        bridge__bridge_command__free_unpacked(cmd, NULL);
        break;
    }
}

/* -------------------------------------------------------------------------- */
/*  Work-queue handler for response callback                                   */
/* -------------------------------------------------------------------------- */

static void bridge_cmd_handle_response(void *priv_data)
{
    bridge_cmd_response_work_t *work = (bridge_cmd_response_work_t *)priv_data;
    if (!work || !work->response_cb) {
        if (work) {
            free(work->resp_data);
            free(work);
        }
        return;
    }
    work->response_cb(work->cmd_id, work->req_id,
                      work->status, work->resp_data, work->resp_len);
    free(work);
}

/* -------------------------------------------------------------------------- */
/*  Work-queue handler for event callback                                      */
/* -------------------------------------------------------------------------- */

static void bridge_cmd_handle_event(void *priv_data)
{
    bridge_cmd_event_work_t *work = (bridge_cmd_event_work_t *)priv_data;
    if (!work || !work->event_cb) {
        if (work) {
            free(work->data);
            free(work);
        }
        return;
    }
    work->event_cb(work->cmd_id, work->status, work->data, work->len);
    free(work);
}

/* -------------------------------------------------------------------------- */
/*  Streaming (callback) handler write-callback implementation                 */
/* -------------------------------------------------------------------------- */

typedef struct {
    uint32_t cmd_id;
    uint32_t req_id;
    uint32_t seq_num;  /* incremented per write_cb call */
} bridge_cmd_write_ctx_t;

/**
 * Internal write callback passed to streaming handlers.
 * Wraps data in a BridgeCommand protobuf and sends via esp_hosted directly.
 * Each call sends one chunk with incrementing seq_num; is_final is set by
 * the framework after the handler returns (the last write_cb call before
 * handler returns is NOT automatically marked final — the framework sends
 * a final empty chunk if needed, or the handler can signal completion).
 */
static esp_err_t bridge_cmd_write_cb_impl(const uint8_t *data, size_t len, void *ctx)
{
    bridge_cmd_write_ctx_t *wctx = (bridge_cmd_write_ctx_t *)ctx;

    Bridge__BridgeCommand resp_msg = BRIDGE__BRIDGE_COMMAND__INIT;
    resp_msg.cmd_id = wctx->cmd_id;
    resp_msg.msg_type = BRIDGE__MSG_TYPE__RSP;
    resp_msg.req_id = wctx->req_id;
    resp_msg.status = (int32_t)ESP_OK;
    resp_msg.seq_num = wctx->seq_num;
    resp_msg.is_final = 0;  /* Not final; framework will handle finalization */
    resp_msg.total_size = 0; /* Unknown in streaming mode */
    if (data && len > 0) {
        resp_msg.payload.data = (uint8_t *)data;
        resp_msg.payload.len = len;
    }

    ESP_LOGD(TAG, "write_cb: sending cmd_id=0x%04" PRIx32 " req=%" PRIu32 " seq=%" PRIu32
             " (%zu bytes payload)",
             wctx->cmd_id, wctx->req_id, wctx->seq_num, len);

    xSemaphoreTake(s_send_mutex, portMAX_DELAY);
    esp_err_t send_ret = bridge_cmd_send_one(&resp_msg);
    xSemaphoreGive(s_send_mutex);

    if (send_ret != ESP_OK) {
        ESP_LOGE(TAG, "write_cb: Failed to send: %s", esp_err_to_name(send_ret));
    } else {
        wctx->seq_num++;
    }
    return send_ret;
}

/* -------------------------------------------------------------------------- */
/*  Work-queue handler for incoming requests                                   */
/* -------------------------------------------------------------------------- */

static void bridge_cmd_handle_request(void *priv_data)
{
    bridge_cmd_work_item_t *item = (bridge_cmd_work_item_t *)priv_data;
    if (!item) {
        return;
    }

    ESP_LOGI(TAG, "Handling request cmd_id=0x%04" PRIx32 " req=%" PRIu32,
             item->cmd_id, item->req_id);

    /* Look up handler entry (copy fields under lock, then release) */
    xSemaphoreTake(s_handler_mutex, portMAX_DELAY);
    bridge_cmd_entry_t *entry = find_handler_entry_locked(item->cmd_id);
    bridge_cmd_entry_t local_entry;
    bool found = false;
    if (entry) {
        local_entry = *entry;
        found = true;
    }
    xSemaphoreGive(s_handler_mutex);

    if (!found) {
        ESP_LOGW(TAG, "No handler registered for cmd_id 0x%04" PRIx32, item->cmd_id);
        /* Send error response */
        bridge_cmd_send_response(item->cmd_id, item->req_id, ESP_ERR_NOT_FOUND, NULL, 0);
        free(item->req_data);
        free(item);
        return;
    }

    if (local_entry.type == BRIDGE_CMD_HANDLER_STREAMING) {
        /* Streaming handler: invoke with write callback */
        bridge_cmd_write_ctx_t wctx = {
            .cmd_id = item->cmd_id,
            .req_id = item->req_id,
            .seq_num = 0,
        };
        esp_err_t handler_status = local_entry.handler.streaming(
            item->cmd_id, item->req_data, item->req_len,
            bridge_cmd_write_cb_impl, &wctx);

        /* Send final marker after streaming handler completes */
        Bridge__BridgeCommand final_msg = BRIDGE__BRIDGE_COMMAND__INIT;
        final_msg.cmd_id = item->cmd_id;
        final_msg.msg_type = BRIDGE__MSG_TYPE__RSP;
        final_msg.req_id = item->req_id;
        final_msg.status = (int32_t)handler_status;
        final_msg.seq_num = wctx.seq_num;
        final_msg.is_final = 1;
        final_msg.total_size = 0;

        xSemaphoreTake(s_send_mutex, portMAX_DELAY);
        bridge_cmd_send_one(&final_msg);
        xSemaphoreGive(s_send_mutex);

        if (handler_status != ESP_OK) {
            ESP_LOGE(TAG, "Streaming handler for cmd_id 0x%04" PRIx32 " failed: %s",
                     item->cmd_id, esp_err_to_name(handler_status));
        }
        free(item->req_data);
        free(item);
        return;
    }

    /* Sync handler: invoke and send response (auto-chunked) */
    esp_err_t handler_status;
    uint8_t *resp_data = NULL;
    size_t resp_len = 0;

    handler_status = local_entry.handler.sync(item->cmd_id, item->req_data, item->req_len,
                                               &resp_data, &resp_len);

    esp_err_t send_ret = bridge_cmd_send_response(item->cmd_id, item->req_id,
                                                    handler_status, resp_data, resp_len);
    if (send_ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send response: %s", esp_err_to_name(send_ret));
    }

    /* Clean up */
    free(resp_data);
    free(item->req_data);
    free(item);
}
