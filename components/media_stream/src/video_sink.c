/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @brief Sink registries for raw and encoded video frames.
 *
 * Encoded side: N sinks, dispatched sequentially on the encoder task with a borrowed
 * frame. Raw side: exactly one sink (the H.264 encoder), same borrowing rules.
 *
 * Nothing here queues or copies a frame. That is the whole point - a borrowed frame
 * cannot overflow, so there is no drop policy to get wrong, which matters because
 * encoded frames cannot be dropped individually anyway (every P-frame references the
 * ones before it). The cost is that sinks share the per-frame budget, which is why
 * every callback is timed and slow ones are named.
 */

#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "video_sink_priv.h"

static const char *TAG = "video_sink";

#ifndef CONFIG_VIDEO_SINK_MAX
#define CONFIG_VIDEO_SINK_MAX 4
#endif

#ifndef CONFIG_VIDEO_SINK_SLOW_WARN_US
#define CONFIG_VIDEO_SINK_SLOW_WARN_US 5000
#endif

#define SINK_NAME_LEN 16

struct video_sink_s {
    bool                    in_use;
    char                    name[SINK_NAME_LEN];
    video_sink_encoded_fn_t on_frame;
    void                   *user;

    bool                    enabled;
    /* Armed on enable; frames are withheld until the next keyframe opens it. See
     * video_sink_register() for why this is unconditional. */
    bool                    gate_armed;

    uint32_t                calls;
    uint32_t                gate_held;
    uint32_t                max_us;
    uint64_t                total_us;
    uint32_t                slow_warns;
};

static struct video_sink_s s_sinks[CONFIG_VIDEO_SINK_MAX];
static SemaphoreHandle_t   s_lock;
static uint32_t            s_enabled_count;

/* Spinlock, not s_lock: it needs no init, and dispatch only copies the slot
 * under it before calling out, so an unregister cannot leave it a NULL fn. */
static portMUX_TYPE        s_raw_mux = portMUX_INITIALIZER_UNLOCKED;
static video_raw_sink_t    s_raw_sink;
static bool                s_raw_registered;

/* Total time spent inside sink callbacks since the last read. The grabber folds
 * this into its 1 s enc_fps line so a drop in capture rate can be attributed to
 * the sinks (or exonerated) at a glance, without correlating two logs. */
static uint64_t            s_window_dispatch_us;

/* Recursive so a callback may call back into this API (e.g. a sink disabling itself)
 * without deadlocking against the dispatch that is running it. */
static esp_err_t sink_lock_init(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateRecursiveMutex();
        if (s_lock == NULL) {
            ESP_LOGE(TAG, "Failed to create sink mutex");
            return ESP_ERR_NO_MEM;
        }
    }
    return ESP_OK;
}

static inline void sink_lock(void)   { xSemaphoreTakeRecursive(s_lock, portMAX_DELAY); }
static inline void sink_unlock(void) { xSemaphoreGiveRecursive(s_lock); }

esp_err_t video_sink_register(const video_sink_config_t *cfg, video_sink_handle_t *out)
{
    if (cfg == NULL || cfg->on_frame == NULL || cfg->name == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = sink_lock_init();
    if (err != ESP_OK) {
        return err;
    }

    *out = NULL;
    sink_lock();
    for (int i = 0; i < CONFIG_VIDEO_SINK_MAX; i++) {
        struct video_sink_s *s = &s_sinks[i];
        if (s->in_use) {
            continue;
        }
        memset(s, 0, sizeof(*s));
        s->in_use = true;
        strlcpy(s->name, cfg->name, sizeof(s->name));
        s->on_frame = cfg->on_frame;
        s->user = cfg->user;
        s->enabled = false;
        *out = s;
        sink_unlock();
        ESP_LOGI(TAG, "Registered sink '%s' (slot %d)", s->name, i);
        return ESP_OK;
    }
    sink_unlock();
    ESP_LOGE(TAG, "No free sink slot for '%s' (max %d, raise CONFIG_VIDEO_SINK_MAX)",
             cfg->name, CONFIG_VIDEO_SINK_MAX);
    return ESP_ERR_NOT_FOUND;
}

esp_err_t video_sink_unregister(video_sink_handle_t sink)
{
    if (sink == NULL || s_lock == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    sink_lock();
    if (!sink->in_use) {
        sink_unlock();
        return ESP_ERR_INVALID_ARG;
    }
    if (sink->enabled && s_enabled_count > 0) {
        s_enabled_count--;
    }
    ESP_LOGI(TAG, "Unregistered sink '%s' (calls=%" PRIu32 " max_us=%" PRIu32 ")",
             sink->name, sink->calls, sink->max_us);
    memset(sink, 0, sizeof(*sink));
    sink_unlock();
    return ESP_OK;
}

esp_err_t video_sink_set_enabled(video_sink_handle_t sink, bool enabled)
{
    if (sink == NULL || s_lock == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    sink_lock();
    if (!sink->in_use) {
        sink_unlock();
        return ESP_ERR_INVALID_ARG;
    }
    if (sink->enabled != enabled) {
        sink->enabled = enabled;
        if (enabled) {
            s_enabled_count++;
            /* Re-arm: a sink that comes back mid-GOP must not resume on a P-frame
             * whose references it never saw. */
            sink->gate_armed = true;
        } else if (s_enabled_count > 0) {
            s_enabled_count--;
        }
        ESP_LOGI(TAG, "Sink '%s' %s (%" PRIu32 " enabled)", sink->name,
                 enabled ? "enabled, waiting for keyframe" : "disabled", s_enabled_count);
    }
    sink_unlock();
    return ESP_OK;
}

bool video_sink_any_enabled(void)
{
    return s_enabled_count > 0;
}

uint32_t video_sink_take_window_us(void)
{
    uint64_t v = s_window_dispatch_us;
    s_window_dispatch_us = 0;
    return (uint32_t)v;
}

void video_sink_dispatch(const video_frame_t *frame)
{
    if (frame == NULL || s_lock == NULL || s_enabled_count == 0) {
        return;
    }

    sink_lock();
    for (int i = 0; i < CONFIG_VIDEO_SINK_MAX; i++) {
        struct video_sink_s *s = &s_sinks[i];
        if (!s->in_use || !s->enabled) {
            continue;
        }
        if (s->gate_armed) {
            if (frame->type != VIDEO_FRAME_TYPE_I) {
                s->gate_held++;
                continue;
            }
            s->gate_armed = false;
            ESP_LOGI(TAG, "Sink '%s' opened on keyframe (held %" PRIu32 " frames)",
                     s->name, s->gate_held);
        }

        uint64_t t0 = esp_timer_get_time();
        s->on_frame(frame, s->user);
        uint32_t dt = (uint32_t)(esp_timer_get_time() - t0);

        s_window_dispatch_us += dt;
        /* The callback may have unregistered itself; don't write into a free slot. */
        if (!s->in_use) {
            continue;
        }
        s->calls++;
        s->total_us += dt;
        if (dt > s->max_us) {
            s->max_us = dt;
        }
        if (dt > CONFIG_VIDEO_SINK_SLOW_WARN_US) {
            /* Rate limited the same way the grabber's own warnings are: first
             * occurrence then every 32nd, so a persistently slow sink stays visible
             * without drowning the log. */
            s->slow_warns++;
            if (s->slow_warns == 1 || (s->slow_warns & 0x1F) == 0) {
                ESP_LOGW(TAG, "Sink '%s' callback took %" PRIu32 " us (count=%" PRIu32
                         ") - it is eating the frame budget", s->name, dt, s->slow_warns);
            }
        }
    }
    sink_unlock();
}

esp_err_t video_sink_get_stats(video_sink_stats_t *out, size_t max, size_t *count)
{
    if (out == NULL || count == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *count = 0;
    if (s_lock == NULL) {
        return ESP_OK;
    }
    sink_lock();
    for (int i = 0; i < CONFIG_VIDEO_SINK_MAX && *count < max; i++) {
        struct video_sink_s *s = &s_sinks[i];
        if (!s->in_use) {
            continue;
        }
        video_sink_stats_t *o = &out[(*count)++];
        o->name      = s->name;
        o->enabled   = s->enabled;
        o->calls     = s->calls;
        o->gate_held = s->gate_held;
        o->max_us    = s->max_us;
        o->avg_us    = s->calls ? (uint32_t)(s->total_us / s->calls) : 0;
    }
    sink_unlock();
    return ESP_OK;
}

void video_sink_reset_stats(void)
{
    if (s_lock == NULL) {
        return;
    }
    sink_lock();
    for (int i = 0; i < CONFIG_VIDEO_SINK_MAX; i++) {
        struct video_sink_s *s = &s_sinks[i];
        if (!s->in_use) {
            continue;
        }
        s->calls = s->gate_held = s->max_us = s->slow_warns = 0;
        s->total_us = 0;
    }
    sink_unlock();
}

esp_err_t video_raw_sink_register(const video_raw_sink_t *sink)
{
    if (sink == NULL || sink->on_frame == NULL || sink->name == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const char *holder = NULL;
    taskENTER_CRITICAL(&s_raw_mux);
    if (s_raw_registered) {
        holder = s_raw_sink.name;
    } else {
        s_raw_sink = *sink;
        s_raw_registered = true;
    }
    taskEXIT_CRITICAL(&s_raw_mux);
    if (holder != NULL) {
        ESP_LOGE(TAG, "Raw sink '%s' rejected: '%s' already holds the single slot",
                 sink->name, holder);
        return ESP_ERR_NOT_SUPPORTED;
    }
    ESP_LOGI(TAG, "Registered raw sink '%s'", sink->name);
    return ESP_OK;
}

esp_err_t video_raw_sink_unregister(void)
{
    taskENTER_CRITICAL(&s_raw_mux);
    s_raw_registered = false;
    memset(&s_raw_sink, 0, sizeof(s_raw_sink));
    taskEXIT_CRITICAL(&s_raw_mux);
    return ESP_OK;
}

bool video_raw_sink_is_registered(void)
{
    return s_raw_registered;
}

esp_err_t video_raw_sink_dispatch(const video_frame_raw_t *frame)
{
    if (frame == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    taskENTER_CRITICAL(&s_raw_mux);
    esp_err_t (*on_frame)(const video_frame_raw_t *, void *) = s_raw_registered ? s_raw_sink.on_frame : NULL;
    void *user = s_raw_sink.user;
    taskEXIT_CRITICAL(&s_raw_mux);
    if (on_frame == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return on_frame(frame, user);
}
