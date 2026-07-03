# WebRTC Bridge

Communication bridge between the signaling processor and the streaming processor in **split mode**. When running with [split mode](../../README.md#deployment-modes), one device handles KVS signaling (e.g. ESP32-C6) while another handles media streaming (e.g. ESP32-P4). The bridge provides the interconnect between them.

## Transport Backends

| Backend | Default | Notes |
|---------|---------|-------|
| **esp_hosted** | Yes | Uses custom data channel with two `msg_id` values: `0x1000` (signaling messages) and `0x1001` (bridge commands). Requires `CONFIG_ESP_WEBRTC_BRIDGE_HOSTED`. |
| **MQTT** | No | Alternative transport using an MQTT broker. Enable via `CONFIG_ESP_WEBRTC_BRIDGE_MQTT`. |

## Two Channels

The bridge exposes two independent communication channels over the chosen transport:

### 1. Signaling Message Channel (msg_id 0x1000)

Passthrough of signaling JSON messages (SDP offers/answers, ICE candidates) between the signaling and streaming processors. Uses a simple send/receive API:

- `webrtc_bridge_send_message()` -- send a signaling message to the remote side
- `webrtc_bridge_register_handler()` -- register a callback for incoming signaling messages

Large messages are automatically chunked at the transport layer (`hosted_chunked_transport`) and reassembled on the receiving side.

### 2. Bridge Command Framework (msg_id 0x1001)

A structured **request/response RPC** channel for control and query operations, independent of the signaling message flow. Examples: querying camera resolution, requesting JPEG snapshots, fetching ICE server configuration, time synchronization.

## Bridge Command Framework Design

### Serialization

All commands use **protobuf-c** serialization (`BridgeCommand` message defined in `bridge_cmd.proto`). Each protobuf message carries built-in chunk metadata (`seq_num`, `is_final`, `total_size`), so chunking is handled at the application protocol level rather than the transport layer.

### Auto-Chunking

Payloads larger than ~8KB are automatically split into ~8KB frames by `bridge_cmd_send_response()`. Each chunk is a self-contained `BridgeCommand` protobuf that fits within a single esp_hosted frame. The sender always chunks the same way regardless of the receiver's handler type.

### Request Handlers (Server Side)

Register on the side that **serves** a command:

| Type | API | Behavior |
|------|-----|----------|
| **Sync** | `bridge_cmd_register_handler()` | Handler allocates a response buffer; framework sends it (auto-chunked if needed) and frees the buffer. |
| **Streaming** | `bridge_cmd_register_with_callback()` | Handler receives a `write_cb` and calls it repeatedly to stream chunks. Framework sends a final marker after the handler returns. |

Both handler types are invoked from a **work-queue task**, not directly from the RX thread.

### Response Handlers (Client Side)

Register on the side that **sends** a command and wants to process the response:

| Type | API | Behavior |
|------|-----|----------|
| **Simple** | `bridge_cmd_register_response_handler()` | Framework aggregates all chunks into a single buffer and delivers the complete payload via a work-queue callback. The callback receives `resp_data` (heap-allocated, caller must free) and `resp_len`. |
| **Chunked** | `bridge_cmd_register_chunked_response_handler()` | Callback is invoked per-chunk directly from the RX thread (no work-queue hop). Useful for streaming large payloads without buffering the entire response. |

The handler type is the **receiver's choice** -- the sender always sends in the same format. The framework transparently handles the mismatch (e.g. aggregating chunks for a simple handler).

### Event Handlers (EVT)

Events are fire-and-forget messages (`msg_type=EVT`) not associated with any CMD. They carry `cmd_id` to identify the event type but no `req_id`. Like RSP, event payloads are auto-chunked if large.

| Type | API | Behavior |
|------|-----|----------|
| **Simple** | `bridge_cmd_register_event_handler()` | Framework aggregates chunks and delivers the complete payload via work-queue callback. Callback receives `data` (heap-allocated, caller must free). |
| **Chunked** | `bridge_cmd_register_chunked_event_handler()` | Callback is invoked per-chunk directly from the RX thread. |

Send events with `bridge_cmd_send_event(cmd_id, data, len)`.

### Three Message Types Summary

| Type | Purpose | req_id | Expects response |
|------|---------|--------|------------------|
| **CMD** | Request | Yes (assigned by sender) | Yes (RSP) |
| **RSP** | Response to CMD | Yes (echoed from CMD) | No |
| **EVT** | Unsolicited notification | No | No |

### Dispatch Model

- **Request handlers** (sync and streaming): dispatched via work-queue, safe for blocking operations.
- **Simple response/event handlers**: dispatched via work-queue after full reassembly.
- **Chunked response/event handlers**: called directly from the RX callback for minimal latency.

## Adding New Commands

1. **Define the command ID** in [`bridge_cmd_defs.h`](include/bridge_cmd_defs.h):
   ```c
   #define BRIDGE_CMD_MY_COMMAND  0x0006
   ```

2. **Define payload structures** (request and/or response) in the same header.

3. **Register a handler on the serving side** (e.g. in the streaming_only app):
   ```c
   // Sync handler
   bridge_cmd_register_handler(BRIDGE_CMD_MY_COMMAND, my_command_handler);

   // Or streaming handler for large responses
   bridge_cmd_register_with_callback(BRIDGE_CMD_MY_COMMAND, my_streaming_handler);
   ```

4. **Register a response handler on the requesting side** (e.g. in the signaling_only app):
   ```c
   // Simple handler (framework aggregates chunks)
   bridge_cmd_register_response_handler(BRIDGE_CMD_MY_COMMAND, my_response_cb);

   // Or chunked handler (per-chunk delivery)
   bridge_cmd_register_chunked_response_handler(BRIDGE_CMD_MY_COMMAND, my_chunk_cb);
   ```

5. **Send the command**:
   ```c
   bridge_cmd_send(BRIDGE_CMD_MY_COMMAND, req_payload, req_len);
   ```
