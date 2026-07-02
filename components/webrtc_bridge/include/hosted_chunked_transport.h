/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file hosted_chunked_transport.h
 * @brief Shared chunked send/receive transport for esp_hosted custom data
 *
 * Consolidates chunking logic used by webrtc_bridge (msg_id 0x1000) and bridge_cmd (msg_id 0x1001).
 * Supports two wire formats for backward compatibility:
 * - NEW: offset-based header with magic (0x43484E4B "CHNK"); raw for small messages
 * - LEGACY: seq_num + total_len + is_fin (webrtc_bridge compatibility)
 *
 * Receive side auto-detects format via magic; send side chooses via wire_format parameter.
 */

/* -------------------------------------------------------------------------- */
/*  Chunk size (esp_hosted max payload 8166 - header 16 - margin 50)           */
/* -------------------------------------------------------------------------- */

#define HOSTED_CHUNK_MAX_DATA  (8166 - 16 - 50)  /* 8100 */

/* -------------------------------------------------------------------------- */
/*  Wire format                                                               */
/* -------------------------------------------------------------------------- */

typedef enum {
    HOSTED_CHUNK_WIRE_NEW,    /**< offset-based + magic (bridge_cmd) */
    HOSTED_CHUNK_WIRE_LEGACY  /**< seq_num + total_len + is_fin (webrtc_bridge compat) */
} hosted_chunk_wire_format_t;

/* -------------------------------------------------------------------------- */
/*  Callback type                                                             */
/* -------------------------------------------------------------------------- */

/**
 * @brief Invoked when a complete message is reassembled from chunks
 *
 * @param data  Pointer to the complete message; callback takes ownership and must free it
 * @param len   Length of the message
 */
typedef void (*hosted_chunked_on_msg_t)(const uint8_t *data, size_t len);

/**
 * @brief Per-CHNK-chunk receive callback (bypasses reassembly)
 *
 * Called once for each CHNK chunk as it arrives from the wire.
 * Raw (non-CHNK) messages still use the regular hosted_chunked_on_msg_t callback.
 *
 * @param data       Chunk payload (after CHNK header stripped)
 * @param len        Chunk payload length
 * @param offset     Byte offset within the complete message
 * @param total_len  Total message size across all chunks
 * @param ctx        User context from registration
 */
typedef void (*hosted_chunked_on_chunk_t)(const uint8_t *data, size_t len,
                                          uint32_t offset, uint32_t total_len,
                                          void *ctx);

/* -------------------------------------------------------------------------- */
/*  API                                                                       */
/* -------------------------------------------------------------------------- */

/**
 * @brief Send data via esp_hosted, chunking if necessary
 *
 * Uses HOSTED_CHUNK_MAX_DATA (8100) for chunk size.
 *
 * @param msg_id       esp_hosted custom msg_id (e.g. 0x1000, 0x1001)
 * @param data         Payload to send
 * @param len          Payload length
 * @param send_mutex   Mutex for serializing sends (can be NULL)
 * @param wire_format  HOSTED_CHUNK_WIRE_NEW or HOSTED_CHUNK_WIRE_LEGACY
 * @return ESP_OK on success
 */
esp_err_t hosted_chunked_send(uint32_t msg_id,
                              const uint8_t *data, size_t len,
                              SemaphoreHandle_t send_mutex,
                              hosted_chunk_wire_format_t wire_format);

/**
 * @brief Register callback for when a complete message is reassembled for this msg_id
 *
 * @param msg_id    esp_hosted custom msg_id
 * @param callback  Invoked with (data, len); data is freed by transport after callback returns
 */
void hosted_chunked_register(uint32_t msg_id, hosted_chunked_on_msg_t callback);

/**
 * @brief Register per-chunk callback for CHNK messages on this msg_id
 *
 * When set, CHNK chunks for this msg_id are delivered individually via chunk_cb
 * instead of being reassembled. Raw (non-CHNK) messages still use the regular
 * callback from hosted_chunked_register().
 *
 * @param msg_id    esp_hosted custom msg_id (must already be registered)
 * @param chunk_cb  Per-chunk callback
 * @param ctx       User context passed to chunk_cb
 */
void hosted_chunked_register_chunk_cb(uint32_t msg_id,
                                      hosted_chunked_on_chunk_t chunk_cb,
                                      void *ctx);

/**
 * @brief Process received chunk (call from esp_hosted RX callback)
 *
 * Auto-detects wire format: if first 4 bytes == 0x43484E4B then new format, else legacy.
 * Reassembles chunks and invokes the registered callback when complete.
 *
 * @param msg_id    esp_hosted custom msg_id (must match registration)
 * @param data      Chunk data (or raw message if not chunked)
 * @param data_len  Chunk length
 */
void hosted_chunked_process_chunk(uint32_t msg_id, const uint8_t *data, size_t data_len);

#ifdef __cplusplus
}
#endif
