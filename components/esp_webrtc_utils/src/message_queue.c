/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include <stdlib.h>
#include <pthread.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "message_queue.h"

#if CONFIG_IDF_TARGET_LINUX
#include <errno.h>
#include <time.h>
#else
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#endif

static const char *TAG = "message_queue";

/* A pthread mutex on every target: on the chips IDF builds it on a FreeRTOS mutex
 * (priority inheritance included), and on the linux target it is the host's, which the
 * KVS SDK's plain pthreads may take; a FreeRTOS one may not be taken from those.
 *
 * The wake-up for a waiting reader differs. IDF's pthread_cond_timedwait() measures
 * its timeout against the wall clock, which SNTP moves, so on the chips a reader
 * waits on a binary semaphore in ticks, as xQueueReceive() does. The linux target
 * uses a condition variable on the monotonic clock. */
struct message_queue_sync {
    pthread_mutex_t lock;
#if CONFIG_IDF_TARGET_LINUX
    pthread_cond_t not_empty;
#else
    SemaphoreHandle_t wake;     /* given on every add; a reader re-checks the count */
#endif
};

static void sync_lock(message_queue_t *queue)
{
    pthread_mutex_lock(&queue->mutex->lock);
}

static void sync_unlock(message_queue_t *queue)
{
    pthread_mutex_unlock(&queue->mutex->lock);
}

/* Wake one waiting reader: called with the lock held, after a message went in */
static void sync_signal(message_queue_t *queue)
{
#if CONFIG_IDF_TARGET_LINUX
    pthread_cond_signal(&queue->mutex->not_empty);
#else
    xSemaphoreGive(queue->mutex->wake);
#endif
}

static struct message_queue_sync *sync_create(void)
{
    struct message_queue_sync *sync = calloc(1, sizeof(*sync));
    if (sync == NULL) {
        return NULL;
    }
    if (pthread_mutex_init(&sync->lock, NULL) != 0) {
        free(sync);
        return NULL;
    }
#if CONFIG_IDF_TARGET_LINUX
    pthread_condattr_t attr;
    pthread_condattr_init(&attr);
#if !defined(__APPLE__)
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
#endif
    int rc = pthread_cond_init(&sync->not_empty, &attr);
    pthread_condattr_destroy(&attr);
    if (rc != 0) {
        pthread_mutex_destroy(&sync->lock);
        free(sync);
        return NULL;
    }
#else
    sync->wake = xSemaphoreCreateBinary();
    if (sync->wake == NULL) {
        pthread_mutex_destroy(&sync->lock);
        free(sync);
        return NULL;
    }
#endif
    return sync;
}

static void sync_destroy(struct message_queue_sync *sync)
{
#if CONFIG_IDF_TARGET_LINUX
    pthread_cond_destroy(&sync->not_empty);
#else
    vSemaphoreDelete(sync->wake);
#endif
    pthread_mutex_destroy(&sync->lock);
    free(sync);
}

/* Wait until the queue holds a message or @p wait_ms has passed. Called and returns
 * with the lock held; the caller re-checks the count either way. */
static void sync_wait_for_message(message_queue_t *queue, uint32_t wait_ms)
{
    struct message_queue_sync *sync = queue->mutex;

    if (queue->count > 0 || wait_ms == 0) {
        return;
    }
#if CONFIG_IDF_TARGET_LINUX
    if (wait_ms == MESSAGE_QUEUE_WAIT_FOREVER) {
        while (queue->count == 0) {
            pthread_cond_wait(&sync->not_empty, &sync->lock);
        }
        return;
    }
    struct timespec deadline;
#if defined(__APPLE__)
    clock_gettime(CLOCK_REALTIME, &deadline);
#else
    clock_gettime(CLOCK_MONOTONIC, &deadline);
#endif
    uint64_t ns = (uint64_t) deadline.tv_nsec + (uint64_t) wait_ms * 1000000ULL;
    deadline.tv_sec += (time_t) (ns / 1000000000ULL);
    deadline.tv_nsec = (long) (ns % 1000000000ULL);
    /* Loop: a wake-up can be spurious, or another reader can take the message first */
    while (queue->count == 0) {
        if (pthread_cond_timedwait(&sync->not_empty, &sync->lock, &deadline) == ETIMEDOUT) {
            break;
        }
    }
#else
    const bool forever = (wait_ms == MESSAGE_QUEUE_WAIT_FOREVER);
    const TickType_t ticks = forever ? portMAX_DELAY : pdMS_TO_TICKS(wait_ms);
    const TickType_t start = xTaskGetTickCount();
    while (queue->count == 0) {
        TickType_t wait = portMAX_DELAY;
        if (!forever) {
            TickType_t elapsed = xTaskGetTickCount() - start;
            if (elapsed >= ticks) {
                break;
            }
            wait = ticks - elapsed;
        }
        /* A give from an add made while nobody waited may still be pending: then this
         * take returns at once, the count is re-checked and the next take blocks. */
        sync_unlock(queue);
        xSemaphoreTake(sync->wake, wait);
        sync_lock(queue);
    }
#endif
}

esp_err_t message_queue_init(message_queue_t *queue)
{
    const message_queue_config_t config = {
        .depth = MESSAGE_QUEUE_DEFAULT_DEPTH,
        .full_policy = MESSAGE_QUEUE_FULL_DROP_OLDEST,
        .caps = 0,
    };
    return message_queue_init_with_config(queue, &config);
}

esp_err_t message_queue_init_with_config(message_queue_t *queue, const message_queue_config_t *config)
{
    if (queue == NULL || config == NULL || config->depth <= 0) {
        ESP_LOGE(TAG, "Invalid arguments");
        return ESP_ERR_INVALID_ARG;
    }

    // Idempotent: the lock is created once and kept alive for the owner's
    // lifetime (reused across connect/disconnect). A repeat init just purges
    // any leftover messages instead of leaking a second lock.
    if (queue->mutex != NULL) {
        return message_queue_cleanup(queue);
    }

    if (config->caps != 0) {
        queue->messages = heap_caps_calloc(config->depth, sizeof(queued_message_t), config->caps);
    } else {
        queue->messages = calloc(config->depth, sizeof(queued_message_t));
    }
    if (queue->messages == NULL) {
        ESP_LOGE(TAG, "Failed to allocate %d slots", config->depth);
        return ESP_ERR_NO_MEM;
    }

    queue->mutex = sync_create();
    if (queue->mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create mutex");
        free(queue->messages);
        queue->messages = NULL;
        return ESP_ERR_NO_MEM;
    }

    queue->depth = config->depth;
    queue->full_policy = config->full_policy;
    queue->caps = config->caps;
    queue->count = 0;

    ESP_LOGI(TAG, "Message queue initialized");
    return ESP_OK;
}

// Purge queued messages but KEEP the lock alive so the queue can be reused on
// the next connect. Destroying the mutex here (as this used to) left
// queue->mutex dangling/NULL for the next session — the root of the
// "mutex became NULL after initialization" reconnect breakage. Use
// message_queue_deinit() for a true teardown.
esp_err_t message_queue_cleanup(message_queue_t *queue)
{
    if (queue == NULL) {
        ESP_LOGE(TAG, "Queue pointer is NULL");
        return ESP_ERR_INVALID_ARG;
    }
    if (queue->mutex == NULL) {
        queue->count = 0;
        return ESP_OK;
    }

    sync_lock(queue);
    for (int i = 0; i < queue->count; i++) {
        if (queue->messages[i].data != NULL) {
            free(queue->messages[i].data);
            queue->messages[i].data = NULL;
        }
    }
    queue->count = 0;
    sync_unlock(queue);

    ESP_LOGD(TAG, "Message queue purged");
    return ESP_OK;
}

esp_err_t message_queue_deinit(message_queue_t *queue)
{
    if (queue == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    message_queue_cleanup(queue);
    if (queue->mutex != NULL) {
        sync_destroy(queue->mutex);
        queue->mutex = NULL;
    }
    free(queue->messages);      /* heap_caps memory included: free() takes any caps */
    queue->messages = NULL;
    queue->depth = 0;
    return ESP_OK;
}

esp_err_t message_queue_add(message_queue_t *queue, const void *data, int len)
{
    esp_err_t ret = ESP_OK;

    if (queue == NULL || data == NULL || len <= 0) {
        ESP_LOGE(TAG, "Invalid arguments");
        return ESP_ERR_INVALID_ARG;
    }

    // Check if the queue is properly initialized
    if (queue->mutex == NULL) {
        ESP_LOGE(TAG, "Message queue not initialized - mutex is NULL (queue: %p)", queue);
        return ESP_ERR_INVALID_STATE;
    }

    sync_lock(queue);

    // Check if queue is full
    if (queue->count >= queue->depth) {
        if (queue->full_policy == MESSAGE_QUEUE_FULL_REJECT_NEW) {
            /* The caller keeps the message and decides what to do; no log here, the
             * media paths count and rate-limit their own */
            sync_unlock(queue);
            return ESP_ERR_NO_MEM;
        }

        ESP_LOGE(TAG, "Queue is full, dropping oldest message");

        // Free the oldest message
        if (queue->messages[0].data != NULL) {
            free(queue->messages[0].data);
        }

        // Shift all messages down
        for (int i = 0; i < queue->depth - 1; i++) {
            queue->messages[i] = queue->messages[i + 1];
        }

        // Decrement count to make room for new message
        queue->count--;
    }

    // Add to queue. The queue takes ownership of `data` and free()s it on
    // pop / destroy, so cast away the caller-side const — the callee
    // contract is "transfer ownership of a malloc'd buffer".
    int idx = queue->count;
    queue->messages[idx].data = (void *)data;
    queue->messages[idx].len = len;
    queue->count++;

    ESP_LOGD(TAG, "Message added to queue (%d/%d)", queue->count, queue->depth);

    sync_signal(queue);
    sync_unlock(queue);
    return ret;
}

/* Take the oldest message: called with the lock held and the queue not empty */
static void take_head(message_queue_t *queue, void **data, int *len)
{
    // Get the oldest message
    *data = queue->messages[0].data;
    *len = queue->messages[0].len;

    // Shift all messages down
    for (int i = 0; i < queue->count - 1; i++) {
        queue->messages[i] = queue->messages[i + 1];
    }

    // Clear the last entry
    queue->messages[queue->count - 1].data = NULL;
    queue->messages[queue->count - 1].len = 0;

    // Decrement count
    queue->count--;

    ESP_LOGD(TAG, "Message retrieved from queue (%d/%d)", queue->count, queue->depth);
}

esp_err_t message_queue_get_next(message_queue_t *queue, void **data, int *len)
{
    esp_err_t ret = ESP_OK;

    if (queue == NULL || data == NULL || len == NULL) {
        ESP_LOGE(TAG, "Invalid arguments");
        return ESP_ERR_INVALID_ARG;
    }

    *data = NULL;
    *len = 0;

    if (queue->mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    sync_lock(queue);

    // Check if queue is empty
    if (queue->count == 0) {
        ESP_LOGW(TAG, "Queue is empty");
        ret = ESP_ERR_NOT_FOUND;
    } else {
        take_head(queue, data, len);
    }

    sync_unlock(queue);
    return ret;
}

esp_err_t message_queue_get_next_wait(message_queue_t *queue, void **data, int *len, uint32_t wait_ms)
{
    esp_err_t ret = ESP_OK;

    if (queue == NULL || data == NULL || len == NULL) {
        ESP_LOGE(TAG, "Invalid arguments");
        return ESP_ERR_INVALID_ARG;
    }

    *data = NULL;
    *len = 0;

    if (queue->mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    sync_lock(queue);
    sync_wait_for_message(queue, wait_ms);
    if (queue->count == 0) {
        ret = ESP_ERR_TIMEOUT;
    } else {
        take_head(queue, data, len);
        if (queue->count > 0) {
            sync_signal(queue);     /* more left: let another waiting reader in */
        }
    }
    sync_unlock(queue);
    return ret;
}

bool message_queue_is_empty(message_queue_t *queue)
{
    bool empty = true;

    if (queue == NULL) {
        ESP_LOGE(TAG, "Queue pointer is NULL");
        return true; // Consider NULL queue as empty
    }

    // Check if the queue is properly initialized
    if (queue->mutex == NULL) {
        ESP_LOGE(TAG, "Message queue not initialized - mutex is NULL");
        return true; // Consider uninitialized queue as empty
    }

    sync_lock(queue);
    empty = (queue->count == 0);
    sync_unlock(queue);

    return empty;
}

int message_queue_get_count(message_queue_t *queue)
{
    int count = 0;

    if (queue == NULL) {
        ESP_LOGE(TAG, "Queue pointer is NULL");
        return 0;
    }

    // Check if the queue is properly initialized
    if (queue->mutex == NULL) {
        ESP_LOGE(TAG, "Message queue not initialized - mutex is NULL");
        return 0; // Consider uninitialized queue as empty
    }

    sync_lock(queue);
    count = queue->count;
    sync_unlock(queue);

    return count;
}
