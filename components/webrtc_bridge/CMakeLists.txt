set(srcs
    "src/hosted_chunked_transport.c"
    "src/webrtc_bridge.c"
    "src/bridge_cmd.c"
    "proto/bridge_cmd.pb-c.c"
)

set(requires mqtt esp_webrtc_utils)

idf_component_register(SRCS ${srcs}
                       REQUIRES ${requires}
                       PRIV_REQUIRES protobuf-c
                       PRIV_INCLUDE_DIRS "proto"
                       INCLUDE_DIRS "include")

# MQTT bridge needs separate publish/subscribe topics per device role. Signalling device
# publishes to streaming topic and subscribes to signalling; streaming device does the opposite.
# Set by project (e.g. signaling_only or streaming_only example) via option().
if (ENABLE_SIGNALLING_ONLY)
    add_definitions(-DENABLE_SIGNALLING_ONLY)
elseif (ENABLE_STREAMING_ONLY)
    add_definitions(-DENABLE_STREAMING_ONLY)
endif ()
