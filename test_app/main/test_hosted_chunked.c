/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* Pins four rules not visible from hosted_chunked_transport's own source:
 * a chunk holds at most HOSTED_CHUNK_MAX_DATA bytes; the receive callback owns
 * and frees the reassembled buffer; bytes given to process_chunk are borrowed;
 * a truncated or out-of-order chunk drops the message. */

#include <stdlib.h>
#include <string.h>

#include "unity.h"
#include "esp_err.h"
#include "hosted_chunked_transport.h"


#define MAX_FRAMES 16

typedef struct {
    uint8_t *buf;
    size_t   len;
} frame_t;

static frame_t s_wire[MAX_FRAMES];
static int     s_frames;

/* Mirrors esp_hosted: the callee copies what it needs, never takes the buffer. */
esp_err_t bridge_peer_data_send(uint32_t msg_id, const uint8_t *data, size_t len)
{
    (void)msg_id;
    if (s_frames >= MAX_FRAMES) {
        return ESP_FAIL;
    }
    s_wire[s_frames].buf = malloc(len);
    TEST_ASSERT_NOT_NULL(s_wire[s_frames].buf);
    memcpy(s_wire[s_frames].buf, data, len);
    s_wire[s_frames].len = len;
    s_frames++;
    return ESP_OK;
}

esp_err_t bridge_peer_data_register(uint32_t msg_id, void (*cb)(uint32_t, const uint8_t *, size_t, void *), void *ctx)
{
    (void)msg_id; (void)cb; (void)ctx;
    return ESP_OK;
}

static void wire_reset(void)
{
    for (int i = 0; i < s_frames; i++) {
        free(s_wire[i].buf);
    }
    s_frames = 0;
}


static uint8_t *s_got;
static size_t   s_got_len;
static int      s_deliveries;

static void on_msg(const uint8_t *data, size_t len)
{
    free(s_got);
    s_got = (uint8_t *)data;
    s_got_len = len;
    s_deliveries++;
}

static void capture_reset(void)
{
    free(s_got);
    s_got = NULL;
    s_got_len = 0;
    s_deliveries = 0;
}

static uint8_t *pattern(size_t n)
{
    uint8_t *p = malloc(n);
    TEST_ASSERT_NOT_NULL(p);
    for (size_t i = 0; i < n; i++) {
        p[i] = (uint8_t)(i * 31u + (i >> 8));
    }
    return p;
}

/* Each frame comes from a private copy freed straight after, so retaining the
 * caller's bytes is a use-after-free rather than a lucky pass. */
static void replay(uint32_t msg_id)
{
    for (int i = 0; i < s_frames; i++) {
        uint8_t *borrowed = malloc(s_wire[i].len);
        TEST_ASSERT_NOT_NULL(borrowed);
        memcpy(borrowed, s_wire[i].buf, s_wire[i].len);
        hosted_chunked_process_chunk(msg_id, borrowed, s_wire[i].len);
        free(borrowed);
    }
}

static void roundtrip(uint32_t msg_id, hosted_chunk_wire_format_t fmt,
                      size_t size, int expect_frames)
{
    wire_reset();
    capture_reset();
    hosted_chunked_register(msg_id, on_msg);

    uint8_t *src = pattern(size);
    TEST_ASSERT_EQUAL(ESP_OK, hosted_chunked_send(msg_id, src, size, NULL, fmt));
    TEST_ASSERT_EQUAL_INT(expect_frames, s_frames);

    replay(msg_id);

    TEST_ASSERT_EQUAL_INT(1, s_deliveries);
    TEST_ASSERT_EQUAL_UINT(size, s_got_len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(src, s_got, size);

    free(src);
    wire_reset();
    capture_reset();
}


TEST_CASE("chunked: a message below the chunk size crosses in one frame", "[chunked]")
{
    roundtrip(0x1001, HOSTED_CHUNK_WIRE_NEW, 64, 1);
}

TEST_CASE("chunked: an 18432-byte SDP crosses in three frames", "[chunked]")
{
    /* KVS_SIGNALING_MESSAGE_LEN: the largest message the bridge carries. */
    const size_t sdp = 18432;
    const int expect = (int)((sdp + HOSTED_CHUNK_MAX_DATA - 1) / HOSTED_CHUNK_MAX_DATA);
    TEST_ASSERT_EQUAL_INT(3, expect);
    roundtrip(0x1000, HOSTED_CHUNK_WIRE_LEGACY, sdp, expect);
}

TEST_CASE("chunked: both wire formats reassemble the same payload", "[chunked]")
{
    roundtrip(0x1000, HOSTED_CHUNK_WIRE_LEGACY, 20000, 3);
    roundtrip(0x1001, HOSTED_CHUNK_WIRE_NEW,    20000, 3);
}

TEST_CASE("chunked: no chunk exceeds the payload limit", "[chunked]")
{
    wire_reset();
    capture_reset();
    hosted_chunked_register(0x1001, on_msg);

    uint8_t *src = pattern(20000);
    TEST_ASSERT_EQUAL(ESP_OK, hosted_chunked_send(0x1001, src, 20000, NULL,
                                                  HOSTED_CHUNK_WIRE_NEW));
    for (int i = 0; i < s_frames; i++) {
        TEST_ASSERT_LESS_OR_EQUAL_UINT(HOSTED_CHUNK_MAX_DATA + 16, s_wire[i].len);
    }
    free(src);
    wire_reset();
    capture_reset();
}

TEST_CASE("chunked: a dropped middle chunk delivers nothing", "[chunked]")
{
    wire_reset();
    capture_reset();
    hosted_chunked_register(0x1000, on_msg);

    uint8_t *src = pattern(20000);
    TEST_ASSERT_EQUAL(ESP_OK, hosted_chunked_send(0x1000, src, 20000, NULL,
                                                  HOSTED_CHUNK_WIRE_LEGACY));
    TEST_ASSERT_GREATER_THAN_INT(2, s_frames);

    hosted_chunked_process_chunk(0x1000, s_wire[0].buf, s_wire[0].len);
    hosted_chunked_process_chunk(0x1000, s_wire[s_frames - 1].buf, s_wire[s_frames - 1].len);

    /* a short message is worse than no message: the caller would parse garbage */
    TEST_ASSERT_EQUAL_INT(0, s_deliveries);

    free(src);
    wire_reset();
    capture_reset();
}
