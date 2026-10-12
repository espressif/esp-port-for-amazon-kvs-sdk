# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
#
# File-backed H.264 + Opus capture for host (linux-target) builds: loops the
# sample frames shipped with the upstream KVS C SDK under samples/.
idf_component_register(
    SRCS "file_capture.c"
    INCLUDE_DIRS "."
    REQUIRES media_stream
)
