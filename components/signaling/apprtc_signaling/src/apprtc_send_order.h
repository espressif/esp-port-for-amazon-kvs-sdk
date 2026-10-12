/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APPRTC_SEND_ORDER_H
#define APPRTC_SEND_ORDER_H

/* Order of outgoing AppRTC messages between direct senders and the queue drain.
 *
 * Messages that cannot go out at once wait in the outgoing queue, and a drain on the
 * work queue sends them in order. A sender on a connected socket may send directly
 * only when it cannot overtake anything: nothing is queued and the drain has no
 * message in flight (taken off the queue, not yet sent). Otherwise it queues behind.
 * Depends on message_queue and libc only, so the host test links it standalone. */

#include <stdbool.h>
#include <esp_err.h>
#include "message_queue.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool in_flight;     /* the drain holds a message off the queue; __atomic access only */
} apprtc_send_order_t;

typedef enum {
    APPRTC_SEND_DIRECT,     /* nothing to overtake: send now */
    APPRTC_SEND_QUEUE,      /* earlier messages pending: queue behind them */
} apprtc_send_decision_t;

/* Result of sending one queued message, from the drain's send callback */
typedef enum {
    APPRTC_DRAIN_SENT,
    APPRTC_DRAIN_NOT_CONNECTED,     /* the socket went away: keep it first for the next connect */
    APPRTC_DRAIN_FAILED,            /* connected, but the send failed */
} apprtc_drain_result_t;

typedef apprtc_drain_result_t (*apprtc_drain_send_fn_t)(void *data, int len, void *ctx);
/* Messages that must not be lost to one failed send (the offer and the answer) */
typedef bool (*apprtc_drain_must_deliver_fn_t)(const void *data, int len);

/**
 * @brief Decide whether a sender on a connected socket may send directly
 *
 * Not for the drain itself. Reads the queue count (under the queue lock) before the
 * in-flight flag; see apprtc_send_order.c for why the order matters.
 */
apprtc_send_decision_t apprtc_send_order_decide(apprtc_send_order_t *order, message_queue_t *queue);

/**
 * @brief Send the queued messages in order (one drain at a time)
 *
 * Each message is marked in flight before it leaves the queue and until it is sent or
 * handed back. On APPRTC_DRAIN_NOT_CONNECTED the message goes back to the front and the
 * drain stops (ESP_ERR_INVALID_STATE). On APPRTC_DRAIN_FAILED a must_deliver message is
 * put back at the front and tried once more; a second failure, or any other message,
 * is dropped and logged (ESP_FAIL). Sent and dropped messages are freed through the
 * queue's free_item.
 *
 * @param processed Optional; set to the number of messages sent
 */
esp_err_t apprtc_send_order_drain(apprtc_send_order_t *order, message_queue_t *queue,
                                  apprtc_drain_send_fn_t send, apprtc_drain_must_deliver_fn_t must_deliver,
                                  void *ctx, int *processed);

#ifdef __cplusplus
}
#endif

#endif /* APPRTC_SEND_ORDER_H */
