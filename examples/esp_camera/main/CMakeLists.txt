if(IDF_TARGET STREQUAL "linux")
    # Host build: no Wi-Fi or NVS; sample frames stand in for the camera, and the
    # console runs on stdin/stdout
    idf_component_register(
        SRCS "esp_webrtc_camera_main.c" "webrtc_cli.c"
        INCLUDE_DIRS "."
        REQUIRES app_webrtc kvs_webrtc apprtc_signaling media_stream file_capture esp_event
                 console esp_webrtc_utils
    )
else()
    idf_component_register(
        SRCS "esp_webrtc_camera_main.c" "webrtc_cli.c"
        INCLUDE_DIRS "."
        REQUIRES "nvs_flash"
                 "esp_wifi"
                 "esp_timer"
                 "esp_netif"
                 "esp_event"
                 "mbedtls"
                 "lwip"
        )

    target_compile_definitions(${COMPONENT_LIB} PRIVATE
        -DESP_PLATFORM
    )
endif()

if (CONFIG_ENABLE_DATA_CHANNEL)
    add_definitions(-DENABLE_DATA_CHANNEL)
endif()
