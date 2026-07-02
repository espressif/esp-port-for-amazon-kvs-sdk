# Imitate esp-video-components simple_video_server but use media_stream for MJPEG.
# Uses example_video_init (like working simple_video_server) before media_stream.
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(simple_video_server)
