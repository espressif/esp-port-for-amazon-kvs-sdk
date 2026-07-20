
# Register as ESP-IDF component
idf_component_register(
    INCLUDE_DIRS
        "libwebsockets/include"
    REQUIRES
        mbedtls
        freertos
        esp_timer
        driver
    )

# Add libwebsockets as a subdirectory with custom options.
# LWS_WITH_CUSTOM_HEADERS is REQUIRED on FreeRTOS — lws disables custom headers
# unless forced, and the signaling path link-fails on lws_hdr_custom_copy without
# it. Do NOT drop this as "redundant" (it is not the default on this build).
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -DLWS_WITH_CUSTOM_HEADERS=1 -DLWS_WITH_CUSTOM_HTTP_HEADERS=1")

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

# Add libwebsockets as a subdirectory
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/libwebsockets
                 ${CMAKE_CURRENT_BINARY_DIR}/libwebsockets)

target_link_libraries(${COMPONENT_LIB} INTERFACE websockets)

# lws is an add_subdirectory subproject, so it doesn't inherit this component's
# REQUIRES include dirs; link idf::mbedtls for the mbedtls headers (incl.
# tf-psa-crypto on mbedTLS 4 / IDF v6).
target_link_libraries(websockets PRIVATE idf::mbedtls)

# add_subdirectory bypasses lws's own mbedtls-version detection; set
# LWS_HAVE_MBEDTLS_V4 so libwebsockets.h uses the v4 include paths (lws sources +
# public consumers of the header).
if(IDF_VERSION_MAJOR GREATER_EQUAL 6)
    target_compile_definitions(websockets PUBLIC LWS_HAVE_MBEDTLS_V4=1)
    target_compile_definitions(${COMPONENT_LIB} INTERFACE LWS_HAVE_MBEDTLS_V4=1)
endif()

# Supply our own dependency-free UTC timegm() (picolibc's symbol isn't linkable here)
# so lws's date.c resolves it under LWS_HAVE_TIMEGM.
target_sources(websockets PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/lws_timegm.c)
