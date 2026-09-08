# person_detect.cpp is C++ (esp-dl is a C++20 library). Everything else stays C;
# having any .cpp in the build is enough for IDF to link libstdc++.
idf_component_register(
    SRCS "main.c"
         "webrtc_side.c"
         "detect_sink.c"
         "preview_display.c"
         "person_detect.cpp"
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

if (CONFIG_ENABLE_DATA_CHANNEL)
    add_definitions(-DENABLE_DATA_CHANNEL)
endif()
