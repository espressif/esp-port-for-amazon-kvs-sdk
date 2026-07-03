/*
 * SPDX-FileCopyrightText: 2024-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Function pointer type for WebRTC bridge message callback
 *
 * @param data Pointer to the received message data
 * @param len Length of the received message data
 */
typedef void (*webrtc_bridge_msg_cb_t) (const void *data, int len);

/**
 * @brief Start the webrtc bridge
 */
void webrtc_bridge_start(void);

/**
 * @brief Send message via webrtc bridge
 *
 * @param data pointer to the data to send
 * @param len length of the data to send
 *
 * @note the data is freed by the webrtc bridge, hence do not free it in the caller
 */
void webrtc_bridge_send_message(const char *data, int len);

/**
 * @brief Register a message handler for the webrtc bridge
 *
 * @param handler function pointer to the message handler
 */
void webrtc_bridge_register_handler(webrtc_bridge_msg_cb_t handler);

/* -------------------------------------------------------------------------- */
/*  Bridge Command Framework                                                  */
/*                                                                            */
/*  A generic request/response command channel that runs alongside the        */
/*  existing signaling message flow on a separate esp_hosted msg_id.          */
/* -------------------------------------------------------------------------- */

/** Maximum number of registered command handlers */
#define BRIDGE_CMD_MAX_HANDLERS  16

/**
 * @brief Command handler callback type
 *
 * Called when a command request is received from the remote side.
 * The handler must allocate *resp_data (the framework frees it after sending).
 * Set *resp_data to NULL and *resp_len to 0 if no payload is needed in the response.
 *
 * @param cmd_id   The command identifier
 * @param req_data Pointer to the request payload (may be NULL if req_len == 0)
 * @param req_len  Length of the request payload
 * @param resp_data[out] Handler-allocated response payload (caller frees)
 * @param resp_len[out]  Length of the response payload
 * @return ESP_OK on success, or an esp_err_t error code propagated to the requester
 */
typedef esp_err_t (*bridge_cmd_handler_t)(uint32_t cmd_id,
                                          const uint8_t *req_data, size_t req_len,
                                          uint8_t **resp_data, size_t *resp_len);

/**
 * @brief Initialize the bridge command subsystem
 *
 * Registers a second esp_hosted custom callback for commands (msg_id 0x1001).
 * Must be called after webrtc_bridge_start().
 *
 * @return ESP_OK on success
 */
esp_err_t bridge_cmd_init(void);

/**
 * @brief Response callback type for registered response handlers
 *
 * Invoked when a response for the given cmd_id arrives (from any prior
 * bridge_cmd_send). Register via bridge_cmd_register_response_handler().
 * resp_data is heap-allocated (SPIRAM-preferred); callback must free it.
 * If status != ESP_OK, resp_data may be NULL and resp_len 0.
 *
 * @param cmd_id    Command identifier
 * @param req_id    Request correlation ID
 * @param status    Remote handler result (esp_err_t)
 * @param resp_data Response payload (caller must free), may be NULL
 * @param resp_len  Response payload length
 */
typedef void (*bridge_cmd_response_cb_t)(uint32_t cmd_id, uint32_t req_id,
                                          esp_err_t status, uint8_t *resp_data, size_t resp_len);

/**
 * @brief Per-chunk response callback for large/chunked responses
 *
 * Called once per chunk as each arrives. Data pointer is valid only during
 * callback. For non-chunked responses (seq_num=0, is_final=true), also
 * delivered via this handler.
 *
 * @param cmd_id     Command identifier
 * @param req_id     Request correlation ID
 * @param status     Remote handler result (esp_err_t)
 * @param data       Chunk payload (valid only during callback)
 * @param len        Chunk payload length
 * @param seq_num    Chunk sequence number (0-based)
 * @param total_size Total payload size across all chunks
 * @param is_final   true if this is the last/only chunk
 */
typedef void (*bridge_cmd_chunked_response_cb_t)(uint32_t cmd_id, uint32_t req_id,
                                                   esp_err_t status,
                                                   const uint8_t *data, size_t len,
                                                   uint32_t seq_num, uint32_t total_size,
                                                   bool is_final);

/**
 * @brief Register a handler for a specific command ID (incoming requests)
 *
 * When a request with this cmd_id arrives from the remote side, the handler
 * is invoked (from the work-queue context, not the RPC rx thread).
 *
 * @param cmd_id  Application-defined command identifier
 * @param handler Callback to invoke on request
 * @return ESP_OK on success, ESP_ERR_NO_MEM if handler table is full
 */
esp_err_t bridge_cmd_register_handler(uint32_t cmd_id, bridge_cmd_handler_t handler);

/** Write callback provided to streaming handlers. Called repeatedly to send chunks. */
typedef esp_err_t (*bridge_cmd_write_cb_t)(const uint8_t *data, size_t len, void *ctx);

/**
 * @brief Streaming handler callback type
 *
 * Same as bridge_cmd_handler_t but receives a write_cb instead of resp_data/resp_len.
 * Handler calls write_cb() sequentially for each data chunk. Framework sends each
 * as a separate bridge_cmd response.
 */
typedef esp_err_t (*bridge_cmd_handler_with_cb_t)(uint32_t cmd_id,
                                                    const uint8_t *req_data, size_t req_len,
                                                    bridge_cmd_write_cb_t write_cb,
                                                    void *write_ctx);

/**
 * @brief Register a streaming handler for a specific command ID
 *
 * When a request with this cmd_id arrives, the handler is invoked with a
 * write callback. The handler calls write_cb() repeatedly with chunks of
 * data; each call results in a separate bridge_cmd response message.
 *
 * @param cmd_id  Application-defined command identifier
 * @param handler Streaming callback to invoke on request
 * @return ESP_OK on success, ESP_ERR_NO_MEM if handler table is full
 */
esp_err_t bridge_cmd_register_with_callback(uint32_t cmd_id,
                                             bridge_cmd_handler_with_cb_t handler);

/**
 * @brief Register a handler for responses to a specific command ID
 *
 * When a response with this cmd_id arrives (from any prior bridge_cmd_send),
 * the handler is invoked from a work-queue task. Only one handler per cmd_id;
 * re-registration updates the handler.
 *
 * @param cmd_id  Command identifier for which to receive responses
 * @param handler Callback to invoke when response arrives
 * @return ESP_OK on success, ESP_ERR_NO_MEM if table is full
 */
esp_err_t bridge_cmd_register_response_handler(uint32_t cmd_id,
                                                bridge_cmd_response_cb_t handler);

/**
 * @brief Register a chunked response handler for a command ID
 *
 * When chunked responses arrive (is_final=false in intermediate chunks),
 * the handler is called per-chunk. For non-chunked responses (seq_num=0,
 * is_final=true), also delivered via this handler.
 *
 * @param cmd_id  Command identifier for which to receive responses
 * @param handler Callback to invoke per-chunk when response arrives
 * @return ESP_OK on success, ESP_ERR_NO_MEM if table is full
 */
esp_err_t bridge_cmd_register_chunked_response_handler(uint32_t cmd_id,
                                                         bridge_cmd_chunked_response_cb_t handler);

/**
 * @brief Send a command (expects response)
 *
 * Serializes and sends the request via esp_hosted. Returns as soon as the
 * message is sent. Register a response handler (simple or chunked) via
 * bridge_cmd_register_response_handler() or
 * bridge_cmd_register_chunked_response_handler() to process the RSP.
 *
 * @param cmd_id   Application-defined command identifier
 * @param req_data Request payload (may be NULL if req_len == 0)
 * @param req_len  Length of request payload
 * @return ESP_OK on success (send succeeded), or error from transport
 */
esp_err_t bridge_cmd_send(uint32_t cmd_id,
                          const uint8_t *req_data, size_t req_len);

/* -------------------------------------------------------------------------- */
/*  Event (EVT) — fire-and-forget messages                                    */
/*                                                                            */
/*  Events are structurally like RSP but carry no req_id (not associated      */
/*  with any CMD).  They use cmd_id to identify the event type and may        */
/*  carry a payload which is auto-chunked like RSP.                           */
/* -------------------------------------------------------------------------- */

/**
 * @brief Simple event callback type
 *
 * Invoked when an event with the given cmd_id arrives.  Events are
 * unsolicited messages not associated with any prior CMD.
 * data is heap-allocated (SPIRAM-preferred); callback must free it.
 *
 * @param cmd_id  Event identifier
 * @param status  Status from sender
 * @param data    Event payload (caller must free), may be NULL
 * @param len     Event payload length
 */
typedef void (*bridge_cmd_event_cb_t)(uint32_t cmd_id, esp_err_t status,
                                       uint8_t *data, size_t len);

/**
 * @brief Per-chunk event callback for large/chunked events
 *
 * Called once per chunk as each arrives.  Data pointer is valid only
 * during callback.
 *
 * @param cmd_id     Event identifier
 * @param status     Status from sender
 * @param data       Chunk payload (valid only during callback)
 * @param len        Chunk payload length
 * @param seq_num    Chunk sequence number (0-based)
 * @param total_size Total payload size across all chunks
 * @param is_final   true if this is the last/only chunk
 */
typedef void (*bridge_cmd_chunked_event_cb_t)(uint32_t cmd_id, esp_err_t status,
                                               const uint8_t *data, size_t len,
                                               uint32_t seq_num, uint32_t total_size,
                                               bool is_final);

/**
 * @brief Register a simple event handler for a command ID
 *
 * When an EVT message with this cmd_id arrives, the framework aggregates
 * chunks (if any) and delivers the complete payload via a work-queue callback.
 *
 * @param cmd_id  Event identifier
 * @param handler Callback to invoke when complete event arrives
 * @return ESP_OK on success, ESP_ERR_NO_MEM if table is full
 */
esp_err_t bridge_cmd_register_event_handler(uint32_t cmd_id,
                                             bridge_cmd_event_cb_t handler);

/**
 * @brief Register a chunked event handler for a command ID
 *
 * When chunked events arrive, the handler is called per-chunk directly
 * from the RX callback (no work-queue hop).
 *
 * @param cmd_id  Event identifier
 * @param handler Per-chunk callback
 * @return ESP_OK on success, ESP_ERR_NO_MEM if table is full
 */
esp_err_t bridge_cmd_register_chunked_event_handler(uint32_t cmd_id,
                                                      bridge_cmd_chunked_event_cb_t handler);

/**
 * @brief Send an event (fire and forget, no response expected)
 *
 * Serializes and sends an EVT message.  Large payloads are auto-chunked.
 * Events carry cmd_id but no req_id.
 *
 * @param cmd_id  Event identifier
 * @param data    Payload (may be NULL if len == 0)
 * @param len     Payload length
 * @return ESP_OK on success, or error from transport
 */
esp_err_t bridge_cmd_send_event(uint32_t cmd_id,
                                 const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif
