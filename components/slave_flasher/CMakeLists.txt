set(_slave_flasher_embed_bins bootloader.bin partition-table.bin app.bin)

set(_slave_flasher_srcs SRCS)
if(CONFIG_SLAVE_FLASHER_ENABLE)
    list(APPEND _slave_flasher_srcs "src/slave_flasher.c" "src/loader_utils.c")
endif()
if(CONFIG_SLAVE_CONSOLE_ENABLE)
    list(APPEND _slave_flasher_srcs "src/slave_console.c")
endif()
# Always registered (the examples depend on it unconditionally); with both off it
# compiles nothing and only exposes the header.

idf_component_register(${_slave_flasher_srcs}
                       INCLUDE_DIRS "include"
                       # IDF v6 split uart/gpio out of `driver`; keep `driver` for v5.x.
                       REQUIRES driver esp_driver_uart esp_driver_gpio mbedtls console )

# Missing or empty images fail at build time, so configure and menuconfig work
# without them. Nothing is embedded or packed until they exist.
set(_missing "")
if(CONFIG_SLAVE_FLASHER_ENABLE)
    idf_build_get_property(_project_dir PROJECT_DIR)
    get_filename_component(_slave_fw_dir "${CONFIG_SLAVE_FLASHER_IMAGE_DIR}" ABSOLUTE BASE_DIR "${_project_dir}")
    foreach(_bin ${_slave_flasher_embed_bins})
        set(_size 0)
        if(EXISTS "${_slave_fw_dir}/${_bin}")
            file(SIZE "${_slave_fw_dir}/${_bin}" _size)
        endif()
        if(_size EQUAL 0)
            list(APPEND _missing "${_bin}")
        endif()
    endforeach()
    if(_missing)
        string(REPLACE ";" ", " _missing_str "${_missing}")
        set(_msg
            "slave_flasher: ${_slave_fw_dir} has no valid ${_missing_str} (missing or empty), so the build fails."
            "Copy the co-processor images there, set CONFIG_SLAVE_FLASHER_IMAGE_DIR, or turn off CONFIG_SLAVE_FLASHER_ENABLE if the co-processor is already programmed.")
        list(JOIN _msg "\n" _warning)
        message(WARNING "${_warning}")
        fail_at_build_time(slave_flasher_images_missing ${_msg})
    endif()
endif()

if(CONFIG_SLAVE_FLASHER_IMAGE_EMBEDDED)
    if(NOT _missing)
        foreach(_bin ${_slave_flasher_embed_bins})
            string(MAKE_C_IDENTIFIER "slave_${_bin}" _sym)
            target_add_binary_data(${COMPONENT_LIB} "${_slave_fw_dir}/${_bin}" BINARY RENAME_TO ${_sym})
        endforeach()
    endif()
elseif(CONFIG_SLAVE_FLASHER_IMAGE_SPIFFS)
    idf_build_get_property(_build_components BUILD_COMPONENTS)
    if(NOT spiffs IN_LIST _build_components)
        message(FATAL_ERROR "slave_flasher: CONFIG_SLAVE_FLASHER_IMAGE_SPIFFS needs the spiffs component in the build.")
    endif()
    idf_component_optional_requires(PUBLIC spiffs)
    if(NOT _missing)
        spiffs_create_partition_image(slave "${_slave_fw_dir}" FLASH_IN_PROJECT)
    endif()
endif()
