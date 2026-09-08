cmake_minimum_required(VERSION 3.5)

# Include common KVS SDK path setting
include(${CMAKE_CURRENT_SOURCE_DIR}/../common_components/set_kvs_sdk_path.cmake)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(webrtc_person_detect)

# Build the inference library at -O2 while the rest of the project stays at -Os.
#
# esp-dl ships a defconfig with CONFIG_COMPILER_OPTIMIZATION_PERF, but setting that
# globally breaks the build: -O2 makes GCC's maybe-uninitialized analysis fire in
# esp_lvgl_port's knob code, and managed components compile with -Werror=all. Scoping
# it here gets the inference speed-up without changing how anything else is built.
# Guarded because the component only exists on targets where pedestrian_detect resolves.
#
# The second flag is a toolchain gap, not a taste: esp-dl 3.3.x writes its BMP header
# structs as `typedef struct { ... } name;` on a packed anonymous type, and the GCC that
# ships with recent IDF rejects exactly that under -Werror=non-c-typedef-for-linkage,
# which managed components compile with. The component is a registry dependency - editing
# it in place would be undone by the next re-fetch - so the warning is demoted here for
# esp-dl alone, leaving every other component as strict as before.
#
# Probed, not assumed: -Wnon-c-typedef-for-linkage only exists in the newer GCC. IDF 5.5.4
# pins GCC 14.2, which does not have it, and -Wno-error= for an unknown warning is a hard
# error rather than something the compiler ignores:
#     cc1plus: error: '-Wno-error=non-c-typedef-for-linkage':
#              no option '-Wnon-c-typedef-for-linkage'
# so passing it unconditionally makes the whole build fail on that toolchain. Ask the
# compiler instead of guessing from the IDF version - the flag belongs to the toolchain, and
# the two do not move together.
#
# esp-dl also compiles two files at a time. Its 96 C++ files take 200-500 MB each in cc1plus
# at -O2 (dl_module_creator.cpp peaks at 490 MB), about 20% more under IDF 6's GCC 16 than
# IDF 5.5's GCC 14, and Ninja otherwise schedules nproc + 2 of them at once. That burst is
# what got cc1plus OOM-killed on the shared CI runners on IDF 6.x. The pool holds only this
# component back, and costs about a minute: the 96 files total ~130 s of CPU.
if(TARGET idf::espressif__esp-dl)
    include(CheckCXXCompilerFlag)
    check_cxx_compiler_flag("-Wnon-c-typedef-for-linkage" HAVE_W_NON_C_TYPEDEF_FOR_LINKAGE)
    idf_component_get_property(_espdl_lib espressif__esp-dl COMPONENT_LIB)
    target_compile_options(${_espdl_lib} PRIVATE -O2)
    if(HAVE_W_NON_C_TYPEDEF_FOR_LINKAGE)
        target_compile_options(${_espdl_lib} PRIVATE -Wno-error=non-c-typedef-for-linkage)
    endif()
    set_property(GLOBAL APPEND PROPERTY JOB_POOLS espdl_compile=2)
    set_property(TARGET ${_espdl_lib} PROPERTY JOB_POOL_COMPILE espdl_compile)
endif()
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
