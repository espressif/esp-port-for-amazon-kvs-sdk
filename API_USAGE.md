# Amazon Kinesis Video Streams WebRTC SDK for ESP - API Usage Guide

This document provides guidance on using the Amazon Kinesis Video Streams WebRTC SDK APIs for ESP platforms.

## API Overview

The ESP port of the KVS WebRTC SDK provides a **simplified high-level API** through `app_webrtc.h` that makes WebRTC integration with ESP devices straightforward and intuitive. The new API features:

### Pluggable Architecture
The SDK uses a modular architecture with interchangeable components:
- **Signaling Interfaces**: `webrtc_signaling_client_if_t` (KVS, AppRTC, Bridge, Custom)
- **Peer Connection Interfaces**: `webrtc_peer_connection_if_t` (KVS, Bridge, Custom)
- **Media Interfaces**: Standardized capture/player interfaces for audio/video

## Deployment Modes

The SDK supports multiple deployment modes using the new pluggable architecture:

### 1. Classic Mode (Single Device)
**Architecture**: `kvs_signaling + kvs_peer_connection`

Both signaling and media streaming handled by one ESP device using AWS KVS.

```c
// Set up KVS signaling configuration
static kvs_signaling_config_t kvs_signaling_cfg = {
 .pChannelName = "MyKvsChannel",
 .useIotCredentials = true, // or false for direct AWS credentials
 .awsRegion = "us-east-1",
 .caCertPath = "/spiffs/certs/cacert.pem",
 // IoT credentials or direct AWS credentials...
};

// Configure WebRTC with simplified API
app_webrtc_config_t config = APP_WEBRTC_CONFIG_DEFAULT();
config.signaling_client_if = kvs_signaling_client_if_get();
config.signaling_cfg = &kvs_signaling_cfg;
config.peer_connection_if = kvs_peer_connection_if_get();
config.video_capture = media_stream_get_video_capture_if();
config.audio_capture = media_stream_get_audio_capture_if();

app_webrtc_init(&config);
app_webrtc_run();
```

**Example**: `webrtc_classic`

### 2. AppRTC Mode (Browser Compatible)
**Architecture**: `apprtc_signaling + kvs_peer_connection`

Uses AppRTC-compatible signaling for direct browser integration.

```c
// Configure AppRTC signaling (browser-compatible)
apprtc_signaling_config_t apprtc_config = {
 .serverUrl = NULL, // Use default AppRTC server
 .roomId = NULL, // Will be set based on role type
 .autoConnect = false,
 .connectionTimeout = 30000,
 .logLevel = 3
};

// Configure WebRTC app with simplified API
app_webrtc_config_t config = APP_WEBRTC_CONFIG_DEFAULT();
config.signaling_client_if = apprtc_signaling_client_if_get();
config.signaling_cfg = &apprtc_config;
config.peer_connection_if = kvs_peer_connection_if_get();
config.video_capture = media_stream_get_video_capture_if();
config.audio_capture = media_stream_get_audio_capture_if();

// Advanced configuration: Set role and enable bidirectional media
app_webrtc_init(&config);
app_webrtc_set_role(WEBRTC_CHANNEL_ROLE_TYPE_MASTER);
app_webrtc_enable_media_reception(true);
app_webrtc_run();
```

**Example**: `esp_camera`

### 3. Split Mode - Streaming Device
**Architecture**: `bridge_signaling + kvs_peer_connection`

Handles media streaming while receiving signaling from partner device.

```c
app_webrtc_config_t config = APP_WEBRTC_CONFIG_DEFAULT();
config.signaling_client_if = bridge_signaling_client_if_get();
config.signaling_cfg = &bridge_config;
config.peer_connection_if = kvs_peer_connection_if_get();
config.video_capture = media_stream_get_video_capture_if();
```

**Example**: `streaming_only`

### 4. Split Mode - Signaling Device
**Architecture**: `kvs_signaling + bridge_peer_connection`

Handles AWS KVS signaling and forwards to streaming device via bridge.

```c
app_webrtc_config_t config = APP_WEBRTC_CONFIG_DEFAULT();
config.signaling_client_if = kvs_signaling_client_if_get();
config.signaling_cfg = &kvs_signaling_cfg;
config.peer_connection_if = bridge_peer_connection_if_get();
// No media interfaces = auto-detected signaling-only mode
```

**Example**: `signaling_only`

### 5. Custom Credentials Mode (ESP RainMaker Integration)

Use a **credential callback function** for dynamic credential provisioning. This is the **recommended approach** for ESP RainMaker integration and other systems that provide AWS credentials at runtime.

```c
// Credential callback implementation (e.g., for ESP RainMaker)
int rmaker_fetch_aws_credentials(uint64_t user_data,
 const char **pAK, uint32_t *pAKLen,
 const char **pSK, uint32_t *pSKLen,
 const char **pTok, uint32_t *pTokLen,
 uint64_t *pExp)
{
 // Use ESP RainMaker's streamlined credential API
 esp_rmaker_aws_credentials_t *credentials = esp_rmaker_get_aws_security_token("esp-videostream-v1-NodeRole");
 if (!credentials) {
 return -1;
 }

 // Set output pointers to credential data
 *pAK = credentials->access_key;
 *pAKLen = credentials->access_key_len;
 *pSK = credentials->secret_key;
 *pSKLen = credentials->secret_key_len;
 *pTok = credentials->session_token;
 *pTokLen = credentials->session_token_len;
 *pExp = credentials->expiration * HUNDREDS_OF_NANOS_IN_A_SECOND; // Convert to 100ns units

 return 0; // Success
}

// Configure KVS signaling with credential callback
static kvs_signaling_config_t kvs_signaling_cfg = {
 .pChannelName = "esp-v1-<node-id>",
 .awsRegion = "us-east-1", // Or use esp_rmaker_get_aws_region()
 .caCertPath = "/spiffs/certs/cacert.pem",

 // Credential callback has highest precedence
 .fetch_credentials_cb = rmaker_fetch_aws_credentials,
 .fetch_credentials_user_data = 0, // Optional user data
};

// Standard WebRTC configuration
app_webrtc_config_t config = APP_WEBRTC_CONFIG_DEFAULT();
config.signaling_client_if = kvs_signaling_client_if_get();
config.signaling_cfg = &kvs_signaling_cfg;
config.peer_connection_if = kvs_peer_connection_if_get();
// ... add media interfaces ...

app_webrtc_init(&config);
app_webrtc_run();
```

**Benefits of Credential Callbacks:**
- Dynamic credentials fetched on-demand
- Automatic renewal handles expiration transparently
- External RAM allocation with proper cleanup
- Works with ESP RainMaker and custom auth systems

### 6. Custom Signaling Mode

Implement your own signaling by creating custom `webrtc_signaling_client_if_t` and/or `webrtc_peer_connection_if_t`. See [docs/custom_signaling.md](docs/custom_signaling.md) for implementation guidance.

## Key API Functions

### Core Application API

```c
// Essential configuration (minimal setup)
app_webrtc_config_t config = APP_WEBRTC_CONFIG_DEFAULT();
config.signaling_client_if = kvs_signaling_client_if_get(); // Required: signaling interface
config.signaling_cfg = &kvs_signaling_cfg; // Required: signaling config
config.peer_connection_if = kvs_peer_connection_if_get(); // Required: peer connection interface

// Optional: Media interfaces (set to NULL for signaling-only applications)
config.video_capture = media_stream_get_video_capture_if();
config.audio_capture = media_stream_get_audio_capture_if();
config.video_player = media_stream_get_video_player_if(); // For receiving video
config.audio_player = media_stream_get_audio_player_if(); // For receiving audio

// Initialize with smart defaults
WEBRTC_STATUS app_webrtc_init(app_webrtc_config_t *config);

// Run the WebRTC application (blocking)
WEBRTC_STATUS app_webrtc_run(void);

// Clean termination
WEBRTC_STATUS app_webrtc_terminate(void);

// Event notifications
int32_t app_webrtc_register_event_callback(app_webrtc_event_callback_t callback, void *user_ctx);
```

### Advanced Configuration APIs

```c
// Role configuration (default: MASTER)
WEBRTC_STATUS app_webrtc_set_role(webrtc_channel_role_type_t role);

// ICE configuration (default: trickle ICE + TURN enabled)
WEBRTC_STATUS app_webrtc_set_ice_config(bool trickle_ice, bool use_turn);

// Logging (default: INFO level)
WEBRTC_STATUS app_webrtc_set_log_level(uint32_t level);

// Codec selection (default: OPUS + H.264)
WEBRTC_STATUS app_webrtc_set_codecs(app_webrtc_rtc_codec_t audio_codec, app_webrtc_rtc_codec_t video_codec);

// Media type (default: auto-detected)
WEBRTC_STATUS app_webrtc_set_media_type(app_webrtc_streaming_media_t media_type);

// Media reception (default: disabled for IoT devices)
WEBRTC_STATUS app_webrtc_enable_media_reception(bool enable);

// Force signaling-only mode (default: auto-detected)
WEBRTC_STATUS app_webrtc_set_signaling_only_mode(bool enable);
```

## Configuration Structures

### KVS Signaling Configuration
```c
typedef struct {
 // Channel configuration
 char *pChannelName; // Required: KVS channel name

 // AWS credentials (choose one approach)
 bool useIotCredentials; // true=IoT Core, false=direct AWS

 // IoT Core credentials (when useIotCredentials=true)
 char *iotCoreCredentialEndpoint; // IoT credential endpoint URL
 char *iotCoreCert; // Path to device certificate
 char *iotCorePrivateKey; // Path to private key
 char *iotCoreRoleAlias; // Role alias for credential exchange
 char *iotCoreThingName; // IoT thing name

 // Direct AWS credentials (when useIotCredentials=false)
 char *awsAccessKey; // AWS access key ID
 char *awsSecretKey; // AWS secret access key
 char *awsSessionToken; // Session token (optional)

 // Credential callback (highest precedence - ESP RainMaker integration)
 kvs_fetch_credentials_cb_t fetch_credentials_cb; // Callback function
 uint64_t fetch_credentials_user_data; // User data for callback

 // Common AWS options
 char *awsRegion; // AWS region (e.g., "us-east-1")
 char *caCertPath; // CA certificate bundle path
} kvs_signaling_config_t;
```

### AppRTC Signaling Configuration
```c
typedef struct {
 char *serverUrl; // AppRTC server URL (NULL=default)
 char *roomId; // Room ID (NULL=auto-generated)
 bool autoConnect; // Auto-connect on init
 uint32_t connectionTimeout; // Connection timeout (ms)
 uint32_t logLevel; // Logging level
} apprtc_signaling_config_t;
```

### Credential Callback Signature
```c
// Credential provider callback for dynamic credential fetching
typedef int (*kvs_fetch_credentials_cb_t)(
 uint64_t customData, // User data from fetch_credentials_user_data
 const char **pAccessKey, // Output: AWS access key
 uint32_t *pAccessKeyLen, // Output: Access key length
 const char **pSecretKey, // Output: AWS secret key
 uint32_t *pSecretKeyLen, // Output: Secret key length
 const char **pSessionToken, // Output: Session token
 uint32_t *pSessionTokenLen, // Output: Session token length
 uint64_t *pExpiration // Output: Expiration (100ns units)
);
```

### Split Mode & Bridge Functions

```c
// Register callback for bridge message forwarding
int app_webrtc_register_msg_callback(app_webrtc_send_msg_cb_t callback);

// Send message from bridge to signaling server
int app_webrtc_send_msg_to_signaling(webrtc_message_t *message);

// Trigger offer creation (for initiator role)
int app_webrtc_trigger_offer(char *pPeerId);
```

### ICE Server Management

```c
// Get ICE servers from signaling interface
WEBRTC_STATUS app_webrtc_get_ice_servers(uint32_t *pIceServerCount, void *pIceConfiguration);

// Query specific ICE server (for bridge/RPC patterns)
WEBRTC_STATUS app_webrtc_get_server_by_idx(int index, bool useTurn, uint8_t **data, int *len, bool *have_more);

// Check if ICE refresh needed
WEBRTC_STATUS app_webrtc_is_ice_refresh_needed(bool *refreshNeeded);

// Trigger background ICE refresh
WEBRTC_STATUS app_webrtc_refresh_ice_configuration(void);

// Update ICE servers during runtime
WEBRTC_STATUS app_webrtc_update_ice_servers(void);
```

### Data Channel Functions

```c
// Set data channel callbacks for peer
WEBRTC_STATUS app_webrtc_set_data_channel_callbacks(const char *peer_id,
 app_webrtc_rtc_on_open_t onOpen,
 app_webrtc_rtc_on_message_t onMessage,
 uint64_t custom_data);

// Send data through data channel
WEBRTC_STATUS app_webrtc_send_data_channel_message(const char *peer_id,
 void *pDataChannel,
 bool isBinary,
 const uint8_t *pMessage,
 uint32_t messageLen);
```

## Media Handling

The SDK uses media capture and player interfaces to handle audio and video. Instead of directly sending frames via API calls, you provide interfaces that the SDK uses to capture and play media.

### Media Interfaces

```c
// Get the default video capture interface
media_stream_video_capture_t* media_stream_get_video_capture_if(void);

// Get the default audio capture interface
media_stream_audio_capture_t* media_stream_get_audio_capture_if(void);

// Get the default video player interface
media_stream_video_player_t* media_stream_get_video_player_if(void);

// Get the default audio player interface
media_stream_audio_player_t* media_stream_get_audio_player_if(void);
```

These interfaces are passed to the WebRTC application configuration:

```c
app_webrtc_config_t config = APP_WEBRTC_CONFIG_DEFAULT();
config.video_capture = media_stream_get_video_capture_if();
config.audio_capture = media_stream_get_audio_capture_if();
config.video_player = media_stream_get_video_player_if();
config.audio_player = media_stream_get_audio_player_if();
```

## Configuration

### WebRTC Configuration

#### Essential Configuration (Required)
```c
app_webrtc_config_t config = APP_WEBRTC_CONFIG_DEFAULT();

// Essential: Signaling interface
config.signaling_client_if = kvs_signaling_client_if_get(); // Choose your signaling
config.signaling_cfg = &kvs_signaling_cfg; // Signaling-specific config

// Essential: Peer connection interface
config.peer_connection_if = kvs_peer_connection_if_get(); // Choose your peer connection
config.implementation_config = NULL; // Implementation-specific config (optional)

// Optional: Media interfaces (NULL = signaling-only mode)
config.video_capture = media_stream_get_video_capture_if();
config.audio_capture = media_stream_get_audio_capture_if();
config.video_player = media_stream_get_video_player_if();
config.audio_player = media_stream_get_audio_player_if();
```

#### Smart Defaults (No Configuration Needed)
The API automatically provides reasonable defaults:
- **Role**: `WEBRTC_CHANNEL_ROLE_TYPE_MASTER` (initiates connections)
- **ICE**: Trickle ICE enabled, TURN servers enabled
- **Codecs**: OPUS (audio), H.264 (video)
- **Logging**: INFO level
- **Media Reception**: Disabled (most IoT devices are senders)
- **Mode**: Auto-detected (from interfaces provided)

#### Advanced Configuration (Override Defaults)
```c
// After app_webrtc_init(), override any defaults:
app_webrtc_set_role(WEBRTC_CHANNEL_ROLE_TYPE_VIEWER); // Change role
app_webrtc_set_ice_config(false, true); // Disable trickle ICE
app_webrtc_set_log_level(2); // Enable DEBUG logging
app_webrtc_enable_media_reception(true); // Enable media reception
app_webrtc_set_codecs(APP_WEBRTC_CODEC_OPUS, APP_WEBRTC_CODEC_VP8); // Use VP8
```

### Core Configuration Fields

| Field | Type | Description |
|-------|------|-------------|
| `signaling_client_if` | `webrtc_signaling_client_if_t*` | **Required**: Signaling interface (KVS, AppRTC, Bridge, Custom) |
| `signaling_cfg` | `void*` | **Required**: Opaque pointer to signaling-specific configuration |
| `peer_connection_if` | `webrtc_peer_connection_if_t*` | **Required**: Peer connection interface (KVS, Bridge, Custom) |
| `implementation_config` | `void*` | Optional: Implementation-specific configuration |
| `video_capture` | `void*` | Optional: Video capture interface (NULL = no video) |
| `audio_capture` | `void*` | Optional: Audio capture interface (NULL = no audio) |
| `video_player` | `void*` | Optional: Video player interface (NULL = no video reception) |
| `audio_player` | `void*` | Optional: Audio player interface (NULL = no audio reception) |
| `data_channel_config` | `webrtc_data_channel_config_t*` | Optional: Data channel callbacks + configuration (NULL = no data channel) |

### Available Interfaces

#### Signaling Interfaces
```c
// AWS KVS signaling (full AWS integration)
webrtc_signaling_client_if_t* kvs_signaling_client_if_get(void);

// AppRTC signaling (browser-compatible)
webrtc_signaling_client_if_t* apprtc_signaling_client_if_get(void);

// Bridge signaling (for split mode streaming device)
webrtc_signaling_client_if_t* bridge_signaling_client_if_get(void);
```

#### Peer Connection Interfaces
```c
// KVS peer connection (full WebRTC functionality)
webrtc_peer_connection_if_t* kvs_peer_connection_if_get(void);

// Bridge peer connection (signaling-only, no WebRTC SDK)
webrtc_peer_connection_if_t* bridge_peer_connection_if_get(void);
```

#### Media Interfaces
```c
// Get default media capture and player interfaces
media_stream_video_capture_t* media_stream_get_video_capture_if(void);
media_stream_audio_capture_t* media_stream_get_audio_capture_if(void);
media_stream_video_player_t* media_stream_get_video_player_if(void);
media_stream_audio_player_t* media_stream_get_audio_player_if(void);
```

### Signaling Role Types

```c
typedef enum {
 WEBRTC_CHANNEL_ROLE_TYPE_MASTER = 0, // Initiates connections (default)
 WEBRTC_CHANNEL_ROLE_TYPE_VIEWER, // Receives connections
} webrtc_channel_role_type_t;
```

### Media Types

```c
typedef enum {
 APP_WEBRTC_MEDIA_VIDEO, // Video only
 APP_WEBRTC_MEDIA_AUDIO_VIDEO, // Both audio and video (default when both interfaces provided)
} app_webrtc_streaming_media_t;
```

### Codec Types

```c
typedef enum {
 APP_WEBRTC_CODEC_H264 = 1, // H.264 video codec (default)
 APP_WEBRTC_CODEC_OPUS = 2, // OPUS audio codec (default)
 APP_WEBRTC_CODEC_VP8 = 3, // VP8 video codec
 APP_WEBRTC_CODEC_MULAW = 4, // MULAW audio codec
 APP_WEBRTC_CODEC_ALAW = 5, // ALAW audio codec
 APP_WEBRTC_CODEC_H265 = 7, // H.265 video codec
} app_webrtc_rtc_codec_t;
```

## Event Handling

The SDK provides an event-based system for handling WebRTC state changes:

```c
// Event callback type
typedef void (*app_webrtc_event_callback_t) (app_webrtc_event_data_t *event_data, void *user_ctx);

// Event data structure
typedef struct {
 app_webrtc_event_t event_id;
 UINT32 status_code;
 CHAR peer_id[MAX_SIGNALING_CLIENT_ID_LEN + 1];
 CHAR message[256];
} app_webrtc_event_data_t;

// Example event handler
static void app_webrtc_event_handler(app_webrtc_event_data_t *event_data, void *user_ctx)
{
 if (event_data == NULL) {
 return;
 }

 switch (event_data->event_id) {
 case APP_WEBRTC_EVENT_INITIALIZED:
 // WebRTC stack initialized
 break;
 case APP_WEBRTC_EVENT_SIGNALING_CONNECTED:
 // Signaling connection established
 break;
 case APP_WEBRTC_EVENT_PEER_CONNECTED:
 // Peer connection established
 break;
 case APP_WEBRTC_EVENT_STREAMING_STARTED:
 // Media streaming started
 break;
 // Handle other events...
 }
}
```

### Key Events

| Event | Description |
|-------|-------------|
| `APP_WEBRTC_EVENT_INITIALIZED` | WebRTC stack has been initialized |
| `APP_WEBRTC_EVENT_DEINITIALIZING` | WebRTC stack is being deinitialized |
| `APP_WEBRTC_EVENT_SIGNALING_CONNECTING` | Attempting to connect to signaling server |
| `APP_WEBRTC_EVENT_SIGNALING_CONNECTED` | Connected to signaling server |
| `APP_WEBRTC_EVENT_SIGNALING_DISCONNECTED` | Disconnected from signaling server |
| `APP_WEBRTC_EVENT_PEER_CONNECTION_REQUESTED` | Peer connection request received |
| `APP_WEBRTC_EVENT_PEER_CONNECTED` | Peer connection established |
| `APP_WEBRTC_EVENT_PEER_DISCONNECTED` | Peer has disconnected |
| `APP_WEBRTC_EVENT_STREAMING_STARTED` | Media streaming has started |
| `APP_WEBRTC_EVENT_STREAMING_STOPPED` | Media streaming has stopped |
| `APP_WEBRTC_EVENT_RECEIVED_OFFER` | Received SDP offer from peer |
| `APP_WEBRTC_EVENT_SENT_ANSWER` | Sent SDP answer to peer |
| `APP_WEBRTC_EVENT_ERROR` | General error occurred |
| `APP_WEBRTC_EVENT_SIGNALING_ERROR` | Error in signaling |
| `APP_WEBRTC_EVENT_PEER_CONNECTION_FAILED` | Peer connection failed |

## Example Usage

For complete working examples, refer to the source files in the `examples/` directory:

| Mode | Example | Source File |
|------|---------|-------------|
| Classic (KVS) | `webrtc_classic` | `examples/webrtc_classic/main/webrtc_main.c` |
| AppRTC | `esp_camera` | `examples/esp_camera/main/esp_webrtc_camera_main.c` |
| Split - Streaming | `streaming_only` | `examples/streaming_only/main/streaming_only_main.c` |
| Split - Signaling | `signaling_only` | `examples/signaling_only/main/signaling_only_main.c` |

See the [Deployment Modes](#deployment-modes) section above for the configuration pattern used in each mode.

For custom signaling implementations, see [docs/custom_signaling.md](docs/custom_signaling.md).

### ESP RainMaker Integration

The SDK can be integrated with ESP RainMaker for device management and cloud connectivity. The credential callback mechanism (`fetch_credentials_cb` in `kvs_signaling_config_t`) enables dynamic credential provisioning from RainMaker.

For a complete working example, see the [kvs_webrtc_camera example](https://github.com/espressif/esp-rainmaker/tree/master/examples/camera) in the ESP RainMaker repository.

## Media Interface Implementation

The SDK provides default implementations for media capture and playback, but you can also implement your own interfaces if needed:

### Video Capture Interface

```c
typedef struct {
 esp_err_t (*init)(video_capture_config_t *config, video_capture_handle_t *handle);
 esp_err_t (*start)(video_capture_handle_t handle);
 esp_err_t (*stop)(video_capture_handle_t handle);
 esp_err_t (*get_frame)(video_capture_handle_t handle, video_frame_t **frame, uint32_t timeout_ms);
 esp_err_t (*release_frame)(video_capture_handle_t handle, video_frame_t *frame);
 esp_err_t (*deinit)(video_capture_handle_t handle);
} media_stream_video_capture_t;
```

### Audio Capture Interface

```c
typedef struct {
 esp_err_t (*init)(audio_capture_config_t *config, audio_capture_handle_t *handle);
 esp_err_t (*start)(audio_capture_handle_t handle);
 esp_err_t (*stop)(audio_capture_handle_t handle);
 esp_err_t (*get_frame)(audio_capture_handle_t handle, audio_frame_t **frame, uint32_t timeout_ms);
 esp_err_t (*release_frame)(audio_capture_handle_t handle, audio_frame_t *frame);
 esp_err_t (*deinit)(audio_capture_handle_t handle);
} media_stream_audio_capture_t;
```

### Video Player Interface

```c
typedef struct {
 esp_err_t (*init)(video_player_config_t *config, video_player_handle_t *handle);
 esp_err_t (*start)(video_player_handle_t handle);
 esp_err_t (*stop)(video_player_handle_t handle);
 esp_err_t (*play_frame)(video_player_handle_t handle, const uint8_t *data,
 uint32_t len, bool is_keyframe);
 esp_err_t (*deinit)(video_player_handle_t handle);
} media_stream_video_player_t;
```

### Audio Player Interface

```c
typedef struct {
 esp_err_t (*init)(audio_player_config_t *config, audio_player_handle_t *handle);
 esp_err_t (*start)(audio_player_handle_t handle);
 esp_err_t (*stop)(audio_player_handle_t handle);
 esp_err_t (*play_frame)(audio_player_handle_t handle, const uint8_t *data, uint32_t len);
 esp_err_t (*deinit)(audio_player_handle_t handle);
} media_stream_audio_player_t;
```

## Troubleshooting

- **Connection Issues**: Check Wi-Fi connectivity and AWS credentials
- **Signaling Failures**: Verify AWS region and KVS channel configuration
- **Media Issues**: Check camera and microphone hardware connections
- **Performance Problems**: Consider reducing video resolution or frame rate
- **Stack Overflow**: Ensure work queue is initialized before WebRTC operations
- **ICE Server Issues**: Check TURN server configuration and network connectivity
- **Custom Signaling**: See [docs/custom_signaling.md](docs/custom_signaling.md) for troubleshooting custom implementations
- **Debugging**: Enable verbose logging by setting `logLevel` to a higher value

## Additional Resources

- **[docs/custom_signaling.md](docs/custom_signaling.md)**: Comprehensive guide for implementing custom signaling protocols
- **Example Applications**: See `examples/` for complete working implementations
- **AWS KVS WebRTC Documentation**: Refer to AWS documentation for more details on the underlying SDK
- **Component Documentation**: Individual component READMEs in `components/`
