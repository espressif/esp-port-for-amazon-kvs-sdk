set(KVS_WEBRTC_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../amazon-kinesis-video-streams-webrtc-sdk-c")

# Auto-apply platform patches on top of the recorded webrtc-c submodule SHA
# before we list its source files. The script is idempotent — no-op if the
# submodule already has the patches applied. Without this, a fresh clone
# would build cleanly but produce a firmware missing the patch fixes
# (silently bad). Set CONFIG_KVS_AUTO_APPLY_PATCHES=n to opt out (e.g. if
# you maintain the submodule by rebasing onto a different SDK tip).
if(CONFIG_KVS_AUTO_APPLY_PATCHES)
    set(_apply_script "${CMAKE_CURRENT_SOURCE_DIR}/../../patches/apply_patches.sh")
    if(EXISTS "${_apply_script}")
        execute_process(
            COMMAND bash "${_apply_script}"
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/../.."
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

# Define source files
set(kvs_webrtc_srcs
      "${KVS_WEBRTC_ROOT}/src/source/Crypto/Crypto.c"
      "${KVS_WEBRTC_ROOT}/src/source/Crypto/Dtls_mbedtls.c"
      "${KVS_WEBRTC_ROOT}/src/source/Crypto/Dtls.c"
      "${KVS_WEBRTC_ROOT}/src/source/Crypto/IOBuffer.c"
      # TLS implementation will be added conditionally below
      "${KVS_WEBRTC_ROOT}/src/source/Crypto/Tls.c"
      "${KVS_WEBRTC_ROOT}/src/source/Ice/ConnectionListener.c"
      "${KVS_WEBRTC_ROOT}/src/source/Ice/IceAgent.c"
      "${KVS_WEBRTC_ROOT}/src/source/Ice/IceAgentStateMachine.c"
      "${KVS_WEBRTC_ROOT}/src/source/Ice/IceUtils.c"
      "${KVS_WEBRTC_ROOT}/src/source/Ice/NatBehaviorDiscovery.c"
      # Upstream Network.c is replaced by port/Network_esp.c (added below) —
      # ESP-IDF doesn't ship getifaddrs(), so the upstream version cannot link.
      "${KVS_WEBRTC_ROOT}/src/source/Ice/SocketConnection.c"
      "${KVS_WEBRTC_ROOT}/src/source/Ice/TurnConnection.c"
      "${KVS_WEBRTC_ROOT}/src/source/Ice/TurnConnectionStateMachine.c"
      "${KVS_WEBRTC_ROOT}/src/source/Metrics/Metrics.c"
      "${KVS_WEBRTC_ROOT}/src/source/PeerConnection/DataChannel.c"
      "${KVS_WEBRTC_ROOT}/src/source/PeerConnection/JitterBuffer.c"
      "${KVS_WEBRTC_ROOT}/src/source/PeerConnection/jsmn.c"
      "${KVS_WEBRTC_ROOT}/src/source/PeerConnection/PeerConnection.c"
      "${KVS_WEBRTC_ROOT}/src/source/PeerConnection/Retransmitter.c"
      "${KVS_WEBRTC_ROOT}/src/source/PeerConnection/Rtcp.c"
      "${KVS_WEBRTC_ROOT}/src/source/PeerConnection/Rtp.c"
      "${KVS_WEBRTC_ROOT}/src/source/PeerConnection/SessionDescription.c"
      "${KVS_WEBRTC_ROOT}/src/source/Rtcp/RtcpPacket.c"
      "${KVS_WEBRTC_ROOT}/src/source/Rtcp/RollingBuffer.c"
      "${KVS_WEBRTC_ROOT}/src/source/Rtcp/RtpRollingBuffer.c"
      "${KVS_WEBRTC_ROOT}/src/source/Rtp/Codecs/RtpG711Payloader.c"
      "${KVS_WEBRTC_ROOT}/src/source/Rtp/Codecs/RtpH264Payloader.c"
      "${KVS_WEBRTC_ROOT}/src/source/Rtp/Codecs/RtpH265Payloader.c"
      "${KVS_WEBRTC_ROOT}/src/source/Rtp/Codecs/RtpOpusPayloader.c"
      "${KVS_WEBRTC_ROOT}/src/source/Rtp/Codecs/RtpVP8Payloader.c"
      "${KVS_WEBRTC_ROOT}/src/source/Rtp/RtpPacket.c"
      "${KVS_WEBRTC_ROOT}/src/source/Signaling/ChannelInfo.c"
      "${KVS_WEBRTC_ROOT}/src/source/Signaling/FileCache.c"
      "${KVS_WEBRTC_ROOT}/src/source/Signaling/Client.c"
      "${KVS_WEBRTC_ROOT}/src/source/Signaling/StateMachine.c"
      "${KVS_WEBRTC_ROOT}/src/source/Sctp/Sctp.c"
      "${KVS_WEBRTC_ROOT}/src/source/Sdp/Deserialize.c"
      "${KVS_WEBRTC_ROOT}/src/source/Sdp/Serialize.c"
      "${KVS_WEBRTC_ROOT}/src/source/Srtp/SrtpSession.c"
      "${KVS_WEBRTC_ROOT}/src/source/Stun/Stun.c"
      "${KVS_WEBRTC_ROOT}/src/source/Threadpool/ThreadPoolContext.c"
)

set(kvs_webrtc_include_dirs
      "${KVS_WEBRTC_ROOT}/src/include"
      "${KVS_WEBRTC_ROOT}/src/source/Crypto"
      "${KVS_WEBRTC_ROOT}/src/source/Ice"
      "${KVS_WEBRTC_ROOT}/src/source/Metrics"
      "${KVS_WEBRTC_ROOT}/src/source/PeerConnection"
      "${KVS_WEBRTC_ROOT}/src/source/Rtcp"
      "${KVS_WEBRTC_ROOT}/src/source/Rtp"
      "${KVS_WEBRTC_ROOT}/src/source/Sdp"
      "${KVS_WEBRTC_ROOT}/src/source/Signaling"
      "${KVS_WEBRTC_ROOT}/src/source/Srtp"
      "${KVS_WEBRTC_ROOT}/src/source/Sctp"
      "${KVS_WEBRTC_ROOT}/src/source/Stun"
      "${KVS_WEBRTC_ROOT}/src/source/Threadpool"
)

list(APPEND kvs_webrtc_srcs
    "${CMAKE_CURRENT_SOURCE_DIR}/src/kvs_media.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/WebRtcLogging.c"
)

# Conditionally add TLS implementation
if(CONFIG_USE_ESP_TLS_FOR_KVS)
    list(APPEND kvs_webrtc_srcs
        "${CMAKE_CURRENT_SOURCE_DIR}/port/Tls_esp.c"
    )
    message(STATUS "Using ESP-TLS for KVS TLS sessions")
else()
    list(APPEND kvs_webrtc_srcs
        "${KVS_WEBRTC_ROOT}/src/source/Crypto/Tls_mbedtls.c"
    )
    message(STATUS "Using direct mbedTLS for KVS TLS sessions")
endif()

# port/Network_esp.c replaces the upstream Network.c unconditionally. The
# upstream version uses POSIX getifaddrs() / freeifaddrs() for local IP
# enumeration, which ESP-IDF deliberately does not ship — there is no scenario
# where the upstream Network.c links cleanly on an ESP target. The port file
# uses esp_netif_get_handle_from_ifkey() over the standard WIFI_STA / WIFI_AP /
# ETH keys and makes SO_SNDBUF setsockopt failure non-fatal on lwIP. It's a
# port file, not one of the numbered patches.
list(APPEND kvs_webrtc_srcs
    "${CMAKE_CURRENT_SOURCE_DIR}/port/Network_esp.c"
)

set(requires "mbedtls" "lwip" "esp_timer" "nvs_flash" "freertos" "esp_system" "heap" "driver" "spi_flash" "esp_http_client")

# Add ESP-TLS dependency if using ESP-TLS implementation
if(CONFIG_USE_ESP_TLS_FOR_KVS)
    list(APPEND requires esp-tls)
endif()

idf_component_register(
    SRCS "${CMAKE_CURRENT_SOURCE_DIR}/src/kvs_webrtc.c"
         "${kvs_webrtc_srcs}"

    INCLUDE_DIRS "${kvs_webrtc_include_dirs}"
        "${CMAKE_CURRENT_SOURCE_DIR}/include"

    PRIV_INCLUDE_DIRS "${KVS_WEBRTC_ROOT}/src/source"

    REQUIRES "mbedtls"
             "lwip"
             "esp_timer"
             "esp_wifi"
             "nvs_flash"
             "freertos"
             "esp_system"
             "heap"
             "driver"
             "spi_flash"
             "esp_http_client"
)

# option(ENABLE_KVS_THREADPOOL "Enable support for KVS thread pool in signaling" OFF)
set(KVS_PLAT_ESP_FREERTOS ON CACHE BOOL "Build for ESP FreeRTOS")
add_definitions(-DKVS_PLAT_ESP_FREERTOS)

# Signaling message sizing (upstream #2356 knob). 17024 yields
# MAX_SESSION_DESCRIPTION_INIT_SDP_LEN = (17024 - 1024) * 3/4 = 12000 — the
# RAM cap this port used to carry as a patch — and MAX_SIGNALING_MESSAGE_LEN
# = 17024 (down from the 18750 default). These macros size public struct
# members, so every component that includes the SDK headers must see the
# SAME value: keep in sync with kvs_signaling and apprtc_signaling.
add_definitions(-DKVS_SIGNALING_MESSAGE_LEN=17024)
# if (ENABLE_KVS_THREADPOOL)
#   add_definitions(-DENABLE_KVS_THREADPOOL)
# endif()

# Check for ifaddrs.h and poll.h
include(CheckIncludeFiles)
CHECK_INCLUDE_FILES(ifaddrs.h HAVE_IFADDRS_H)
if(HAVE_IFADDRS_H)
  add_definitions(-DHAVE_IFADDRS_H=1)
endif()

CHECK_INCLUDE_FILES(poll.h HAVE_POLL_H)
if(HAVE_POLL_H)
  add_definitions(-DHAVE_POLL_H=1)
endif()

# Check for socketpair availability
include(CheckFunctionExists)
CHECK_FUNCTION_EXISTS(socketpair HAVE_SOCKETPAIR)
if(HAVE_SOCKETPAIR)
  add_definitions(-DHAVE_SOCKETPAIR=1)
endif()

if (CONFIG_ENABLE_DATA_CHANNEL)
    message("#####################  ENABLE_DATA_CHANNEL: ${CONFIG_ENABLE_DATA_CHANNEL}  #####################")
    add_definitions(-DENABLE_DATA_CHANNEL)
endif()

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

# Add definitions for usrsctp
target_compile_definitions(${COMPONENT_LIB} PUBLIC
    -DHAVE_SCONN_LEN=1
    -DSCTP_USE_LWIP=1
    -DSCTP_USE_RTOS=1
    -DSCTP_SIMPLE_ALLOCATOR
    -D__Userspace__
    -DINET=1
    -DSCTP_DEBUG=1
    -DSCTP_USE_IPV6=0
    -DSCTP_USE_MBEDTLS_SHA1=1
)

target_compile_options(${COMPONENT_LIB} PRIVATE
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
    -Wno-error=type-limits
    -Wno-error=array-bounds
    -Wno-error=maybe-uninitialized
    -Wno-maybe-uninitialized
)

# Add definitions for usrsctp
add_definitions(
  "-DHAVE_SCONN_LEN=1"
  "-DSCTP_USE_LWIP=1"
  "-DSCTP_USE_RTOS=1"
  "-DSCTP_SIMPLE_ALLOCATOR"
  "-D__Userspace__"
  "-DINET=1"
  "-DSCTP_DEBUG=1"
  "-DSCTP_USE_IPV6=0"
  "-DSCTP_USE_MBEDTLS_SHA1=1"
)

if (CONFIG_PREFER_DYNAMIC_ALLOCS)
    add_definitions(-DPREFER_DYNAMIC_ALLOCS=1)
endif()
