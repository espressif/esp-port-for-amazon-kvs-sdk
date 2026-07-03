/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include "unity.h"
#include "common_defs.h"
#include "crc32.h"

TEST_CASE("CRC32 known value for '123456789'", "[crc32]")
{
    /* Standard CRC-32 test vector: "123456789" -> 0xCBF43926 */
    const char *input = "123456789";
    UINT32 crc = COMPUTE_CRC32((PBYTE)input, strlen(input));
    TEST_ASSERT_EQUAL_HEX32(0xCBF43926, crc);
}

TEST_CASE("CRC32 empty input", "[crc32]")
{
    UINT32 crc = COMPUTE_CRC32((PBYTE)"", 0);
    TEST_ASSERT_EQUAL_HEX32(0x00000000, crc);
}

TEST_CASE("CRC32 incremental matches single-pass", "[crc32]")
{
    const char *full_input = "Hello, World!";
    UINT32 full_len = strlen(full_input);

    /* Single-pass CRC */
    UINT32 crc_single = COMPUTE_CRC32((PBYTE)full_input, full_len);

    /* Incremental CRC in two parts */
    UINT32 split = 5; /* "Hello" and ", World!" */
    UINT32 crc_part1 = updateCrc32(0, (PBYTE)full_input, split);
    UINT32 crc_incremental = updateCrc32(crc_part1, (PBYTE)(full_input + split), full_len - split);

    TEST_ASSERT_EQUAL_HEX32(crc_single, crc_incremental);
}

TEST_CASE("CRC32 different inputs produce different results", "[crc32]")
{
    const char *input1 = "abc";
    const char *input2 = "abd";

    UINT32 crc1 = COMPUTE_CRC32((PBYTE)input1, strlen(input1));
    UINT32 crc2 = COMPUTE_CRC32((PBYTE)input2, strlen(input2));

    TEST_ASSERT_NOT_EQUAL(crc1, crc2);
}

TEST_CASE("CRC32 single byte values", "[crc32]")
{
    BYTE byte_a = 'A';
    BYTE byte_b = 'B';

    UINT32 crc_a = COMPUTE_CRC32(&byte_a, 1);
    UINT32 crc_b = COMPUTE_CRC32(&byte_b, 1);

    TEST_ASSERT_NOT_EQUAL(0, crc_a);
    TEST_ASSERT_NOT_EQUAL(0, crc_b);
    TEST_ASSERT_NOT_EQUAL(crc_a, crc_b);
}
