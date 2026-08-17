# Resolve KVS_SDK_PATH — the tree all kvs components are consumed from.
# REALPATH (not ABSOLUTE): the comparison below must not be fooled by symlinked
# paths such as macOS /tmp -> /private/tmp.
get_filename_component(_kvs_sdk_this_repo "${CMAKE_CURRENT_SOURCE_DIR}/../.." REALPATH)

if(NOT DEFINED ENV{KVS_SDK_PATH})
    message(WARNING "KVS_SDK_PATH not set in environment. Setting to ${_kvs_sdk_this_repo}")
    set(ENV{KVS_SDK_PATH} "${_kvs_sdk_this_repo}")
else()
    # An exported KVS_SDK_PATH silently redirects every kvs component to another
    # checkout — the build works while you edit sources it never compiles, so
    # warn on the mismatch. Skipped when the example is vendored into another
    # project (rainmaker etc.), where pointing elsewhere is by design.
    get_filename_component(_kvs_sdk_env "$ENV{KVS_SDK_PATH}" REALPATH)
    if(EXISTS "${_kvs_sdk_this_repo}/components/kvs_webrtc"
       AND NOT _kvs_sdk_env STREQUAL _kvs_sdk_this_repo)
        message(WARNING
            "KVS_SDK_PATH is set to '${_kvs_sdk_env}', which is NOT the repo "
            "containing this example ('${_kvs_sdk_this_repo}'). All kvs "
            "components will be consumed from that other tree. Unset "
            "KVS_SDK_PATH if this is not intentional.")
    endif()
endif()

message(STATUS "KVS_SDK_PATH: $ENV{KVS_SDK_PATH}")
