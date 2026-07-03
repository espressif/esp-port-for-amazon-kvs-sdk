/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include "unity.h"
#include "common_defs.h"
#include "hex.h"

TEST_CASE("hex encode/decode roundtrip", "[hex]")
{
    BYTE input[] = {0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF};
    UINT32 input_len = sizeof(input);

    /* Get required output size */
    UINT32 encoded_len = 0;
    STATUS ret = hexEncode(input, input_len, NULL, &encoded_len);
    TEST_ASSERT_EQUAL(STATUS_SUCCESS, ret);

    char encoded[64];
    UINT32 encoded_buf_len = sizeof(encoded);
    ret = hexEncode(input, input_len, encoded, &encoded_buf_len);
    TEST_ASSERT_EQUAL(STATUS_SUCCESS, ret);

    /* Decode back */
    BYTE decoded[16];
    UINT32 decoded_len = sizeof(decoded);
    ret = hexDecode(encoded, encoded_buf_len, decoded, &decoded_len);
    TEST_ASSERT_EQUAL(STATUS_SUCCESS, ret);
    TEST_ASSERT_EQUAL(input_len, decoded_len);
    TEST_ASSERT_EQUAL_MEMORY(input, decoded, input_len);
}

TEST_CASE("hex encode known value: DEADBEEF", "[hex]")
{
    BYTE input[] = {0xDE, 0xAD, 0xBE, 0xEF};
    char encoded[16];
    UINT32 encoded_len = sizeof(encoded);

    /* Default hex encode produces uppercase (hexEncode calls hexEncodeCase with TRUE) */
    STATUS ret = hexEncode(input, sizeof(input), encoded, &encoded_len);
    TEST_ASSERT_EQUAL(STATUS_SUCCESS, ret);
    TEST_ASSERT_EQUAL_STRING("DEADBEEF", encoded);
}

TEST_CASE("hex encode uppercase with hexEncodeCase", "[hex]")
{
    BYTE input[] = {0xDE, 0xAD, 0xBE, 0xEF};
    char encoded[16];
    UINT32 encoded_len = sizeof(encoded);

    STATUS ret = hexEncodeCase(input, sizeof(input), encoded, &encoded_len, TRUE);
    TEST_ASSERT_EQUAL(STATUS_SUCCESS, ret);
    TEST_ASSERT_EQUAL_STRING("DEADBEEF", encoded);
}

TEST_CASE("hex encode lowercase with hexEncodeCase", "[hex]")
{
    BYTE input[] = {0xDE, 0xAD, 0xBE, 0xEF};
    char encoded[16];
    UINT32 encoded_len = sizeof(encoded);

    STATUS ret = hexEncodeCase(input, sizeof(input), encoded, &encoded_len, FALSE);
    TEST_ASSERT_EQUAL(STATUS_SUCCESS, ret);
    TEST_ASSERT_EQUAL_STRING("deadbeef", encoded);
}

TEST_CASE("hex decode known value", "[hex]")
{
    const char *hex_str = "cafebabe";
    BYTE decoded[8];
    UINT32 decoded_len = sizeof(decoded);

    STATUS ret = hexDecode((PCHAR)hex_str, strlen(hex_str), decoded, &decoded_len);
    TEST_ASSERT_EQUAL(STATUS_SUCCESS, ret);
    TEST_ASSERT_EQUAL(4, decoded_len);

    BYTE expected[] = {0xCA, 0xFE, 0xBA, 0xBE};
    TEST_ASSERT_EQUAL_MEMORY(expected, decoded, sizeof(expected));
}

TEST_CASE("hex size calculation with NULL output", "[hex]")
{
    BYTE input[] = {0xAA, 0xBB};
    UINT32 required_len = 0;

    STATUS ret = hexEncode(input, sizeof(input), NULL, &required_len);
    TEST_ASSERT_EQUAL(STATUS_SUCCESS, ret);
    TEST_ASSERT_GREATER_THAN(0, required_len);
}
