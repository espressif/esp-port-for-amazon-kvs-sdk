/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Regression tests for the signaling re-send buffer (ICE candidates, and
 * SDP answers further down).
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

#include <stdio.h>
#include <stdlib.h>
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

/* ---- SDP answers buffered across a signaling reconnect ------------------
 *
 * Guards the fix where an answer whose send fails while the signaling websocket
 * reconnects (a viewer offer woke a parked host, then the link dropped) must be
 * buffered and re-sent once the client reconnects, ahead of that peer's ICE
 * candidates. Without it the viewer never gets an answer.
 *
 * A counting allocator checks the buffers are allocated only while something
 * waits and that every answer payload is freed.
 */

static int s_live_allocs;
static int s_alloc_budget = -1;   /* allocations left before calloc fails; -1: unlimited */

static void *counting_calloc(size_t num, size_t size)
{
    if (s_alloc_budget == 0) {
        return NULL;
    }
    if (s_alloc_budget > 0) {
        s_alloc_budget--;
    }
    void *p = calloc(num, size);
    if (p != NULL) {
        s_live_allocs++;
    }
    return p;
}

static void counting_free(void *ptr)
{
    if (ptr != NULL) {
        s_live_allocs--;
    }
    free(ptr);
}

#define SIG_CAP_MAX 24

typedef struct {
    int                    n;
    bool                   answer_ok;   /* what send_fn returns for answers */
    bool                   ice_ok;      /* ... and for candidates */
    kvs_pending_msg_type_t type[SIG_CAP_MAX];
    char                   peer[SIG_CAP_MAX][32];
    char                   corr[SIG_CAP_MAX][32];
    char                   payload[SIG_CAP_MAX][32];
} sig_capture_t;

static sig_capture_t s_sig;

static bool sig_capture_send(void *ctx, kvs_pending_msg_type_t type, const char *peerId,
                             const char *correlationId, const char *payload,
                             uint32_t payloadLen, uint32_t version)
{
    sig_capture_t *c = (sig_capture_t *) ctx;
    (void) version;
    if (c->n < SIG_CAP_MAX) {
        c->type[c->n] = type;
        snprintf(c->peer[c->n], sizeof(c->peer[0]), "%s", peerId);
        snprintf(c->corr[c->n], sizeof(c->corr[0]), "%s", correlationId);
        uint32_t len = payloadLen < sizeof(c->payload[0]) - 1 ? payloadLen : (uint32_t) sizeof(c->payload[0]) - 1;
        memcpy(c->payload[c->n], payload, len);
        c->payload[c->n][len] = '\0';
    }
    c->n++;
    return (type == KVS_PENDING_MSG_ANSWER) ? c->answer_ok : c->ice_ok;
}

static void sig_capture_reset(bool answer_ok, bool ice_ok)
{
    memset(&s_sig, 0, sizeof(s_sig));
    s_sig.answer_ok = answer_ok;
    s_sig.ice_ok = ice_ok;
}

static const char *answer_payload_for(const KvsPendingSignaling *p, const char *peer)
{
    for (uint32_t i = 0; i < p->answer_count; i++) {
        if (strcmp(p->answers[i].peer_client_id, peer) == 0) {
            return p->answers[i].payload;
        }
    }
    return NULL;
}

static void put_answer(KvsPendingSignaling *p, const char *peer, const char *sdp, uint64_t now_ms)
{
    TEST_ASSERT_TRUE(kvsPendingSignalingPutAnswer(p, peer, "corr", sdp, (uint32_t) strlen(sdp), 2, now_ms));
}

TEST_CASE("pending-answer: a newer answer for the same peer replaces the older one", "[pending_ice]")
{
    KvsPendingSignaling p;
    s_live_allocs = 0;
    kvsPendingSignalingInit(&p, counting_calloc, counting_free);

    put_answer(&p, "peer-a", "sdp-old", 1000);
    TEST_ASSERT_TRUE(kvsPendingSignalingPutAnswer(&p, "peer-a", "corr-new", "sdp-new", 7, 3, 2000));

    TEST_ASSERT_EQUAL_UINT32(1, p.answer_count);   /* one slot per peer */
    TEST_ASSERT_EQUAL_STRING("sdp-new", p.answers[0].payload);
    TEST_ASSERT_EQUAL_STRING("corr-new", p.answers[0].correlation_id);
    TEST_ASSERT_EQUAL_UINT32(3, p.answers[0].version);
    TEST_ASSERT_EQUAL_UINT64(2000, p.answers[0].queued_at_ms);
    TEST_ASSERT_EQUAL_INT(2, s_live_allocs);       /* slot array + one payload: the old one was freed */

    kvsPendingSignalingFree(&p);
    TEST_ASSERT_EQUAL_INT(0, s_live_allocs);
}

TEST_CASE("pending-answer: with every slot taken the oldest answer is evicted", "[pending_ice]")
{
    KvsPendingSignaling p;
    s_live_allocs = 0;
    kvsPendingSignalingInit(&p, counting_calloc, counting_free);
    char peer[16];

    for (int i = 0; i < KVS_PENDING_ANSWER_MAX; i++) {
        snprintf(peer, sizeof(peer), "peer-%d", i);
        put_answer(&p, peer, "sdp", 1000 + (uint64_t) i);
    }
    put_answer(&p, "peer-late", "sdp", 5000);

    TEST_ASSERT_EQUAL_UINT32(KVS_PENDING_ANSWER_MAX, p.answer_count);
    TEST_ASSERT_FALSE(kvsPendingSignalingHasAnswer(&p, "peer-0"));   /* the oldest */
    TEST_ASSERT_TRUE(kvsPendingSignalingHasAnswer(&p, "peer-1"));
    TEST_ASSERT_TRUE(kvsPendingSignalingHasAnswer(&p, "peer-late"));
    TEST_ASSERT_EQUAL_INT(1 + KVS_PENDING_ANSWER_MAX, s_live_allocs);

    kvsPendingSignalingFree(&p);
    TEST_ASSERT_EQUAL_INT(0, s_live_allocs);
}

TEST_CASE("pending-answer: empty / NULL answers are rejected without keeping memory", "[pending_ice]")
{
    KvsPendingSignaling p;
    s_live_allocs = 0;
    kvsPendingSignalingInit(&p, counting_calloc, counting_free);

    TEST_ASSERT_FALSE(kvsPendingSignalingPutAnswer(&p, "peer-a", "corr", "sdp", 0, 2, 0));
    TEST_ASSERT_FALSE(kvsPendingSignalingPutAnswer(&p, "peer-a", "corr", NULL, 3, 2, 0));
    TEST_ASSERT_EQUAL_UINT32(0, kvsPendingSignalingCount(&p));
    TEST_ASSERT_NULL(p.answers);
    TEST_ASSERT_EQUAL_INT(0, s_live_allocs);
}

TEST_CASE("pending-signaling: drain sends each answer before any candidate", "[pending_ice]")
{
    KvsPendingSignaling p;
    s_live_allocs = 0;
    kvsPendingSignalingInit(&p, counting_calloc, counting_free);

    /* Candidates were buffered first (trickled after the failed answer, or before
     * it in the queue): the drain must still put the answer out first. */
    TEST_ASSERT_TRUE(kvsPendingSignalingPushIce(&p, "peer-a", "candA1", 6, 2));
    TEST_ASSERT_TRUE(kvsPendingSignalingPushIce(&p, "peer-b", "candB1", 6, 2));
    put_answer(&p, "peer-a", "sdp-a", 1000);
    TEST_ASSERT_TRUE(kvsPendingSignalingPushIce(&p, "peer-a", "candA2", 6, 2));

    sig_capture_reset(true, true);
    KvsPendingDrainStats st;
    uint32_t remaining = kvsPendingSignalingDrain(&p, sig_capture_send, &s_sig, 2000, &st);

    TEST_ASSERT_EQUAL_UINT32(0, remaining);
    TEST_ASSERT_EQUAL_INT(4, s_sig.n);
    TEST_ASSERT_EQUAL_INT(KVS_PENDING_MSG_ANSWER, s_sig.type[0]);
    TEST_ASSERT_EQUAL_STRING("sdp-a", s_sig.payload[0]);
    TEST_ASSERT_EQUAL_STRING("corr", s_sig.corr[0]);     /* correlation id survives the buffer */
    TEST_ASSERT_EQUAL_STRING("candA1", s_sig.payload[1]); /* then candidates, FIFO */
    TEST_ASSERT_EQUAL_STRING("candB1", s_sig.payload[2]);
    TEST_ASSERT_EQUAL_STRING("candA2", s_sig.payload[3]);
    TEST_ASSERT_EQUAL_STRING("", s_sig.corr[1]);
    TEST_ASSERT_EQUAL_UINT32(1, st.answers_sent);
    TEST_ASSERT_EQUAL_UINT32(3, st.ice_sent);

    /* Everything delivered: both arrays and the payload are released. */
    TEST_ASSERT_NULL(p.answers);
    TEST_ASSERT_NULL(p.ice);
    TEST_ASSERT_EQUAL_INT(0, s_live_allocs);
}

TEST_CASE("pending-signaling: candidates wait behind an answer whose re-send fails", "[pending_ice]")
{
    KvsPendingSignaling p;
    s_live_allocs = 0;
    kvsPendingSignalingInit(&p, counting_calloc, counting_free);

    put_answer(&p, "peer-a", "sdp-a", 1000);
    TEST_ASSERT_TRUE(kvsPendingSignalingPushIce(&p, "peer-a", "candA", 5, 2));
    TEST_ASSERT_TRUE(kvsPendingSignalingPushIce(&p, "peer-b", "candB", 5, 2));

    /* The link drops again during the answer re-send. */
    sig_capture_reset(false, true);
    uint32_t remaining = kvsPendingSignalingDrain(&p, sig_capture_send, &s_sig, 2000, NULL);

    TEST_ASSERT_EQUAL_UINT32(2, remaining);              /* the answer and peer-a's candidate */
    TEST_ASSERT_EQUAL_INT(2, s_sig.n);                   /* answer tried, peer-b's candidate sent */
    TEST_ASSERT_EQUAL_STRING("candB", s_sig.payload[1]);
    TEST_ASSERT_TRUE(kvsPendingSignalingHasAnswer(&p, "peer-a"));
    TEST_ASSERT_EQUAL_UINT32(1, p.ice_count);
    TEST_ASSERT_EQUAL_STRING("candA", p.ice[0].payload);
    TEST_ASSERT_EQUAL_UINT32(0, p.ice[0].attempts);      /* held, not a failed attempt */

    /* Next reconnect works: answer first, then the held candidate. */
    sig_capture_reset(true, true);
    TEST_ASSERT_EQUAL_UINT32(0, kvsPendingSignalingDrain(&p, sig_capture_send, &s_sig, 3000, NULL));
    TEST_ASSERT_EQUAL_INT(2, s_sig.n);
    TEST_ASSERT_EQUAL_INT(KVS_PENDING_MSG_ANSWER, s_sig.type[0]);
    TEST_ASSERT_EQUAL_STRING("candA", s_sig.payload[1]);
    TEST_ASSERT_EQUAL_INT(0, s_live_allocs);
}

TEST_CASE("pending-answer: an answer past the age bound is dropped with its candidates", "[pending_ice]")
{
    KvsPendingSignaling p;
    s_live_allocs = 0;
    kvsPendingSignalingInit(&p, counting_calloc, counting_free);

    put_answer(&p, "peer-old", "sdp-old", 1000);
    put_answer(&p, "peer-new", "sdp-new", 1000 + KVS_PENDING_ANSWER_MAX_AGE_MS);
    TEST_ASSERT_TRUE(kvsPendingSignalingPushIce(&p, "peer-old", "candOld", 7, 2));
    TEST_ASSERT_TRUE(kvsPendingSignalingPushIce(&p, "peer-new", "candNew", 7, 2));

    /* peer-old's answer is one ms past the bound, peer-new's exactly at it. */
    sig_capture_reset(true, true);
    KvsPendingDrainStats st;
    uint32_t remaining = kvsPendingSignalingDrain(&p, sig_capture_send, &s_sig,
                                                  1000 + KVS_PENDING_ANSWER_MAX_AGE_MS + 1, &st);

    TEST_ASSERT_EQUAL_UINT32(0, remaining);
    TEST_ASSERT_EQUAL_UINT32(1, st.answers_expired);
    TEST_ASSERT_EQUAL_UINT32(1, st.answers_sent);
    TEST_ASSERT_EQUAL_UINT32(1, st.ice_dropped);         /* peer-old's candidate went with it */
    TEST_ASSERT_EQUAL_UINT32(1, st.ice_sent);
    TEST_ASSERT_EQUAL_INT(2, s_sig.n);
    TEST_ASSERT_EQUAL_STRING("sdp-new", s_sig.payload[0]);
    TEST_ASSERT_EQUAL_STRING("candNew", s_sig.payload[1]);
    TEST_ASSERT_EQUAL_INT(0, s_live_allocs);
}

TEST_CASE("pending-signaling: buffers exist only while something waits", "[pending_ice]")
{
    KvsPendingSignaling p;
    s_live_allocs = 0;
    kvsPendingSignalingInit(&p, counting_calloc, counting_free);
    TEST_ASSERT_EQUAL_INT(0, s_live_allocs);

    TEST_ASSERT_TRUE(kvsPendingSignalingPushIce(&p, "peer-a", "candA", 5, 2));
    TEST_ASSERT_NOT_NULL(p.ice);
    TEST_ASSERT_NULL(p.answers);                         /* no answer waiting: no answer slots */
    TEST_ASSERT_EQUAL_INT(1, s_live_allocs);

    put_answer(&p, "peer-a", "sdp-a", 1000);
    TEST_ASSERT_EQUAL_INT(3, s_live_allocs);             /* + answer slots + payload */

    /* A newer answer went out directly: the stale one is dropped and, being the
     * only one, its slot array is released. */
    TEST_ASSERT_TRUE(kvsPendingSignalingDropAnswer(&p, "peer-a"));
    TEST_ASSERT_NULL(p.answers);
    TEST_ASSERT_EQUAL_INT(1, s_live_allocs);

    /* The departed peer's candidate goes too, and the last array with it. */
    TEST_ASSERT_EQUAL_UINT32(1, kvsPendingSignalingPurgePeer(&p, "peer-a"));
    TEST_ASSERT_NULL(p.ice);
    TEST_ASSERT_EQUAL_INT(0, s_live_allocs);
}

TEST_CASE("pending-signaling: purge drops the departed peer's answer and candidates only", "[pending_ice]")
{
    KvsPendingSignaling p;
    s_live_allocs = 0;
    kvsPendingSignalingInit(&p, counting_calloc, counting_free);

    put_answer(&p, "peer-a", "sdp-a", 1000);
    put_answer(&p, "peer-b", "sdp-b", 1000);
    TEST_ASSERT_TRUE(kvsPendingSignalingPushIce(&p, "peer-a", "candA", 5, 2));
    TEST_ASSERT_TRUE(kvsPendingSignalingPushIce(&p, "peer-b", "candB", 5, 2));

    TEST_ASSERT_EQUAL_UINT32(2, kvsPendingSignalingPurgePeer(&p, "peer-a"));
    TEST_ASSERT_EQUAL_UINT32(2, kvsPendingSignalingCount(&p));
    TEST_ASSERT_FALSE(kvsPendingSignalingHasAnswer(&p, "peer-a"));
    TEST_ASSERT_TRUE(kvsPendingSignalingHasAnswer(&p, "peer-b"));
    TEST_ASSERT_EQUAL_STRING("candB", p.ice[0].payload);

    kvsPendingSignalingFree(&p);
    TEST_ASSERT_EQUAL_INT(0, s_live_allocs);
}

TEST_CASE("pending-signaling: merge puts survivors back, a newer answer wins", "[pending_ice]")
{
    KvsPendingSignaling p;
    KvsPendingSignaling batch;
    s_live_allocs = 0;
    kvsPendingSignalingInit(&p, counting_calloc, counting_free);

    put_answer(&p, "peer-a", "sdp-a-old", 1000);
    put_answer(&p, "peer-b", "sdp-b", 1000);
    TEST_ASSERT_TRUE(kvsPendingSignalingPushIce(&p, "peer-a", "candA", 5, 2));

    /* The owner takes the batch to send it without its lock... */
    kvsPendingSignalingTake(&p, &batch);
    TEST_ASSERT_EQUAL_UINT32(0, kvsPendingSignalingCount(&p));
    TEST_ASSERT_EQUAL_UINT32(3, kvsPendingSignalingCount(&batch));

    /* ...every send fails (link dropped again) while a newer answer for peer-a
     * and a new candidate are buffered. */
    sig_capture_reset(false, false);
    TEST_ASSERT_EQUAL_UINT32(3, kvsPendingSignalingDrain(&batch, sig_capture_send, &s_sig, 2000, NULL));
    put_answer(&p, "peer-a", "sdp-a-new", 2000);
    TEST_ASSERT_TRUE(kvsPendingSignalingPushIce(&p, "peer-b", "candB", 5, 2));

    uint32_t dropped = kvsPendingSignalingMerge(&p, &batch);

    TEST_ASSERT_EQUAL_UINT32(1, dropped);                /* the stale peer-a answer */
    TEST_ASSERT_EQUAL_UINT32(2, p.answer_count);
    TEST_ASSERT_EQUAL_UINT32(2, p.ice_count);
    TEST_ASSERT_EQUAL_STRING("sdp-a-new", answer_payload_for(&p, "peer-a"));
    TEST_ASSERT_EQUAL_STRING("sdp-b", answer_payload_for(&p, "peer-b"));
    TEST_ASSERT_NULL(batch.answers);
    TEST_ASSERT_NULL(batch.ice);

    kvsPendingSignalingFree(&p);
    TEST_ASSERT_EQUAL_INT(0, s_live_allocs);
}

TEST_CASE("pending-answer: an evicted answer takes its peer's candidates with it", "[pending_ice]")
{
    KvsPendingSignaling p;
    s_live_allocs = 0;
    kvsPendingSignalingInit(&p, counting_calloc, counting_free);
    char peer[16];

    for (int i = 0; i < KVS_PENDING_ANSWER_MAX; i++) {
        snprintf(peer, sizeof(peer), "peer-%d", i);
        put_answer(&p, peer, "sdp", 1000 + (uint64_t) i);
    }
    TEST_ASSERT_TRUE(kvsPendingSignalingPushIce(&p, "peer-0", "cand0", 5, 2));
    TEST_ASSERT_TRUE(kvsPendingSignalingPushIce(&p, "peer-1", "cand1", 5, 2));

    /* peer-0's answer is the oldest and goes. Its candidate must go too, or the
     * next drain sends it with no answer ahead of it. */
    put_answer(&p, "peer-late", "sdp", 5000);

    TEST_ASSERT_FALSE(kvsPendingSignalingHasAnswer(&p, "peer-0"));
    TEST_ASSERT_EQUAL_UINT32(1, p.ice_count);
    TEST_ASSERT_EQUAL_STRING("cand1", p.ice[0].payload);

    kvsPendingSignalingFree(&p);
    TEST_ASSERT_EQUAL_INT(0, s_live_allocs);
}

TEST_CASE("pending-signaling: a peer purged while the batch is out stays out after merge", "[pending_ice]")
{
    KvsPendingSignaling p;
    KvsPendingSignaling batch;
    s_live_allocs = 0;
    kvsPendingSignalingInit(&p, counting_calloc, counting_free);

    put_answer(&p, "peer-a", "sdp-a", 1000);
    put_answer(&p, "peer-b", "sdp-b", 1000);
    TEST_ASSERT_TRUE(kvsPendingSignalingPushIce(&p, "peer-a", "candA", 5, 2));
    TEST_ASSERT_TRUE(kvsPendingSignalingPushIce(&p, "peer-b", "candB", 5, 2));
    kvsPendingSignalingTake(&p, &batch);
    TEST_ASSERT_TRUE(p.batch_out);

    /* peer-a's session is torn down while the flush's re-sends are failing: there
     * is nothing in p to purge, but it must not come back with the batch. */
    TEST_ASSERT_EQUAL_UINT32(0, kvsPendingSignalingPurgePeer(&p, "peer-a"));
    TEST_ASSERT_TRUE(kvsPendingSignalingIsPurged(&p, "peer-a"));
    TEST_ASSERT_FALSE(kvsPendingSignalingIsPurged(&p, "peer-b"));

    sig_capture_reset(false, false);
    TEST_ASSERT_EQUAL_UINT32(4, kvsPendingSignalingDrain(&batch, sig_capture_send, &s_sig, 2000, NULL));
    TEST_ASSERT_EQUAL_UINT32(2, kvsPendingSignalingMerge(&p, &batch));   /* peer-a's answer and candidate */

    TEST_ASSERT_FALSE(kvsPendingSignalingHasAnswer(&p, "peer-a"));
    TEST_ASSERT_TRUE(kvsPendingSignalingHasAnswer(&p, "peer-b"));
    TEST_ASSERT_EQUAL_UINT32(1, p.ice_count);
    TEST_ASSERT_EQUAL_STRING("candB", p.ice[0].payload);

    /* The batch has ended: the purge list is gone with it. */
    TEST_ASSERT_FALSE(p.batch_out);
    TEST_ASSERT_FALSE(kvsPendingSignalingIsPurged(&p, "peer-a"));
    TEST_ASSERT_NULL(p.purged);

    kvsPendingSignalingFree(&p);
    TEST_ASSERT_EQUAL_INT(0, s_live_allocs);
}

TEST_CASE("pending-signaling: a purge with no batch out is not remembered", "[pending_ice]")
{
    KvsPendingSignaling p;
    s_live_allocs = 0;
    kvsPendingSignalingInit(&p, counting_calloc, counting_free);

    put_answer(&p, "peer-a", "sdp-a", 1000);
    TEST_ASSERT_EQUAL_UINT32(1, kvsPendingSignalingPurgePeer(&p, "peer-a"));
    TEST_ASSERT_FALSE(kvsPendingSignalingIsPurged(&p, "peer-a"));
    TEST_ASSERT_NULL(p.purged);
    TEST_ASSERT_EQUAL_INT(0, s_live_allocs);
}

TEST_CASE("pending-signaling: an empty take opens no batch window", "[pending_ice]")
{
    KvsPendingSignaling p;
    KvsPendingSignaling batch;
    s_live_allocs = 0;
    kvsPendingSignalingInit(&p, counting_calloc, counting_free);

    /* The flush at the first CONNECTED edge finds nothing buffered and frees the
     * batch without a Merge(). That must not leave a batch "out". */
    kvsPendingSignalingTake(&p, &batch);
    TEST_ASSERT_EQUAL_UINT32(0, kvsPendingSignalingCount(&batch));
    TEST_ASSERT_FALSE(p.batch_out);
    kvsPendingSignalingFree(&batch);

    /* A viewer leaves, so its session is purged... */
    TEST_ASSERT_EQUAL_UINT32(0, kvsPendingSignalingPurgePeer(&p, "peer-a"));
    TEST_ASSERT_FALSE(kvsPendingSignalingIsPurged(&p, "peer-a"));
    TEST_ASSERT_NULL(p.purged);

    /* ...and comes back with the same client id. Its answer, lost to a reconnect
     * and then to a failed re-send, survives the merge. */
    put_answer(&p, "peer-a", "sdp-a", 1000);
    kvsPendingSignalingTake(&p, &batch);
    TEST_ASSERT_TRUE(p.batch_out);
    sig_capture_reset(false, false);
    TEST_ASSERT_EQUAL_UINT32(1, kvsPendingSignalingDrain(&batch, sig_capture_send, &s_sig, 2000, NULL));
    TEST_ASSERT_EQUAL_UINT32(0, kvsPendingSignalingMerge(&p, &batch));
    TEST_ASSERT_TRUE(kvsPendingSignalingHasAnswer(&p, "peer-a"));
    TEST_ASSERT_FALSE(p.batch_out);

    kvsPendingSignalingFree(&p);
    TEST_ASSERT_EQUAL_INT(0, s_live_allocs);
}

TEST_CASE("pending-signaling: merge with no room for an answer drops its candidates too", "[pending_ice]")
{
    KvsPendingSignaling p;
    KvsPendingSignaling batch;
    s_live_allocs = 0;
    kvsPendingSignalingInit(&p, counting_calloc, counting_free);
    char peer[16];

    put_answer(&p, "peer-x", "sdp-x", 1000);
    TEST_ASSERT_TRUE(kvsPendingSignalingPushIce(&p, "peer-x", "candX", 5, 2));
    kvsPendingSignalingTake(&p, &batch);

    /* While the batch is out, newer answers for other peers fill every slot. */
    for (int i = 0; i < KVS_PENDING_ANSWER_MAX; i++) {
        snprintf(peer, sizeof(peer), "peer-%d", i);
        put_answer(&p, peer, "sdp", 2000 + (uint64_t) i);
    }
    sig_capture_reset(false, false);
    TEST_ASSERT_EQUAL_UINT32(2, kvsPendingSignalingDrain(&batch, sig_capture_send, &s_sig, 2000, NULL));

    TEST_ASSERT_EQUAL_UINT32(2, kvsPendingSignalingMerge(&p, &batch));
    TEST_ASSERT_FALSE(kvsPendingSignalingHasAnswer(&p, "peer-x"));
    TEST_ASSERT_NULL(p.ice);                             /* candX did not come back alone */
    TEST_ASSERT_EQUAL_UINT32(KVS_PENDING_ANSWER_MAX, p.answer_count);

    kvsPendingSignalingFree(&p);
    TEST_ASSERT_EQUAL_INT(0, s_live_allocs);
}

TEST_CASE("pending-signaling: an answer evicted during merge takes its batch candidates with it", "[pending_ice]")
{
    KvsPendingSignaling p;
    KvsPendingSignaling batch;
    s_live_allocs = 0;
    kvsPendingSignalingInit(&p, counting_calloc, counting_free);
    char peer[16];

    /* The batch holds every answer slot, and peer-0's candidate. */
    for (int i = 0; i < KVS_PENDING_ANSWER_MAX; i++) {
        snprintf(peer, sizeof(peer), "peer-%d", i);
        put_answer(&p, peer, "sdp", 1000 + (uint64_t) i);
    }
    TEST_ASSERT_TRUE(kvsPendingSignalingPushIce(&p, "peer-0", "cand0", 5, 2));
    kvsPendingSignalingTake(&p, &batch);

    /* While it is out, a newer answer arrives for another peer. */
    put_answer(&p, "peer-new", "sdp-new", 2000);
    sig_capture_reset(false, false);
    kvsPendingSignalingDrain(&batch, sig_capture_send, &s_sig, 1500, NULL);

    /* Merging the last batch answer evicts the oldest, peer-0, merged just before. */
    TEST_ASSERT_EQUAL_UINT32(2, kvsPendingSignalingMerge(&p, &batch));
    TEST_ASSERT_FALSE(kvsPendingSignalingHasAnswer(&p, "peer-0"));
    TEST_ASSERT_TRUE(kvsPendingSignalingHasAnswer(&p, "peer-new"));
    TEST_ASSERT_EQUAL_UINT32(KVS_PENDING_ANSWER_MAX, p.answer_count);
    TEST_ASSERT_EQUAL_UINT32(0, p.ice_count);           /* cand0 did not come back alone */

    kvsPendingSignalingFree(&p);
    TEST_ASSERT_EQUAL_INT(0, s_live_allocs);
}

TEST_CASE("pending-signaling: merge appends to new candidates and keeps attempt counts", "[pending_ice]")
{
    KvsPendingSignaling p;
    KvsPendingSignaling batch;
    s_live_allocs = 0;
    kvsPendingSignalingInit(&p, counting_calloc, counting_free);

    TEST_ASSERT_TRUE(kvsPendingSignalingPushIce(&p, "peer-a", "candOld", 7, 2));
    kvsPendingSignalingTake(&p, &batch);
    sig_capture_reset(true, false);
    TEST_ASSERT_EQUAL_UINT32(1, kvsPendingSignalingDrain(&batch, sig_capture_send, &s_sig, 2000, NULL));
    TEST_ASSERT_EQUAL_UINT32(1, batch.ice[0].attempts);
    TEST_ASSERT_TRUE(kvsPendingSignalingPushIce(&p, "peer-a", "candNew", 7, 2));

    TEST_ASSERT_EQUAL_UINT32(0, kvsPendingSignalingMerge(&p, &batch));
    TEST_ASSERT_EQUAL_UINT32(2, p.ice_count);
    TEST_ASSERT_EQUAL_STRING("candNew", p.ice[0].payload);
    TEST_ASSERT_EQUAL_STRING("candOld", p.ice[1].payload);
    TEST_ASSERT_EQUAL_UINT32(1, p.ice[1].attempts);      /* the failed re-send still counts */

    kvsPendingSignalingFree(&p);
    TEST_ASSERT_EQUAL_INT(0, s_live_allocs);
}

TEST_CASE("pending-answer: a failed allocation keeps nothing", "[pending_ice]")
{
    KvsPendingSignaling p;
    s_live_allocs = 0;
    kvsPendingSignalingInit(&p, counting_calloc, counting_free);

    s_alloc_budget = 0;                                  /* the slot array fails */
    TEST_ASSERT_FALSE(kvsPendingSignalingPutAnswer(&p, "peer-a", "corr", "sdp", 3, 2, 1000));
    s_alloc_budget = 1;                                  /* slots ok, the payload copy fails */
    TEST_ASSERT_FALSE(kvsPendingSignalingPutAnswer(&p, "peer-a", "corr", "sdp", 3, 2, 1000));
    s_alloc_budget = 0;
    TEST_ASSERT_FALSE(kvsPendingSignalingPushIce(&p, "peer-a", "cand", 4, 2));
    s_alloc_budget = -1;

    TEST_ASSERT_EQUAL_UINT32(0, kvsPendingSignalingCount(&p));
    TEST_ASSERT_NULL(p.answers);
    TEST_ASSERT_NULL(p.ice);
    TEST_ASSERT_EQUAL_INT(0, s_live_allocs);
}

TEST_CASE("pending-answer: a clock that steps back does not expire an answer", "[pending_ice]")
{
    KvsPendingSignaling p;
    s_live_allocs = 0;
    kvsPendingSignalingInit(&p, counting_calloc, counting_free);

    put_answer(&p, "peer-a", "sdp-a", 50000);
    sig_capture_reset(true, true);
    KvsPendingDrainStats st;
    TEST_ASSERT_EQUAL_UINT32(0, kvsPendingSignalingDrain(&p, sig_capture_send, &s_sig, 1000, &st));
    TEST_ASSERT_EQUAL_UINT32(1, st.answers_sent);
    TEST_ASSERT_EQUAL_UINT32(0, st.answers_expired);
    TEST_ASSERT_EQUAL_INT(0, s_live_allocs);
}
