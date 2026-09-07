# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0

idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES app_webrtc kvs_signaling kvs_webrtc esp_event media_stream file_capture
)
