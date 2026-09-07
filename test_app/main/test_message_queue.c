/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* esp_webrtc_utils' message_queue, as its owners use it:
 * - AppRTC signaling: the default queue (depth 10, the oldest dropped when full);
 * - kvs_media's video intake: depth 3, a full queue refuses the new frame, the sender
 *   waits about one frame period for the next one, teardown drains what is left.
 * The waits are checked from a plain pthread, as the SDK's sender thread on the host.
 *
 * Coverage by configuration: depth (validation, 1, 3, 10, 64), full policy (drop the
 * oldest, refuse the new), heap caps, the wait modes (none, timed, forever), lifecycle
 * (cleanup, deinit, repeat init) and several producers against one consumer.
 * deinit must not race a reader: callers stop their readers first (kvs_media joins
 * its sender thread before it tears the queue down). */

#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <time.h>
#include "sdkconfig.h"
#include "unity.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "message_queue.h"

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t) ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int *new_item(int seq)
{
    int *it = malloc(sizeof(*it));
    TEST_ASSERT_NOT_NULL(it);
    *it = seq;
    return it;
}

static void add_seq(message_queue_t *q, int seq)
{
    TEST_ASSERT_EQUAL(ESP_OK, message_queue_add(q, new_item(seq), (int) sizeof(int)));
}

static int pop_seq(message_queue_t *q)
{
    void *data = NULL;
    int len = 0;
    TEST_ASSERT_EQUAL(ESP_OK, message_queue_get_next(q, &data, &len));
    TEST_ASSERT_EQUAL((int) sizeof(int), len);
    int seq = *(int *) data;
    free(data);
    return seq;
}

static void setup_video(message_queue_t *q)
{
    const message_queue_config_t cfg = {
        .depth = 3,
        .full_policy = MESSAGE_QUEUE_FULL_REJECT_NEW,
    };
    memset(q, 0, sizeof(*q));
    TEST_ASSERT_EQUAL(ESP_OK, message_queue_init_with_config(q, &cfg));
}

/* A reader on its own pthread. On the linux target it starts with SIGALRM blocked, as
 * the SDK's threads do: the FreeRTOS port's tick must only reach task threads. */
typedef struct {
    message_queue_t *q;
    uint32_t wait_ms;
    esp_err_t ret;
    int seq;
    int64_t waited_ms;
} reader_t;

static void *reader_main(void *arg)
{
    reader_t *r = (reader_t *) arg;
    void *data = NULL;
    int len = 0;
    int64_t start = now_ms();
    r->ret = message_queue_get_next_wait(r->q, &data, &len, r->wait_ms);
    r->waited_ms = now_ms() - start;
    if (r->ret == ESP_OK) {
        r->seq = *(int *) data;
        free(data);
    }
    return NULL;
}

static pthread_t start_reader(reader_t *r)
{
    pthread_t tid;
#if CONFIG_IDF_TARGET_LINUX
    sigset_t block, old;
    sigemptyset(&block);
    sigaddset(&block, SIGALRM);
    pthread_sigmask(SIG_BLOCK, &block, &old);
#endif
    TEST_ASSERT_EQUAL(0, pthread_create(&tid, NULL, reader_main, r));
#if CONFIG_IDF_TARGET_LINUX
    pthread_sigmask(SIG_SETMASK, &old, NULL);
#endif
    return tid;
}

TEST_CASE("message_queue: default init holds 10 and drops the oldest when full", "[message_queue]")
{
    message_queue_t q;
    memset(&q, 0, sizeof(q));
    TEST_ASSERT_EQUAL(ESP_OK, message_queue_init(&q));

    for (int i = 0; i < MESSAGE_QUEUE_DEFAULT_DEPTH + 1; i++) {
        add_seq(&q, i);
    }
    TEST_ASSERT_EQUAL(MESSAGE_QUEUE_DEFAULT_DEPTH, message_queue_get_count(&q));
    TEST_ASSERT_EQUAL(1, pop_seq(&q));                 /* 0 was dropped */
    for (int i = 2; i <= MESSAGE_QUEUE_DEFAULT_DEPTH; i++) {
        TEST_ASSERT_EQUAL(i, pop_seq(&q));             /* FIFO */
    }
    TEST_ASSERT_TRUE(message_queue_is_empty(&q));
    TEST_ASSERT_EQUAL(ESP_OK, message_queue_deinit(&q));
}

TEST_CASE("message_queue: get_next on an empty queue returns at once", "[message_queue]")
{
    message_queue_t q;
    setup_video(&q);
    void *data = (void *) 1;
    int len = 1;
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, message_queue_get_next(&q, &data, &len));
    TEST_ASSERT_NULL(data);
    TEST_ASSERT_EQUAL(0, len);
    int64_t start = now_ms();
    TEST_ASSERT_EQUAL(ESP_ERR_TIMEOUT, message_queue_get_next_wait(&q, &data, &len, 0));
    TEST_ASSERT_LESS_THAN(200, now_ms() - start);
    TEST_ASSERT_EQUAL(ESP_OK, message_queue_deinit(&q));
}

TEST_CASE("message_queue: video intake refuses a frame when full and keeps the queue", "[message_queue]")
{
    message_queue_t q;
    setup_video(&q);

    for (int i = 0; i < 3; i++) {
        add_seq(&q, i);
    }
    int *extra = new_item(3);
    int64_t start = now_ms();
    TEST_ASSERT_EQUAL(ESP_ERR_NO_MEM, message_queue_add(&q, extra, (int) sizeof(int)));
    TEST_ASSERT_LESS_THAN(200, now_ms() - start);      /* no wait for room */
    TEST_ASSERT_EQUAL(3, *extra);                      /* still the caller's */
    free(extra);

    TEST_ASSERT_EQUAL(3, message_queue_get_count(&q));
    for (int i = 0; i < 3; i++) {
        TEST_ASSERT_EQUAL(i, pop_seq(&q));
    }
    add_seq(&q, 4);                                    /* room again */
    TEST_ASSERT_EQUAL(4, pop_seq(&q));
    TEST_ASSERT_EQUAL(ESP_OK, message_queue_deinit(&q));
}

TEST_CASE("message_queue: a timed get on an empty queue waits its timeout", "[message_queue]")
{
    message_queue_t q;
    setup_video(&q);
    reader_t r = { .q = &q, .wait_ms = 100 };
    pthread_t tid = start_reader(&r);
    TEST_ASSERT_EQUAL(0, pthread_join(tid, NULL));
    TEST_ASSERT_EQUAL(ESP_ERR_TIMEOUT, r.ret);
    /* Tick-rounded on the chips like xQueueReceive(), so allow a tick short */
    TEST_ASSERT_GREATER_OR_EQUAL(80, r.waited_ms);
    TEST_ASSERT_LESS_THAN(1000, r.waited_ms);
    TEST_ASSERT_EQUAL(ESP_OK, message_queue_deinit(&q));
}

TEST_CASE("message_queue: a waiting reader is woken by an add from another thread", "[message_queue]")
{
    message_queue_t q;
    setup_video(&q);
    reader_t r = { .q = &q, .wait_ms = 5000 };
    pthread_t tid = start_reader(&r);
    vTaskDelay(pdMS_TO_TICKS(50));
    add_seq(&q, 7);
    TEST_ASSERT_EQUAL(0, pthread_join(tid, NULL));
    TEST_ASSERT_EQUAL(ESP_OK, r.ret);
    TEST_ASSERT_EQUAL(7, r.seq);
    TEST_ASSERT_LESS_THAN(2000, r.waited_ms);          /* woken, not timed out */
    TEST_ASSERT_TRUE(message_queue_is_empty(&q));
    TEST_ASSERT_EQUAL(ESP_OK, message_queue_deinit(&q));
}

TEST_CASE("message_queue: a reader waiting forever is woken by an add", "[message_queue]")
{
    message_queue_t q;
    setup_video(&q);
    reader_t r = { .q = &q, .wait_ms = MESSAGE_QUEUE_WAIT_FOREVER };
    pthread_t tid = start_reader(&r);
    vTaskDelay(pdMS_TO_TICKS(50));
    add_seq(&q, 9);
    TEST_ASSERT_EQUAL(0, pthread_join(tid, NULL));
    TEST_ASSERT_EQUAL(ESP_OK, r.ret);
    TEST_ASSERT_EQUAL(9, r.seq);
    TEST_ASSERT_EQUAL(ESP_OK, message_queue_deinit(&q));
}

TEST_CASE("message_queue: a message already queued is returned without waiting", "[message_queue]")
{
    message_queue_t q;
    setup_video(&q);
    add_seq(&q, 1);
    reader_t r = { .q = &q, .wait_ms = 1000 };
    pthread_t tid = start_reader(&r);
    TEST_ASSERT_EQUAL(0, pthread_join(tid, NULL));
    TEST_ASSERT_EQUAL(ESP_OK, r.ret);
    TEST_ASSERT_EQUAL(1, r.seq);
    TEST_ASSERT_LESS_THAN(200, r.waited_ms);
    TEST_ASSERT_EQUAL(ESP_OK, message_queue_deinit(&q));
}

TEST_CASE("message_queue: drain, deinit and a repeat init", "[message_queue]")
{
    message_queue_t q;
    setup_video(&q);
    add_seq(&q, 0);
    add_seq(&q, 1);

    /* The video teardown drain: poll until empty */
    void *data;
    int len;
    int drained = 0;
    while (message_queue_get_next_wait(&q, &data, &len, 0) == ESP_OK) {
        free(data);
        drained++;
    }
    TEST_ASSERT_EQUAL(2, drained);

    /* A repeat init only purges; deinit frees what is left */
    add_seq(&q, 2);
    const message_queue_config_t other = { .depth = 8, .full_policy = MESSAGE_QUEUE_FULL_DROP_OLDEST };
    TEST_ASSERT_EQUAL(ESP_OK, message_queue_init_with_config(&q, &other));
    TEST_ASSERT_EQUAL(0, message_queue_get_count(&q));
    TEST_ASSERT_EQUAL(3, q.depth);                     /* the first configuration stays */
    add_seq(&q, 3);
    TEST_ASSERT_EQUAL(ESP_OK, message_queue_deinit(&q));
    TEST_ASSERT_NULL(q.mutex);

    setup_video(&q);
    add_seq(&q, 4);
    TEST_ASSERT_EQUAL(4, pop_seq(&q));
    TEST_ASSERT_EQUAL(ESP_OK, message_queue_deinit(&q));
}

/* ---- configuration ---- */

TEST_CASE("message_queue: config: a NULL queue or config and a depth below 1 are refused", "[message_queue]")
{
    message_queue_t q;
    memset(&q, 0, sizeof(q));
    message_queue_config_t cfg = { .depth = 4, .full_policy = MESSAGE_QUEUE_FULL_DROP_OLDEST };
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, message_queue_init_with_config(NULL, &cfg));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, message_queue_init_with_config(&q, NULL));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, message_queue_init(NULL));
    cfg.depth = 0;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, message_queue_init_with_config(&q, &cfg));
    cfg.depth = -1;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, message_queue_init_with_config(&q, &cfg));
    TEST_ASSERT_NULL(q.mutex);                         /* nothing was set up */
    TEST_ASSERT_NULL(q.messages);
    /* Calls on a queue that was never set up fail instead of crashing */
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, message_queue_add(&q, &cfg, 1));
    void *data;
    int len;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, message_queue_get_next_wait(&q, &data, &len, 0));
    TEST_ASSERT_EQUAL(ESP_OK, message_queue_deinit(&q));
}

TEST_CASE("message_queue: config: depth 1 under either full policy", "[message_queue]")
{
    message_queue_t q;
    memset(&q, 0, sizeof(q));
    message_queue_config_t cfg = { .depth = 1, .full_policy = MESSAGE_QUEUE_FULL_DROP_OLDEST };
    TEST_ASSERT_EQUAL(ESP_OK, message_queue_init_with_config(&q, &cfg));
    add_seq(&q, 1);
    add_seq(&q, 2);                                    /* replaces 1 */
    TEST_ASSERT_EQUAL(1, message_queue_get_count(&q));
    TEST_ASSERT_EQUAL(2, pop_seq(&q));
    TEST_ASSERT_EQUAL(ESP_OK, message_queue_deinit(&q));

    memset(&q, 0, sizeof(q));
    cfg.full_policy = MESSAGE_QUEUE_FULL_REJECT_NEW;
    TEST_ASSERT_EQUAL(ESP_OK, message_queue_init_with_config(&q, &cfg));
    add_seq(&q, 1);
    int *extra = new_item(2);
    TEST_ASSERT_EQUAL(ESP_ERR_NO_MEM, message_queue_add(&q, extra, (int) sizeof(int)));
    free(extra);
    TEST_ASSERT_EQUAL(1, pop_seq(&q));
    TEST_ASSERT_EQUAL(ESP_OK, message_queue_deinit(&q));
}

TEST_CASE("message_queue: config: a depth of 64 fills to exactly 64", "[message_queue]")
{
    for (int policy = 0; policy < 2; policy++) {
        message_queue_t q;
        memset(&q, 0, sizeof(q));
        const message_queue_config_t cfg = {
            .depth = 64,
            .full_policy = policy ? MESSAGE_QUEUE_FULL_REJECT_NEW : MESSAGE_QUEUE_FULL_DROP_OLDEST,
        };
        TEST_ASSERT_EQUAL(ESP_OK, message_queue_init_with_config(&q, &cfg));
        for (int i = 0; i < 64; i++) {
            add_seq(&q, i);
        }
        TEST_ASSERT_EQUAL(64, message_queue_get_count(&q));
        int *extra = new_item(64);
        esp_err_t err = message_queue_add(&q, extra, (int) sizeof(int));
        TEST_ASSERT_EQUAL(64, message_queue_get_count(&q));
        if (policy) {
            TEST_ASSERT_EQUAL(ESP_ERR_NO_MEM, err);
            free(extra);
            TEST_ASSERT_EQUAL(0, pop_seq(&q));
        } else {
            TEST_ASSERT_EQUAL(ESP_OK, err);
            TEST_ASSERT_EQUAL(1, pop_seq(&q));         /* 0 was dropped */
        }
        TEST_ASSERT_EQUAL(ESP_OK, message_queue_deinit(&q));
    }
}

TEST_CASE("message_queue: config: heap caps for the slots", "[message_queue]")
{
    message_queue_t q;
    memset(&q, 0, sizeof(q));
    message_queue_config_t cfg = {
        .depth = 4,
        .full_policy = MESSAGE_QUEUE_FULL_REJECT_NEW,
        .caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT,
    };
    TEST_ASSERT_EQUAL(ESP_OK, message_queue_init_with_config(&q, &cfg));
    add_seq(&q, 1);
    TEST_ASSERT_EQUAL(1, pop_seq(&q));
    TEST_ASSERT_EQUAL(ESP_OK, message_queue_deinit(&q));

    /* The linux target ignores caps; a chip without PSRAM refuses the request */
    memset(&q, 0, sizeof(q));
    cfg.caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    esp_err_t err = message_queue_init_with_config(&q, &cfg);
#if CONFIG_IDF_TARGET_LINUX
    TEST_ASSERT_EQUAL(ESP_OK, err);
#else
    if (err == ESP_ERR_NO_MEM) {
        TEST_ASSERT_NULL(q.mutex);
        return;
    }
    TEST_ASSERT_EQUAL(ESP_OK, err);
#endif
    add_seq(&q, 2);
    TEST_ASSERT_EQUAL(2, pop_seq(&q));
    TEST_ASSERT_EQUAL(ESP_OK, message_queue_deinit(&q));
}

TEST_CASE("message_queue: cleanup empties the queue and it can be reused", "[message_queue]")
{
    message_queue_t q;
    setup_video(&q);
    add_seq(&q, 0);
    add_seq(&q, 1);
    TEST_ASSERT_EQUAL(ESP_OK, message_queue_cleanup(&q));
    TEST_ASSERT_EQUAL(0, message_queue_get_count(&q));
    TEST_ASSERT_NOT_NULL(q.mutex);                     /* the lock stays */
    add_seq(&q, 2);
    TEST_ASSERT_EQUAL(2, pop_seq(&q));
    TEST_ASSERT_EQUAL(ESP_OK, message_queue_deinit(&q));
    TEST_ASSERT_EQUAL(ESP_OK, message_queue_deinit(&q)); /* a second deinit is harmless */
}

/* ---- several producers, one consumer ---- */

#define MQ_PRODUCERS     4
#define MQ_PER_PRODUCER  200

typedef struct {
    message_queue_t *q;
    int id;
    int rejected;                                      /* refused and freed by the producer */
} producer_t;

static void *producer_main(void *arg)
{
    producer_t *p = (producer_t *) arg;
    for (int i = 0; i < MQ_PER_PRODUCER; i++) {
        int *it = new_item(p->id * 100000 + i);
        if (message_queue_add(p->q, it, (int) sizeof(int)) != ESP_OK) {
            free(it);
            p->rejected++;
        }
        if ((i % 16) == 0) {
            sched_yield();
        }
    }
    return NULL;
}

static pthread_t start_thread(void *(*fn)(void *), void *arg)
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

typedef struct {
    message_queue_t *q;
    int expected;                                      /* stop once this many arrived or all rejected */
    volatile int *producers_done;
    int received;
    int last_seq[MQ_PRODUCERS];
    bool in_order;
} consumer_t;

static void *consumer_main(void *arg)
{
    consumer_t *c = (consumer_t *) arg;
    for (int p = 0; p < MQ_PRODUCERS; p++) {
        c->last_seq[p] = -1;
    }
    c->in_order = true;
    for (;;) {
        void *data;
        int len;
        if (message_queue_get_next_wait(c->q, &data, &len, 100) == ESP_OK) {
            int v = *(int *) data;
            free(data);
            int p = v / 100000;
            int seq = v % 100000;
            if (p < 0 || p >= MQ_PRODUCERS || seq <= c->last_seq[p]) {
                c->in_order = false;
            } else {
                c->last_seq[p] = seq;
            }
            c->received++;
        } else if (*c->producers_done) {
            /* Every add happened before the flag; take any that landed after the timeout */
            while (message_queue_get_next_wait(c->q, &data, &len, 0) == ESP_OK) {
                int v = *(int *) data;
                free(data);
                int p = v / 100000;
                if (p < 0 || p >= MQ_PRODUCERS || v % 100000 <= c->last_seq[p]) {
                    c->in_order = false;
                } else {
                    c->last_seq[p] = v % 100000;
                }
                c->received++;
            }
            break;
        }
    }
    return NULL;
}

static void run_producers(int depth, message_queue_full_policy_t policy, int *received, int *rejected, bool *in_order)
{
    message_queue_t q;
    memset(&q, 0, sizeof(q));
    const message_queue_config_t cfg = { .depth = depth, .full_policy = policy };
    TEST_ASSERT_EQUAL(ESP_OK, message_queue_init_with_config(&q, &cfg));

    volatile int done = 0;
    consumer_t c = { .q = &q, .producers_done = &done };
    pthread_t ctid = start_thread(consumer_main, &c);
    producer_t prod[MQ_PRODUCERS];
    pthread_t ptid[MQ_PRODUCERS];
    for (int i = 0; i < MQ_PRODUCERS; i++) {
        prod[i] = (producer_t) { .q = &q, .id = i };
        ptid[i] = start_thread(producer_main, &prod[i]);
    }
    *rejected = 0;
    for (int i = 0; i < MQ_PRODUCERS; i++) {
        TEST_ASSERT_EQUAL(0, pthread_join(ptid[i], NULL));
        *rejected += prod[i].rejected;
    }
    done = 1;
    TEST_ASSERT_EQUAL(0, pthread_join(ctid, NULL));
    *received = c.received;
    *in_order = c.in_order;
    TEST_ASSERT_TRUE(message_queue_is_empty(&q));
    TEST_ASSERT_EQUAL(ESP_OK, message_queue_deinit(&q));
}

TEST_CASE("message_queue: concurrency: every item from 4 producers arrives once, in order", "[message_queue]")
{
    int received, rejected;
    bool in_order;
    run_producers(MQ_PRODUCERS * MQ_PER_PRODUCER, MESSAGE_QUEUE_FULL_REJECT_NEW, &received, &rejected, &in_order);
    TEST_ASSERT_EQUAL(0, rejected);
    TEST_ASSERT_EQUAL(MQ_PRODUCERS * MQ_PER_PRODUCER, received);
    TEST_ASSERT_TRUE(in_order);                        /* FIFO per producer */
}

TEST_CASE("message_queue: concurrency: a small refusing queue loses nothing it accepted", "[message_queue]")
{
    int received, rejected;
    bool in_order;
    run_producers(2, MESSAGE_QUEUE_FULL_REJECT_NEW, &received, &rejected, &in_order);
    TEST_ASSERT_EQUAL(MQ_PRODUCERS * MQ_PER_PRODUCER, received + rejected);
    TEST_ASSERT_GREATER_THAN(0, received);
    TEST_ASSERT_TRUE(in_order);
}
