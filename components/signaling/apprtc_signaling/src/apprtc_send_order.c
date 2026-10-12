/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>
#include "esp_log.h"
#include "apprtc_send_order.h"

static const char *TAG = "apprtc_send_order";

static void free_message(const message_queue_t *queue, void *data)
{
    if (queue->free_item != NULL) {
        queue->free_item(data);
    } else {
        free(data);
    }
}

/* The count first, then the flag. The drain raises the flag before its pop, and the pop
 * takes the queue lock that message_queue_get_count() takes too: a sender that finds
 * the queue empty after the drain popped its last message therefore also sees the
 * flag. Read the other way round, a sender could see the flag still clear, then find
 * the queue empty because the drain popped meanwhile, and send ahead of the message
 * the drain is about to send (a trickled candidate overtaking the answer). */
apprtc_send_decision_t apprtc_send_order_decide(apprtc_send_order_t *order, message_queue_t *queue)
{
    if (message_queue_get_count(queue) > 0) {
        return APPRTC_SEND_QUEUE;
    }
    if (__atomic_load_n(&order->in_flight, __ATOMIC_ACQUIRE)) {
        return APPRTC_SEND_QUEUE;
    }
    return APPRTC_SEND_DIRECT;
}

esp_err_t apprtc_send_order_drain(apprtc_send_order_t *order, message_queue_t *queue,
                                  apprtc_drain_send_fn_t send, apprtc_drain_must_deliver_fn_t must_deliver,
                                  void *ctx, int *processed)
{
    esp_err_t ret = ESP_OK;
    int sent = 0;
    bool retried = false;   /* the message at the front has been put back once already */

    while (!message_queue_is_empty(queue)) {
        void *data = NULL;
        int len = 0;

        /* In flight from before the pop: once the message is off the queue, the queue
         * looks empty, and a sender must not take that as its turn */
        __atomic_store_n(&order->in_flight, true, __ATOMIC_SEQ_CST);
        if (message_queue_get_next(queue, &data, &len) != ESP_OK) {
            __atomic_store_n(&order->in_flight, false, __ATOMIC_RELEASE);
            ESP_LOGE(TAG, "Failed to get message from queue");
            ret = ESP_FAIL;
            break;
        }

        apprtc_drain_result_t result = send(data, len, ctx);
        if (result == APPRTC_DRAIN_NOT_CONNECTED) {
            ESP_LOGW(TAG, "Disconnected while sending queued messages; %d left for the next connect",
                     message_queue_get_count(queue) + 1);
            if (message_queue_add_front(queue, data, len) != ESP_OK) {
                free_message(queue, data);
            }
            __atomic_store_n(&order->in_flight, false, __ATOMIC_RELEASE);
            ret = ESP_ERR_INVALID_STATE;
            break;
        }
        if (result == APPRTC_DRAIN_FAILED && !retried && must_deliver != NULL && must_deliver(data, len) &&
            message_queue_add_front(queue, data, len) == ESP_OK) {
            /* Still connected but the send failed: an offer or answer gets one more try
             * before anything behind it goes out */
            ESP_LOGW(TAG, "Send of a queued offer/answer failed; trying it once more");
            retried = true;
            __atomic_store_n(&order->in_flight, false, __ATOMIC_RELEASE);
            continue;
        }
        __atomic_store_n(&order->in_flight, false, __ATOMIC_RELEASE);
        retried = false;
        if (result == APPRTC_DRAIN_FAILED) {
            ESP_LOGE(TAG, "Dropping a queued message after a failed send");
            ret = ESP_FAIL;
        } else {
            sent++;
        }
        free_message(queue, data);
    }

    if (processed != NULL) {
        *processed = sent;
    }
    return ret;
}
