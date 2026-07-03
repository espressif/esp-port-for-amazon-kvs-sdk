set(PROTOBUF_C_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../components/esp_hosted/common/protobuf-c")

idf_component_register(
    SRCS "${PROTOBUF_C_DIR}/protobuf-c/protobuf-c.c"
    INCLUDE_DIRS "${PROTOBUF_C_DIR}"
)
