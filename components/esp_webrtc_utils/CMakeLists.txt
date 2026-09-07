# Base list — compiles on every target; target-specific files/deps appended below.
set(srcs "src/message_utils.c" "src/webrtc_mem_utils.c" "src/esp_work_queue.c" "src/message_queue.c")
set(requires esp_common freertos nvs_flash pthread)

if(${IDF_TARGET} STREQUAL "linux")
    # Linux target: libc-backed flash_wrapper (also provides a host
    # esp_timer_get_time(), since there's no esp_timer here).
    list(APPEND srcs "src/flash_wrapper_linux.c")
else()
    list(APPEND srcs "src/esp_cli.c" "src/wifi_cli.c" "src/esp_webrtc_time.c" "src/flash_wrapper.c")
    list(APPEND requires esp_wifi console mqtt esp_timer)
    if(ENABLE_STREAMING_ONLY)
        list(APPEND requires webrtc_bridge)
    endif()
endif()

idf_component_register(SRCS "${srcs}"
                       REQUIRES ${requires}
                       INCLUDE_DIRS "include")
if (ENABLE_SIGNALLING_ONLY)
    add_definitions(-DENABLE_SIGNALLING_ONLY)
elseif (ENABLE_STREAMING_ONLY)
    add_definitions(-DENABLE_STREAMING_ONLY)
endif ()

