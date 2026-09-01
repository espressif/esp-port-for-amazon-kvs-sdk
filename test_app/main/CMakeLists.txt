set(srcs
    "test_app_main.c"
    "test_signaling_serializer.c"
    "test_base64.c"
    "test_hex.c"
    "test_crc32.c"
    "test_state_machine.c"
    "test_aws_signer_v4.c"
    "test_pending_ice.c"
    "test_stale_peer_watchdog.c"
    "test_bridge_ready_query.c"
    # P4 bridge signaling compiled standalone: the test stubs its app_webrtc /
    # webrtc_bridge / ice_bridge_client deps and drives bridge_message_handler()
    # directly to lock in the "answer READY_QUERY only once offers can be consumed"
    # invariant.
    "../../components/signaling/bridge_signaling/src/bridge_signaling.c"
    # SigV4 signer compiled standalone: the credential component pulls esp_http_client
    # (no linux-host build), but the signer itself only needs kvs_utils + mbedtls.
    "../../components/credential/src/aws_signer_v4.c"
    # ICE re-trickle buffer compiled standalone: it is dependency-free (stdint
    # only), so the host test links it without the KVS signaling client.
    "../../components/signaling/kvs_signaling/port/kvs_pending_ice.c"
    # Liveness watchdog: dependency-free (stdint only), linked standalone for host test.
    "../../components/kvs_webrtc/src/kvs_liveness.c")

idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS "."
                       PRIV_INCLUDE_DIRS
                           "../../components/credential/src"
                           "../../components/kvs_utils/src"
                           "../../components/signaling/kvs_signaling/port"
                           "../../components/kvs_webrtc/src"
                           "../../components/signaling/bridge_signaling/include"
                           "../../components/signaling/bridge_signaling/src"
                           "../../components/app_webrtc/include"
                           "../../components/webrtc_bridge/include"
                           "../../components/esp_webrtc_utils/include"
                           # esp_work_queue.h transitively includes esp_event.h; we only
                           # need it to parse (esp_work_queue_add_task is stubbed), so pull
                           # the headers, not the component.
                           "$ENV{IDF_PATH}/components/esp_event/include"
                       PRIV_REQUIRES "unity" "kvs_utils" "mbedtls" "signaling_serializer"
                       WHOLE_ARCHIVE)
