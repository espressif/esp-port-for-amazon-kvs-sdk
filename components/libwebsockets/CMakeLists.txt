
# Register as ESP-IDF component. `driver` exists only on real ESP targets.
set(_lws_requires mbedtls freertos esp_timer)
if(NOT IDF_TARGET STREQUAL "linux")
    # lws-freertos.h (a public header) includes driver/gpio.h. IDF v5.x resolved that
    # through the legacy driver component, which publicly re-exported the esp_driver_*
    # components; v6 dropped that, so name esp_driver_gpio directly.
    list(APPEND _lws_requires driver esp_driver_gpio)
endif()
idf_component_register(
    INCLUDE_DIRS
        "libwebsockets/include"
    REQUIRES
        ${_lws_requires}
    )

# IDF's mbedtls port headers use bare `#if CONFIG_IDF_TARGET_ESP32xx`; on the
# Linux target those macros are undefined and lws builds with -Werror,-Wundef.
if(IDF_TARGET STREQUAL "linux")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wno-error=undef -Wno-undef")
endif()

# Add libwebsockets as a subdirectory with custom options.
# LWS_WITH_CUSTOM_HEADERS is REQUIRED on FreeRTOS — lws disables custom headers
# unless forced, and the signaling path link-fails on lws_hdr_custom_copy without
# it. Do NOT drop this as "redundant" (it is not the default on this build).
# On the Linux target lws_config.h defines it (LWS_WITH_NETWORK path), so forcing
# it here too triggers -Werror=macro-redefined — gate the manual flag to ESP.
if(NOT IDF_TARGET STREQUAL "linux")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -DLWS_WITH_CUSTOM_HEADERS=1 -DLWS_WITH_CUSTOM_HTTP_HEADERS=1")
endif()

# Force-include our UTC timegm() prototype so lws's date.c resolves it (see below).
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -include ${CMAKE_CURRENT_SOURCE_DIR}/lws_timegm_shim.h")

# Disable deprecation warnings for esp_spi_flash.h
# Suppress unused variable warnings from upstream libwebsockets code
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wno-error=cpp -Wno-deprecated-declarations -Wno-unused-variable")

# Disable libwebsockets export targets before adding subdirectory
set(LWS_WITH_EXPORT_LWSTARGETS OFF CACHE BOOL "Disable lws export targets" FORCE)

# Force extensions to be enabled before subdirectory is added
set(LWS_WITHOUT_EXTENSIONS OFF CACHE INTERNAL "Enable extensions" FORCE)
set(LWS_WITH_FILE_OPS OFF CACHE INTERNAL "Disable file operations" FORCE)
set(LWS_WITH_THREADPOOL ON CACHE INTERNAL "Enable threadpool" FORCE)

# ESP picolibc's timegm() isn't linkable here and lws's CHECK_FUNCTION_EXISTS(timegm)
# fails, so lws falls back to local-TZ mktime() and mis-parses the server GMT Date
# header -> SigV4 clock skew / AWS 403. Force LWS_HAVE_TIMEGM; we supply timegm() below.
set(LWS_HAVE_TIMEGM 1 CACHE INTERNAL "ESP: use UTC timegm(), avoid mktime local-TZ fallback" FORCE)

# Linux IDF target is POSIX, not FreeRTOS. Force lws off the ESP32/FreeRTOS plat
# (which pulls host-incompatible IDF portmacro/pthread headers) so it uses
# plat/unix, and pin lws's mbedtls to IDF's so find_library doesn't pick up a
# system/Homebrew copy. Also trim testapps/examples/shared/zlib that don't build
# against IDF's static archive set.
if(IDF_TARGET STREQUAL "linux")
    set(LWS_WITH_ESP32 OFF CACHE BOOL "" FORCE)
    set(LWS_PLAT_FREERTOS OFF CACHE BOOL "" FORCE)
    set(LWS_WITH_MBEDTLS ON CACHE BOOL "" FORCE)
    idf_component_get_property(_idf_mbedtls_dir mbedtls COMPONENT_DIR)
    set(LWS_MBEDTLS_INCLUDE_DIRS
        "${_idf_mbedtls_dir}/mbedtls/include"
        "${_idf_mbedtls_dir}/port/include"
        "${_idf_mbedtls_dir}/port/psa_driver/include" CACHE STRING "" FORCE)
    if(IDF_VERSION_MAJOR GREATER_EQUAL 6)
        # mbedTLS 4 splits PSA crypto into tfpsacrypto (mbedcrypto is gone).
        set(LWS_MBEDTLS_LIBRARIES mbedtls mbedx509 tfpsacrypto CACHE STRING "" FORCE)
        list(APPEND LWS_MBEDTLS_INCLUDE_DIRS
            "${_idf_mbedtls_dir}/mbedtls/tf-psa-crypto/include"
            "${_idf_mbedtls_dir}/mbedtls/tf-psa-crypto/drivers/builtin/include")
        set(LWS_MBEDTLS_INCLUDE_DIRS "${LWS_MBEDTLS_INCLUDE_DIRS}" CACHE STRING "" FORCE)
        # lws's v4 probe (gated on LWS_MBEDTLS_INCLUDE_DIRS) then does
        # find_library(TFPSACRYPTO_LIBRARY) which can't resolve IDF's build-tree
        # archive. Pre-set it to IDF's `tfpsacrypto` target so the find is
        # skipped and it links the IDF component.
        set(TFPSACRYPTO_LIBRARY tfpsacrypto CACHE FILEPATH "" FORCE)
    else()
        set(LWS_MBEDTLS_LIBRARIES mbedtls mbedx509 mbedcrypto CACHE STRING "" FORCE)
    endif()
    set(LWS_WITHOUT_TESTAPPS ON CACHE BOOL "" FORCE)
    set(LWS_WITH_MINIMAL_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(LWS_WITH_FILE_OPS ON CACHE INTERNAL "" FORCE)
    set(LWS_WITH_SHARED OFF CACHE BOOL "" FORCE)
    set(LWS_WITH_STATIC ON CACHE BOOL "" FORCE)
    set(LWS_WITHOUT_EXTENSIONS ON CACHE INTERNAL "" FORCE)
    set(LWS_WITH_ZIP_FOPS OFF CACHE BOOL "" FORCE)
    set(LWS_WITH_HTTP_STREAM_COMPRESSION OFF CACHE BOOL "" FORCE)
    set(LWS_WITH_ZLIB OFF CACHE BOOL "" FORCE)
    # QUIC/HTTP3 needs a QUIC-patched mbedtls (upstream mbedtls lacks it); KVS
    # WSS signaling doesn't use it, so turn the role off on linux.
    set(LWS_WITH_HTTP3 OFF CACHE BOOL "" FORCE)
    set(LWS_ROLE_QUIC OFF CACHE BOOL "" FORCE)
endif()

# Add libwebsockets as a subdirectory
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/libwebsockets
                 ${CMAKE_CURRENT_BINARY_DIR}/libwebsockets)

target_link_libraries(${COMPONENT_LIB} INTERFACE websockets)

# GCC 13+ (IDF v5.5.4 toolchain) flags lws's own mbedtls-client.c ALPN code
# (temp_alpn stack buffer aliased into alpn_comma, used after its scope ends) —
# a latent upstream lws issue, not ours to patch. Applied on the websockets
# target so it lands AFTER lws's own -Werror=all (a global CMAKE_C_FLAGS entry
# is overridden by lws re-enabling -Werror). Clang ignores the unknown option.
# ESP-only: the linux/host build doesn't hit this GCC dangling-pointer path.
if(NOT IDF_TARGET STREQUAL "linux")
    target_compile_options(websockets PRIVATE -Wno-error=dangling-pointer)
endif()

# On the Linux IDF target the lwip component is registered (transitively
# required) but provides no headers; its propagated port/include shadows the
# host's <netinet/in.h> with a wrapper forwarding to lwip/inet.h (not on path).
# Filter lwip dirs out of the websockets target so <netinet/in.h> resolves to libc.
if(IDF_TARGET STREQUAL "linux")
    get_target_property(_lws_inc websockets INCLUDE_DIRECTORIES)
    if(_lws_inc)
        list(FILTER _lws_inc EXCLUDE REGEX "components/lwip/")
        set_target_properties(websockets PROPERTIES INCLUDE_DIRECTORIES "${_lws_inc}")
    endif()
    # lws generates lws_config.h in two dirs (build-root + include/), both on the
    # consumer include path and both defining LWS_WITH_CUSTOM_HEADERS with no
    # guard -> harmless redefinition in lws's own sources AND in every
    # libwebsockets.h consumer (kvs_signaling etc.). Suppress -Werror for the
    # redefinition on lws itself (PRIVATE) and its consumers (INTERFACE).
    # -Wmacro-redefined is a Clang spelling (macOS linux-host build); GCC (real
    # Linux linux-host) rejects the option name outright, so gate it to Clang.
    if(CMAKE_C_COMPILER_ID MATCHES "Clang")
        target_compile_options(websockets PRIVATE -Wno-error=macro-redefined)
        target_compile_options(${COMPONENT_LIB} INTERFACE -Wno-error=macro-redefined)
    endif()
endif()

# lws is an add_subdirectory subproject, so it doesn't inherit this component's
# REQUIRES include dirs. On mbedTLS 4 / IDF v6 the mbedtls headers moved to
# tf-psa-crypto/include (e.g. platform_util.h), which the lws sources need.
if(IDF_TARGET STREQUAL "linux")
    # lws links mbedtls itself on linux (LWS_MBEDTLS_LIBRARIES); its sources
    # just need the include dirs. Avoid target_link_libraries here — lws uses
    # the plain signature on linux and mixing keyword/plain is a CMake error.
    idf_component_get_property(_lws_m mbedtls COMPONENT_DIR)
    target_include_directories(websockets PRIVATE
        "${_lws_m}/mbedtls/include" "${_lws_m}/port/include")
    if(EXISTS "${_lws_m}/mbedtls/tf-psa-crypto/include")
        target_include_directories(websockets PRIVATE
            "${_lws_m}/mbedtls/tf-psa-crypto/include"
            "${_lws_m}/mbedtls/tf-psa-crypto/drivers/builtin/include")
    endif()
else()
    target_link_libraries(websockets PRIVATE idf::mbedtls)
endif()

# add_subdirectory bypasses lws's own mbedtls-version detection; set
# LWS_HAVE_MBEDTLS_V4 so libwebsockets.h uses the v4 include paths (lws sources +
# public consumers of the header). On linux we pin LWS_MBEDTLS_INCLUDE_DIRS, so
# lws's own v4 probe runs and sets LWS_HAVE_MBEDTLS_V4 in lws_config.h — setting
# it manually too would redefine it.
if(IDF_VERSION_MAJOR GREATER_EQUAL 6 AND NOT IDF_TARGET STREQUAL "linux")
    target_compile_definitions(websockets PUBLIC LWS_HAVE_MBEDTLS_V4=1)
    target_compile_definitions(${COMPONENT_LIB} INTERFACE LWS_HAVE_MBEDTLS_V4=1)
endif()

# Same reason for spi_flash: lws romfs.c includes esp_flash.h, whose v6 header
# chain (esp_hal_mspi, esp_blockdev) an add_subdirectory subproject cannot see
# through the component REQUIRES.
#
# Not on linux: there is no esp_flash.h there, and lws's own lib/CMakeLists.txt links
# websockets with the plain signature on that target, so a keyword-form call here makes
# CMake reject the mix ("all-keyword or all-plain").
if(NOT IDF_TARGET STREQUAL "linux")
    target_link_libraries(websockets PRIVATE idf::spi_flash)
endif()

# Supply our own dependency-free UTC timegm() (picolibc's symbol isn't linkable here)
# so lws's date.c resolves it under LWS_HAVE_TIMEGM.
target_sources(websockets PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/lws_timegm.c)
