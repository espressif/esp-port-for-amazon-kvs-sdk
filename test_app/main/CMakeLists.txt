set(srcs
    "test_app_main.c"
    "test_signaling_serializer.c"
    "test_base64.c"
    "test_hex.c"
    "test_crc32.c"
    "test_state_machine.c"
    "test_aws_signer_v4.c"
    "test_pending_ice.c"
    # SigV4 signer compiled standalone: the credential component pulls esp_http_client
    # (no linux-host build), but the signer itself only needs kvs_utils + mbedtls.
    "../../components/credential/src/aws_signer_v4.c"
    # ICE re-trickle buffer compiled standalone: it is dependency-free (stdint
    # only), so the host test links it without the KVS signaling client.
    "../../components/signaling/kvs_signaling/port/kvs_pending_ice.c")

idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS "."
                       PRIV_INCLUDE_DIRS
                           "../../components/credential/src"
                           "../../components/kvs_utils/src"
                           "../../components/signaling/kvs_signaling/port"
                       PRIV_REQUIRES "unity" "kvs_utils" "mbedtls"
                       WHOLE_ARCHIVE)
