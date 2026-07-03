/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include "unity.h"
#include "common_defs.h"
#include "aws_signer_v4.h"

/* generateSignatureDateTime() takes time in 100ns units
 * (HUNDREDS_OF_NANOS_IN_A_SECOND = 1e7) and formats AWS SigV4's
 * "%Y%m%dT%H%M%SZ". 2015-08-30T12:36:00Z (the AWS SigV4 reference
 * example) is epoch 1440938160. */

TEST_CASE("SigV4 datetime: AWS reference timestamp", "[aws_sigv4]")
{
    CHAR out[SIGNATURE_DATE_TIME_STRING_LEN];
    STATUS s = generateSignatureDateTime(1440938160ULL * 10000000ULL, out);
    TEST_ASSERT_EQUAL(STATUS_SUCCESS, s);
    TEST_ASSERT_EQUAL_STRING("20150830T123600Z", out);
}

TEST_CASE("SigV4 datetime: epoch zero", "[aws_sigv4]")
{
    CHAR out[SIGNATURE_DATE_TIME_STRING_LEN];
    STATUS s = generateSignatureDateTime(0, out);
    TEST_ASSERT_EQUAL(STATUS_SUCCESS, s);
    TEST_ASSERT_EQUAL_STRING("19700101T000000Z", out);
}

TEST_CASE("SigV4 datetime: NULL output rejected", "[aws_sigv4]")
{
    STATUS s = generateSignatureDateTime(0, NULL);
    TEST_ASSERT_NOT_EQUAL(STATUS_SUCCESS, s);
}
