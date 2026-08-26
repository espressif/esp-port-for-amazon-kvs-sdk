idf_component_register(
    SRCS
        "src/apprtc_signaling.c"
        "src/apprtc_signaling_internal.h"
        "src/apprtc_signaling_interface.c"
        "src/message_queue.c"
        "src/signaling_conversion.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_client
)

# Signaling message sizing (upstream #2356 knob) — sizes public struct members
# (SignalingMessage payload), so the value must match kvs_webrtc's exactly or
# struct layouts silently fork across components. Keep in sync with kvs_webrtc
# and kvs_signaling (canonical value + rationale in kvs_webrtc/CMakeLists.txt).
target_compile_definitions(${COMPONENT_LIB} PUBLIC -DKVS_SIGNALING_MESSAGE_LEN=18432)
