# Network Coprocessor Component
# Wraps the slave-side firmware from the esp_hosted submodule
# (esp_hosted/slave/main/*.c) so it can be consumed as an IDF component.
# Active ONLY when this build IS the SDIO/SPI co-processor slave (e.g.
# the network_adapter example on ESP32-C5/C6, or any future slave
# firmware). For host-side consumers (webrtc_classic, signaling_only,
# esp_camera, …) the component registers no sources so the slave-side
# vhci / HCI / wifi-cp code never gets linked into a host image. Toggle
# CONFIG_NETWORK_COPROCESSOR_BUILD_FIRMWARE=y in the slave example's
# sdkconfig.defaults to enable.
set(esp_hosted_path ${CMAKE_CURRENT_SOURCE_DIR}/../esp_hosted)
set(slave_main_path ${esp_hosted_path}/slave/main)
set(common_dir ${esp_hosted_path}/common)

# All slave-side SRCS are collected under the
# CONFIG_NETWORK_COPROCESSOR_BUILD_FIRMWARE gate. REQUIRES /
# INCLUDE_DIRS are always declared at the bottom so IDF's
# early-requires resolution pass sees the same dep graph
# regardless of Kconfig value (an `if NOT CONFIG … return endif`
# at the top would silently capture an empty REQUIRES list during
# that early pass, breaking includes for downstream slave_*.c
# files in the late full-build pass).
set(app_srcs "")
if(CONFIG_NETWORK_COPROCESSOR_BUILD_FIRMWARE)
    list(APPEND app_srcs
        "${slave_main_path}/slave_control.c"
        "${common_dir}/proto/esp_hosted_rpc.pb-c.c"
        # esp_hosted bundles its own protobuf-c runtime under
        # common/protobuf-c/protobuf-c/protobuf-c.c — compile it
        # alongside the .pb-c.c so the slave firmware doesn't depend
        # on IDF's protobuf-c component layout for these headers.
        "${common_dir}/protobuf-c/protobuf-c/protobuf-c.c"
        "${common_dir}/utils/esp_hosted_cli.c"
        "${slave_main_path}/protocomm_pserial.c"
        "${slave_main_path}/esp_hosted_coprocessor.c"
        "${slave_main_path}/stats.c"
        "${slave_main_path}/host_power_save.c"
        "${slave_main_path}/slave_light_sleep.c"
        "${slave_main_path}/nw_split_router.c"
        "${slave_main_path}/slave_transport_gpio_pin_guard.c"
        "${slave_main_path}/slave_gpio_expander.c"
        "${slave_main_path}/slave_ext_coex.c"
        "${slave_main_path}/slave_util.c"
    )

    if(CONFIG_ESP_HOSTED_USE_MEMPOOL)
        list(APPEND app_srcs
            "${common_dir}/mempool/mempool.c"
            "${common_dir}/mempool/mempool_ll.c"
        )
    endif()

    if(CONFIG_SOC_BT_SUPPORTED)
        list(APPEND app_srcs "${slave_main_path}/slave_bt.c")
    endif()

    if(CONFIG_ESP_HOSTED_CP_WIFI)
        list(APPEND app_srcs "${slave_main_path}/slave_wifi_std.c")
    endif()

    if(CONFIG_ESP_WIFI_ENTERPRISE_SUPPORT)
        list(APPEND app_srcs "${slave_main_path}/slave_wifi_enterprise.c")
    endif()

    if(CONFIG_ESP_HOSTED_NETWORK_SPLIT_ENABLED)
        list(APPEND app_srcs "${slave_main_path}/slave_network_split.c")
    endif()
endif()

# Public include directories - expose slave headers to dependent components.
# esp_hosted ships its own copy of protobuf-c under common/protobuf-c/;
# the .pb-c.h files generated for slave RPC include it via
# `#include "protobuf-c/protobuf-c.h"` so the `protobuf-c` subdir must
# be on the include search path of any compiler invocation that
# touches those headers.
set(PUBLIC_INCLUDE_DIRS
    "${slave_main_path}"
    "${common_dir}"
    "${common_dir}/log"
    "${common_dir}/proto"
    "${common_dir}/protobuf-c"
    "${common_dir}/rpc"
    "${common_dir}/transport"
    "${common_dir}/utils"
    "${common_dir}/mempool/include"
)

# Select BT UART code based on IDF Target (per-target selection)
if(CONFIG_IDF_TARGET_ESP32)
    list(APPEND app_srcs "${slave_main_path}/slave_bt_uart_esp32.c")
elseif(CONFIG_IDF_TARGET_ESP32C3 OR CONFIG_IDF_TARGET_ESP32S3)
    list(APPEND app_srcs "${slave_main_path}/slave_bt_uart_esp32c3_s3.c")
else()
    list(APPEND app_srcs "${slave_main_path}/slave_bt_uart_esp32xx.c")
endif()

# Add transport-specific source files (includes SPI Half-duplex)
if(CONFIG_ESP_SDIO_HOST_INTERFACE)
    list(APPEND app_srcs "${slave_main_path}/sdio_slave_api.c")
elseif(CONFIG_ESP_SPI_HOST_INTERFACE)
    list(APPEND app_srcs "${slave_main_path}/spi_slave_api.c")
elseif(CONFIG_ESP_SPI_HD_MODE)
    list(APPEND app_srcs "${slave_main_path}/spi_hd_slave_api.c")
else() # CONFIG_ESP_UART_HOST_INTERFACE
    list(APPEND app_srcs "${slave_main_path}/uart_slave_api.c")
endif()

# Optional features (can be enabled via menuconfig)
if(CONFIG_ESP_HOSTED_ENABLE_PEER_DATA_TRANSFER)
    list(APPEND app_srcs "${slave_main_path}/example_peer_data_transfer.c")
endif()

if(CONFIG_ESP_HOSTED_COPROCESSOR_EXAMPLE_MQTT)
    list(APPEND app_srcs "${slave_main_path}/example_mqtt_client.c")
endif()

if(CONFIG_ESP_HOSTED_COPROCESSOR_EXAMPLE_HTTP_CLIENT)
    list(APPEND app_srcs "${slave_main_path}/example_http_client.c")
endif()

# Private requirements
set(priv_requires
    esp_wifi
    protobuf-c
    esp_driver_gpio
    esp_driver_uart
    esp_driver_sdio
    wpa_supplicant
    app_update
    esp_pm
)

# Public requirements
set(requires
    esp_wifi
    nvs_flash
    protocomm
    esp_webrtc_utils
)

# Register component
idf_component_register(
    SRCS "${app_srcs}"
    INCLUDE_DIRS "${PUBLIC_INCLUDE_DIRS}"
    PRIV_REQUIRES ${priv_requires}
    REQUIRES ${requires}
)

# All target-level modifications below — compile defs / link wraps /
# protocomm internal headers / LWIP hook — only make sense when the
# slave-firmware SRCS are actually being compiled. Skip on host-side
# (headers-only) builds; the linker wrap especially would leave
# `__wrap_esp_wifi_init` undefined when slave_wifi_*.c isn't linked.
if(CONFIG_NETWORK_COPROCESSOR_BUILD_FIRMWARE)

# Set compile definitions
# Note: Version is defined in esp_hosted/slave/main/esp_hosted_coprocessor_fw_ver.h
# Do NOT redefine PROJECT_VERSION_* here to avoid conflicts
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    "-DESP_HOSTED_SLAVE_BUILD=1"
)

# Suppress format warnings from third-party esp_hosted code (e.g. missing args in ESP_LOGW)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error=format -Wno-format)

# Include protocomm private headers
idf_component_get_property(protocomm_dir protocomm COMPONENT_DIR)
target_include_directories(${COMPONENT_LIB} PRIVATE "${protocomm_dir}/src/common")

# LWIP hook configuration for network split mode (aligned with slave/CMakeLists.txt)
if(CONFIG_ESP_HOSTED_NETWORK_SPLIT_ENABLED)
    if(EXISTS "${common_dir}/esp_hosted_lwip_src_port_hook.h")
        idf_component_get_property(lwip lwip COMPONENT_LIB)
        if(TARGET ${lwip})
            # Use generator expressions to only apply to non-INTERFACE targets
            get_target_property(lwip_type ${lwip} TYPE)
            if(NOT lwip_type STREQUAL "INTERFACE_LIBRARY")
                # Some IDF release/v5.4 commits fail to attach hook file for udp.c,
                # so mandate attach for every file in lwip build
                message(STATUS "********** Configuring LWIP for network split port configs **********")
                message(STATUS "********** file: ${common_dir}/esp_hosted_lwip_src_port_hook.h  **********")
                # Properly pass the -include flag as a single option
                target_compile_options(${lwip} PRIVATE -include${common_dir}/esp_hosted_lwip_src_port_hook.h)
            endif()
        endif()
    else()
        message(STATUS "Couldn't find lwip hook file in ${common_dir}/esp_hosted_lwip_src_port_hook.h")
    endif()
endif()

# Add linker options to wrap esp_wifi_init function (required for slave operation)
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_wifi_init")

endif() # CONFIG_NETWORK_COPROCESSOR_BUILD_FIRMWARE

