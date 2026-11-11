# esp_hosted is unconditional: the only consumer is the signaling_only
# co-processor, which always builds it. Making it conditional on CONFIG_* does
# not work — REQUIRES is evaluated during early expansion, before sdkconfig
# exists, so the condition is always false and the dependency vanishes silently.
idf_component_register(
    SRCS "src/signaling_bridge_adapter.c" "src/bridge_peer_connection.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_common esp_hosted
)
