/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "video_raw_sink.h"
#include "detect_sink.h"

static const char *TAG = "detect_sink";

/* The model's native input. Feeding it exactly this avoids esp-dl's internal resize, and the
 * converter has already cropped to square so nothing is distorted getting here. */
#define DETECT_INPUT_W  224
#define DETECT_INPUT_H  224

/* Big: esp-dl runs the whole network on this stack. 4 KB (the bus default) is nowhere near it. */
#define DETECT_TASK_STACK  (32 * 1024)
#define DETECT_TASK_PRIO   3

static video_raw_sink_handle_t s_sink;
static TaskHandle_t            s_task;
static StaticTask_t           *s_task_tcb;
static void                   *s_task_stack;
static volatile bool           s_run;

static SemaphoreHandle_t s_lock;
static detect_results_t  s_results;
static bool              s_have_results;

/* PULL mode: acquire, infer, release, repeat. The bus offers a push mode that would run this
 * callback on a task it owns, but a consumer with a model loaded and a 32 KB stack is exactly the
 * case its header says to use pull for - a callback would just mean copying the frame into a queue
 * of our own, which is the copy this whole design exists to avoid. */
static void detect_task(void *arg)
{
    (void)arg;
    person_box_t boxes[PERSON_DETECT_MAX_BOXES];

    while (s_run) {
        video_raw_frame_t frame;
        if (video_raw_sink_acquire(s_sink, &frame, 200) != ESP_OK) {
            continue;   /* Timed out: sink disabled, or the camera is not running yet. */
        }

        /* Already a 224x224 RGB565 copy: the bus converted it on THIS task and handed the
         * camera buffer back before acquire() returned, so nothing the model does below is
         * on the camera's clock. */
        const uint64_t t0 = esp_timer_get_time();
        const int n = person_detect_run(frame.buffer, frame.width, frame.height,
                                        boxes, PERSON_DETECT_MAX_BOXES);
        const uint32_t dt = (uint32_t)(esp_timer_get_time() - t0);

        /* Release once inference is done: this returns the converter's pool buffer, not a
         * camera buffer - that one went back before this frame was handed over. */
        video_raw_sink_release(s_sink, &frame);

        if (n < 0) {
            continue;
        }

        xSemaphoreTake(s_lock, portMAX_DELAY);
        memcpy(s_results.boxes, boxes, sizeof(person_box_t) * (size_t)n);
        s_results.count      = n;
        /* The converted geometry, not the camera's: the boxes the model returns are in the
         * converted frame's coordinates, and preview_display.c scales them by this. */
        s_results.src_width  = frame.width;
        s_results.src_height = frame.height;
        s_results.seq        = frame.seq;
        s_results.infer_us   = dt;
        s_results.inferences++;
        s_have_results       = true;
        xSemaphoreGive(s_lock);
    }
    vTaskDelete(NULL);
}

esp_err_t detect_sink_init(void)
{
    if (s_sink != NULL) {
        return ESP_OK;
    }

    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = person_detect_init(CONFIG_PERSON_DETECT_SCORE_THRESHOLD / 100.0f);
    if (err != ESP_OK) {
        return err;
    }

    const video_raw_sink_config_t cfg = {
        .name          = "detect",
        /* CONVERTED: the bus runs the PPA pass on this sink's own task and releases the
         * camera buffer before handing the frame over, so a 90 ms inference costs the
         * camera nothing. */
        .mode          = VIDEO_RAW_MODE_CONVERTED,
        /* PULL mode - see the comment on detect_task(). */
        .on_frame      = NULL,
        /* No staging: one frame at a time, so the claim is a single camera buffer - and it
         * is held only across the conversion, never across inference. Staging a second one
         * would buy nothing here, because a detector this slow wants the newest frame. */
        .queue_depth   = 0,
        /* Latest-frame-wins. A detector running at a few fps against a 30 fps camera wants the
         * newest frame, not a queued one from half a second ago. */
        .overflow      = VIDEO_RAW_OVERFLOW_DROP_OLD,
        .fps_limit     = CONFIG_PERSON_DETECT_FPS,
        .want_fourcc   = VIDEO_FOURCC_RGB565,
        .want_width    = DETECT_INPUT_W,
        .want_height   = DETECT_INPUT_H,
    };
    err = video_raw_sink_register(&cfg, &s_sink);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to register the detect sink: %s", esp_err_to_name(err));
        return err;
    }

    /* Stack in SPIRAM, TCB in internal.
     *
     * NOT plain xTaskCreate(): CONFIG_SPIRAM_USE_CAPS_ALLOC=y means malloc serves internal RAM
     * only, so a 32 KB task stack would come straight out of the same small heap that libsrtp,
     * lwip and the esp_hosted SDIO mempool are competing for. The TCB stays internal because the
     * scheduler tick touches it. Same split the bus uses for its own sink tasks. */
    s_task_tcb   = heap_caps_calloc(1, sizeof(StaticTask_t), MALLOC_CAP_INTERNAL);
    s_task_stack = heap_caps_calloc(1, DETECT_TASK_STACK, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_task_tcb == NULL || s_task_stack == NULL) {
        ESP_LOGE(TAG, "out of memory for the inference task");
        goto fail;
    }
    s_run = true;
    s_task = xTaskCreateStatic(detect_task, "detect", DETECT_TASK_STACK, NULL,
                               DETECT_TASK_PRIO, s_task_stack, s_task_tcb);
    if (s_task == NULL) {
        s_run = false;
        goto fail;
    }

#if CONFIG_PERSON_DETECT_ENABLE_AT_BOOT
    video_raw_sink_set_enabled(s_sink, true);
#endif
    ESP_LOGI(TAG, "inference sink ready: %dx%d RGB565 at up to %d fps",
             DETECT_INPUT_W, DETECT_INPUT_H, CONFIG_PERSON_DETECT_FPS);
    return ESP_OK;

fail:
    if (s_task_stack) { heap_caps_free(s_task_stack); s_task_stack = NULL; }
    if (s_task_tcb)   { heap_caps_free(s_task_tcb);   s_task_tcb   = NULL; }
    video_raw_sink_unregister(s_sink);
    s_sink = NULL;
    return ESP_ERR_NO_MEM;
}

esp_err_t detect_sink_set_enabled(bool enabled)
{
    if (s_sink == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return video_raw_sink_set_enabled(s_sink, enabled);
}

bool detect_sink_is_enabled(void)
{
    if (s_sink == NULL) {
        return false;
    }
    video_raw_sink_stats_t st[4];
    size_t n = 0;
    if (video_raw_sink_get_stats(st, 4, &n) != ESP_OK) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if (strcmp(st[i].name, "detect") == 0) {
            return st[i].enabled;
        }
    }
    return false;
}

esp_err_t detect_sink_get_results(detect_results_t *out)
{
    if (out == NULL || s_lock == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool have = s_have_results;
    if (have) {
        *out = s_results;
    }
    xSemaphoreGive(s_lock);
    return have ? ESP_OK : ESP_ERR_INVALID_STATE;
}
