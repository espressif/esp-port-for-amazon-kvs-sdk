/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Regression tests for the ICE-candidate re-trickle buffer.
 *
 * Guards the fix where a trickle ICE candidate whose signaling send fails
 * (websocket momentarily down after the SDP answer) must be BUFFERED for
 * re-send on reconnect rather than dropped. If this silently regresses, a
 * viewer on a relay-only path (e.g. cellular) never learns the device's
 * relay candidate and ICE fails with NO_CONNECTED_CANDIDATE_PAIR.
 *
 * The slot array (~12 KB) and capture struct are kept `static` so the tests
 * don't blow the small main-task stack on the esp32s3 target.
 */

#include <string.h>
#include "unity.h"
#include "kvs_pending_ice.h"

static KvsPendingIceCandidate s_slots[KVS_PENDING_ICE_MAX];

TEST_CASE("pending-ice: a failed candidate is buffered with its payload preserved", "[pending_ice]")
{
    uint32_t count = 0;
    const char *cand = "{\"candidate\":\"candidate:1 1 udp 41821439 3.89.244.3 51259 typ relay\",\"sdpMid\":\"0\"}";

    bool ok = kvsPendingIcePush(s_slots, &count, "peer-abc", cand, (uint32_t) strlen(cand), 2);

    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_UINT32(1, count);
    TEST_ASSERT_EQUAL_STRING(cand, s_slots[0].payload);
    TEST_ASSERT_EQUAL_UINT32((uint32_t) strlen(cand), s_slots[0].payload_len);
    TEST_ASSERT_EQUAL_STRING("peer-abc", s_slots[0].peer_client_id);
    TEST_ASSERT_EQUAL_UINT32(2, s_slots[0].version);
}

TEST_CASE("pending-ice: buffer fills to capacity then rejects extras (never overruns)", "[pending_ice]")
{
    uint32_t count = 0;

    for (int i = 0; i < KVS_PENDING_ICE_MAX; i++) {
        TEST_ASSERT_TRUE(kvsPendingIcePush(s_slots, &count, "p", "cand", 4, 0));
    }
    TEST_ASSERT_EQUAL_UINT32(KVS_PENDING_ICE_MAX, count);

    /* One past capacity is rejected and the count does not grow. */
    TEST_ASSERT_FALSE(kvsPendingIcePush(s_slots, &count, "p", "cand", 4, 0));
    TEST_ASSERT_EQUAL_UINT32(KVS_PENDING_ICE_MAX, count);
}

TEST_CASE("pending-ice: oversized / empty / NULL payloads are rejected", "[pending_ice]")
{
    uint32_t count = 0;
    static char big[KVS_PENDING_ICE_PAYLOAD_LEN + 8];
    memset(big, 'x', sizeof(big));

    TEST_ASSERT_FALSE(kvsPendingIcePush(s_slots, &count, "p", big, KVS_PENDING_ICE_PAYLOAD_LEN, 0));
    TEST_ASSERT_FALSE(kvsPendingIcePush(s_slots, &count, "p", "cand", 0, 0));
    TEST_ASSERT_FALSE(kvsPendingIcePush(s_slots, &count, "p", NULL, 4, 0));

    TEST_ASSERT_EQUAL_UINT32(0, count);
}

TEST_CASE("pending-ice: NULL peer id stores an empty peer_client_id", "[pending_ice]")
{
    uint32_t count = 0;

    TEST_ASSERT_TRUE(kvsPendingIcePush(s_slots, &count, NULL, "cand", 4, 0));
    TEST_ASSERT_EQUAL_UINT32(1, count);
    TEST_ASSERT_EQUAL_STRING("", s_slots[0].peer_client_id);
}

/* Capture what the drain re-sends, and let the test choose success/failure so we
 * can exercise the compaction + retry-cap paths. */
typedef struct {
    int      n;
    bool     succeed;   /* what send_fn returns */
    char     peer[KVS_PENDING_ICE_MAX][KVS_PENDING_ICE_PEERID_LEN];
    char     payload[KVS_PENDING_ICE_MAX][KVS_PENDING_ICE_PAYLOAD_LEN];
    uint32_t version[KVS_PENDING_ICE_MAX];
} drain_capture_t;

static drain_capture_t s_cap;

static bool capture_send(void *ctx, const char *peerId, const char *payload,
                         uint32_t payloadLen, uint32_t version)
{
    drain_capture_t *c = (drain_capture_t *) ctx;
    if (c->n < KVS_PENDING_ICE_MAX) {
        strncpy(c->peer[c->n], peerId ? peerId : "", KVS_PENDING_ICE_PEERID_LEN - 1);
        memcpy(c->payload[c->n], payload, payloadLen);
        c->payload[c->n][payloadLen] = '\0';
        c->version[c->n] = version;
    }
    c->n++;
    return c->succeed;
}

TEST_CASE("pending-ice: drain re-sends all buffered candidates in order, then clears", "[pending_ice]")
{
    uint32_t count = 0;
    kvsPendingIcePush(s_slots, &count, "p1", "candA", 5, 1);
    kvsPendingIcePush(s_slots, &count, "p2", "candB", 5, 2);

    memset(&s_cap, 0, sizeof(s_cap));
    s_cap.succeed = true;
    uint32_t remaining = kvsPendingIceDrain(s_slots, &count, capture_send, &s_cap);

    TEST_ASSERT_EQUAL_UINT32(0, remaining);
    TEST_ASSERT_EQUAL_UINT32(0, count);            /* cleared -> a later flush can't double-send */
    TEST_ASSERT_EQUAL_INT(2, s_cap.n);
    TEST_ASSERT_EQUAL_STRING("candA", s_cap.payload[0]);   /* FIFO order preserved */
    TEST_ASSERT_EQUAL_STRING("candB", s_cap.payload[1]);
    TEST_ASSERT_EQUAL_STRING("p1", s_cap.peer[0]);
    TEST_ASSERT_EQUAL_UINT32(2, s_cap.version[1]);
}

TEST_CASE("pending-ice: failed re-sends are kept for retry, then aged out at the cap", "[pending_ice]")
{
    uint32_t count = 0;
    kvsPendingIcePush(s_slots, &count, "p1", "candA", 5, 0);
    kvsPendingIcePush(s_slots, &count, "p2", "candB", 5, 0);

    /* Every send fails: entries must survive (not be cleared) so a later working
     * reconnect can still deliver them — the whole point of the buffer. */
    for (int attempt = 1; attempt < KVS_PENDING_ICE_MAX_ATTEMPTS; attempt++) {
        memset(&s_cap, 0, sizeof(s_cap));
        s_cap.succeed = false;
        uint32_t remaining = kvsPendingIceDrain(s_slots, &count, capture_send, &s_cap);
        TEST_ASSERT_EQUAL_UINT32(2, remaining);    /* still buffered */
        TEST_ASSERT_EQUAL_UINT32(2, count);
        TEST_ASSERT_EQUAL_INT(2, s_cap.n);         /* attempted again */
    }

    /* The attempt that reaches the cap drops them so an unreachable peer can't
     * pin slots forever. */
    memset(&s_cap, 0, sizeof(s_cap));
    s_cap.succeed = false;
    uint32_t remaining = kvsPendingIceDrain(s_slots, &count, capture_send, &s_cap);
    TEST_ASSERT_EQUAL_UINT32(0, remaining);
    TEST_ASSERT_EQUAL_UINT32(0, count);
}

TEST_CASE("pending-ice: purge drops only the departed peer's candidates", "[pending_ice]")
{
    uint32_t count = 0;
    kvsPendingIcePush(s_slots, &count, "p1", "candA", 5, 0);
    kvsPendingIcePush(s_slots, &count, "p2", "candB", 5, 0);
    kvsPendingIcePush(s_slots, &count, "p1", "candC", 5, 0);

    uint32_t removed = kvsPendingIcePurgePeer(s_slots, &count, "p1");

    TEST_ASSERT_EQUAL_UINT32(2, removed);
    TEST_ASSERT_EQUAL_UINT32(1, count);
    TEST_ASSERT_EQUAL_STRING("p2", s_slots[0].peer_client_id);
    TEST_ASSERT_EQUAL_STRING("candB", s_slots[0].payload);
}

TEST_CASE("pending-ice: draining an empty buffer re-sends nothing", "[pending_ice]")
{
    uint32_t count = 0;
    memset(&s_cap, 0, sizeof(s_cap));
    s_cap.succeed = true;

    TEST_ASSERT_EQUAL_UINT32(0, kvsPendingIceDrain(s_slots, &count, capture_send, &s_cap));
    TEST_ASSERT_EQUAL_INT(0, s_cap.n);
}
