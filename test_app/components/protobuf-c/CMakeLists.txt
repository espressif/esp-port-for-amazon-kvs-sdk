# Re-register IDF's bundled protobuf-c so the linux host-test build gets it too.
set(PROTOBUF_C_DIR "$ENV{IDF_PATH}/components/protobuf-c/protobuf-c")

idf_component_register(
    SRCS "${PROTOBUF_C_DIR}/protobuf-c/protobuf-c.c"
    INCLUDE_DIRS "${PROTOBUF_C_DIR}"
)
