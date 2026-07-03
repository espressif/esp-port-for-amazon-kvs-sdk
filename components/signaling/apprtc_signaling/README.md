# AppRTC Signaling

This component provides an AppRTC-compatible signaling client for use with the ESP32 platform and the Kinesis Video Streams WebRTC SDK. It enables WebRTC peer connection establishment by handling signaling messages (offer, answer, ICE candidates) over a WebSocket connection to an AppRTC server.

## Features

- Connects to an AppRTC signaling server via WebSocket.
- Handles room registration, client ID management, and initiator logic.
- Parses and forwards WebRTC signaling messages (offer, answer, candidate) to the application.
- Supports reconnection and error handling.
- Integrates with the KVS WebRTC SDK for media and ICE negotiation.

## Usage

1. **Initialization**:
 Initialize the component and register a message handler to receive signaling messages.

2. **Connect to Room**:
 Use the provided API to connect to an AppRTC room by room ID.

3. **Send/Receive Messages**:
 Use the API to send signaling messages (SDP, ICE) and process incoming messages via the registered handler.

4. **Disconnection and Reconnection**:
 The component handles clean disconnection and can automatically attempt to reconnect if the connection is lost.

## API Overview

- `apprtc_signaling_init(...)` — initialize the client and pass the incoming-message handler as a parameter.
- `apprtc_signaling_send_webrtc_message(webrtc_message_t *pMsg)` — send a WebRTC message to the signaling server.
- `apprtc_signaling_get_room_id()` — return the current room ID (NULL if not connected).
- `apprtc_signaling_client_if_get()` — return the `webrtc_signaling_client_if_t*` for wiring into `app_webrtc_config_t`.
- `apprtc_signaling_deinit()` — disconnect and free resources.

## Integration

This component is designed to be used as part of the ESP32 KVS WebRTC camera example, but can be adapted for other WebRTC use cases requiring AppRTC signaling.

## Configuration

- The AppRTC server URL and ICE server URL can be configured via Kconfig or by modifying the component source.

## Dependencies

- ESP-IDF (FreeRTOS, WebSocket, HTTP client)
- cJSON

## Example

See the `esp_camera` example for usage details.

## License

Apache 2.0. See LICENSE file for details.
