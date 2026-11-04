# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.5)

# Include common KVS SDK path setting
include(${CMAKE_CURRENT_SOURCE_DIR}/../common_components/set_kvs_sdk_path.cmake)

# Only use the streaming part of the KVS WebRTC SDK
option(ENABLE_STREAMING_ONLY "enable only the streaming" ON)
option(ENABLE_DATA_CHANNEL "enable data channel support" ON)
option(ENABLE_SIGNALLING_ONLY "enable only the streaming" OFF)

if(IDF_TARGET STREQUAL "linux")
    # Host build: only main's dependency closure, so IDF's linux target does not
    # pull peripheral esp_driver_* components that self-exclude on linux.
    set(COMPONENTS main)
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

idf_build_set_property(__BUILD_COMPONENT_DEPGRAPH_ENABLED 1)

project(esp_camera)

if(IDF_TARGET STREQUAL "linux" AND CMAKE_C_COMPILER_ID MATCHES "Clang")
    # IDF's linux toolchain config injects a GCC-only warning option that Apple
    # Clang rejects under -Werror (same as linux_test).
    idf_build_set_property(COMPILE_OPTIONS "-Wno-unknown-warning-option" APPEND)
endif()

# esp_websocket_client (1.8.0 and earlier) stores portMAX_DELAY in an `int`
# (`const int lock_timeout = portMAX_DELAY;`). On the Linux target TickType_t is
# unsigned long, so the value overflows int and IDF 6 builds that as an error.
# The stored -1 converts back to ULONG_MAX (= portMAX_DELAY) when passed to the
# semaphore calls, so behaviour is unchanged; relax only this warning, only for
# that component, only on the host, until the upstream fix is released.
if(IDF_TARGET STREQUAL "linux")
    # idf_component_get_property() is fatal for a component that is not in the build
    idf_build_get_property(_build_components BUILD_COMPONENTS)
    if("espressif__esp_websocket_client" IN_LIST _build_components)
        idf_component_get_property(_ws_lib espressif__esp_websocket_client COMPONENT_LIB)
        target_compile_options(${_ws_lib} PRIVATE -Wno-error=overflow)
    endif()
endif()
