if(CONFIG_CAPTURE_DIRECT_V4L2)
    set(CAPTURE_SRCS "video_capture_direct.c")
    set(CAPTURE_REQUIRES "example_video_common")
else()
    set(CAPTURE_SRCS "video_capture_media_stream.c")
    # example_video_common required to call example_video_init() before media_stream
    # so camera uses same setup as direct path (esp_video_if then reuses /dev/video0)
    set(CAPTURE_REQUIRES "example_video_common")
endif()

idf_component_register(SRCS "simple_video_server_example.c" ${CAPTURE_SRCS}
                       INCLUDE_DIRS "."
                       REQUIRES "esp_http_server"
                                 "esp_timer"
                                 "nvs_flash"
                                 "lwip"
                                 ${CAPTURE_REQUIRES})
