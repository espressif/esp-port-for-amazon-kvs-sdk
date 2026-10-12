/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file message_queue.h
 * @brief Bounded FIFO of owned messages, shared by AppRTC signaling and the media paths
 *
 * Each message is a pointer and a length; the queue takes ownership of the buffer on
 * add and hands it back on get. Every call is safe from FreeRTOS tasks and from plain
 * pthreads, including the KVS SDK's threads on the linux target. Not for ISRs.
 *
 * Each owner picks its depth and what happens when the queue is full. A wait for a
 * message is timed like xQueueReceive(): in ticks on the chips, on the monotonic
 * clock on the linux target, so it is not cut short or stretched by a wall-clock
 * change (SNTP).
 */

#ifndef MESSAGE_QUEUE_H
#define MESSAGE_QUEUE_H

#include <stdint.h>
#include <stdbool.h>
#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Depth of a queue set up with message_queue_init() */
#define MESSAGE_QUEUE_DEFAULT_DEPTH 10

/* wait_ms for message_queue_get_next_wait(): block until a message arrives */
#define MESSAGE_QUEUE_WAIT_FOREVER UINT32_MAX

/* What an add does when the queue is full */
typedef enum {
    MESSAGE_QUEUE_FULL_DROP_OLDEST,     /* discard the oldest message to make room */
    MESSAGE_QUEUE_FULL_REJECT_NEW,      /* refuse the new one; the queue is left as is */
} message_queue_full_policy_t;

typedef struct {
    int depth;                                  /* messages held at most, > 0 */
    message_queue_full_policy_t full_policy;
    uint32_t caps;                              /* heap caps for the slot array; 0 = default heap */
} message_queue_config_t;

// Structure to hold a queued message
typedef struct {
    void *data;
    int len;
} queued_message_t;

struct message_queue_sync;

// Message queue structure
typedef struct {
    queued_message_t *messages;                 /* depth slots, allocated once by init */
    int depth;
    int count;
    message_queue_full_policy_t full_policy;
    uint32_t caps;
    /* Lock and wake-up; NULL until initialised, so an owner can test it */
    struct message_queue_sync *mutex;
    /* Optional, set by the owner after init. droppable: which item may be shed when
     * the queue is full (the oldest such one goes; the oldest item when none is).
     * free_item: how to free an item the queue discards (default: free()). */
    bool (*droppable)(const void *data, int len);
    void (*free_item)(void *data);
} message_queue_t;

/**
 * @brief Initialize a message queue with the defaults
 *
 * MESSAGE_QUEUE_DEFAULT_DEPTH messages, the oldest dropped when full, default heap.
 *
 * The struct must be zeroed before the first init (static storage, memset or = {0}):
 * init takes a non-NULL mutex to mean the queue is already set up.
 *
 * @param queue Pointer to the message queue to initialize
 * @return esp_err_t ESP_OK on success, error code otherwise
 */
esp_err_t message_queue_init(message_queue_t *queue);

/**
 * @brief Initialize a message queue with its own depth and full policy
 *
 * Idempotent like message_queue_init(): on a queue that is already initialised it only
 * purges the queued messages and keeps the first configuration. As there, the struct
 * must be zeroed before the first init.
 *
 * @param queue Pointer to the message queue to initialize
 * @param config Depth, full policy and heap caps
 * @return esp_err_t ESP_OK on success, ESP_ERR_INVALID_ARG or ESP_ERR_NO_MEM otherwise
 */
esp_err_t message_queue_init_with_config(message_queue_t *queue, const message_queue_config_t *config);

/**
 * @brief Purge all queued messages but keep the queue alive for reuse.
 *
 * Safe to call across connect/disconnect cycles; the lock and slots are created once
 * by message_queue_init() and persist until message_queue_deinit().
 *
 * @param queue Pointer to the message queue to purge
 * @return esp_err_t ESP_OK on success, error code otherwise
 */
esp_err_t message_queue_cleanup(message_queue_t *queue);

/**
 * @brief Fully tear down a message queue (purge + free the lock and slots).
 *
 * Only for final teardown of the owner, with nobody waiting on the queue; after this
 * the queue must be initialised again before use.
 *
 * @param queue Pointer to the message queue to destroy
 * @return esp_err_t ESP_OK on success, error code otherwise
 */
esp_err_t message_queue_deinit(message_queue_t *queue);

/**
 * @brief Add a message to the queue
 *
 * Never waits for room. When the queue is full, MESSAGE_QUEUE_FULL_DROP_OLDEST frees the
 * oldest droppable message (the oldest one when none is) and adds this one;
 * MESSAGE_QUEUE_FULL_REJECT_NEW returns ESP_ERR_NO_MEM and the caller keeps @p data.
 *
 * @param queue Pointer to the message queue
 * @param data Pointer to the message data (the queue takes ownership on ESP_OK)
 * @param len Length of the message data
 * @return esp_err_t ESP_OK on success, ESP_ERR_NO_MEM when full and rejected, error code otherwise
 */
esp_err_t message_queue_add(message_queue_t *queue, const void *data, int len);

/**
 * @brief Put a message back at the head of the queue
 *
 * For a message taken with message_queue_get_next() that could not be sent: it goes
 * out first next time. Makes room the same way as message_queue_add().
 *
 * @param queue Pointer to the message queue
 * @param data Pointer to the message data (the queue takes ownership on ESP_OK)
 * @param len Length of the message data
 * @return esp_err_t ESP_OK on success, ESP_ERR_NO_MEM when full and rejected, error code otherwise
 */
esp_err_t message_queue_add_front(message_queue_t *queue, const void *data, int len);

/**
 * @brief Get the next message from the queue without waiting
 *
 * @param queue Pointer to the message queue
 * @param data Pointer to store the message data (caller must free)
 * @param len Pointer to store the message length
 * @return esp_err_t ESP_OK on success, ESP_ERR_NOT_FOUND when empty, error code otherwise
 */
esp_err_t message_queue_get_next(message_queue_t *queue, void **data, int *len);

/**
 * @brief Get the next message, waiting up to @p wait_ms for one
 *
 * Like xQueueReceive(): 0 polls, MESSAGE_QUEUE_WAIT_FOREVER blocks. Quiet when the
 * queue is empty, for owners that poll it on a timer.
 *
 * @param queue Pointer to the message queue
 * @param data Pointer to store the message data (caller must free)
 * @param len Pointer to store the message length
 * @param wait_ms Longest wait for a message
 * @return esp_err_t ESP_OK on success, ESP_ERR_TIMEOUT when none arrived, error code otherwise
 */
esp_err_t message_queue_get_next_wait(message_queue_t *queue, void **data, int *len, uint32_t wait_ms);

/**
 * @brief Check if the queue is empty
 *
 * @param queue Pointer to the message queue
 * @return true if the queue is empty, false otherwise
 */
bool message_queue_is_empty(message_queue_t *queue);

/**
 * @brief Get the number of messages in the queue
 *
 * @param queue Pointer to the message queue
 * @return int Number of messages in the queue
 */
int message_queue_get_count(message_queue_t *queue);

#ifdef __cplusplus
}
#endif

#endif /* MESSAGE_QUEUE_H */
