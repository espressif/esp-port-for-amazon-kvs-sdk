/*
 * SPDX-FileCopyrightText: 2024-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <esp_err.h>
#include <esp_log.h>

#include <esp_work_queue.h>
#include "sdkconfig.h"
#if CONFIG_IDF_TARGET_LINUX
#include <pthread.h>
#include <stdlib.h>
#include <time.h>
#endif

static const char *TAG = "esp_work_queue";

typedef enum {
    WORK_QUEUE_STATE_DEINIT = 0,
    WORK_QUEUE_STATE_INIT_DONE,
    WORK_QUEUE_STATE_RUNNING,
    WORK_QUEUE_STATE_STOP_REQUESTED,
} esp_work_queue_state_t;

typedef struct {
    esp_work_fn_t work_fn;
    void *priv_data;
} esp_work_queue_entry_t;

static esp_work_queue_state_t queue_state;
static esp_work_queue_config_t queue_config;

#if CONFIG_IDF_TARGET_LINUX
/* On the IDF linux target the SDK's own threads (ICE connection listener, media
 * senders, the libwebsockets service thread) are plain pthreads, and the app event
 * callbacks they raise post here. A FreeRTOS queue call from a thread that is not a
 * task corrupts the POSIX port: it re-enables the tick signal on that thread, which
 * the port then parks as if it were a task. So the host queue is a pthread-mutex
 * ring, safe to post from any thread, and the worker task polls it. */
static pthread_mutex_t s_wq_lock = PTHREAD_MUTEX_INITIALIZER;
static esp_work_queue_entry_t *s_wq_items;
static uint32_t s_wq_head, s_wq_count;
static bool s_wq_created;
#define WORK_QUEUE_READY() (s_wq_created)

static bool wq_create(uint32_t size)
{
    s_wq_items = calloc(size, sizeof(*s_wq_items));
    s_wq_head = s_wq_count = 0;
    s_wq_created = (s_wq_items != NULL);
    return s_wq_created;
}

static void wq_delete(void)
{
    pthread_mutex_lock(&s_wq_lock);
    s_wq_created = false;
    free(s_wq_items);
    s_wq_items = NULL;
    s_wq_count = 0;
    pthread_mutex_unlock(&s_wq_lock);
}

static bool wq_try_send(const esp_work_queue_entry_t *e)
{
    bool ok = false;
    pthread_mutex_lock(&s_wq_lock);
    if (s_wq_created && s_wq_count < queue_config.size) {
        s_wq_items[(s_wq_head + s_wq_count) % queue_config.size] = *e;
        s_wq_count++;
        ok = true;
    }
    pthread_mutex_unlock(&s_wq_lock);
    return ok;
}

static bool wq_send(const esp_work_queue_entry_t *e, uint32_t timeout_ms)
{
    /* Host sleep, not vTaskDelay: the caller may be a plain pthread */
    const struct timespec one_ms = { .tv_sec = 0, .tv_nsec = 1000 * 1000 };
    for (uint32_t waited = 0;; ++waited) {
        if (wq_try_send(e)) {
            return true;
        }
        if (waited >= timeout_ms) {
            return false;
        }
        nanosleep(&one_ms, NULL);
    }
}

static bool wq_receive(esp_work_queue_entry_t *e)
{
    bool ok = false;
    pthread_mutex_lock(&s_wq_lock);
    if (s_wq_created && s_wq_count > 0) {
        *e = s_wq_items[s_wq_head];
        s_wq_head = (s_wq_head + 1) % queue_config.size;
        s_wq_count--;
        ok = true;
    }
    pthread_mutex_unlock(&s_wq_lock);
    return ok;
}

static uint32_t wq_pending(void)
{
    pthread_mutex_lock(&s_wq_lock);
    uint32_t n = s_wq_count;
    pthread_mutex_unlock(&s_wq_lock);
    return n;
}

static void esp_webrtc_handle_work_queue(void)
{
    esp_work_queue_entry_t work_queue_entry;
    if (!wq_receive(&work_queue_entry)) {
        vTaskDelay(pdMS_TO_TICKS(10));   /* poll: posters cannot wake a task from a pthread */
        return;
    }
    do {
        work_queue_entry.work_fn(work_queue_entry.priv_data);
        vTaskDelay(pdMS_TO_TICKS(10)); // Yield to avoid starvation to other tasks
    } while (wq_receive(&work_queue_entry));
}
#else
static QueueHandle_t work_queue;
#define WORK_QUEUE_READY() (work_queue != NULL)

static void esp_webrtc_handle_work_queue(void)
{
    esp_work_queue_entry_t work_queue_entry;
    /* 2 sec delay to prevent spinning */
    BaseType_t ret = xQueueReceive(work_queue, &work_queue_entry, 2000 / portTICK_PERIOD_MS);
    while (ret == pdTRUE) {
        work_queue_entry.work_fn(work_queue_entry.priv_data);
        vTaskDelay(pdMS_TO_TICKS(10)); // Yield to avoid starvation to other tasks
        ret = xQueueReceive(work_queue, &work_queue_entry, 0);
    }
}
#endif

/* The worker task, for esp_work_queue_is_worker(); NULL while none runs */
static TaskHandle_t s_worker_task;

bool esp_work_queue_is_worker(void)
{
    return s_worker_task != NULL && xTaskGetCurrentTaskHandle() == s_worker_task;
}

static void esp_work_queue_task(void *param)
{
    s_worker_task = xTaskGetCurrentTaskHandle();
    ESP_LOGI(TAG, "ESP Work Queue task started.");
    while (queue_state != WORK_QUEUE_STATE_STOP_REQUESTED) {
        esp_webrtc_handle_work_queue();
    }
    ESP_LOGI(TAG, "Stopping Work Queue task");
    s_worker_task = NULL;
    queue_state = WORK_QUEUE_STATE_INIT_DONE;
    vTaskDelete(NULL);
}

esp_err_t esp_work_queue_add_task_timeout(esp_work_fn_t work_fn, void *priv_data, uint32_t timeout_ms)
{
    if (!WORK_QUEUE_READY()) {
        ESP_LOGE(TAG, "Cannot enqueue function as Work Queue hasn't been created.");
        return ESP_ERR_INVALID_STATE;
    }
    esp_work_queue_entry_t work_queue_entry = {
        .work_fn = work_fn,
        .priv_data = priv_data,
    };

#if CONFIG_IDF_TARGET_LINUX
    if (wq_send(&work_queue_entry, timeout_ms)) {
        return ESP_OK;
    }
    unsigned pending = (unsigned) wq_pending();
#else
    if (xQueueSend(work_queue, &work_queue_entry, pdMS_TO_TICKS(timeout_ms)) == pdTRUE) {
        return ESP_OK;
    }
    unsigned pending = (unsigned) uxQueueMessagesWaiting(work_queue);
#endif

    ESP_LOGW(TAG, "Failed to add work task (queue full: %u/%u pending, waited %u ms)",
             pending, (unsigned) queue_config.size, (unsigned) timeout_ms);
    return ESP_FAIL;
}

esp_err_t esp_work_queue_add_task(esp_work_fn_t work_fn, void *priv_data)
{
    /* Non-blocking by design: most callers are on paths that must not stall. */
    return esp_work_queue_add_task_timeout(work_fn, priv_data, 0);
}

/* Barrier task: signals the caller once the single FIFO worker reaches it,
 * i.e. after every task queued before it has run. */
static void esp_work_queue_barrier(void *arg)
{
    xSemaphoreGive((SemaphoreHandle_t) arg);
}

esp_err_t esp_work_queue_sync(uint32_t timeout_ms)
{
    if (!WORK_QUEUE_READY()) {
        return ESP_ERR_INVALID_STATE;
    }

    SemaphoreHandle_t done = xSemaphoreCreateBinary();
    if (done == NULL) {
        return ESP_ERR_NO_MEM;
    }

    /* A full queue is likely when this is needed most (a burst of ICE candidates at
     * teardown), so wait for room for the barrier within the same budget */
    TickType_t start = xTaskGetTickCount();
    esp_err_t err;
    do {
        err = esp_work_queue_add_task_timeout(esp_work_queue_barrier, done, timeout_ms ? timeout_ms : 1000);
    } while (err == ESP_FAIL && timeout_ms == 0);
    if (err != ESP_OK) {
        vSemaphoreDelete(done);
        return (err == ESP_FAIL) ? ESP_ERR_TIMEOUT : err;
    }

    TickType_t wait = portMAX_DELAY;
    if (timeout_ms != 0) {
        TickType_t budget = pdMS_TO_TICKS(timeout_ms), used = xTaskGetTickCount() - start;
        wait = (used < budget) ? budget - used : 0;
    }
    if (xSemaphoreTake(done, wait) != pdTRUE) {
        /* Barrier may still fire later and give a deleted semaphore, so leak it
         * (rare, small) rather than risk a use-after-free on the handle. */
        ESP_LOGW(TAG, "esp_work_queue_sync timed out after %u ms", (unsigned) timeout_ms);
        return ESP_ERR_TIMEOUT;
    }

    vSemaphoreDelete(done);
    return ESP_OK;
}

esp_err_t esp_work_queue_init(void)
{
    esp_work_queue_config_t config = ESP_WORK_QUEUE_CONFIG_DEFAULT();
    return esp_work_queue_init_with_config(&config);
}

esp_err_t esp_work_queue_init_with_config(esp_work_queue_config_t *config)
{
    /* Bail before copying: a later caller must not overwrite the config of an
     * already-running task (split mode inits this early with a smaller stack). */
    if (queue_state != WORK_QUEUE_STATE_DEINIT) {
        ESP_LOGW(TAG, "Work Queue already initialiased/started.");
        return ESP_OK;
    }
    queue_config = *config; // Copy the config
#if CONFIG_IDF_TARGET_LINUX
    if (!wq_create(queue_config.size)) {
#else
    work_queue = xQueueCreate(queue_config.size, sizeof(esp_work_queue_entry_t));
    if (!work_queue) {
#endif
        ESP_LOGE(TAG, "Failed to create Work Queue.");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Work Queue created.");
    queue_state = WORK_QUEUE_STATE_INIT_DONE;
    return ESP_OK;
}

esp_err_t esp_work_queue_deinit(void)
{
    if (queue_state != WORK_QUEUE_STATE_STOP_REQUESTED) {
        esp_work_queue_stop();
    }

    while (queue_state == WORK_QUEUE_STATE_STOP_REQUESTED) {
        ESP_LOGI(TAG, "Waiting for esp_work_queue being stopped...");
        vTaskDelay(2000 / portTICK_PERIOD_MS);
    }

    if (queue_state == WORK_QUEUE_STATE_DEINIT) {
        return ESP_OK;
    } else if (queue_state != WORK_QUEUE_STATE_INIT_DONE) {
        ESP_LOGE(TAG, "Cannot deinitialize Work Queue as the task is still running.");
        return ESP_ERR_INVALID_STATE;
    } else {
#if CONFIG_IDF_TARGET_LINUX
        wq_delete();
#else
        vQueueDelete(work_queue);
        work_queue = NULL;
#endif
        queue_state = WORK_QUEUE_STATE_DEINIT;
    }
    ESP_LOGI(TAG, "esp_work_queue was successfully deinitialized");
    return ESP_OK;
}

esp_err_t esp_work_queue_start(void)
{
    StaticTask_t *task_buffer = NULL;
    void *task_stack = NULL;

    if (queue_state == WORK_QUEUE_STATE_RUNNING) {
        ESP_LOGW(TAG, "Work Queue already started.");
        return ESP_OK;
    }
    if (queue_state != WORK_QUEUE_STATE_INIT_DONE) {
        ESP_LOGE(TAG, "Failed to start Work Queue as it wasn't initialized.");
        return ESP_ERR_INVALID_STATE;
    }

    /* Create the task with the appropriate stack and buffer */
    esp_err_t ret = ESP_OK;
#if CONFIG_IDF_TARGET_LINUX
    /* The FreeRTOS POSIX/linux port maps each task onto a pthread and has no
     * SPIRAM, so the static-stack + heap_caps(SPIRAM) path below crashes in
     * prvInitialiseNewTask. Always use the dynamic task on the linux target. */
    (void) task_buffer;
    (void) task_stack;
    ret = xTaskCreate(&esp_work_queue_task, "esp_workq_task", queue_config.stack_size,
                      NULL, queue_config.priority, NULL) != pdPASS ? ESP_FAIL : ESP_OK;
#else
    if (queue_config.prefer_ext_ram) {
        task_buffer = heap_caps_calloc(1, sizeof(StaticTask_t), MALLOC_CAP_INTERNAL);
        task_stack = heap_caps_malloc_prefer(queue_config.stack_size, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_INTERNAL);
        assert(task_buffer && task_stack);

        /* the task never exits, so do not bother to free the buffers */
        ret = xTaskCreateStatic(&esp_work_queue_task, "esp_workq_task", queue_config.stack_size,
                                NULL, queue_config.priority, task_stack, task_buffer) == NULL ? ESP_FAIL : ESP_OK;
    } else {
        ret = xTaskCreate(&esp_work_queue_task, "esp_workq_task", queue_config.stack_size,
                          NULL, queue_config.priority, NULL) != pdPASS ? ESP_FAIL : ESP_OK;
    }
#endif


    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create Work Queue task.");
        goto cleanup;
    }

    queue_state = WORK_QUEUE_STATE_RUNNING;
    return ESP_OK;

cleanup:

    if (task_stack) {
        heap_caps_free(task_stack);
    }
    if (task_buffer) {
        heap_caps_free(task_buffer);
    }
    return ret;
}

esp_err_t esp_work_queue_stop(void)
{
    if (queue_state == WORK_QUEUE_STATE_RUNNING) {
        queue_state = WORK_QUEUE_STATE_STOP_REQUESTED;
    }
    return ESP_OK;
}
