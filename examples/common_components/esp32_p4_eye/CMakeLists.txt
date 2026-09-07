idf_component_register(
    SRCS "esp32_p4_eye.c" "esp32_p4_eye_init.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "priv_include"
    # IDF v6 split the monolithic `driver` component; the headers this BSP uses
    # (i2s_std/i2s_pdm, sdmmc_host, gpio, ledc, spi_master, i2c_master, rtc_io)
    # now live in esp_driver_* components. `driver` stays for v5.x.
    REQUIRES driver spiffs knob
             esp_driver_i2s esp_driver_sdmmc esp_driver_gpio esp_driver_ledc
             esp_driver_spi esp_driver_i2c
    PRIV_REQUIRES fatfs esp_lcd
)

idf_component_set_property(${COMPONENT_NAME} WHOLE_ARCHIVE TRUE)