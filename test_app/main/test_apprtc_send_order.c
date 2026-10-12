/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* AppRTC's outgoing order (apprtc_send_order.c): a sender may send directly only when
 * it cannot overtake a queued or in-flight message, and the drain sends the queue in
 * order. A failed send while connected gives the offer or answer one more try; a lost
 * connection keeps the message first for the next connect. */

#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <time.h>
#include "sdkconfig.h"
#include "unity.h"
#include "message_queue.h"
#include "apprtc_send_order.h"

typedef struct {
    int seq;
    bool must_deliver;      /* an offer or answer; false for an ICE candidate */
} so_msg_t;

#define SO_MAX_LOG 64

/* What the fake socket saw, and what the queue freed */
typedef struct {
    message_queue_t *q;
    apprtc_send_order_t *order;
    int sent[SO_MAX_LOG];
    int n_sent;
    int frees;
    int fail_seq;           /* this message fails ... */
    int fail_times;         /* ... this many times */
    int disconnect_seq;     /* this message finds the socket gone (once) */
    int enqueue_during;     /* while this one is in flight, a sender adds enqueue_seq */
    int enqueue_seq;
    apprtc_send_decision_t decided_during;
} so_ctx_t;

static so_ctx_t *s_ctx;

static void so_free(void *data)
{
    s_ctx->frees++;
    free(data);
}

static bool so_must_deliver(const void *data, int len)
{
    (void) len;
    return ((const so_msg_t *) data)->must_deliver;
}

static so_msg_t *so_new(int seq, bool must_deliver)
{
    so_msg_t *m = malloc(sizeof(*m));
    TEST_ASSERT_NOT_NULL(m);
    m->seq = seq;
    m->must_deliver = must_deliver;
    return m;
}

static void so_add(message_queue_t *q, int seq, bool must_deliver)
{
    TEST_ASSERT_EQUAL(ESP_OK, message_queue_add(q, so_new(seq, must_deliver), (int) sizeof(so_msg_t)));
}

static apprtc_drain_result_t so_send(void *data, int len, void *arg)
{
    (void) len;
    so_ctx_t *c = (so_ctx_t *) arg;
    so_msg_t *m = (so_msg_t *) data;

    if (m->seq == c->enqueue_during) {
        /* A sender while this message is off the queue: it must queue behind */
        c->decided_during = apprtc_send_order_decide(c->order, c->q);
        if (c->decided_during == APPRTC_SEND_QUEUE) {
            so_add(c->q, c->enqueue_seq, false);
        }
        c->enqueue_during = -1;
    }
    if (m->seq == c->disconnect_seq) {
        c->disconnect_seq = -1;
        return APPRTC_DRAIN_NOT_CONNECTED;
    }
    if (m->seq == c->fail_seq && c->fail_times > 0) {
        c->fail_times--;
        return APPRTC_DRAIN_FAILED;
    }
    TEST_ASSERT_LESS_THAN(SO_MAX_LOG, c->n_sent);
    c->sent[c->n_sent++] = m->seq;
    return APPRTC_DRAIN_SENT;
}

static message_queue_t s_q;
static apprtc_send_order_t s_order;
static so_ctx_t s_c;

static void so_setup(void)
{
    static const message_queue_config_t cfg = {
        .depth = 32,
        .full_policy = MESSAGE_QUEUE_FULL_DROP_OLDEST,
    };
    memset(&s_q, 0, sizeof(s_q));
    memset(&s_order, 0, sizeof(s_order));
    memset(&s_c, 0, sizeof(s_c));
    TEST_ASSERT_EQUAL(ESP_OK, message_queue_init_with_config(&s_q, &cfg));
    s_q.free_item = so_free;
    s_c.q = &s_q;
    s_c.order = &s_order;
    s_c.fail_seq = s_c.disconnect_seq = s_c.enqueue_during = -1;
    s_ctx = &s_c;
}

static void so_teardown(void)
{
    message_queue_deinit(&s_q);
    s_ctx = NULL;
}

TEST_CASE("apprtc_send_order: an idle queue lets a sender send directly", "[apprtc_send_order]")
{
    so_setup();
    TEST_ASSERT_EQUAL(APPRTC_SEND_DIRECT, apprtc_send_order_decide(&s_order, &s_q));
    so_teardown();
}

TEST_CASE("apprtc_send_order: queued messages make a sender queue behind them", "[apprtc_send_order]")
{
    so_setup();
    so_add(&s_q, 1, true);
    TEST_ASSERT_EQUAL(APPRTC_SEND_QUEUE, apprtc_send_order_decide(&s_order, &s_q));
    so_teardown();
}

TEST_CASE("apprtc_send_order: the drain's last message in flight is not overtaken", "[apprtc_send_order]")
{
    /* The answer is the last queued message: once the drain has taken it, the queue is
     * empty, but a candidate decided then must still go behind it */
    so_setup();
    so_add(&s_q, 1, true);
    s_c.enqueue_during = 1;
    s_c.enqueue_seq = 2;
    int processed = -1;
    TEST_ASSERT_EQUAL(ESP_OK, apprtc_send_order_drain(&s_order, &s_q, so_send, so_must_deliver, &s_c, &processed));
    TEST_ASSERT_EQUAL(APPRTC_SEND_QUEUE, s_c.decided_during);
    /* ... and the same drain sends it after the answer */
    TEST_ASSERT_EQUAL(2, processed);
    TEST_ASSERT_EQUAL(2, s_c.n_sent);
    TEST_ASSERT_EQUAL(1, s_c.sent[0]);
    TEST_ASSERT_EQUAL(2, s_c.sent[1]);
    TEST_ASSERT_EQUAL(2, s_c.frees);
    TEST_ASSERT_EQUAL(APPRTC_SEND_DIRECT, apprtc_send_order_decide(&s_order, &s_q));
    so_teardown();
}

TEST_CASE("apprtc_send_order: a message queued mid-drain goes behind the rest", "[apprtc_send_order]")
{
    so_setup();
    so_add(&s_q, 1, true);
    so_add(&s_q, 2, false);
    so_add(&s_q, 3, false);
    s_c.enqueue_during = 1;
    s_c.enqueue_seq = 4;
    TEST_ASSERT_EQUAL(ESP_OK, apprtc_send_order_drain(&s_order, &s_q, so_send, so_must_deliver, &s_c, NULL));
    TEST_ASSERT_EQUAL(4, s_c.n_sent);
    for (int i = 0; i < 4; i++) {
        TEST_ASSERT_EQUAL(i + 1, s_c.sent[i]);
    }
    so_teardown();
}

TEST_CASE("apprtc_send_order: a lost connection keeps the message first for the next drain", "[apprtc_send_order]")
{
    so_setup();
    so_add(&s_q, 1, true);
    so_add(&s_q, 2, false);
    s_c.disconnect_seq = 1;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE,
                      apprtc_send_order_drain(&s_order, &s_q, so_send, so_must_deliver, &s_c, NULL));
    TEST_ASSERT_EQUAL(0, s_c.n_sent);
    TEST_ASSERT_EQUAL(0, s_c.frees);
    TEST_ASSERT_EQUAL(2, message_queue_get_count(&s_q));
    /* Nothing in flight any more, but the queue still holds the answer */
    TEST_ASSERT_EQUAL(APPRTC_SEND_QUEUE, apprtc_send_order_decide(&s_order, &s_q));

    TEST_ASSERT_EQUAL(ESP_OK, apprtc_send_order_drain(&s_order, &s_q, so_send, so_must_deliver, &s_c, NULL));
    TEST_ASSERT_EQUAL(2, s_c.n_sent);
    TEST_ASSERT_EQUAL(1, s_c.sent[0]);
    TEST_ASSERT_EQUAL(2, s_c.sent[1]);
    so_teardown();
}

TEST_CASE("apprtc_send_order: a failed answer is tried once more before what follows", "[apprtc_send_order]")
{
    so_setup();
    so_add(&s_q, 1, true);
    so_add(&s_q, 2, false);
    s_c.fail_seq = 1;
    s_c.fail_times = 1;
    TEST_ASSERT_EQUAL(ESP_OK, apprtc_send_order_drain(&s_order, &s_q, so_send, so_must_deliver, &s_c, NULL));
    TEST_ASSERT_EQUAL(2, s_c.n_sent);
    TEST_ASSERT_EQUAL(1, s_c.sent[0]);
    TEST_ASSERT_EQUAL(2, s_c.sent[1]);
    TEST_ASSERT_EQUAL(2, s_c.frees);
    so_teardown();
}

TEST_CASE("apprtc_send_order: an answer failing twice is dropped once, the rest still goes", "[apprtc_send_order]")
{
    so_setup();
    so_add(&s_q, 1, true);
    so_add(&s_q, 2, false);
    s_c.fail_seq = 1;
    s_c.fail_times = 2;
    TEST_ASSERT_EQUAL(ESP_FAIL, apprtc_send_order_drain(&s_order, &s_q, so_send, so_must_deliver, &s_c, NULL));
    TEST_ASSERT_EQUAL(1, s_c.n_sent);
    TEST_ASSERT_EQUAL(2, s_c.sent[0]);
    TEST_ASSERT_EQUAL(2, s_c.frees);
    TEST_ASSERT_EQUAL(0, message_queue_get_count(&s_q));
    so_teardown();
}

TEST_CASE("apprtc_send_order: a failed candidate is dropped without a retry", "[apprtc_send_order]")
{
    so_setup();
    so_add(&s_q, 1, false);
    so_add(&s_q, 2, false);
    s_c.fail_seq = 1;
    s_c.fail_times = 1;
    TEST_ASSERT_EQUAL(ESP_FAIL, apprtc_send_order_drain(&s_order, &s_q, so_send, so_must_deliver, &s_c, NULL));
    TEST_ASSERT_EQUAL(1, s_c.n_sent);
    TEST_ASSERT_EQUAL(2, s_c.sent[0]);
    TEST_ASSERT_EQUAL(2, s_c.frees);
    so_teardown();
}

/* Two threads, as on a board: the drain sends the queued answer while a sender keeps
 * deciding for a candidate. Whenever the sender is told to send directly, the answer
 * must already be on the wire. */
#define SO_ROUNDS 300

typedef struct {
    message_queue_t *q;
    apprtc_send_order_t *order;
    volatile int answer_sent;   /* set by the drain once the answer is "on the wire" */
    volatile int start;
    int overtaken;
} so_race_t;

static so_race_t *s_race;

static apprtc_drain_result_t race_send(void *data, int len, void *arg)
{
    (void) len;
    (void) arg;
    if (((so_msg_t *) data)->must_deliver) {
        for (volatile int i = 0; i < 200; i++) {
        }
        __atomic_store_n(&s_race->answer_sent, 1, __ATOMIC_RELEASE);
    }
    return APPRTC_DRAIN_SENT;
}

static void *race_sender(void *arg)
{
    so_race_t *r = (so_race_t *) arg;
    while (!__atomic_load_n(&r->start, __ATOMIC_ACQUIRE)) {
        sched_yield();
    }
    for (;;) {
        if (apprtc_send_order_decide(r->order, r->q) == APPRTC_SEND_DIRECT) {
            if (!__atomic_load_n(&r->answer_sent, __ATOMIC_ACQUIRE)) {
                r->overtaken++;
            }
            return NULL;
        }
    }
}

static void *race_drain(void *arg)
{
    so_race_t *r = (so_race_t *) arg;
    while (!__atomic_load_n(&r->start, __ATOMIC_ACQUIRE)) {
        sched_yield();
    }
    apprtc_send_order_drain(r->order, r->q, race_send, so_must_deliver, NULL, NULL);
    return NULL;
}

static pthread_t so_start_thread(void *(*fn)(void *), void *arg)
{
    pthread_t tid;
#if CONFIG_IDF_TARGET_LINUX
    sigset_t block, old;
    sigemptyset(&block);
    sigaddset(&block, SIGALRM);
    pthread_sigmask(SIG_BLOCK, &block, &old);
#endif
    TEST_ASSERT_EQUAL(0, pthread_create(&tid, NULL, fn, arg));
#if CONFIG_IDF_TARGET_LINUX
    pthread_sigmask(SIG_SETMASK, &old, NULL);
#endif
    return tid;
}

TEST_CASE("apprtc_send_order: a racing sender never overtakes the drained answer", "[apprtc_send_order]")
{
    so_setup();
    so_race_t race = { .q = &s_q, .order = &s_order };
    s_race = &race;
    for (int round = 0; round < SO_ROUNDS; round++) {
        so_add(&s_q, round, true);
        race.answer_sent = 0;
        race.start = 0;
        pthread_t drain = so_start_thread(race_drain, &race);
        pthread_t sender = so_start_thread(race_sender, &race);
        __atomic_store_n(&race.start, 1, __ATOMIC_RELEASE);
        pthread_join(drain, NULL);
        pthread_join(sender, NULL);
    }
    TEST_ASSERT_EQUAL(0, race.overtaken);
    TEST_ASSERT_EQUAL(SO_ROUNDS, s_c.frees);
    s_race = NULL;
    so_teardown();
}
