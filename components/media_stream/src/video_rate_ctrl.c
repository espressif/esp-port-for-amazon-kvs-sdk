/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include <stdatomic.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "video_rate_ctrl.h"

static const char *TAG = "vrate";

#ifndef CONFIG_VIDEO_RATE_CTRL_MAX
#define CONFIG_VIDEO_RATE_CTRL_MAX 4
#endif

#define VRATE_NAME_LEN 16

/* Operating-point ladder.
 *
 * fps and bitrate are NOT tracked as two free counters - they're tied to a
 * single `level` index into this ladder, so every operating point is a (fps,
 * bitrate) pair we picked to make sense together. Congestion steps the level
 * down the ladder (higher index), recovery steps it back up. One variable, so
 * degrade/recover are symmetric and the two can't drift into a nonsensical
 * combination.
 *
 * fps is absolute (clamped to [FPS_FLOOR, camera_fps]); bitrate is a percentage
 * of the resolution-derived max so the ladder scales across 720p/1080p. */
typedef struct {
    uint8_t fps;
    uint8_t pct;   /* bitrate as % of max_bitrate_bps */
} vrate_rung_t;

static const vrate_rung_t LADDER[] = {
    {  0, 100 },  /* L0 - full quality: fps 0 means the camera's native rate,
                   *      whatever the driver reported, so this rung is not
                   *      pinned to one sensor's frame rate (uplink must prove
                   *      it can take this) */
    { 24,  80 },  /* L1 */
    { 22,  65 },  /* L2 */
    { 20,  50 },  /* L3 - conservative start (smooth connect, then probe up) */
    { 16,  38 },  /* L4 */
    { 12,  28 },  /* L5 - fps now at floor; below here only bitrate moves */
    { 12,  18 },  /* L6 */
    { 12,  10 },  /* L7 - survival floor */
};
#define LADDER_N             (sizeof(LADDER) / sizeof(LADDER[0]))

/* Start mid-ladder, not at the top: slamming full bitrate into a link that
 * hasn't settled (ICE/TURN still negotiating) is what triggers the initial
 * congestion collapse. Begin gentle, recover upward only if the link earns it. */
#define START_LEVEL          3

/* fps adaptation floor. 12 (not 15): on the contended 2.4 GHz uplink a single
 * frame can take ~70 ms to send, and a 15-fps budget (67 ms) stays under water,
 * so the controller can't actually catch up. 12 fps = ~83 ms budget gives the
 * headroom to match those sends. The ladder never schedules below this. */
#define FPS_FLOOR            12

/* Send-latency EMA smoothing: ema = ema*(1-a) + sample*a, a = 1/4. */
#define EMA_A_NUM            1
#define EMA_A_DEN            4

/* Congestion / recovery thresholds relative to the per-frame budget
 * (1000/target_fps ms). Congested above 1.3x budget; healthy below 0.6x.
 * The gap is the hysteresis band. */
#define CONGEST_NUM          13
#define CONGEST_DEN          10
#define HEALTHY_NUM          6
#define HEALTHY_DEN          10

/* Recovery gate. Deliberately slower than degradation (AIMD: back off fast,
 * recover slow).
 *
 * This is a DWELL, not a report count. Controllers now report at different and
 * independently varying rates - a real-time sender reports once per sent frame
 * and goes quiet exactly when congested, while a queue sampler reports per
 * encoded frame regardless - so "60 reports" no longer means a fixed duration
 * for anyone. The minimum count rides along so that silence cannot be mistaken
 * for health: a controller that observed nothing has not earned a rung. */
#define RECOVER_DWELL_MS     3000
#define RECOVER_MIN_REPORTS  8

/* Queue (LEVEL signal) front-end.
 *
 * Evaluated on a fixed cadence rather than per report, because a queue cannot
 * respond to a rung change until the existing backlog drains - seconds later at
 * these depths. Reacting per report would walk the ladder to the floor in a few
 * hundred milliseconds on a transient and then ring. The cadence IS the
 * step-down rate limiter.
 *
 * The trend term is what makes a level signal usable: a shallow-but-climbing
 * queue is congestion, a deep-but-draining one is already being handled. Entry
 * uses the trend-projected value; severity uses the smoothed level alone, so a
 * fast transient cannot trigger a multi-rung drop. */
#define QUEUE_EVAL_MS        500
#define QUEUE_CONGEST_PCT    35
#define QUEUE_HEALTHY_PCT    10
#define QUEUE_TREND_GAIN     2
#define QUEUE_JUMP2_PCT      60
#define QUEUE_JUMP3_PCT      85

/* A controller that stops reporting must not pin the encoder at its last rung
 * forever - a sender thread wedged inside a socket write neither reports nor
 * disables. Past this it is dropped from arbitration, but KEEPS its level, so it
 * resumes where it left off rather than re-probing from the start rung. */
#define VRATE_STALE_MS       5000

/* Absolute bitrate floor (bps). Matches esp_h264_hw_enc.c MIN_BITRATE - sub-
 * floor targets there are clamped, so the ladder's lowest rung must not ask for
 * less than the encoder will honor. The ladder L7 bitrate is clamped up to this. */
#define MIN_BITRATE_BPS      (100 * 1024)

struct video_rate_ctrl_s {
    bool     in_use;
    bool     enabled;
    char     name[VRATE_NAME_LEN];

    uint32_t level;                /* current ladder index */
    uint32_t target_fps;           /* derived from level */
    uint32_t target_bitrate_bps;
    uint32_t net_ceiling_bps;      /* TWCC network ceiling; 0 = no network limit */

    uint32_t ema_ms;               /* send-latency front-end */

    uint32_t q_used;               /* queue front-end: latest sample */
    uint32_t q_cap;
    uint32_t q_ema_pct;
    int32_t  q_delta;
    bool     q_primed;
    int64_t  q_next_eval_us;

    uint32_t load_pct;             /* 100 == at this controller's congest threshold */

    int64_t  healthy_since_us;
    uint32_t healthy_reports;

    int64_t  last_report_us;
    bool     stale;                /* cached, so the transition can be logged once */
    uint32_t reports;
};

static struct video_rate_ctrl_s s_ctrl[CONFIG_VIDEO_RATE_CTRL_MAX];
static SemaphoreHandle_t        s_lock;

/* Encoder-wide state. Written under s_lock. */
static struct {
    bool     inited;
    /* False when the frame rate is not ours to change like when the camera hands us
     * already-encoded H.264 */
    bool     fps_actuable;
    uint32_t camera_fps;
    uint32_t width;
    uint32_t height;
    uint32_t max_bitrate_bps;
    uint32_t min_bitrate_bps;
    uint32_t applied_bitrate;      /* what the encoder was last told */
} s_agg;

/* Published to the encoder task. Written under s_lock, read lock-free - which is
 * what keeps the per-frame path off the contention graph entirely. */
static _Atomic uint32_t s_pub_target_fps;   /* 0 = native, nothing to throttle */
static _Atomic uint32_t s_pub_camera_fps;
static _Atomic uint32_t s_pending_bitrate;  /* 0 = nothing to apply */
static _Atomic int64_t  s_stale_check_us;   /* next time staleness could matter */

/* Skip accumulator. There is one encoder, so there is one skip decision and one
 * accumulator; it lives here rather than per controller. Touched only by the
 * encoder task inside video_rate_ctrl_should_encode(), so it needs no lock. */
static uint32_t s_accum;

/* ------------------------------------------------------------------------- */

static esp_err_t vrate_lock_init(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == NULL) {
            ESP_LOGE(TAG, "Failed to create rate-control mutex");
            return ESP_ERR_NO_MEM;
        }
    }
    return ESP_OK;
}

static inline void vrate_lock(void)   { xSemaphoreTake(s_lock, portMAX_DELAY); }
static inline void vrate_unlock(void) { xSemaphoreGive(s_lock); }

static uint32_t vrate_rung_bitrate(uint32_t level)
{
    uint32_t b = (uint32_t)(((uint64_t)s_agg.max_bitrate_bps * LADDER[level].pct) / 100);
    return (b < s_agg.min_bitrate_bps) ? s_agg.min_bitrate_bps : b;
}

static uint32_t vrate_rung_fps(uint32_t level)
{
    if (!s_agg.fps_actuable) {
        return s_agg.camera_fps;
    }

    uint32_t f = LADDER[level].fps;
    if (f == 0) {
        f = s_agg.camera_fps;   /* L0 sentinel: run at whatever the sensor delivers */
    }
    /* Raise to the floor BEFORE clamping to the camera, not after. The other
     * order can return target_fps > camera_fps on a slow sensor, which makes the
     * Bresenham accumulator grow without bound and silently disables throttling. */
    if (f < FPS_FLOOR) {
        f = FPS_FLOOR;
    }
    if (f > s_agg.camera_fps) {
        f = s_agg.camera_fps;
    }
    return f;
}

/* Effective bitrate ceiling for one controller = min(configured max, its TWCC
 * network ceiling), never below the survival floor. */
static uint32_t vrate_eff_max(const struct video_rate_ctrl_s *h)
{
    uint32_t eff = s_agg.max_bitrate_bps;
    if (h->net_ceiling_bps != 0 && h->net_ceiling_bps < eff) {
        eff = h->net_ceiling_bps;
    }
    return (eff < s_agg.min_bitrate_bps) ? s_agg.min_bitrate_bps : eff;
}

/* Highest-quality (lowest-index) rung whose bitrate fits this controller's
 * effective ceiling. Recovery can't climb above this; a low TWCC estimate raises
 * it, snapping the controller down the ladder. */
static uint32_t vrate_ceiling_level(const struct video_rate_ctrl_s *h)
{
    uint32_t eff = vrate_eff_max(h);
    for (uint32_t i = 0; i < LADDER_N; i++) {
        if (vrate_rung_bitrate(i) <= eff) {
            return i;
        }
    }
    return LADDER_N - 1;
}

/* Move one controller to a ladder level, clamped to what its own network
 * ceiling affords. Arbitration across controllers happens separately. */
static void vrate_apply_level(struct video_rate_ctrl_s *h, uint32_t level)
{
    uint32_t floor_lvl = vrate_ceiling_level(h);
    if (level < floor_lvl) {
        level = floor_lvl;
    }
    if (level > LADDER_N - 1) {
        level = LADDER_N - 1;
    }
    h->level              = level;
    h->target_fps         = vrate_rung_fps(level);
    h->target_bitrate_bps = vrate_rung_bitrate(level);
}

static void vrate_reset_signal(struct video_rate_ctrl_s *h)
{
    h->ema_ms          = 0;
    h->q_ema_pct       = 0;
    h->q_delta         = 0;
    h->q_primed        = false;
    h->q_next_eval_us  = 0;
    h->load_pct        = 0;
    h->healthy_since_us = 0;
    h->healthy_reports  = 0;
}

static inline bool vrate_is_stale(const struct video_rate_ctrl_s *h, int64_t now)
{
    return (now - h->last_report_us) > (int64_t)VRATE_STALE_MS * 1000;
}

static inline bool vrate_eligible(const struct video_rate_ctrl_s *h, int64_t now)
{
    return h->in_use && h->enabled && !vrate_is_stale(h, now);
}

/* Reduce every eligible controller to the single operating point the one encoder
 * can serve: strict minimum, i.e. worst sink wins. Call with the lock held after
 * anything that could change a controller's target or eligibility. */
static void vrate_rearbitrate(int64_t now)
{
    uint32_t min_fps = 0;
    uint32_t min_bps = 0;
    bool     any     = false;
    int64_t  soonest = INT64_MAX;

    for (int i = 0; i < CONFIG_VIDEO_RATE_CTRL_MAX; i++) {
        struct video_rate_ctrl_s *h = &s_ctrl[i];
        if (!h->in_use || !h->enabled) {
            continue;
        }
        if (vrate_is_stale(h, now)) {
            continue;
        }
        int64_t expires = h->last_report_us + (int64_t)VRATE_STALE_MS * 1000;
        if (expires < soonest) {
            soonest = expires;
        }
        if (!any || h->target_fps < min_fps) {
            min_fps = h->target_fps;
        }
        if (!any || h->target_bitrate_bps < min_bps) {
            min_bps = h->target_bitrate_bps;
        }
        any = true;
    }

    if (!s_agg.inited) {
        /* No encoder yet. Controllers may exist and be enabled; there is simply
         * nothing to actuate until init() describes what they are steering.
         * Park the staleness deadline too, or the encoder task takes the lock on
         * every frame to re-discover that there is nothing to do. */
        atomic_store(&s_pub_target_fps, 0);
        atomic_store(&s_stale_check_us, INT64_MAX);
        return;
    }

    if (!any) {
        /* Back to native. Unlike init, the encoder may be mid-run at a degraded
         * bitrate here, so it has to be told to go back up. */
        atomic_store(&s_pub_target_fps, 0);
        if (s_agg.applied_bitrate != s_agg.max_bitrate_bps) {
            s_agg.applied_bitrate = s_agg.max_bitrate_bps;
            atomic_store(&s_pending_bitrate, s_agg.max_bitrate_bps);
        }
        atomic_store(&s_stale_check_us, INT64_MAX);
        return;
    }

    atomic_store(&s_pub_target_fps, min_fps);
    if (min_bps != s_agg.applied_bitrate) {
        s_agg.applied_bitrate = min_bps;
        atomic_store(&s_pending_bitrate, min_bps);
    }
    atomic_store(&s_stale_check_us, soonest);
}

/* Shared ladder movement, used by both front-ends. Returns true if the rung
 * actually moved, so the caller can log it AFTER dropping the lock. */
static bool vrate_step_down(struct video_rate_ctrl_s *h, uint32_t jump)
{
    h->healthy_since_us = 0;
    h->healthy_reports  = 0;
    if (h->level >= LADDER_N - 1) {
        return false;
    }
    uint32_t prev = h->level;
    uint32_t nl   = (h->level + jump < LADDER_N) ? h->level + jump : LADDER_N - 1;
    vrate_apply_level(h, nl);
    return h->level != prev;
}

static bool vrate_mark_healthy(struct video_rate_ctrl_s *h, int64_t now)
{
    if (h->healthy_since_us == 0) {
        h->healthy_since_us = now;
        h->healthy_reports  = 0;
    }
    h->healthy_reports++;

    if (now - h->healthy_since_us < (int64_t)RECOVER_DWELL_MS * 1000 ||
        h->healthy_reports < RECOVER_MIN_REPORTS) {
        return false;
    }
    h->healthy_since_us = 0;
    h->healthy_reports  = 0;

    if (h->level == 0) {
        return false;
    }
    uint32_t prev = h->level;
    vrate_apply_level(h, h->level - 1);
    return h->level != prev;
}

static void vrate_reset_healthy(struct video_rate_ctrl_s *h)
{
    h->healthy_since_us = 0;
    h->healthy_reports  = 0;
}

/* ------------------------------------------------------------------------- */

void video_rate_ctrl_init(uint32_t camera_fps, uint32_t max_bitrate_bps,
                          uint32_t width, uint32_t height, bool fps_actuable)
{
    if (vrate_lock_init() != ESP_OK) {
        return;
    }
    int64_t now = esp_timer_get_time();

    vrate_lock();
    s_agg.camera_fps      = camera_fps ? camera_fps : 25;
    s_agg.fps_actuable    = fps_actuable;
    s_agg.width           = width;
    s_agg.height          = height;
    s_agg.max_bitrate_bps = max_bitrate_bps;
    s_agg.min_bitrate_bps = (max_bitrate_bps > MIN_BITRATE_BPS) ? MIN_BITRATE_BPS
                                                                : max_bitrate_bps;
    /* The encoder was just configured at this bitrate, so nothing is pending. */
    s_agg.applied_bitrate = max_bitrate_bps;
    s_agg.inited          = true;
    atomic_store(&s_pub_camera_fps, s_agg.camera_fps);
    atomic_store(&s_pending_bitrate, 0);

    /* Controllers are NOT reset: a transport may have opted in before the camera
     * came up, and capture is reference counted so this can also be a restart
     * under live controllers. Re-seed whoever is enabled to the start rung. */
    uint32_t seeded = 0;
    for (int i = 0; i < CONFIG_VIDEO_RATE_CTRL_MAX; i++) {
        struct video_rate_ctrl_s *h = &s_ctrl[i];
        if (!h->in_use) {
            continue;
        }
        vrate_reset_signal(h);
        h->last_report_us = now;
        h->stale          = false;
        if (h->enabled) {
            vrate_apply_level(h, START_LEVEL);
            seeded++;
        }
    }
    vrate_rearbitrate(now);
    uint32_t cam = s_agg.camera_fps;
    uint32_t flr = s_agg.min_bitrate_bps;
    vrate_unlock();

    ESP_LOGI(TAG, "init: cam_fps=%" PRIu32 "%s res=%" PRIu32 "x%" PRIu32
             " max=%" PRIu32 "bps floor=%" PRIu32 "bps, %" PRIu32 " controller(s) adapting",
             cam, fps_actuable ? "" : " (fixed - bitrate only)",
             width, height, max_bitrate_bps, flr, seeded);
}

esp_err_t video_rate_ctrl_create(const char *name, video_rate_ctrl_handle_t *out)
{
    if (name == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = vrate_lock_init();
    if (err != ESP_OK) {
        return err;
    }

    *out = NULL;
    vrate_lock();
    for (int i = 0; i < CONFIG_VIDEO_RATE_CTRL_MAX; i++) {
        struct video_rate_ctrl_s *h = &s_ctrl[i];
        if (h->in_use) {
            continue;
        }
        memset(h, 0, sizeof(*h));
        h->in_use         = true;
        h->enabled        = false;
        h->last_report_us = esp_timer_get_time();
        strlcpy(h->name, name, sizeof(h->name));
        if (s_agg.inited) {
            vrate_apply_level(h, START_LEVEL);
        }
        *out = h;
        vrate_unlock();
        ESP_LOGI(TAG, "Created controller '%s' (slot %d)", name, i);
        return ESP_OK;
    }
    vrate_unlock();
    ESP_LOGE(TAG, "No free controller slot for '%s' (max %d, raise CONFIG_VIDEO_RATE_CTRL_MAX)",
             name, CONFIG_VIDEO_RATE_CTRL_MAX);
    return ESP_ERR_NOT_FOUND;
}

void video_rate_ctrl_destroy(video_rate_ctrl_handle_t h)
{
    if (h == NULL || s_lock == NULL) {
        return;
    }
    char name[VRATE_NAME_LEN];
    vrate_lock();
    if (!h->in_use) {
        vrate_unlock();
        return;
    }
    strlcpy(name, h->name, sizeof(name));
    memset(h, 0, sizeof(*h));
    vrate_rearbitrate(esp_timer_get_time());
    vrate_unlock();
    ESP_LOGI(TAG, "Destroyed controller '%s'", name);
}

void video_rate_ctrl_enable(video_rate_ctrl_handle_t h, bool enable)
{
    if (h == NULL || s_lock == NULL) {
        return;
    }
    bool     inited;
    char     name[VRATE_NAME_LEN];
    uint32_t lvl = 0, fps = 0, bps = 0;

    int64_t now = esp_timer_get_time();
    vrate_lock();
    if (!h->in_use || h->enabled == enable) {
        vrate_unlock();
        return;
    }
    h->enabled = enable;
    inited     = s_agg.inited;
    strlcpy(name, h->name, sizeof(name));

    /* Drop any history from a previous session either way: a controller coming
     * back must not act on a stale picture of a link it is no longer using. */
    vrate_reset_signal(h);
    h->last_report_us = now;
    h->stale          = false;

    if (enable && inited) {
        vrate_apply_level(h, START_LEVEL);
    }
    vrate_rearbitrate(now);
    lvl = h->level; fps = h->target_fps; bps = h->target_bitrate_bps;
    vrate_unlock();

    if (!inited) {
        ESP_LOGI(TAG, "'%s' adaptation %s (pending camera init)", name, enable ? "on" : "off");
    } else if (enable) {
        ESP_LOGI(TAG, "'%s' adaptation on: L%" PRIu32 " (%" PRIu32 "fps/%" PRIu32 "bps)",
                 name, lvl, fps, bps);
    } else {
        ESP_LOGI(TAG, "'%s' adaptation off, withdrawn from arbitration", name);
    }
}

void video_rate_ctrl_report_send_ms(video_rate_ctrl_handle_t h, uint32_t send_ms)
{
    if (h == NULL || s_lock == NULL) {
        return;
    }
    bool     moved = false, up = false;
    char     name[VRATE_NAME_LEN];
    uint32_t lvl = 0, fps = 0, bps = 0, ema = 0, budget = 0;

    int64_t now = esp_timer_get_time();
    vrate_lock();
    if (!h->in_use || !h->enabled || !s_agg.inited) {
        vrate_unlock();
        return;   /* Not an error: adaptation is opt-in and this path is per frame. */
    }
    /* Was this controller excluded? If so it has to be re-admitted even when the
     * rung does not move, because nothing else will re-arbitrate it back in. */
    bool was_stale = h->stale || vrate_is_stale(h, now);

    h->last_report_us = now;
    h->stale          = false;
    h->reports++;

    h->ema_ms = (h->ema_ms * (EMA_A_DEN - EMA_A_NUM) + send_ms * EMA_A_NUM) / EMA_A_DEN;
    uint32_t budget_ms = 1000u / (h->target_fps ? h->target_fps : 1u);

    /* Normalised so 100 == this controller's congestion threshold, which is what
     * makes a latency controller and a queue controller comparable in the stats. */
    uint32_t congest_ms = (budget_ms * CONGEST_NUM) / CONGEST_DEN;
    h->load_pct = congest_ms ? (h->ema_ms * 100u) / congest_ms : 0;

    if (h->ema_ms * CONGEST_DEN > (uint32_t)budget_ms * CONGEST_NUM) {
        /* Congested - step down the ladder (fps + bitrate together). Severity-
         * scaled: the further over budget we are, the more rungs we drop in one
         * report, so a single sparse report during a bad spell still does real
         * work (reports arrive only as fast as frames send, which collapses
         * exactly when congested). Self-arrests as ema falls back under budget. */
        uint32_t jump = 1;
        if (h->ema_ms >= (uint32_t)budget_ms * 3) {
            jump = 3;
        } else if (h->ema_ms >= (uint32_t)budget_ms * 2) {
            jump = 2;
        }
        moved = vrate_step_down(h, jump);
    } else if (h->ema_ms * HEALTHY_DEN < (uint32_t)budget_ms * HEALTHY_NUM) {
        moved = vrate_mark_healthy(h, now);
        up    = moved;
    } else {
        vrate_reset_healthy(h);
    }

    if (moved || was_stale) {
        vrate_rearbitrate(now);
    }
    strlcpy(name, h->name, sizeof(name));
    lvl = h->level; fps = h->target_fps; bps = h->target_bitrate_bps;
    ema = h->ema_ms; budget = budget_ms;
    vrate_unlock();

    if (moved) {
        ESP_LOGI(TAG, "'%s' %s (ema=%" PRIu32 "ms budget=%" PRIu32 "ms): L%" PRIu32
                 " (%" PRIu32 "fps/%" PRIu32 "bps)",
                 name, up ? "recovering" : "congested", ema, budget, lvl, fps, bps);
    }
}

void video_rate_ctrl_report_queue(video_rate_ctrl_handle_t h,
                                  uint32_t used, uint32_t capacity)
{
    if (h == NULL || s_lock == NULL || capacity == 0) {
        return;
    }
    int64_t now = esp_timer_get_time();

    /* Runs inside the sink dispatch loop, so never block the encoder task behind
     * whoever else holds the lock - the next frame carries the same information. */
    if (xSemaphoreTake(s_lock, 0) != pdTRUE) {
        return;
    }
    if (!h->in_use || !h->enabled || !s_agg.inited) {
        vrate_unlock();
        return;
    }
    bool was_stale = h->stale || vrate_is_stale(h, now);

    h->last_report_us = now;
    h->stale          = false;
    h->reports++;
    h->q_used = used;
    h->q_cap  = capacity;

    if (was_stale) {
        /* Re-admit immediately; the cadence gate below must not delay it. */
        vrate_rearbitrate(now);
    }

    if (now < h->q_next_eval_us) {
        /* Between evaluations: the sample is recorded and the liveness stamp
         * refreshed, but the ladder is left alone. This cadence gate is what
         * rate-limits step-downs on an integrating signal. */
        vrate_unlock();
        return;
    }
    h->q_next_eval_us = now + (int64_t)QUEUE_EVAL_MS * 1000;

    uint32_t occ = (used * 100u) / capacity;
    if (!h->q_primed) {
        h->q_ema_pct = occ;
        h->q_delta   = 0;
        h->q_primed  = true;
    } else {
        uint32_t prev = h->q_ema_pct;
        h->q_ema_pct  = (prev * (EMA_A_DEN - EMA_A_NUM) + occ * EMA_A_NUM) / EMA_A_DEN;
        h->q_delta    = (int32_t)h->q_ema_pct - (int32_t)prev;
    }
    h->load_pct = (h->q_ema_pct * 100u) / QUEUE_CONGEST_PCT;

    /* Entry uses the trend-projected level, severity uses the smoothed level. */
    int32_t eff = (int32_t)h->q_ema_pct + QUEUE_TREND_GAIN * h->q_delta;

    bool moved = false, up = false;
    if (eff >= QUEUE_CONGEST_PCT) {
        uint32_t jump = 1;
        if (h->q_ema_pct >= QUEUE_JUMP3_PCT) {
            jump = 3;
        } else if (h->q_ema_pct >= QUEUE_JUMP2_PCT) {
            jump = 2;
        }
        moved = vrate_step_down(h, jump);
    } else if (h->q_ema_pct <= QUEUE_HEALTHY_PCT && h->q_delta <= 0) {
        /* Shallow AND not growing. Requiring both is what stops the ladder from
         * climbing back into a queue that is already refilling. */
        moved = vrate_mark_healthy(h, now);
        up    = moved;
    } else {
        vrate_reset_healthy(h);
    }

    if (moved) {
        vrate_rearbitrate(now);
    }
    char     name[VRATE_NAME_LEN];
    strlcpy(name, h->name, sizeof(name));
    uint32_t lvl = h->level, fps = h->target_fps, bps = h->target_bitrate_bps;
    uint32_t occ_ema = h->q_ema_pct;
    int32_t  delta   = h->q_delta;
    vrate_unlock();

    if (moved) {
        ESP_LOGI(TAG, "'%s' %s (queue=%" PRIu32 "%% d=%+" PRId32 "): L%" PRIu32
                 " (%" PRIu32 "fps/%" PRIu32 "bps)",
                 name, up ? "recovering" : "congested", occ_ema, delta, lvl, fps, bps);
    }
}

void video_rate_ctrl_report_overflow(video_rate_ctrl_handle_t h)
{
    if (h == NULL || s_lock == NULL) {
        return;
    }
    bool     moved = false;
    char     name[VRATE_NAME_LEN];
    uint32_t lvl = 0, fps = 0, bps = 0;

    int64_t now = esp_timer_get_time();
    vrate_lock();
    if (!h->in_use || !h->enabled || !s_agg.inited) {
        vrate_unlock();
        return;
    }
    bool was_stale = h->stale || vrate_is_stale(h, now);

    h->last_report_us = now;
    h->stale          = false;
    h->reports++;

    /* Data was actually lost. Treat the queue as full regardless of what the
     * last sample said, and re-arm the cadence so the drop is not immediately
     * followed by another one before it can take effect. */
    h->q_primed       = true;
    h->q_ema_pct      = 100;
    h->q_delta        = 0;
    h->load_pct       = (100u * 100u) / QUEUE_CONGEST_PCT;
    h->q_next_eval_us = now + (int64_t)QUEUE_EVAL_MS * 1000;

    moved = vrate_step_down(h, 2);
    if (moved || was_stale) {
        vrate_rearbitrate(now);
    }
    strlcpy(name, h->name, sizeof(name));
    lvl = h->level; fps = h->target_fps; bps = h->target_bitrate_bps;
    vrate_unlock();

    if (moved) {
        ESP_LOGW(TAG, "'%s' overflow, data lost: L%" PRIu32 " (%" PRIu32 "fps/%" PRIu32 "bps)",
                 name, lvl, fps, bps);
    }
}

void video_rate_ctrl_set_network_ceiling_bps(video_rate_ctrl_handle_t h, uint32_t bps)
{
    if (h == NULL || s_lock == NULL) {
        return;
    }
    bool     moved = false;
    char     name[VRATE_NAME_LEN];
    uint32_t lvl = 0, fps = 0, tbps = 0;

    int64_t now = esp_timer_get_time();
    vrate_lock();
    if (!h->in_use) {
        vrate_unlock();
        return;
    }
    /* Recorded even while disabled, so it is already in force if this transport
     * later opts in - but not acted on, since a disabled controller must not
     * touch fps or bitrate. */
    h->net_ceiling_bps = bps;
    if (h->enabled && s_agg.inited) {
        /* Re-clamp to the new ceiling. If the estimate dropped below the current
         * rung this snaps down the ladder now, instead of waiting for the local
         * congestion loop to notice. */
        uint32_t prev = h->level;
        vrate_apply_level(h, h->level);
        moved = (h->level != prev);
        if (moved) {
            vrate_rearbitrate(now);
        }
    }
    strlcpy(name, h->name, sizeof(name));
    lvl = h->level; fps = h->target_fps; tbps = h->target_bitrate_bps;
    vrate_unlock();

    if (moved) {
        ESP_LOGI(TAG, "'%s' net ceiling=%" PRIu32 "bps: L%" PRIu32
                 " (%" PRIu32 "fps/%" PRIu32 "bps)", name, bps, lvl, fps, tbps);
    }
}

/* ------------------------------------------------------------------------- */
/* Encoder-task side: lock-free                                               */
/* ------------------------------------------------------------------------- */

/* Staleness is the one state change no report drives, so it has to be noticed
 * from here. Costs a load and a compare per frame in the common case. */
static void vrate_expire_if_due(int64_t now)
{
    if (now < atomic_load(&s_stale_check_us)) {
        return;
    }
    if (s_lock == NULL || xSemaphoreTake(s_lock, 0) != pdTRUE) {
        return;   /* Someone is mid-update; they will re-arbitrate anyway. */
    }
    /* Names of controllers that just went stale, logged after the unlock -
     * ESP_LOG can block on the stdout lock and this runs on the encoder task. */
    const char *expired[CONFIG_VIDEO_RATE_CTRL_MAX];
    int n_expired = 0;

    for (int i = 0; i < CONFIG_VIDEO_RATE_CTRL_MAX; i++) {
        struct video_rate_ctrl_s *h = &s_ctrl[i];
        if (h->in_use && h->enabled && !h->stale && vrate_is_stale(h, now)) {
            h->stale = true;
            expired[n_expired++] = h->name;   /* Stable: the slot stays in_use. */
        }
    }
    vrate_rearbitrate(now);
    vrate_unlock();

    for (int i = 0; i < n_expired; i++) {
        ESP_LOGW(TAG, "'%s' stopped reporting, dropped from arbitration", expired[i]);
    }
}

bool video_rate_ctrl_should_encode(void)
{
    vrate_expire_if_due(esp_timer_get_time());

    uint32_t target = atomic_load(&s_pub_target_fps);
    if (target == 0) {
        s_accum = 0;   /* native rate: nothing is skipped */
        return true;
    }
    uint32_t cam = atomic_load(&s_pub_camera_fps);
    if (cam == 0 || target >= cam) {
        return true;
    }
    /* Bresenham-style even distribution: keep target frames out of every cam
     * frames. Deliberately NOT reset when the target changes - accum is always
     * in [0, cam), so a new increment stays correct, and resetting would emit a
     * burst at every rung change. */
    s_accum += target;
    if (s_accum >= cam) {
        s_accum -= cam;
        return true;
    }
    return false;
}

uint32_t video_rate_ctrl_pull_bitrate_bps(void)
{
    vrate_expire_if_due(esp_timer_get_time());

    return atomic_exchange(&s_pending_bitrate, 0);
}

esp_err_t video_rate_ctrl_get_stats(video_rate_ctrl_stats_t *out, size_t max, size_t *count,
                                    uint32_t *enc_fps, uint32_t *enc_bps)
{
    if (out == NULL || count == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *count = 0;
    if (s_lock == NULL) {
        if (enc_fps) { *enc_fps = 0; }
        if (enc_bps) { *enc_bps = 0; }
        return ESP_OK;
    }

    int64_t now = esp_timer_get_time();
    vrate_lock();
    for (int i = 0; i < CONFIG_VIDEO_RATE_CTRL_MAX && *count < max; i++) {
        struct video_rate_ctrl_s *h = &s_ctrl[i];
        if (!h->in_use) {
            continue;
        }
        video_rate_ctrl_stats_t *o = &out[(*count)++];
        o->name               = h->name;
        o->enabled            = h->enabled;
        o->stale              = h->enabled && vrate_is_stale(h, now);
        o->level              = h->level;
        o->target_fps         = h->target_fps;
        o->target_bitrate_bps = h->target_bitrate_bps;
        o->load_pct           = h->load_pct;
        o->net_ceiling_bps    = h->net_ceiling_bps;
        o->reports            = h->reports;
    }
    if (enc_fps) {
        uint32_t t = atomic_load(&s_pub_target_fps);
        *enc_fps = t ? t : s_agg.camera_fps;
    }
    if (enc_bps) {
        *enc_bps = s_agg.inited ? s_agg.applied_bitrate : 0;
    }
    vrate_unlock();
    return ESP_OK;
}
