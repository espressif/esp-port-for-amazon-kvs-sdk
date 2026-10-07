set(srcs
    "src/signaling_serializer.c"
    "src/json_serializer.c"
    "src/protobuf_serializer.c"
    "src/raw_serializer.c"
    "proto/signaling.pb-c.c"
)

if (LINUX_BUILD)
    # The linux build has no IDF component graph, so compile protobuf-c straight
    # from IDF's bundled copy rather than depending on the component.
    set(PROTOBUF_C_SRCS
        "$ENV{IDF_PATH}/components/protobuf-c/protobuf-c/protobuf-c/protobuf-c.c"
    )

    set(PROTOBUF_C_INCLUDE_DIRS
        "$ENV{IDF_PATH}/components/protobuf-c/protobuf-c"
    )

    add_library(signaling_serializer STATIC ${srcs} ${PROTOBUF_C_SRCS})

    target_include_directories(signaling_serializer
        PUBLIC
        "include"
        PRIVATE
        "proto"
        # Include protobuf-c headers
        ${PROTOBUF_C_INCLUDE_DIRS}
    )

    # Add a definition for Linux builds
    target_compile_definitions(signaling_serializer PRIVATE KVS_PLAT_LINUX_UNIX)
else()
    # ESP-IDF specific build
    idf_component_register(
        SRCS ${srcs}
        PRIV_INCLUDE_DIRS "proto"
        INCLUDE_DIRS "include"
        PRIV_REQUIRES protobuf-c mbedtls
    )
endif()
