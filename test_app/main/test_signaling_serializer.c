/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include <stdlib.h>
#include "unity.h"
#include "signaling_serializer.h"

/* Reset serializer state before each test group */
static void _init_serializer(void)
{
    signaling_serializer_init();
}

TEST_CASE("serialize/deserialize roundtrip with OFFER message", "[signaling_serializer]")
{
    _init_serializer();

    const char *test_payload = "v=0\r\no=- 12345 2 IN IP4 127.0.0.1\r\ns=-\r\n";

    signaling_msg_t msg_in;
    memset(&msg_in, 0, sizeof(msg_in));
    msg_in.version = 1;
    msg_in.messageType = SIGNALING_MSG_TYPE_OFFER;
    strncpy(msg_in.correlationId, "test-corr-id-123", sizeof(msg_in.correlationId) - 1);
    strncpy(msg_in.peerClientId, "test-peer-client", sizeof(msg_in.peerClientId) - 1);
    msg_in.payload = (char *)test_payload;
    msg_in.payloadLen = strlen(test_payload);

    size_t serialized_len = 0;
    char *serialized = serialize_signaling_message(&msg_in, &serialized_len);
    TEST_ASSERT_NOT_NULL(serialized);
    TEST_ASSERT_GREATER_THAN(0, serialized_len);

    signaling_msg_t msg_out;
    memset(&msg_out, 0, sizeof(msg_out));
    esp_err_t ret = deserialize_signaling_message(serialized, serialized_len, &msg_out);
    TEST_ASSERT_EQUAL(ESP_OK, ret);

    TEST_ASSERT_EQUAL(msg_in.version, msg_out.version);
    TEST_ASSERT_EQUAL(SIGNALING_MSG_TYPE_OFFER, msg_out.messageType);
    TEST_ASSERT_EQUAL_STRING("test-corr-id-123", msg_out.correlationId);
    TEST_ASSERT_EQUAL_STRING("test-peer-client", msg_out.peerClientId);
    TEST_ASSERT_NOT_NULL(msg_out.payload);
    TEST_ASSERT_EQUAL_STRING(test_payload, msg_out.payload);

    free(serialized);
    free(msg_out.payload);
}

TEST_CASE("serialize/deserialize roundtrip with ANSWER message", "[signaling_serializer]")
{
    _init_serializer();

    const char *test_payload = "v=0\r\no=- 67890 2 IN IP4 127.0.0.1\r\ns=answer\r\n";

    signaling_msg_t msg_in;
    memset(&msg_in, 0, sizeof(msg_in));
    msg_in.version = 1;
    msg_in.messageType = SIGNALING_MSG_TYPE_ANSWER;
    strncpy(msg_in.correlationId, "answer-corr-456", sizeof(msg_in.correlationId) - 1);
    strncpy(msg_in.peerClientId, "answerer-client", sizeof(msg_in.peerClientId) - 1);
    msg_in.payload = (char *)test_payload;
    msg_in.payloadLen = strlen(test_payload);

    size_t serialized_len = 0;
    char *serialized = serialize_signaling_message(&msg_in, &serialized_len);
    TEST_ASSERT_NOT_NULL(serialized);

    signaling_msg_t msg_out;
    memset(&msg_out, 0, sizeof(msg_out));
    esp_err_t ret = deserialize_signaling_message(serialized, serialized_len, &msg_out);
    TEST_ASSERT_EQUAL(ESP_OK, ret);

    TEST_ASSERT_EQUAL(SIGNALING_MSG_TYPE_ANSWER, msg_out.messageType);
    TEST_ASSERT_EQUAL_STRING("answer-corr-456", msg_out.correlationId);
    TEST_ASSERT_NOT_NULL(msg_out.payload);
    TEST_ASSERT_EQUAL_STRING(test_payload, msg_out.payload);

    free(serialized);
    free(msg_out.payload);
}

TEST_CASE("serialize/deserialize roundtrip with ICE_CANDIDATE message", "[signaling_serializer]")
{
    _init_serializer();

    const char *test_payload = "candidate:1 1 UDP 2130706431 192.168.1.5 54321 typ host";

    signaling_msg_t msg_in;
    memset(&msg_in, 0, sizeof(msg_in));
    msg_in.version = 1;
    msg_in.messageType = SIGNALING_MSG_TYPE_ICE_CANDIDATE;
    strncpy(msg_in.correlationId, "ice-corr-789", sizeof(msg_in.correlationId) - 1);
    strncpy(msg_in.peerClientId, "ice-peer-client", sizeof(msg_in.peerClientId) - 1);
    msg_in.payload = (char *)test_payload;
    msg_in.payloadLen = strlen(test_payload);

    size_t serialized_len = 0;
    char *serialized = serialize_signaling_message(&msg_in, &serialized_len);
    TEST_ASSERT_NOT_NULL(serialized);

    signaling_msg_t msg_out;
    memset(&msg_out, 0, sizeof(msg_out));
    esp_err_t ret = deserialize_signaling_message(serialized, serialized_len, &msg_out);
    TEST_ASSERT_EQUAL(ESP_OK, ret);

    TEST_ASSERT_EQUAL(SIGNALING_MSG_TYPE_ICE_CANDIDATE, msg_out.messageType);
    TEST_ASSERT_EQUAL_STRING("ice-corr-789", msg_out.correlationId);
    TEST_ASSERT_EQUAL_STRING("ice-peer-client", msg_out.peerClientId);
    TEST_ASSERT_NOT_NULL(msg_out.payload);
    TEST_ASSERT_EQUAL_STRING(test_payload, msg_out.payload);

    free(serialized);
    free(msg_out.payload);
}

TEST_CASE("ICE request roundtrip", "[signaling_serializer]")
{
    _init_serializer();

    signaling_msg_t msg;
    esp_err_t ret = create_ice_request_message(2, true, &msg);
    TEST_ASSERT_EQUAL(ESP_OK, ret);
    TEST_ASSERT_EQUAL(SIGNALING_MSG_TYPE_ICE_REQUEST, msg.messageType);
    TEST_ASSERT_NOT_NULL(msg.payload);

    /* Serialize and deserialize the message */
    size_t serialized_len = 0;
    char *serialized = serialize_signaling_message(&msg, &serialized_len);
    TEST_ASSERT_NOT_NULL(serialized);

    signaling_msg_t msg_out;
    memset(&msg_out, 0, sizeof(msg_out));
    ret = deserialize_signaling_message(serialized, serialized_len, &msg_out);
    TEST_ASSERT_EQUAL(ESP_OK, ret);

    /* Extract ICE request from deserialized message */
    ss_ice_request_payload_t ice_req;
    ret = extract_ice_request_from_message(&msg_out, &ice_req);
    TEST_ASSERT_EQUAL(ESP_OK, ret);
    TEST_ASSERT_EQUAL(2, ice_req.index);
    TEST_ASSERT_TRUE(ice_req.use_turn);

    free(msg.payload);
    free(serialized);
    free(msg_out.payload);
}

TEST_CASE("ICE server response roundtrip", "[signaling_serializer]")
{
    _init_serializer();

    /* Create a fake RtcIceServer-like structure: urls[128] + username[257] + credential[257] */
    char ice_server_data[SS_MAX_ICE_CONFIG_URI_LEN + 1 + SS_MAX_ICE_CONFIG_USER_NAME_LEN + 1 + SS_MAX_ICE_CONFIG_CREDENTIAL_LEN + 1];
    memset(ice_server_data, 0, sizeof(ice_server_data));

    strncpy(ice_server_data, "stun:stun.example.com:3478", SS_MAX_ICE_CONFIG_URI_LEN);
    strncpy(ice_server_data + SS_MAX_ICE_CONFIG_URI_LEN + 1, "testuser", SS_MAX_ICE_CONFIG_USER_NAME_LEN);
    strncpy(ice_server_data + SS_MAX_ICE_CONFIG_URI_LEN + 1 + SS_MAX_ICE_CONFIG_USER_NAME_LEN + 1,
            "testcredential", SS_MAX_ICE_CONFIG_CREDENTIAL_LEN);

    signaling_msg_t msg;
    esp_err_t ret = create_ice_server_response_message(ice_server_data, true, &msg);
    TEST_ASSERT_EQUAL(ESP_OK, ret);
    TEST_ASSERT_EQUAL(SIGNALING_MSG_TYPE_ICE_SERVER_RESPONSE, msg.messageType);

    /* Serialize and deserialize the message */
    size_t serialized_len = 0;
    char *serialized = serialize_signaling_message(&msg, &serialized_len);
    TEST_ASSERT_NOT_NULL(serialized);

    signaling_msg_t msg_out;
    memset(&msg_out, 0, sizeof(msg_out));
    ret = deserialize_signaling_message(serialized, serialized_len, &msg_out);
    TEST_ASSERT_EQUAL(ESP_OK, ret);

    /* Extract ICE server response */
    ss_ice_server_response_t ice_resp;
    ret = extract_ice_server_response_from_message(&msg_out, &ice_resp);
    TEST_ASSERT_EQUAL(ESP_OK, ret);
    TEST_ASSERT_EQUAL_STRING("stun:stun.example.com:3478", ice_resp.urls);
    TEST_ASSERT_EQUAL_STRING("testuser", ice_resp.username);
    TEST_ASSERT_EQUAL_STRING("testcredential", ice_resp.credential);
    TEST_ASSERT_TRUE(ice_resp.have_more);

    free(msg.payload);
    free(serialized);
    free(msg_out.payload);
}

TEST_CASE("create_ice_request_message returns error for NULL output", "[signaling_serializer]")
{
    esp_err_t ret = create_ice_request_message(0, false, NULL);
    TEST_ASSERT_NOT_EQUAL(ESP_OK, ret);
}

TEST_CASE("extract_ice_request rejects wrong message type", "[signaling_serializer]")
{
    signaling_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.messageType = SIGNALING_MSG_TYPE_OFFER;

    ss_ice_request_payload_t ice_req;
    esp_err_t ret = extract_ice_request_from_message(&msg, &ice_req);
    TEST_ASSERT_NOT_EQUAL(ESP_OK, ret);
}

TEST_CASE("extract_ice_server_response rejects wrong message type", "[signaling_serializer]")
{
    signaling_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.messageType = SIGNALING_MSG_TYPE_OFFER;

    ss_ice_server_response_t ice_resp;
    esp_err_t ret = extract_ice_server_response_from_message(&msg, &ice_resp);
    TEST_ASSERT_NOT_EQUAL(ESP_OK, ret);
}
