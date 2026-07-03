set(KVS_WEBRTC_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../amazon-kinesis-video-streams-webrtc-sdk-c")

# Auto-apply platform patches on top of the recorded webrtc-c submodule SHA
# before we list its source files. Mirrored from kvs_webrtc/CMakeLists.txt
# so that builds which include only kvs_signaling (e.g. signaling_only on
# the C6 co-processor) also get the patches applied. The script is
# idempotent — when both components are built, the second invocation
# is a no-op. CONFIG_KVS_AUTO_APPLY_PATCHES is declared in
# kvs_webrtc/Kconfig.projbuild (visible to all components).
if(CONFIG_KVS_AUTO_APPLY_PATCHES)
    set(_apply_script "${CMAKE_CURRENT_SOURCE_DIR}/../../../patches/apply_patches.sh")
    if(EXISTS "${_apply_script}")
        execute_process(
            COMMAND bash "${_apply_script}"
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/../../.."
            RESULT_VARIABLE _patch_result
            OUTPUT_VARIABLE _patch_output
            ERROR_VARIABLE _patch_error)
        if(NOT _patch_result EQUAL 0)
            message(FATAL_ERROR
                "Failed to apply webrtc-c platform patches.\n"
                "stdout:\n${_patch_output}\n"
                "stderr:\n${_patch_error}\n"
                "Run `patches/apply_patches.sh` manually, or set "
                "CONFIG_KVS_AUTO_APPLY_PATCHES=n if you maintain the submodule "
                "by hand.")
        endif()
    endif()
endif()

set(component_srcs
    "${KVS_WEBRTC_ROOT}/src/source/Signaling/Client.c"
    "${KVS_WEBRTC_ROOT}/src/source/Signaling/StateMachine.c"
    "${KVS_WEBRTC_ROOT}/src/source/Signaling/FileCache.c"
    "${KVS_WEBRTC_ROOT}/src/source/Signaling/ChannelInfo.c"
    "src/kvs_signaling.c"
)

if(CONFIG_USE_ESP_WEBSOCKET_CLIENT)
    list(APPEND component_srcs
        "port/LwsApiCalls_esp.c"
        "port/Signaling_esp.c"
        "port/DataBuffer.c")
else()
    list(APPEND component_srcs
        "${KVS_WEBRTC_ROOT}/src/source/Signaling/Signaling.c"
        "${KVS_WEBRTC_ROOT}/src/source/Signaling/LwsApiCalls.c"
    )
endif()

set(component_add_includedirs
    "include"
    "${KVS_WEBRTC_ROOT}/src/include"
    "${KVS_WEBRTC_ROOT}/src/source"
    "${KVS_WEBRTC_ROOT}/src/source/Signaling"
)

# port/ holds the ESP-IDF replacements for the upstream Signaling.c and
# LwsApiCalls.c. Headers there (Signaling_esp.h, DataBuffer.h) are component-
# internal — kept off the public include set. Gated by the same Kconfig
# that selects the port sources so headers don't shadow upstream when the
# port is disabled.
set(component_priv_includes "")
if(CONFIG_USE_ESP_WEBSOCKET_CLIENT)
    list(APPEND component_priv_includes "port")
endif()

set(component_requires
    "esp_common"
    "esp_timer"
    "esp_http_client"
    "json"
    "libwebsockets"
    "mbedtls"
    "kvs_utils"
    "credential"
    "esp_webrtc_utils"
    "state_machine"
)

set(component_priv_requires
    "esp_netif"
    "esp_wifi"
)

idf_component_register(
    SRCS
        "${component_srcs}"
    INCLUDE_DIRS "${component_add_includedirs}"
    PRIV_INCLUDE_DIRS "${component_priv_includes}"
    REQUIRES "${component_requires}"
    PRIV_REQUIRES "${component_priv_requires}"
)

if(CONFIG_USE_ESP_WEBSOCKET_CLIENT)
    target_compile_definitions(${COMPONENT_LIB} PUBLIC -DUSE_ESP_WEBSOCKET -DESP_PLATFORM)
endif()

# Add the necessary compile definitions for KVS signaling
target_compile_definitions(${COMPONENT_LIB} PUBLIC
    -DKVS_USE_MBEDTLS
    -DKVS_BUILD_WITH_MBEDTLS
    -DKVS_USE_POOL_ALLOCATOR
    -DESP_PLATFORM
    -DAWS_IOT_SDK
    -DKVS_PLAT_ESP
    -DLWS_WITH_CUSTOM_HEADERS
    -DENABLE_STREAMING
    -DKVS_USE_PLATFORM_TYPES
    -DKVS_PLAT_ESP_FREERTOS
)

# Propagate the CONFIG_PREFER_DYNAMIC_ALLOCS Kconfig to the un-prefixed
# PREFER_DYNAMIC_ALLOCS source macro that the submodule's Include.h
# checks (Include.h:1463). kvs_webrtc/CMakeLists.txt sets this directory-
# globally via add_definitions(), but that does NOT cross into sibling
# component builds — kvs_signaling needs its own target-level define so
# LwsApiCallsESP.c uses the dynamic-payload path instead of the fixed
# 2 KB static buffer (fatal on C5/C6 where SDP offers run > 5 KB).
if(CONFIG_PREFER_DYNAMIC_ALLOCS)
    target_compile_definitions(${COMPONENT_LIB} PUBLIC -DPREFER_DYNAMIC_ALLOCS=1)
endif()

# Suppress warnings for KVS code
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-error=format
    -Wno-error=unused-function
    -Wno-error=unused-variable
    -Wno-error=unused-but-set-variable
    -Wno-error=sign-compare
    -Wno-error=cpp
    -Wno-error=nonnull
    -Wno-nonnull
    -Wno-error=undef
    -Wno-error=builtin-macro-redefined
    -Wno-builtin-macro-redefined
    -Wno-error=pointer-sign
    -Wno-error=incompatible-pointer-types
    -Wno-error=pointer-to-int-cast
    -Wno-error=int-to-pointer-cast
    -Wno-pointer-to-int-cast
    -Wno-int-to-pointer-cast
    -Wno-error=format-truncation
    -Wno-error=restrict
    -Wno-error=implicit-fallthrough
    -Wno-error=type-limits
    -Wno-error=array-bounds
    -Wno-error=maybe-uninitialized
    -Wno-maybe-uninitialized
)
