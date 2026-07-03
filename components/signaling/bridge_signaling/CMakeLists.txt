# Bridge Signaling Component (for streaming_only example)
# Provides bridge-based signaling functionality

# Set requires based on target
set(COMPONENT_REQUIRES
    signaling_serializer
    webrtc_bridge
)

idf_component_register(
    SRCS
        "src/bridge_signaling.c"
        "src/ice_bridge_client.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        ${COMPONENT_REQUIRES}
)
