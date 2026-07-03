# Set requires based on target
set(COMPONENT_REQUIRES esp_common)

# Add network_coprocessor for targets that support it
if(CONFIG_IDF_TARGET_ESP32C5 OR CONFIG_IDF_TARGET_ESP32C6)
    list(APPEND COMPONENT_REQUIRES network_coprocessor)
endif()

idf_component_register(
    SRCS "src/signaling_bridge_adapter.c" "src/bridge_peer_connection.c"
    INCLUDE_DIRS "include"
    REQUIRES ${COMPONENT_REQUIRES}
)
