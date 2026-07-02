# signaling_serializer

Serializes and deserializes WebRTC signaling messages (SDP offer/answer, ICE candidates, ICE-server
config) over a protobuf-c wire format. Used by the split-mode bridge and by custom signaling
backends that need a compact, language-neutral message encoding between the C6 and P4.

## Key APIs

- `signaling_serializer_init()` — one-time init.
- `serialize_signaling_message(msg, &outLen)` — encode a `signaling_msg_t` to a buffer.
- `deserialize_signaling_message(data, len, &msg)` — decode back into a `signaling_msg_t`.
- `create_ice_servers_message(rtcConfig, count, &msg)` — build an ICE-servers message from an RTC
  configuration.

Message types are described by `signaling_serializer_type`.

- **Private requires:** `protobuf-c`, `mbedtls`. The protobuf-c sources are generated from the
  `proto/` definitions and compiled in.

## See also

- [webrtc_bridge](../webrtc_bridge/) — the transport that carries these serialized messages in split mode.
- [docs/custom_signaling.md](../../docs/custom_signaling.md) — implementing a custom signaling backend.
