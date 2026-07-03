# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.5)

# Include common KVS SDK path setting
include(${CMAKE_CURRENT_SOURCE_DIR}/../common_components/set_kvs_sdk_path.cmake)

# Only use the streaming part of the KVS WebRTC SDK
option(ENABLE_STREAMING_ONLY "enable only the streaming" ON)
option(ENABLE_DATA_CHANNEL "enable data channel support" ON)
option(ENABLE_SIGNALLING_ONLY "enable only the streaming" OFF)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

idf_build_set_property(__BUILD_COMPONENT_DEPGRAPH_ENABLED 1)

project(esp_camera)

# When SLAVE_FLASHER_ENABLE is on (P4-EYE with on-board C6), the P4 host writes
# the C6 image — read from the `slave` SPIFFS partition — to the co-processor on
# first boot. The C6 binaries are built per-checkout and not committed, so embed
# them only when present. If missing, warn and skip (non-fatal): the C6 may
# already be flashed, and skipping keeps the app buildable (e.g. in CI). Without
# the embed the `slave` partition is empty, so host-side slave-flashing is a
# no-op until the binaries are staged.
if(CONFIG_SLAVE_FLASHER_ENABLE)
    set(_slave_fw_dir "${CMAKE_CURRENT_SOURCE_DIR}/target-firmware")
    set(_missing "")
    foreach(_bin bootloader.bin partition-table.bin app.bin)
        if(NOT EXISTS "${_slave_fw_dir}/${_bin}")
            list(APPEND _missing "${_bin}")
        endif()
    endforeach()
    if(_missing)
        message(WARNING
            "SLAVE_FLASHER_ENABLE is on but these C6 image files are missing from "
            "target-firmware/: ${_missing}. Skipping the C6 image embed — the C6 "
            "may already be flashed. To (re)flash it from the host, build the "
            "network_adapter example for the C6 and copy bootloader.bin, "
            "partition-table.bin, and app.bin into ${_slave_fw_dir}/ "
            "(see target-firmware/README.md).")
    else()
        spiffs_create_partition_image(slave ${_slave_fw_dir} FLASH_IN_PROJECT)
    endif()
endif()
