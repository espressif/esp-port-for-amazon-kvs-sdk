# Base list — compiles on every target; target-specific files/deps appended below.
set(srcs "src/message_utils.c" "src/webrtc_mem_utils.c" "src/esp_work_queue.c" "src/message_queue.c")
set(requires esp_common freertos nvs_flash pthread)

if(${IDF_TARGET} STREQUAL "linux")
    # Linux target: libc-backed flash_wrapper, plus esp_timer_get_time() and the
    # esp_timer timers for IDF < 6.2, where the linux esp_timer is headers-only.
    list(APPEND srcs "src/flash_wrapper_linux.c" "src/esp_timer_linux_compat.c")
    list(APPEND requires esp_timer)
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

