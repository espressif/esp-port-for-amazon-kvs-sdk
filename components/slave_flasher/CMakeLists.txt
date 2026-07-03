if(CONFIG_SLAVE_FLASHER_ENABLE)
    set(_slave_flasher_srcs SRC_DIRS "src")
else()
    # Component is referenced unconditionally as a dependency; when the host-side
    # OTA path for the C6 slave is disabled (e.g. EV Function Board kit where the
    # C6 has its own USB and is flashed independently) we still want the include
    # surface available but no sources compiled. The src files reference
    # CONFIG_SLAVE_* knobs that only exist when SLAVE_FLASHER_ENABLE=y.
    set(_slave_flasher_srcs SRCS)
endif()

idf_component_register(${_slave_flasher_srcs}
                       INCLUDE_DIRS "include"
                       REQUIRES driver spiffs mbedtls console )
