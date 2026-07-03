/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include "unity.h"
#include "common_defs.h"
#include "base64.h"

TEST_CASE("base64 encode/decode roundtrip", "[base64]")
{
    const char *input = "Hello, World!";
    UINT32 input_len = strlen(input);

    /* Get required output size */
    UINT32 encoded_len = 0;
    STATUS ret = base64Encode((PVOID)input, input_len, NULL, &encoded_len);
    TEST_ASSERT_EQUAL(STATUS_SUCCESS, ret);
    TEST_ASSERT_GREATER_THAN(0, encoded_len);

    char encoded[128];
    UINT32 encoded_buf_len = sizeof(encoded);
    ret = base64Encode((PVOID)input, input_len, encoded, &encoded_buf_len);
    TEST_ASSERT_EQUAL(STATUS_SUCCESS, ret);

    /* Decode back - use strlen(encoded) because encoded_buf_len includes null terminator */
    UINT32 decode_input_len = strlen(encoded);
    UINT32 decoded_len = 0;
    ret = base64Decode(encoded, decode_input_len, NULL, &decoded_len);
    TEST_ASSERT_EQUAL(STATUS_SUCCESS, ret);

    BYTE decoded[128];
    UINT32 decoded_buf_len = sizeof(decoded);
    ret = base64Decode(encoded, decode_input_len, decoded, &decoded_buf_len);
    TEST_ASSERT_EQUAL(STATUS_SUCCESS, ret);
    TEST_ASSERT_EQUAL(input_len, decoded_buf_len);
    TEST_ASSERT_EQUAL_MEMORY(input, decoded, input_len);
}

TEST_CASE("base64 known vector: A -> QQ==", "[base64]")
{
    const char *input = "A";
    char encoded[16];
    UINT32 encoded_len = sizeof(encoded);

    STATUS ret = base64Encode((PVOID)input, 1, encoded, &encoded_len);
    TEST_ASSERT_EQUAL(STATUS_SUCCESS, ret);
    TEST_ASSERT_EQUAL_STRING("QQ==", encoded);
}

TEST_CASE("base64 known vector: AB -> QUI=", "[base64]")
{
    const char *input = "AB";
    char encoded[16];
    UINT32 encoded_len = sizeof(encoded);

    STATUS ret = base64Encode((PVOID)input, 2, encoded, &encoded_len);
    TEST_ASSERT_EQUAL(STATUS_SUCCESS, ret);
    TEST_ASSERT_EQUAL_STRING("QUI=", encoded);
}

TEST_CASE("base64 known vector: ABC -> QUJD", "[base64]")
{
    const char *input = "ABC";
    char encoded[16];
    UINT32 encoded_len = sizeof(encoded);

    STATUS ret = base64Encode((PVOID)input, 3, encoded, &encoded_len);
    TEST_ASSERT_EQUAL(STATUS_SUCCESS, ret);
    TEST_ASSERT_EQUAL_STRING("QUJD", encoded);
}

TEST_CASE("base64 size calculation with NULL output", "[base64]")
{
    const char *input = "Hello";
    UINT32 required_len = 0;

    STATUS ret = base64Encode((PVOID)input, strlen(input), NULL, &required_len);
    TEST_ASSERT_EQUAL(STATUS_SUCCESS, ret);
    TEST_ASSERT_GREATER_THAN(0, required_len);

    /* Encode to verify the size was correct */
    char encoded[64];
    UINT32 encoded_len = sizeof(encoded);
    ret = base64Encode((PVOID)input, strlen(input), encoded, &encoded_len);
    TEST_ASSERT_EQUAL(STATUS_SUCCESS, ret);
    TEST_ASSERT_EQUAL(required_len, encoded_len);
}

TEST_CASE("base64 decode known vector: QUJD -> ABC", "[base64]")
{
    const char *encoded = "QUJD";
    BYTE decoded[16];
    UINT32 decoded_len = sizeof(decoded);

    STATUS ret = base64Decode((PCHAR)encoded, strlen(encoded), decoded, &decoded_len);
    TEST_ASSERT_EQUAL(STATUS_SUCCESS, ret);
    TEST_ASSERT_EQUAL(3, decoded_len);
    TEST_ASSERT_EQUAL_MEMORY("ABC", decoded, 3);
}
