cmake_minimum_required(VERSION 3.5)

# Include common KVS SDK path setting
include(${CMAKE_CURRENT_SOURCE_DIR}/../common_components/set_kvs_sdk_path.cmake)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(webrtc_classic)
option(ENABLE_DATA_CHANNEL "Enable data_channel support" ON)

# On the P4 dual-chip board overlays that enable it, the P4 host flashes the
# attached C6/C5 co-processor in-system: the co-processor image is packed into
# a 'slave' SPIFFS partition and written by slave_flasher's flash_slave().
# Only the P4 slave-enabled overlays declare a 'slave' partition (16 MB table),
# so gate this on CONFIG_SLAVE_FLASHER_ENABLE — single-chip ESP32/S3 builds have
# neither the partition nor the images. Fail fast if the images are missing,
# otherwise the build produces an empty SPIFFS and slave_flasher fails at runtime.
if(CONFIG_SLAVE_FLASHER_ENABLE)
    set(_slave_fw_dir "${CMAKE_CURRENT_SOURCE_DIR}/target-firmware")
    set(_missing "")
    foreach(_bin bootloader.bin partition-table.bin app.bin)
        if(NOT EXISTS "${_slave_fw_dir}/${_bin}")
            list(APPEND _missing "${_bin}")
        endif()
    endforeach()
    if(_missing)
        message(FATAL_ERROR
            "SLAVE_FLASHER_ENABLE is on but these C6/C5 image files are missing "
            "from target-firmware/: ${_missing}.\n"
            "Build the network_adapter example for the co-processor target, then "
            "copy bootloader.bin, partition-table.bin and app.bin into "
            "${_slave_fw_dir}/ before re-running this build. "
            "See examples/webrtc_classic/target-firmware/README.md.")
    endif()
    spiffs_create_partition_image(slave ${_slave_fw_dir} FLASH_IN_PROJECT)
endif()
