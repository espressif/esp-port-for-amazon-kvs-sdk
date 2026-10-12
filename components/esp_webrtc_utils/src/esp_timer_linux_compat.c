/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * esp_timer timers for the IDF linux target before ESP-IDF 6.2, where esp_timer
 * is headers-only there. Backed by FreeRTOS software timers; esp_timer_get_time()
 * is in flash_wrapper_linux.c. IDF 6.2+ implements esp_timer on linux itself.
 */

#include "esp_idf_version.h"

#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(6, 2, 0)

#include <stdbool.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "esp_err.h"
#include "esp_timer.h"

/* Callbacks always run in the timer daemon task (no ISR dispatch), and only the
 * functions below exist. */

struct esp_timer {
    TimerHandle_t timer;
    esp_timer_cb_t callback;
    void *arg;
    bool armed;       /* our own state: xTimerIsTimerActive() lags until the daemon runs */
    bool periodic;
};

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

/* A timer API call from the daemon itself (a callback re-arming its timer) must not
 * block on the daemon's own command queue. */
static TickType_t block_time(void)
{
    return xTaskGetCurrentTaskHandle() == xTimerGetTimerDaemonTaskHandle() ? 0 : portMAX_DELAY;
}

static void trampoline(TimerHandle_t t)
{
    struct esp_timer *self = (struct esp_timer *) pvTimerGetTimerID(t);
    bool fire;

    taskENTER_CRITICAL(&s_lock);
    fire = self->armed;
    if (!self->periodic) {
        self->armed = false;
    }
    taskEXIT_CRITICAL(&s_lock);
    if (fire) {
        self->callback(self->arg);
    }
}

/* Round up: esp_timer never fires early */
static TickType_t us_to_ticks(uint64_t us)
{
    uint64_t ticks = (us * (uint64_t) configTICK_RATE_HZ + 999999ULL) / 1000000ULL;
    if (ticks == 0) {
        ticks = 1;   /* FreeRTOS rejects a zero period */
    }
    return ticks >= portMAX_DELAY ? portMAX_DELAY - 1 : (TickType_t) ticks;
}

esp_err_t esp_timer_create(const esp_timer_create_args_t *args, esp_timer_handle_t *out_handle)
{
    if (args == NULL || args->callback == NULL || out_handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    struct esp_timer *self = calloc(1, sizeof(*self));
    if (self == NULL) {
        return ESP_ERR_NO_MEM;
    }
    self->callback = args->callback;
    self->arg = args->arg;
    self->timer = xTimerCreate(args->name ? args->name : "esp_timer", 1, pdFALSE, self, trampoline);
    if (self->timer == NULL) {
        free(self);
        return ESP_ERR_NO_MEM;
    }
    *out_handle = self;
    return ESP_OK;
}

static esp_err_t start(esp_timer_handle_t timer, uint64_t period_us, bool periodic)
{
    if (timer == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    taskENTER_CRITICAL(&s_lock);
    bool was_armed = timer->armed;
    if (!was_armed) {
        timer->armed = true;
        timer->periodic = periodic;
    }
    taskEXIT_CRITICAL(&s_lock);
    if (was_armed) {
        return ESP_ERR_INVALID_STATE;   /* same contract as on target */
    }
    vTimerSetReloadMode(timer->timer, periodic ? pdTRUE : pdFALSE);
    /* Changing the period also starts a dormant timer */
    if (xTimerChangePeriod(timer->timer, us_to_ticks(period_us), block_time()) != pdPASS) {
        taskENTER_CRITICAL(&s_lock);
        timer->armed = false;
        taskEXIT_CRITICAL(&s_lock);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t esp_timer_start_once(esp_timer_handle_t timer, uint64_t timeout_us)
{
    return start(timer, timeout_us, false);
}

esp_err_t esp_timer_start_periodic(esp_timer_handle_t timer, uint64_t period)
{
    return start(timer, period, true);
}

esp_err_t esp_timer_stop(esp_timer_handle_t timer)
{
    if (timer == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    taskENTER_CRITICAL(&s_lock);
    bool was_armed = timer->armed;
    timer->armed = false;   /* a pending expiry is dropped by the trampoline */
    taskEXIT_CRITICAL(&s_lock);
    if (!was_armed) {
        return ESP_ERR_INVALID_STATE;
    }
    return xTimerStop(timer->timer, block_time()) == pdPASS ? ESP_OK : ESP_FAIL;
}

static void free_after_delete(void *self, uint32_t unused)
{
    (void) unused;
    free(self);
}

esp_err_t esp_timer_delete(esp_timer_handle_t timer)
{
    if (timer == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (timer->armed) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xTimerDelete(timer->timer, block_time()) != pdPASS) {
        return ESP_FAIL;
    }
    /* Free on the daemon, after it has processed the delete: until then an expiry
     * already queued can still reach the trampoline. */
    if (xTimerPendFunctionCall(free_after_delete, timer, 0, block_time()) != pdPASS) {
        return ESP_FAIL;   /* leaks the small struct rather than risk a use-after-free */
    }
    return ESP_OK;
}

bool esp_timer_is_active(esp_timer_handle_t timer)
{
    return timer != NULL && timer->armed;
}

#endif /* ESP_IDF_VERSION < 6.2 */
