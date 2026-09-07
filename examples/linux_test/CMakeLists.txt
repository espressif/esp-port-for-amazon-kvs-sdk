# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
#
# Linux-target build of the SDK as a KVS WebRTC master, with file-backed media.
# tests/linux_test_e2e/ runs it against an aiortc viewer.

cmake_minimum_required(VERSION 3.16)

# Only build for the Linux IDF target. Other targets fail fast with a clear message.
if(NOT IDF_TARGET STREQUAL "linux")
    message(FATAL_ERROR
        "examples/linux_test only builds for IDF_TARGET=linux. "
        "Run: idf.py --preview set-target linux")
endif()

# Re-use the shared KVS SDK path helper. Sets KVS_SDK_PATH=<repo root> when not in env.
include(${CMAKE_CURRENT_SOURCE_DIR}/../common_components/set_kvs_sdk_path.cmake)

# Build only main's dependency closure — avoids IDF's linux-target pulling
# peripheral esp_driver_* whose esp_hal_* self-excludes linux.
set(COMPONENTS main)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(linux_test)

# IDF's linux toolchain config adds a GCC-only warning option that Apple Clang
# rejects under -Werror.
if(CMAKE_C_COMPILER_ID MATCHES "Clang")
    idf_build_set_property(COMPILE_OPTIONS "-Wno-unknown-warning-option" APPEND)
endif()

# Optional AddressSanitizer + UBSan: `idf.py -DLINUX_TEST_ASAN=ON build`.
option(LINUX_TEST_ASAN "Build linux_test with AddressSanitizer + UBSan" OFF)
if(LINUX_TEST_ASAN)
    set(_asan_flags "-fsanitize=address -fsanitize=undefined")
    idf_build_set_property(COMPILE_OPTIONS "-fsanitize=address;-fsanitize=undefined;-fno-omit-frame-pointer;-fno-sanitize-recover=all" APPEND)
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} ${_asan_flags}")
endif()
