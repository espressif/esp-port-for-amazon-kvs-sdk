# WebRTC SDK Connection Process

This document outlines the sequential steps that occur during the WebRTC connection process with Kinesis Video Streams, based on detailed logs from a successful connection.

## 1. Initial Setup
- Application initializes with channel name `demo-channel`
- Loads certificates from specified paths (`certificate.pem`, `private.key`, `cacert.pem`)

## 2. Credential Setup
- Creates IoT credential provider with endpoint: `<your-iot-endpoint>.credentials.iot.<region>.amazonaws.com`
- Uses role alias `<your-role-alias>` and thing name `<your-thing-name>`
- Fetches IoT credentials via HTTPS request to the credentials endpoint
  - Getting IoT credential with URL: `https://<your-iot-endpoint>.credentials.iot.<region>.amazonaws.com/role-aliases/<your-role-alias>/credentials`
- Receives credentials with expiration time (valid for 1 hour)

## 3. Media Source Initialization
- Discovers sample H.264 video frames from the samples directory
- Initializes WebRTC and SRTP security components

## 4. Signaling Client Creation
- Initializes signaling client in "New" state
- Transitions to "Get Security Credentials" state using the IoT credentials fetched earlier
- Control plane URL is generated: `https://kinesisvideo.us-east-1.amazonaws.com`

## 5. Channel Description
- Transitions to "Describe Channel" state
- Sends API request to: `https://kinesisvideo.us-east-1.amazonaws.com/describeSignalingChannel`
- Request body:
  ```json
  {
    "ChannelName": "demo-channel"
  }
  ```
- API Response:
  ```json
  {
    "ChannelInfo": {
      "ChannelARN": "arn:aws:kinesisvideo:us-east-1:123456789012:channel/demo-channel/1234567890123",
      "ChannelName": "demo-channel",
      "ChannelStatus": "ACTIVE",
      "ChannelType": "SINGLE_MASTER",
      "CreationTime": 1.234567890123E9,
      "FullMeshConfiguration": null,
      "SingleMasterConfiguration": {
        "MessageTtlSeconds": 60
      },
      "Version": "<channel-version>"
    }
  }
  ```

## 6. Channel Endpoint Retrieval
- Transitions to "Get Channel Endpoint" state
- Sends API request to: `https://kinesisvideo.us-east-1.amazonaws.com/getSignalingChannelEndpoint`
- Request body:
  ```json
  {
    "ChannelARN": "arn:aws:kinesisvideo:us-east-1:123456789012:channel/demo-channel/1234567890123",
    "SingleMasterChannelEndpointConfiguration": {
      "Protocols": ["WSS", "HTTPS"],
      "Role": "MASTER"
    }
  }
  ```
- API Response:
  ```json
  {
    "ResourceEndpointList": [
      {
        "Protocol": "HTTPS",
        "ResourceEndpoint": "https://r-<endpoint-id>.kinesisvideo.us-east-1.amazonaws.com"
      },
      {
        "Protocol": "WSS",
        "ResourceEndpoint": "wss://m-<endpoint-id>.kinesisvideo.us-east-1.amazonaws.com"
      }
    ]
  }
  ```

## 7. ICE Server Configuration
- Transitions to "Get ICE Server Configuration" state
- Sends API request to: `https://r-<endpoint-id>.kinesisvideo.us-east-1.amazonaws.com/v1/get-ice-server-config`
- Request body:
  ```json
  {
    "ChannelARN": "arn:aws:kinesisvideo:us-east-1:123456789012:channel/demo-channel/1234567890123",
    "ClientId": "ProducerMaster",
    "Service": "TURN"
  }
  ```
- API Response:
  ```json
  {
    "IceServerList": [
      {
        "Password": "<turn-password-1>",
        "Ttl": 300,
        "Uris": [
          "turn:<turn-host-1>.kinesisvideo.us-east-1.amazonaws.com:443?transport=udp",
          "turns:<turn-host-1>.kinesisvideo.us-east-1.amazonaws.com:443?transport=udp",
          "turns:<turn-host-1>.kinesisvideo.us-east-1.amazonaws.com:443?transport=tcp"
        ],
        "Username": "<turn-username>"
      },
      {
        "Password": "<turn-password-2>",
        "Ttl": 300,
        "Uris": [
          "turn:<turn-host-2>.kinesisvideo.us-east-1.amazonaws.com:443?transport=udp",
          "turns:<turn-host-2>.kinesisvideo.us-east-1.amazonaws.com:443?transport=udp",
          "turns:<turn-host-2>.kinesisvideo.us-east-1.amazonaws.com:443?transport=tcp"
        ],
        "Username": "<turn-username>"
      }
    ]
  }
  ```

## 8. Signaling Connection
- Transitions to "Ready" state once all configurations are obtained
- Begins connection to the WSS endpoint: `wss://m-<endpoint-id>.kinesisvideo.us-east-1.amazonaws.com`
- Fully qualified URL example: `wss://m-<endpoint-id>.kinesisvideo.us-east-1.amazonaws.com?X-Amz-ChannelARN=arn:aws:kinesisvideo:us-east-1:123456789012:channel/demo-channel/1234567890123`
- Successfully establishes WebSocket connection
- Transitions to "Connected" state when the connection is established

## 9. WebRTC Peer Connection (After Connected State)
- Once connected, the signaling client can exchange SDP offers/answers and ICE candidates
- WebRTC peer connection is established using the acquired ICE server configurations
- Media streaming begins after successful connection
