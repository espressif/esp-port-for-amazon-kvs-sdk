/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @brief Raw-frame bus: one producer, N parallel consumers, camera buffers shared by
 *        reference.
 *
 * The pump publishes a camera frame and moves on. Each sink either runs inline (on the
 * pump, borrowed) or gets a descriptor in its own bounded queue and works on its own
 * task. The buffer goes back to the driver when the last holder releases it.
 *
 * Two invariants carry the whole design
 *
 * 1. Retirement is unordered. Frame 7 may be released before frame 5, and with eviction
 *    that is the normal case rather than an edge case.
 *
 * 2. The pump never blocks while holding a lock. VIDEO_RAW_OVERFLOW_BLOCK lets a sink
 *    stall the pump on purpose if you need so
 *
 * Frame data is never copied here. What travels through a sink's queue is the ~40-byte
 * descriptor, by value, so fan-out costs the same whether the frame is 100 KiB or 3 MiB.
 */

#include "sdkconfig.h"
#include "media_stream_caps.h"

/* Camera buffers come from esp_video_if.c, so this follows the same capability as the
 * rest of the capture side rather than a target check: any target with the V4L2 capture
 * stack needs the bus, however it reaches its camera. Compiles to empty elsewhere. */
#if MEDIA_STREAM_HAS_ESP_VIDEO_CAPTURE

#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "video_raw_bus_priv.h"

static const char *TAG = "video_raw_bus";

#ifndef CONFIG_VIDEO_RAW_SINK_MAX
#define CONFIG_VIDEO_RAW_SINK_MAX 4
#endif
#ifndef CONFIG_VIDEO_RAW_SINK_SLOW_WARN_US
#define CONFIG_VIDEO_RAW_SINK_SLOW_WARN_US 20000
#endif
#ifndef CONFIG_VIDEO_RAW_SINK_BLOCK_TIMEOUT_MS
#define CONFIG_VIDEO_RAW_SINK_BLOCK_TIMEOUT_MS 100
#endif
#ifndef CONFIG_VIDEO_RAW_SINK_TASK_STACK
#define CONFIG_VIDEO_RAW_SINK_TASK_STACK 4096
#endif
#ifndef CONFIG_MEDIA_STREAM_CAM_BUFFER_RESERVE
#define CONFIG_MEDIA_STREAM_CAM_BUFFER_RESERVE 2
#endif

#define SINK_NAME_LEN     16
#define SINK_TASK_PRIO    4
#define NO_SLOT           0xFF

/* Buffers a sink may ever be loaned. The reserve is what the camera keeps to rotate
 * through, so it can keep capturing while every other buffer is out with a sink. */
#define LOAN_CAP  (MEDIA_STREAM_CAM_BUFFER_COUNT - CONFIG_MEDIA_STREAM_CAM_BUFFER_RESERVE)
/* Two, not one: the encoder sink is mandatory and registers queue_depth 1, so its claim is 2
 * (one queued, one in hand). At LOAN_CAP 1 its registration is refused and the pipeline comes
 * up with the camera running and nothing encoding it. Kconfig constrains RESERVE against COUNT
 * so this is unreachable from menuconfig; it stays as the backstop for a hand-edited
 * sdkconfig. */
#if LOAN_CAP < 2
#error "MEDIA_STREAM_CAM_BUFFER_RESERVE leaves fewer than 2 loanable buffers; the encoder sink needs 2"
#endif

typedef struct {
    uint8_t     refs;        /* Holders outstanding. 0 == back with the driver. */
    uint8_t     generation;  /* Bumped on every publish; catches stale/double release. */
    video_fb_t *fb;          /* What esp_video_if_release_frame() wants back. */
    bool        counted;     /* Whether this slot is currently charged to s_pinned. */
} raw_slot_t;

struct video_raw_sink_s {
    bool                 in_use;
    char                 name[SINK_NAME_LEN];
    video_raw_mode_t     mode;
    video_raw_sink_fn_t  on_frame;
    void                *user;
    bool                 enabled;

    QueueHandle_t        q;
    uint8_t              queue_depth;
    uint8_t              cap;            /* queue_depth + 1. Derived, never configured. */
    video_raw_overflow_t overflow;
    uint32_t             block_timeout_ms;
    uint8_t              fps_limit;

    uint8_t              in_flight;      /* Queued plus in hand. Under s_slot_lock. */
    bool                 held;           /* Pull mode: a frame is out with the consumer. */
    uint16_t             dispatch_refs;  /* Under s_sink_lock. Guards against free-under-dispatch. */

    /* Push mode. NULL task means pull mode. */
    TaskHandle_t         task;
    StaticTask_t        *task_tcb;
    void                *task_stack;
    volatile bool        task_run;
    SemaphoreHandle_t    exited;     /* Given by sink_task just before it parks; the join. */

    /* Rate gate: fixed-point accumulator, so 30 -> 7 fps lands on 7 and not 6 or 8. */
    uint32_t             rate_acc;

    uint32_t             delivered;
    uint32_t             drop_queue;
    uint32_t             drop_inflight;
    uint32_t             drop_nobuf;
    uint32_t             drop_rate;
    uint32_t             drop_timeout;
    uint64_t             blocked_us;
    uint32_t             max_hold_us;
    uint64_t             total_hold_us;
    uint32_t             hold_samples;
    uint32_t             slow_warns;
};

static raw_slot_t                s_slots[MEDIA_STREAM_CAM_BUFFER_COUNT];
static struct video_raw_sink_s   s_sinks[CONFIG_VIDEO_RAW_SINK_MAX];

static SemaphoreHandle_t s_sink_lock;   /* Sink table. Never held while blocking. */
static SemaphoreHandle_t s_slot_lock;   /* Refcounts + loan counter. Held for a few lines. */
static SemaphoreHandle_t s_freed_sem;   /* Given on every slot release. BLOCK waits here. */

static uint8_t  s_pinned;       /* Camera buffers currently loaned out. Under s_slot_lock. */
static uint32_t s_seq;          /* Monotonic capture counter. Pump task only. */
static bool     s_stopping;     /* Set by halt(); releases any blocked pump. */
static uint64_t s_window_us;    /* Pump time spent in the bus since the last read. */

static inline void sink_lock(void)   { xSemaphoreTake(s_sink_lock, portMAX_DELAY); }
static inline void sink_unlock(void) { xSemaphoreGive(s_sink_lock); }
static inline void slot_lock(void)   { xSemaphoreTake(s_slot_lock, portMAX_DELAY); }
static inline void slot_unlock(void) { xSemaphoreGive(s_slot_lock); }

esp_err_t video_raw_bus_init(void)
{
    if (s_sink_lock != NULL) {
        return ESP_OK;
    }
    s_sink_lock = xSemaphoreCreateMutex();
    s_slot_lock = xSemaphoreCreateMutex();
    /* Counting, not binary: several releases can land before a blocked pump wakes, and a
     * binary semaphore would collapse them into one and leave the pump asleep. */
    s_freed_sem = xSemaphoreCreateCounting(MEDIA_STREAM_CAM_BUFFER_COUNT * 2, 0);
    if (s_sink_lock == NULL || s_slot_lock == NULL || s_freed_sem == NULL) {
        ESP_LOGE(TAG, "Failed to create bus primitives");
        return ESP_ERR_NO_MEM;
    }
    s_stopping = false;
    return ESP_OK;
}

/* Drop one reference. When the last one goes the buffer is handed back to the driver.
 *
 * Callable from any task, and deliberately takes no lock the pump could be blocked
 * behind - see invariant 2 at the top of this file. The ioctl inside
 * esp_video_if_release_frame() runs OUTSIDE s_slot_lock: it is slow, and holding a lock
 * across it would put every sink's release behind one driver call.
 */
static void slot_release(uint8_t slot, uint8_t generation)
{
    if (slot == NO_SLOT) {
        return;                                  /* Frame with no camera buffer behind it. */
    }
    if (slot >= MEDIA_STREAM_CAM_BUFFER_COUNT) {
        ESP_LOGE(TAG, "release of out-of-range slot %u", slot);
        return;
    }

    video_fb_t *to_return = NULL;

    slot_lock();
    raw_slot_t *s = &s_slots[slot];
    if (s->generation != generation) {
        /* The descriptor names a frame that has already been retired and its buffer
         * reused. Requeueing now would hand the driver a buffer somebody else is reading.
         * Refuse, and make the bug audible rather than corrupting the pool. */
        slot_unlock();
        ESP_LOGE(TAG, "stale release of slot %u (gen %u, now %u)", slot, generation, s->generation);
        return;
    }
    if (s->refs == 0) {
        slot_unlock();
        ESP_LOGE(TAG, "double release of slot %u", slot);
        return;
    }
    if (--s->refs == 0) {
        if (s->counted) {
            s->counted = false;
            s_pinned--;
        }
        to_return = s->fb;
        s->fb = NULL;
    }
    slot_unlock();

    if (to_return != NULL) {
        esp_video_if_release_frame(to_return);
    }
    /* Given unconditionally: a BLOCK sink may be waiting on in_flight headroom, which a
     * mere refcount drop can free even when the buffer itself stays out. */
    xSemaphoreGive(s_freed_sem);
}

/* Give a frame back. Split from slot_release() because a frame carries its generation and a
 * slot does not, and because a frame not backed by a camera buffer is coming. */
static void frame_release(struct video_raw_sink_s *s, const video_raw_frame_t *f)
{
    (void)s;
    slot_release(f->slot, f->generation);
}

/* ------------------------------------------------------------------ delivery */

/* True when this frame passes the sink's fps_limit gate. */
static bool rate_gate_pass(struct video_raw_sink_s *s, uint8_t camera_fps)
{
    if (s->fps_limit == 0 || camera_fps == 0 || s->fps_limit >= camera_fps) {
        return true;
    }
    s->rate_acc += s->fps_limit;
    if (s->rate_acc >= camera_fps) {
        s->rate_acc -= camera_fps;
        return true;
    }
    return false;
}

/* Attribute a drop to the constraint that actually bit. One place, because the three
 * counters call for three different fixes: a slow consumer, a sink holding too many, or a
 * camera pool exhausted by somebody else. */
static void count_drop(struct video_raw_sink_s *s, bool no_loan, bool at_limit)
{
    if (no_loan) {
        s->drop_nobuf++;
    } else if (at_limit) {
        s->drop_inflight++;
    } else {
        s->drop_queue++;
    }
}

/* Evict this sink's oldest queued frame. Caller holds s_slot_lock.
 *
 * The evicted frame's reference MUST be dropped or its buffer leaks. Because the slot
 * table is unordered, retiring it here has no effect on any other in-flight frame - which
 * is exactly what a ring-with-a-read-pointer cannot promise. */
static bool evict_oldest_locked(struct video_raw_sink_s *s, video_fb_t **to_return)
{
    *to_return = NULL;

    video_raw_frame_t old;
    if (xQueueReceive(s->q, &old, 0) != pdTRUE) {
        return false;
    }
    if (old.slot != NO_SLOT) {
        raw_slot_t *slot = &s_slots[old.slot];
        if (slot->generation == old.generation && slot->refs > 0) {
            if (--slot->refs == 0) {
                if (slot->counted) {
                    slot->counted = false;
                    s_pinned--;
                }
                /* Usually the case: a frame queued by an EARLIER publish is held only by
                 * this sink, so evicting it retires the buffer. It cannot be requeued
                 * here - that is an ioctl, and this runs under s_slot_lock - so hand it
                 * back to the caller to return once the lock is dropped. */
                *to_return = slot->fb;
                slot->fb   = NULL;
            }
        }
    }
    s->in_flight--;
    s->drop_queue++;
    return true;
}

/* Hand one descriptor to one sink. Never blocks unless the sink asked for it.
 * Returns true when the frame was queued. */
static bool sink_enqueue(struct video_raw_sink_s *s, const video_raw_frame_t *d)
{
    /* `owned`, not `slot != NO_SLOT`: a frame the sink owns rather than borrows carries an
     * index of its own in slot, and charging the loan budget for it would spend a camera
     * buffer nobody took. `owned` is the only thing that distinguishes the two. */
    const bool     uses_slot = !d->owned;
    const uint64_t t_start   = esp_timer_get_time();
    uint64_t       deadline  = 0;
    bool           waited    = false;

    for (;;) {
        slot_lock();

        /* A slot already charged to s_pinned by an earlier sink this publish costs
         * nothing more; only the first taker pays for it. */
        const bool counted  = uses_slot && s_slots[d->slot].counted;
        const bool at_limit = (s->in_flight >= s->cap);
        const bool no_loan  = uses_slot && !counted && (s_pinned >= LOAN_CAP);

        /* Send first, commit on success. Testing for space and then sending as two steps
         * would drop a frame whenever the consumer happened to drain in between. Both
         * halves run under s_slot_lock, so the consumer cannot dequeue and release before
         * the reference has been taken. */
        if (!at_limit && !no_loan && xQueueSend(s->q, d, 0) == pdTRUE) {
            if (uses_slot) {
                s_slots[d->slot].refs++;
                if (!counted) {
                    s_slots[d->slot].counted = true;
                    s_pinned++;
                }
            }
            s->in_flight++;
            s->delivered++;
            slot_unlock();
            if (waited) {
                s->blocked_us += esp_timer_get_time() - t_start;
            }
            return true;
        }

        switch (s->overflow) {
        case VIDEO_RAW_OVERFLOW_DROP_OLD:
            /* Worth trying whichever constraint bit, because dropping our oldest frame
             * frees a queue slot AND an in-flight slot AND - when that was the frame's
             * last reference - a loan. Terminates: an empty queue has nothing to evict. */
            {
                video_fb_t *evicted = NULL;
                if (evict_oldest_locked(s, &evicted)) {
                    slot_unlock();
                    if (evicted != NULL) {
                        esp_video_if_release_frame(evicted);
                    }
                    xSemaphoreGive(s_freed_sem);
                    continue;
                }
            }
            count_drop(s, no_loan, at_limit);
            slot_unlock();
            return false;

        case VIDEO_RAW_OVERFLOW_DROP_NEW:
            count_drop(s, no_loan, at_limit);
            slot_unlock();
            return false;

        case VIDEO_RAW_OVERFLOW_BLOCK:
            slot_unlock();
            if (s_stopping) {
                s->drop_timeout++;
                return false;
            }
            if (deadline == 0) {
                deadline = t_start + (uint64_t)s->block_timeout_ms * 1000;
                waited   = true;
            }
            {
                const int64_t left_us = (int64_t)(deadline - esp_timer_get_time());
                if (left_us <= 0 ||
                    xSemaphoreTake(s_freed_sem, pdMS_TO_TICKS(left_us / 1000 + 1)) != pdTRUE) {
                    s->blocked_us += esp_timer_get_time() - t_start;
                    s->drop_timeout++;
                    return false;
                }
            }
            continue;                            /* Something was released; re-test. */
        }
    }
}

void video_raw_bus_publish(video_fb_t *fb, uint32_t fourcc, uint8_t camera_fps)
{
    if (fb == NULL || s_sink_lock == NULL) {
        if (fb != NULL) {
            esp_video_if_release_frame(fb);
        }
        return;
    }

    const uint64_t t0   = esp_timer_get_time();
    const uint8_t  slot = fb->index;
    if (slot >= MEDIA_STREAM_CAM_BUFFER_COUNT) {
        ESP_LOGE(TAG, "publish of out-of-range buffer index %u", slot);
        esp_video_if_release_frame(fb);
        return;
    }

    slot_lock();
    raw_slot_t *sl = &s_slots[slot];
    if (sl->refs != 0) {
        /* The driver handed back a buffer index we still think is checked out, which
         * means our accounting and its fb_used[] have diverged. Say so: silently
         * overwriting would strand the old reference and lose the buffer for good. */
        ESP_LOGE(TAG, "publish into slot %u which still has %u reference(s)", slot, sl->refs);
    }
    sl->generation++;
    sl->refs    = 1;          /* Publisher's own reference; dropped at the end. */
    sl->fb      = fb;
    sl->counted = false;
    const uint8_t generation = sl->generation;
    slot_unlock();

    const video_raw_frame_t desc = {
        .buffer       = fb->buf,
        .len          = fb->len,
        .width        = (uint16_t)fb->width,
        .height       = (uint16_t)fb->height,
        .fourcc       = fourcc,
        .timestamp_us = (uint64_t)fb->timestamp.tv_sec * 1000000ULL + fb->timestamp.tv_usec,
        .seq          = ++s_seq,
        .slot         = slot,
        .generation   = generation,
        .owned        = false,
    };

    /* Snapshot under the table lock, taking a dispatch reference on each sink, then DROP
     * the lock: everything below may block, and blocking with this held would deadlock
     * against a consumer trying to release. The dispatch reference is what stops a sink
     * being freed while we are inside its enqueue. */
    struct video_raw_sink_s *list[CONFIG_VIDEO_RAW_SINK_MAX];
    int n = 0;

    sink_lock();
    for (int i = 0; i < CONFIG_VIDEO_RAW_SINK_MAX; i++) {
        struct video_raw_sink_s *s = &s_sinks[i];
        if (s->in_use && s->enabled) {
            s->dispatch_refs++;
            list[n++] = s;
        }
    }
    sink_unlock();

    for (int i = 0; i < n; i++) {
        struct video_raw_sink_s *s = list[i];

        if (!rate_gate_pass(s, camera_fps)) {
            s->drop_rate++;
            goto next;
        }

        if (s->mode == VIDEO_RAW_MODE_INLINE) {
            const uint64_t c0 = esp_timer_get_time();
            s->on_frame(&desc, s->user);
            const uint32_t dt = (uint32_t)(esp_timer_get_time() - c0);
            s->delivered++;
            s->total_hold_us += dt;
            s->hold_samples++;
            if (dt > s->max_hold_us) {
                s->max_hold_us = dt;
            }
            if (dt > CONFIG_VIDEO_RAW_SINK_SLOW_WARN_US) {
                s->slow_warns++;
                if (s->slow_warns == 1 || (s->slow_warns & 0x1F) == 0) {
                    ESP_LOGW(TAG, "inline sink '%s' took %" PRIu32 " us (count=%" PRIu32 ")",
                             s->name, dt, s->slow_warns);
                }
            }
            goto next;
        }

        sink_enqueue(s, &desc);

next:
        sink_lock();
        s->dispatch_refs--;
        sink_unlock();
    }

    slot_release(slot, generation);   /* Drop the publisher reference. */
    s_window_us += esp_timer_get_time() - t0;
}

uint32_t video_raw_bus_take_window_us(void)
{
    const uint32_t v = (uint32_t)s_window_us;
    s_window_us = 0;
    return v;
}

static void sink_task(void *arg)
{
    struct video_raw_sink_s *s = (struct video_raw_sink_s *)arg;

    while (s->task_run) {
        video_raw_frame_t f;
        if (xQueueReceive(s->q, &f, pdMS_TO_TICKS(100)) != pdTRUE) {
            continue;
        }
        const uint64_t t0 = esp_timer_get_time();
        s->on_frame(&f, s->user);
        const uint32_t dt = (uint32_t)(esp_timer_get_time() - t0);

        slot_lock();
        s->in_flight--;
        s->total_hold_us += dt;
        s->hold_samples++;
        if (dt > s->max_hold_us) {
            s->max_hold_us = dt;
        }
        slot_unlock();

        frame_release(s, &f);
    }

    /* Signal the joiner, then park where no core is running us, and let unregister() do the
     * delete. Deleting ourselves here instead would put a STATIC TCB on the termination list
     * until the idle task got round to reaping it, while unregister() frees this very stack the
     * moment it returns. Suspended, we are on no core, so its vTaskDelete() is synchronous. */
    xSemaphoreGive(s->exited);
    for (;;) {
        vTaskSuspend(NULL);
    }
}

static void sink_free_resources(struct video_raw_sink_s *s)
{
    if (s->q != NULL) {
        vQueueDelete(s->q);
        s->q = NULL;
    }
    if (s->task_stack != NULL) {
        heap_caps_free(s->task_stack);
        s->task_stack = NULL;
    }
    if (s->task_tcb != NULL) {
        heap_caps_free(s->task_tcb);
        s->task_tcb = NULL;
    }
    if (s->exited != NULL) {
        vSemaphoreDelete(s->exited);
        s->exited = NULL;
    }
}

esp_err_t video_raw_sink_register(const video_raw_sink_config_t *cfg,
                                  video_raw_sink_handle_t *out)
{
    if (cfg == NULL || cfg->name == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (cfg->mode == VIDEO_RAW_MODE_INLINE && cfg->on_frame == NULL) {
        ESP_LOGE(TAG, "'%s': inline sinks must supply on_frame", cfg->name);
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = video_raw_bus_init();
    if (err != ESP_OK) {
        return err;
    }

    /* depth 0 is legal and means no staging: one frame at a time, claim 1. */
    const uint8_t depth = cfg->queue_depth;

    /* The cap is always depth + 1 - depth staged plus the one being worked on - and it is
     * derived rather than configured because nothing can exceed it. A push sink runs one
     * callback at a time, and a pull sink is refused a second acquire() before release().
     * This is also the sink's claim on the camera pool, charged below. */
    if (depth == UINT8_MAX) {
        return ESP_ERR_INVALID_ARG;     /* depth + 1 would wrap. */
    }
    const uint8_t cap = (uint8_t)(depth + 1);

    *out = NULL;
    sink_lock();

    /* Reject passthrough sinks that cannot be served. */
    if (cfg->mode == VIDEO_RAW_MODE_PASSTHROUGH) {
        uint32_t committed = 0;
        for (int i = 0; i < CONFIG_VIDEO_RAW_SINK_MAX; i++) {
            if (s_sinks[i].in_use && s_sinks[i].mode == VIDEO_RAW_MODE_PASSTHROUGH) {
                committed += s_sinks[i].cap;
            }
        }
        if (committed + cap > LOAN_CAP) {
            sink_unlock();
            ESP_LOGE(TAG,
                     "'%s': needs %u camera buffer(s), only %" PRIu32 " of %d left. "
                     "Lower its queue_depth (0 claims just 1), or raise "
                     "CONFIG_MEDIA_STREAM_CAM_BUFFER_COUNT (currently %d).",
                     cfg->name, cap, (uint32_t)LOAN_CAP - committed, LOAN_CAP,
                     MEDIA_STREAM_CAM_BUFFER_COUNT);
            return ESP_ERR_NO_MEM;
        }
    }

    for (int i = 0; i < CONFIG_VIDEO_RAW_SINK_MAX; i++) {
        struct video_raw_sink_s *s = &s_sinks[i];
        if (s->in_use) {
            continue;
        }
        memset(s, 0, sizeof(*s));
        s->in_use           = true;
        strlcpy(s->name, cfg->name, sizeof(s->name));
        s->mode             = cfg->mode;
        s->on_frame         = cfg->on_frame;
        s->user             = cfg->user;
        s->queue_depth      = depth;
        s->cap              = cap;
        s->overflow         = cfg->overflow;
        s->block_timeout_ms = cfg->block_timeout_ms ? cfg->block_timeout_ms
                                                    : CONFIG_VIDEO_RAW_SINK_BLOCK_TIMEOUT_MS;
        s->fps_limit        = cfg->fps_limit;

        if (s->mode != VIDEO_RAW_MODE_INLINE) {
            /* depth 0 still needs one slot to pass a frame through; the cap of 1 is what
             * stops a second frame being queued behind the one in hand. */
            s->q = xQueueCreate(depth ? depth : 1, sizeof(video_raw_frame_t));
            if (s->q == NULL) {
                memset(s, 0, sizeof(*s));
                sink_unlock();
                return ESP_ERR_NO_MEM;
            }
            if (cfg->on_frame != NULL) {
                const uint32_t stack = cfg->task_stack ? cfg->task_stack
                                                       : CONFIG_VIDEO_RAW_SINK_TASK_STACK;
                /* TCB internal (touched from the scheduler tick), stack in SPIRAM - the
                 * same split the encoder task uses. */
                s->task_tcb   = heap_caps_calloc(1, sizeof(StaticTask_t), MALLOC_CAP_INTERNAL);
                s->task_stack = heap_caps_calloc(1, stack, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                s->exited     = xSemaphoreCreateBinary();
                if (s->task_tcb == NULL || s->task_stack == NULL || s->exited == NULL) {
                    sink_free_resources(s);
                    memset(s, 0, sizeof(*s));
                    sink_unlock();
                    return ESP_ERR_NO_MEM;
                }
                s->task_run = true;
                s->task = xTaskCreateStatic(sink_task, s->name, stack, s,
                                            cfg->task_prio ? cfg->task_prio : SINK_TASK_PRIO,
                                            s->task_stack, s->task_tcb);
                if (s->task == NULL) {
                    s->task_run = false;
                    sink_free_resources(s);
                    memset(s, 0, sizeof(*s));
                    sink_unlock();
                    return ESP_ERR_NO_MEM;
                }
            }
        }

        *out = s;
        sink_unlock();

        ESP_LOGI(TAG, "Registered raw sink '%s' (slot %d, mode %d, depth %u, cap %u)",
                 s->name, i, (int)s->mode, depth, cap);
        if (s->overflow == VIDEO_RAW_OVERFLOW_BLOCK) {
            ESP_LOGW(TAG,
                     "'%s' uses VIDEO_RAW_OVERFLOW_BLOCK: when it falls behind the pump "
                     "waits (up to %" PRIu32 " ms), which stops frame delivery to EVERY "
                     "other sink for that time.",
                     s->name, s->block_timeout_ms);
        }
        return ESP_OK;
    }

    sink_unlock();
    ESP_LOGE(TAG, "No free raw sink slot for '%s' (max %d, raise CONFIG_VIDEO_RAW_SINK_MAX)",
             cfg->name, CONFIG_VIDEO_RAW_SINK_MAX);
    return ESP_ERR_NOT_FOUND;
}

/* Drain everything queued for this sink, releasing each frame's reference. */
static void sink_drain(struct video_raw_sink_s *s)
{
    if (s->q == NULL) {
        return;
    }
    video_raw_frame_t f;
    while (xQueueReceive(s->q, &f, 0) == pdTRUE) {
        slot_lock();
        s->in_flight--;
        slot_unlock();
        frame_release(s, &f);
    }
}

esp_err_t video_raw_sink_set_enabled(video_raw_sink_handle_t sink, bool enabled)
{
    if (sink == NULL || !sink->in_use) {
        return ESP_ERR_INVALID_ARG;
    }
    sink_lock();
    sink->enabled = enabled;
    sink_unlock();

    if (!enabled) {
        /* Drop what is queued so a re-enabled sink resumes on a current frame instead of
         * working through a backlog captured before it was switched off. */
        sink_drain(sink);
    }
    return ESP_OK;
}

esp_err_t video_raw_sink_set_fps_limit(video_raw_sink_handle_t sink, uint8_t fps)
{
    if (sink == NULL || !sink->in_use) {
        return ESP_ERR_INVALID_ARG;
    }

    sink_lock();
    sink->fps_limit = fps;
    sink_unlock();
    return ESP_OK;
}

esp_err_t video_raw_sink_unregister(video_raw_sink_handle_t sink)
{
    if (sink == NULL || !sink->in_use) {
        return ESP_ERR_INVALID_ARG;
    }

    sink_lock();
    sink->enabled = false;
    sink_unlock();

    /* Wait for any dispatch that is already inside this sink - possibly blocked in its
     * enqueue - to finish before the slot can be reused. */
    for (int i = 0; i < 200; i++) {
        sink_lock();
        const uint16_t refs = sink->dispatch_refs;
        sink_unlock();
        if (refs == 0) {
            break;
        }
        xSemaphoreGive(s_freed_sem);          /* Nudge a blocked pump. */
        vTaskDelay(pdMS_TO_TICKS(5));
    }

    sink->task_run = false;
    sink_drain(sink);

    if (sink->task != NULL) {
        /* push mode: join the task.
         *
         * Unbounded on purpose. The alternative to waiting is freeing memory out from under a
         * live task
         */
        for (int secs = 1; xSemaphoreTake(sink->exited, pdMS_TO_TICKS(1000)) != pdTRUE; secs++) {
            ESP_LOGW(TAG, "'%s': still inside on_frame after %d s; waiting to unregister",
                     sink->name, secs);
        }
        /* It has signalled but may not have reached vTaskSuspend() yet; deleting a task that is
         * still running on the other core would only defer the teardown we are about to rely on. */
        while (eTaskGetState(sink->task) != eSuspended) {
            vTaskDelay(1);
        }
        vTaskDelete(sink->task);
        sink->task = NULL;
    } else {
        /* pull mode: nothing to join, and a consumer sitting on an acquired frame is outside our
         * control. Bounded wait, then leak the frame rather than reclaim a buffer it may still
         * be reading. */
        for (int i = 0; i < 200 && sink->in_flight > 0; i++) {
            vTaskDelay(pdMS_TO_TICKS(5));
        }
        if (sink->in_flight > 0) {
            ESP_LOGE(TAG, "'%s' still holds %u frame(s) at unregister; leaking them rather "
                          "than reclaiming a buffer it may still be reading",
                     sink->name, sink->in_flight);
        }
    }

    sink_lock();
    sink_free_resources(sink);
    memset(sink, 0, sizeof(*sink));
    sink_unlock();
    return ESP_OK;
}

esp_err_t video_raw_sink_acquire(video_raw_sink_handle_t sink, video_raw_frame_t *frame,
                                 uint32_t timeout_ms)
{
    if (sink == NULL || !sink->in_use || frame == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (sink->q == NULL || sink->on_frame != NULL) {
        return ESP_ERR_INVALID_STATE;       /* Inline or push mode: frames are delivered. */
    }
    /* One frame at a time. Without this a consumer could sit on several acquired frames and
     * the cap of queue_depth + 1 would stop being a bound on what it pins.
     *
     * Read under the slot lock, like every other write to it, though a pull sink has one
     * consumer task by contract - it drives acquire/release from a task of its own - so
     * there is no second caller to race the check against the set below. */
    slot_lock();
    const bool already_held = sink->held;
    slot_unlock();
    if (already_held) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xQueueReceive(sink->q, frame, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    slot_lock();
    sink->held = true;
    slot_unlock();
    return ESP_OK;
}

esp_err_t video_raw_sink_release(video_raw_sink_handle_t sink, const video_raw_frame_t *frame)
{
    if (sink == NULL || !sink->in_use || frame == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    slot_lock();
    if (sink->in_flight > 0) {
        sink->in_flight--;
    }
    sink->held = false;
    slot_unlock();
    frame_release(sink, frame);
    return ESP_OK;
}

esp_err_t video_raw_sink_get_stats(video_raw_sink_stats_t *out, size_t max, size_t *count)
{
    if (out == NULL || count == NULL || s_sink_lock == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t n = 0;
    sink_lock();
    for (int i = 0; i < CONFIG_VIDEO_RAW_SINK_MAX && n < max; i++) {
        struct video_raw_sink_s *s = &s_sinks[i];
        if (!s->in_use) {
            continue;
        }
        out[n++] = (video_raw_sink_stats_t){
            .name             = s->name,
            .enabled          = s->enabled,
            .delivered        = s->delivered,
            .dropped_queue    = s->drop_queue,
            .dropped_inflight = s->drop_inflight,
            .dropped_nobuf    = s->drop_nobuf,
            .dropped_rate     = s->drop_rate,
            .dropped_timeout  = s->drop_timeout,
            .blocked_us       = s->blocked_us,
            .max_hold_us      = s->max_hold_us,
            .avg_hold_us      = s->hold_samples ? (uint32_t)(s->total_hold_us / s->hold_samples) : 0,
            .in_flight        = s->in_flight,
        };
    }
    sink_unlock();
    *count = n;
    return ESP_OK;
}

void video_raw_sink_reset_stats(void)
{
    if (s_sink_lock == NULL) {
        return;
    }
    sink_lock();
    for (int i = 0; i < CONFIG_VIDEO_RAW_SINK_MAX; i++) {
        struct video_raw_sink_s *s = &s_sinks[i];
        if (!s->in_use) {
            continue;
        }
        s->delivered = s->drop_queue = s->drop_inflight = s->drop_nobuf = 0;
        s->drop_rate = s->drop_timeout = s->slow_warns = 0;
        s->blocked_us = s->total_hold_us = 0;
        s->max_hold_us = s->hold_samples = 0;
    }
    sink_unlock();
}

void video_raw_bus_halt(void)
{
    if (s_sink_lock == NULL) {
        return;
    }
    /* Set the flag first, then nudge: this is what frees a pump currently blocked inside
     * a VIDEO_RAW_OVERFLOW_BLOCK sink. It has to happen BEFORE the caller waits for the
     * pump to park, or stop would wait behind exactly the sink it is shutting down. */
    s_stopping = true;
    for (int i = 0; i < MEDIA_STREAM_CAM_BUFFER_COUNT * 2; i++) {
        xSemaphoreGive(s_freed_sem);
    }
}

void video_raw_bus_resume(void)
{
    s_stopping = false;
}

void video_raw_bus_drain(void)
{
    if (s_sink_lock == NULL) {
        return;
    }
    video_raw_bus_halt();   /* Idempotent; covers a caller that drains without halting. */

    sink_lock();
    struct video_raw_sink_s *list[CONFIG_VIDEO_RAW_SINK_MAX];
    int n = 0;
    for (int i = 0; i < CONFIG_VIDEO_RAW_SINK_MAX; i++) {
        if (s_sinks[i].in_use) {
            list[n++] = &s_sinks[i];
        }
    }
    sink_unlock();

    for (int i = 0; i < n; i++) {
        sink_drain(list[i]);
    }

    /* A consumer mid-callback must finish; a forcibly reclaimed buffer it is still
     * reading would be a use-after-free, and a leaked buffer is only a degraded camera. */
    for (int attempt = 0; attempt < 200; attempt++) {
        bool busy = false;
        for (int i = 0; i < n; i++) {
            if (list[i]->in_flight > 0) {
                busy = true;
            }
        }
        if (!busy) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    for (int i = 0; i < n; i++) {
        if (list[i]->in_flight > 0) {
            ESP_LOGE(TAG, "'%s' still holds %u frame(s) after drain", list[i]->name,
                     list[i]->in_flight);
        }
    }

    for (int i = 0; i < MEDIA_STREAM_CAM_BUFFER_COUNT; i++) {
        if (s_slots[i].refs != 0) {
            ESP_LOGE(TAG, "slot %d still has %u reference(s) after drain", i, s_slots[i].refs);
        }
    }
}

#endif /* MEDIA_STREAM_HAS_ESP_VIDEO_CAPTURE */
