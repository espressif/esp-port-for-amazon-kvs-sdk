if (ENABLE_SIGNALLING_ONLY)
    set(srcs )
else()
    # Every file is listed unconditionally. Sources that depend on
    # P4/S3-only headers (esp_video / esp_h264 / esp_image_effects)
    # self-gate at the top of their own translation unit, so on
    # unsupported targets they compile to an empty object with no
    # link-visible symbols.
    set(srcs
        "src/esp_video_if.c"
        "src/esp_h264_hw_enc.c"
        "src/app_camera_esp.c"
        "src/esp32p4_frame_grabber.c"
        "src/video_rate_ctrl.c"
        "src/H264FrameGrabber.c"
        "src/MJPEGFrameGrabber.c"
        "src/OpusFrameGrabber.c"
        "src/OpusAudioPlayer.c"
        "src/resampling.c"
        "src/ringbuf.c"
        "src/video_sink.c"
        "src/video_capture_adapter.c"
        "src/audio_capture_adapter.c"
        "src/audio_player_adapter.c"
        "src/video_player_adapter.c"
        "src/h264_nalu_utils.c"
        "src/video_render_display.c"
        "src/media_stream.c"
    )
endif()

set(priv_reqs "")
if(CONFIG_MEDIA_STREAM_ENABLE_VIDEO_PLAYER)
    list(APPEND priv_reqs espressif__esp_h264)
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_jpeg
    PRIV_REQUIRES ${priv_reqs}
    PRIV_INCLUDE_DIRS "src"
)
