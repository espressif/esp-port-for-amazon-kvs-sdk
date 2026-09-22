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
#include "video_rate_ctrl_priv.h"

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

/* Where a controller starts when fps is NOT ours to lower (UVC H.264
 * passthrough). START_LEVEL's reasoning inverts there.
 *
 * Mid-ladder exists to avoid slamming full bitrate into a link that has not
 * settled. But on a camera we do not own, L0 is not "full quality we might not
 * afford" - it is the rate the camera is ALREADY streaming at when init() reads
 * its committed value. Starting at L0 changes nothing about the link; starting
 * at L3 is itself the intervention, a 50% bitrate cut applied before any
 * congestion has been observed. And a camera holding bits-per-pixel roughly
 * constant answers a bitrate cut by shedding frame rate, so that opening move
 * costs frames for nothing. Descend on evidence instead. */
#define START_LEVEL_FPS_FIXED 0

/* fps adaptation floor. 12 (not 15): on the contended 2.4 GHz uplink a single
 * frame can take ~70 ms to send, and a 15-fps budget (67 ms) stays under water,
 * so the controller can't actually catch up. 12 fps = ~83 ms budget gives the
 * headroom to match those sends. The ladder never schedules below this.
 *
 * Kconfig-driven: a consumer with a hard frame-rate requirement (an archive upload that
 * must stay at 20 fps) raises it and gives up quality instead of smoothness. */
#ifndef CONFIG_VIDEO_RATE_CTRL_FPS_FLOOR
#define CONFIG_VIDEO_RATE_CTRL_FPS_FLOOR 12
#endif
#define FPS_FLOOR            CONFIG_VIDEO_RATE_CTRL_FPS_FLOOR

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

/* Minimum bitrate for a RESOLUTION, in bits per pixel per second and independent of
 * frame rate: the point below which the picture stops being worth sending at all, however
 * few frames of it there are.
 *
 * This is the floor a UVC camera enforces on its own behalf - the test camera clamps to
 * ~256 kbps at 720p and the ladder learns that through
 * video_rate_ctrl_set_min_bitrate_bps(). On the CSI path we ARE the encoder, so nobody
 * pushes back and the only unconditional floor was MIN_BITRATE_BPS, a flat 100 kbps that
 * means something entirely different at 320x240 than at 1080p. At 720p it let the ladder
 * ask for 157 kbps and the encoder answered with ~3 KB key frames: technically a stream,
 * not usefully a picture.
 *
 * 0.28 bps/px puts 720p at 258 kbps and 1080p at 581 kbps, which matches where the UVC
 * camera independently clamps and sits just above the 254 kbps/720p point measured as the
 * bottom of usable. Deliberately NOT scaled by fps: fewer frames of mush is still mush,
 * and the fps-scaled question - can a pinned rate be paid for - is what PINNED_BPP below
 * answers. A deployment that knows better can raise the floor further with
 * video_rate_ctrl_set_min_bitrate_bps(); the two are combined with max(). */
#ifndef CONFIG_VIDEO_RATE_CTRL_MIN_MBPS_PER_PX
#define CONFIG_VIDEO_RATE_CTRL_MIN_MBPS_PER_PX 280
#endif
#define RES_FLOOR_MBPS_PER_PX  CONFIG_VIDEO_RATE_CTRL_MIN_MBPS_PER_PX

/* Bits per pixel per frame below which a PINNED frame rate cannot be honoured.
 *
 * Holding the frame rate only works while the bitrate can still pay for that many
 * frames. Below this the encoder runs out of budget at maximum QP and starts
 * discarding frames anyway - and because how many it has to discard depends on the
 * scene, the result is not a lower frame rate but an unstable one. Measured on
 * ESP32-S31 at 720p with the sensor pinned at 30 fps: 254 kbps (0.0092 bpp) gave
 * 15-22 fps, stdev 1.6, with the upload queue permanently full; 520 kbps (0.019 bpp)
 * gave a rock-steady 29-30. At 1080p, ~900 kbps (0.014 bpp) held 29-30.
 *
 * 0.015 sits just under the two stable points and well above the unstable one, and
 * puts the floor at 415 kbps for 720p30 and 933 kbps for 1080p30. */
#define PINNED_BPP_NUM       15
#define PINNED_BPP_DEN       1000

/* How long a rung that congested stays barred from recovery, and how long after a
 * forced pin release before the pin may be re-asserted.
 *
 * The bar is TCP's ssthresh. Without it recovery climbs straight back into the rung
 * that just collapsed: measured at 1080p, L0 saturated the uplink reproducibly and
 * the ladder re-entered it every time, a ~5-6 minute cycle costing two stream
 * restarts and a queue excursion to ~650 buffers per lap. One rung of the bar decays
 * per interval, so a link that genuinely improves is still found - just not
 * immediately, and not repeatedly. */
#define PROBE_BAR_DECAY_MS   60000
#define PIN_REASSERT_DWELL_MS 30000

/* Recovery evaluation cadence.
 *
 * Deliberately NOT the actuation period. That period exists so a step-DOWN cannot be
 * issued again before the previous one could have taken effect; an upward move the
 * actuator has not applied yet costs nothing, and the actuator enforces its own
 * minimum interval anyway. Charging recovery the full period measured 120 s for a
 * single rung on a camera whose only lever is a stream restart - 7-14 minutes from
 * the floor back to the top, which reads to a user as "it never recovers". */
#define RECOVER_EVAL_MS      3000

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

    /* Best (lowest-index) rung recovery may re-enter; 0 = unrestricted. Raised to
     * L+1 whenever rung L congests, and decayed one rung per PROBE_BAR_DECAY_MS. */
    uint32_t probe_bar_level;
    int64_t  probe_bar_us;

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
    /* What the encoder was last TOLD. Named for the request, not the result,
     * because on UVC passthrough the two differ: see committed_bitrate. This is
     * what drives change detection on the pull path. */
    uint32_t requested_bitrate;
    /* What the encoder said it actually took, 0 until something reports one.
     * Never feeds change detection - a camera that answers every request with
     * the same number would otherwise look like a permanent pending change. */
    uint32_t committed_bitrate;
    /* Cleared when a bitrate change is discovered not to reach the encoder at
     * all. With fps_actuable false as well, no controller has a lever and the
     * ladder is frozen rather than left issuing decisions that reach nothing. */
    bool     bitrate_actuable;
    /* How long a change takes to reach the encoder. The level front-ends must not
     * re-evaluate faster than this or they step down repeatedly on a signal that
     * cannot have responded yet. 0 = the encoder is driven directly. Measured without
     * it on a restart-actuated camera: all eight rungs in three seconds (queue 33% ->
     * 39%), re-evaluating every 500 ms while the first step had not reached the camera. */
    uint32_t actuation_period_ms;
    /* What the operator asked for, versus what is actually in effect. They differ
     * when the bitrate has fallen below the bits-per-pixel floor: the pin is then
     * released, because holding it there produces an UNSTABLE frame rate rather than
     * a lower one, and re-asserted once the arbitrated bitrate can pay for it again.
     * Keeping the request separate is what makes that reversible. */
    bool     fps_pin_wanted;
    bool     fps_pinned;
    int64_t  pin_released_us;
} s_agg;

/* Published to the encoder task. Written under s_lock, read lock-free - which is
 * what keeps the per-frame path off the contention graph entirely. */
static _Atomic uint32_t s_pub_target_fps;   /* 0 = native, nothing to throttle */
static _Atomic uint32_t s_pub_camera_fps;
static _Atomic uint32_t s_pending_bitrate;  /* 0 = nothing to apply */
/* Frame-rate pin change waiting for the capture task: -1 none, 0 release, 1 assert.
 * Published rather than called back, because applying it means restarting the camera
 * stream and that may only happen where no capture buffer is held. */
static _Atomic int32_t s_pending_pin = -1;
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

static void vrate_expire_if_due(int64_t now);

/* The starting rung depends on whether fps is a lever; see the two constants. */
/* Cadence for the level (queue) front-end: never faster than the encoder can
 * actually respond. See video_rate_ctrl_set_actuation_period_ms(). */
static int64_t vrate_level_eval_period_us(void)
{
    uint32_t ms = QUEUE_EVAL_MS;
    if (s_agg.actuation_period_ms > ms) {
        ms = s_agg.actuation_period_ms;
    }
    return (int64_t) ms * 1000;
}

static int64_t vrate_recover_eval_period_us(void)
{
    uint32_t ms = (RECOVER_EVAL_MS > QUEUE_EVAL_MS) ? RECOVER_EVAL_MS : QUEUE_EVAL_MS;
    return (int64_t) ms * 1000;
}

static uint32_t vrate_start_level(void)
{
    return s_agg.fps_actuable ? START_LEVEL : START_LEVEL_FPS_FIXED;
}

/* Bitrate below which a pinned frame rate stops being deliverable. Computed
 * independently of whether the pin is currently in effect, because both the release
 * and the re-assert decision need to compare against it. */
static uint32_t vrate_pinned_floor_raw(void)
{
    const uint64_t px  = (uint64_t) s_agg.width * (uint64_t) s_agg.height;
    const uint32_t fps = s_agg.camera_fps ? s_agg.camera_fps : 30u;
    if (px == 0) {
        return 0;
    }
    return (uint32_t)((px * (uint64_t) fps * PINNED_BPP_NUM) / PINNED_BPP_DEN);
}

/* Lowest bitrate at which this resolution is worth encoding at all. Frame-rate
 * independent - see RES_FLOOR_MBPS_PER_PX. */
static uint32_t vrate_resolution_floor_bps(void)
{
    const uint64_t px = (uint64_t) s_agg.width * (uint64_t) s_agg.height;
    if (px == 0) {
        return 0;   /* Resolution not declared yet; nothing to scale from. */
    }
    return (uint32_t)((px * RES_FLOOR_MBPS_PER_PX) / 1000u);
}

/* The floor every rung is clamped up to: the encoder's absolute minimum, the resolution
 * floor, and - while the frame rate is actually pinned - the bits-per-pixel floor that
 * pinning needs. Whichever is highest wins. */
static uint32_t vrate_floor_bps(void)
{
    uint32_t f = s_agg.min_bitrate_bps;
    const uint32_t rf = vrate_resolution_floor_bps();
    if (rf > f) {
        f = rf;
    }
    if (s_agg.fps_pinned) {
        const uint32_t pf = vrate_pinned_floor_raw();
        if (pf > f) {
            f = pf;
        }
    }
    if (s_agg.max_bitrate_bps && f > s_agg.max_bitrate_bps) {
        f = s_agg.max_bitrate_bps;
    }
    return f;
}

static uint32_t vrate_rung_fps(uint32_t level);

static uint32_t vrate_rung_bitrate(uint32_t level)
{
    uint32_t b = (uint32_t)(((uint64_t)s_agg.max_bitrate_bps * LADDER[level].pct) / 100);
    const uint32_t flr = vrate_floor_bps();
    return (b < flr) ? flr : b;
}

/* Deepest rung worth entering: the last one that still differs from the rung above it in
 * SOMETHING the encoder can see.
 *
 * Rungs that clamp onto the same floor carry the same bitrate, so descending into them
 * changes nothing - measured on a camera whose floor was 256 kbps: L5, L6 and L7 all read
 * 256000, and the overflow path dived through all three in 4 seconds, then took minutes to
 * climb back out of rungs that had never bought anything.
 *
 * This used to exempt the fps_actuable path entirely, on the grounds that a deeper rung
 * still lowers fps even at an identical bitrate. True for some of them and not others: the
 * ladder's bottom three rungs are all 12 fps, so once the resolution floor catches their
 * bitrates they become indistinguishable from each other and the same dive happens on the
 * CSI path. Comparing both dimensions covers both cases and needs no special-casing. */
static uint32_t vrate_deepest_useful_level(void)
{
    uint32_t last = LADDER_N - 1;
    while (last > 0 &&
           vrate_rung_fps(last)     == vrate_rung_fps(last - 1) &&
           vrate_rung_bitrate(last) == vrate_rung_bitrate(last - 1)) {
        last--;
    }
    return last;
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
    const uint32_t flr = vrate_floor_bps();
    return (eff < flr) ? flr : eff;
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
    const uint32_t deepest = vrate_deepest_useful_level();
    if (level > deepest) {
        level = deepest;
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
    /* The bar is evidence about the LINK, not about the signal, so a front-end reset
     * (enable, re-admission after staleness) keeps it. It is cleared only in init(),
     * where the encoder itself is being redescribed. */
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
    /* Nothing to steer: neither column reaches the encoder, so publishing a new
     * operating point would only misreport one. Freeze at whatever is in force
     * and let get_stats() say why. */
    if (s_agg.inited && !s_agg.fps_actuable && !s_agg.bitrate_actuable) {
        /* Park the staleness deadline too: nothing here will ever change, and without
         * this the encoder task takes the lock on every frame once it has passed. */
        atomic_store(&s_stale_check_us, INT64_MAX);
        return;
    }

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
        if (s_agg.requested_bitrate != s_agg.max_bitrate_bps) {
            s_agg.requested_bitrate = s_agg.max_bitrate_bps;
            atomic_store(&s_pending_bitrate, s_agg.max_bitrate_bps);
        }
        atomic_store(&s_stale_check_us, INT64_MAX);
        return;
    }

    /* Re-assert the pin here rather than in the ladder: whether the frame rate is
     * affordable is a property of the ARBITRATED bitrate - the one the single encoder
     * will actually run at - not of any one controller's rung. Release is the ladder's
     * job (it needs evidence the floor cannot be carried); this only needs headroom.
     *
     * The dwell stops it flapping: without it the re-assert fires on the same
     * arbitration pass as the release, since the rung it was released at still reads
     * exactly the floor bitrate. */
    if (s_agg.fps_pin_wanted && !s_agg.fps_pinned) {
        const uint32_t pf = vrate_pinned_floor_raw();
        if (min_bps >= pf &&
                (now - s_agg.pin_released_us) >= (int64_t) PIN_REASSERT_DWELL_MS * 1000) {
            s_agg.fps_pinned = true;
            atomic_store(&s_pending_pin, 1);
        }
    }

    atomic_store(&s_pub_target_fps, min_fps);
    if (min_bps != s_agg.requested_bitrate) {
        s_agg.requested_bitrate = min_bps;
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
    /* Nothing this controller decides can reach the encoder, so moving its rung
     * would only print a descent that is not happening - which is how
     * `sink-stats` came to show rung 7 / 102400 bps beside an encoder still
     * running at 304000. Hold position instead. */
    if (s_agg.inited && !s_agg.fps_actuable && !s_agg.bitrate_actuable) {
        return false;
    }

    const int64_t now = esp_timer_get_time();

    /* This rung just congested, so recovery must not walk straight back into it.
     * Recorded before any early return: the fact is true even when there is nowhere
     * left to descend to, and that is exactly the case where it matters most. */
    if (h->level + 1 > h->probe_bar_level) {
        h->probe_bar_level = (h->level + 1 <= LADDER_N - 1) ? h->level + 1 : LADDER_N - 1;
    }
    h->probe_bar_us = now;

    if (h->level >= vrate_deepest_useful_level()) {
        /* At the bottom of what the current floor allows. If that floor is the
         * bits-per-pixel one, the pin is the thing standing in the way - and holding
         * it here is actively worse than giving it up, because the encoder cannot fit
         * the frames it is still being handed and sheds a scene-dependent number of
         * them. Give up the frame rate and let the ladder keep going; a stable 12 fps
         * beats an unstable 18. */
        if (s_agg.fps_pinned) {
            s_agg.fps_pinned      = false;
            s_agg.pin_released_us = now;
            atomic_store(&s_pending_pin, 0);
        } else {
            return false;
        }
    }
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
        /* A SNAPSHOT of the report counter, not a counter of its own. The gate is
         * meant to stop a controller that observed nothing from claiming a rung, so
         * it has to count actual reports - and this function is called once per
         * ladder EVALUATION, which is a completely different rate. The level
         * front-end evaluates on a cadence that now tracks how long actuation takes
         * (seconds, or tens of seconds on a camera where a stream restart is the
         * only lever), so counting calls here made recovery need
         * RECOVER_MIN_REPORTS whole cadences per rung: measured at 8 x 30 s = 240 s
         * for one step, ~28 min from the floor back to the top, i.e. never. */
        h->healthy_reports  = h->reports;
    }

    if (now - h->healthy_since_us < (int64_t)RECOVER_DWELL_MS * 1000 ||
        (h->reports - h->healthy_reports) < RECOVER_MIN_REPORTS) {
        return false;
    }
    h->healthy_since_us = 0;
    h->healthy_reports  = 0;

    if (h->level == 0) {
        return false;
    }

    /* Decay the bar one rung at a time, so a link that has genuinely improved is
     * eventually re-probed - but only after it has been quiet long enough that the
     * probe is not just the previous collapse repeating. */
    if (h->probe_bar_level > 0 &&
            (now - h->probe_bar_us) >= (int64_t) PROBE_BAR_DECAY_MS * 1000) {
        h->probe_bar_level--;
        h->probe_bar_us = now;
    }
    const uint32_t bar = h->probe_bar_level;
    if (h->level <= bar) {
        return false;   /* already as high as the evidence supports */
    }

    /* Jump, rather than single-step. Where fps is fixed, several deep rungs collapse
     * onto the same floor bitrate, and a single step through them spends a whole
     * evaluation cadence producing nothing the encoder can see. Only from a queue
     * that is completely empty (q_primed says this IS a queue controller - the latency
     * front-end never writes q_ema_pct, so 0 there means nothing), and never past the
     * bar. */
    uint32_t nl = h->level - 1;
    if (h->q_primed && h->q_ema_pct == 0 && h->level >= bar + 3) {
        nl = h->level - 2;
    }
    if (nl < bar) {
        nl = bar;
    }

    uint32_t prev = h->level;
    vrate_apply_level(h, nl);
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
    s_agg.requested_bitrate = max_bitrate_bps;
    /* Assumed until proven otherwise: the refusal is only observable at runtime,
     * on the first commit that comes back with a different number. */
    s_agg.bitrate_actuable  = true;
    s_agg.committed_bitrate = 0;
    /* Set by the grabber right after this, if the encoder is one that needs it. */
    s_agg.actuation_period_ms = 0;
    /* The pin follows the request again: this is a fresh encoder configuration, so any
     * earlier forced release was about a link condition that is no longer evidenced.
     * fps_pin_wanted itself is deliberately NOT cleared - it is the operator's choice
     * and outlives a stream restart, which is exactly what a restart-based bitrate
     * actuator performs on every rung change. */
    s_agg.fps_pinned      = s_agg.fps_pin_wanted;
    s_agg.pin_released_us = 0;
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
        /* Cleared only here: the encoder is being redescribed (possibly at a new
         * resolution and a new max), so which rungs previously congested says nothing
         * useful about the new ladder. */
        h->probe_bar_level = 0;
        h->probe_bar_us    = now;
        if (h->enabled) {
            vrate_apply_level(h, vrate_start_level());
            seeded++;
        }
    }
    vrate_rearbitrate(now);
    uint32_t cam    = s_agg.camera_fps;
    uint32_t flr    = vrate_floor_bps();
    uint32_t pinflr = s_agg.fps_pinned ? vrate_pinned_floor_raw() : 0;
    uint32_t deep   = vrate_deepest_useful_level();
    vrate_unlock();

    ESP_LOGI(TAG, "init: cam_fps=%" PRIu32 "%s res=%" PRIu32 "x%" PRIu32
             " max=%" PRIu32 "bps floor=%" PRIu32 "bps deepest=L%" PRIu32
             ", %" PRIu32 " controller(s) adapting",
             cam, fps_actuable ? "" : " (fixed - bitrate only)",
             width, height, max_bitrate_bps, flr, deep, seeded);
    if (pinflr) {
        ESP_LOGI(TAG, "frame rate pinned at %" PRIu32 " fps: will not descend below %"
                      PRIu32 " bps (%d/%d bits per pixel per frame), and gives the pin up "
                      "rather than deliver an unstable rate below it",
                 cam, pinflr, PINNED_BPP_NUM, PINNED_BPP_DEN);
    }
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
            vrate_apply_level(h, vrate_start_level());
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

esp_err_t video_rate_ctrl_find(const char *name, video_rate_ctrl_handle_t *out)
{
    if (name == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = NULL;
    if (s_lock == NULL) {
        return ESP_ERR_NOT_FOUND;   /* Nothing has been created yet. */
    }
    vrate_lock();
    for (int i = 0; i < CONFIG_VIDEO_RATE_CTRL_MAX; i++) {
        struct video_rate_ctrl_s *h = &s_ctrl[i];
        if (h->in_use && strcmp(h->name, name) == 0) {
            *out = h;
            break;
        }
    }
    vrate_unlock();
    return (*out != NULL) ? ESP_OK : ESP_ERR_NOT_FOUND;
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
        vrate_apply_level(h, vrate_start_level());
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
    /* Armed provisionally with the step-down cadence; the branch below re-arms it
     * with the recovery cadence when it turns out we are climbing, not falling. */
    h->q_next_eval_us = now + vrate_level_eval_period_us();

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
        h->q_next_eval_us = now + vrate_recover_eval_period_us();
    } else {
        vrate_reset_healthy(h);
        h->q_next_eval_us = now + vrate_recover_eval_period_us();
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
    bool     moved = false, held = false;
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

    /* Data was actually lost, so record the queue as full whatever the last sample
     * said. Nothing here is discarded if the step below is held off: the next
     * evaluation, from either front-end, sees 100% and steps down with severity. */
    h->q_primed  = true;
    h->q_ema_pct = 100;
    h->q_delta   = 0;
    h->load_pct  = (100u * 100u) / QUEUE_CONGEST_PCT;

    /* Overflow bypasses the SMOOTHING - no EMA to wait for, no trend to agree - but
     * NOT the actuation cadence. A step the encoder has not yet had the chance to
     * apply cannot be the reason the queue is still overflowing, so treating the loss
     * as fresh evidence against it just spends the ladder for nothing. Measured on a
     * camera whose only lever is a stream restart, 30 s apart: two overflows 3 s
     * apart took it L3 -> L5 -> L7 while the wire was still at its original rate, and
     * the floor it landed on delivered 12 fps instead of 30.
     *
     * The deadline moves only when a step actually happens, so a sink that keeps
     * overflowing still descends - one step per cadence rather than all of them
     * before the first can land. */
    if (now >= h->q_next_eval_us) {
        h->q_next_eval_us = now + vrate_level_eval_period_us();
        moved = vrate_step_down(h, 2);
    } else {
        held = true;
    }

    if (moved || was_stale) {
        vrate_rearbitrate(now);
    }
    strlcpy(name, h->name, sizeof(name));
    lvl = h->level; fps = h->target_fps; bps = h->target_bitrate_bps;
    vrate_unlock();

    if (moved) {
        ESP_LOGW(TAG, "'%s' overflow, data lost: L%" PRIu32 " (%" PRIu32 "fps/%" PRIu32 "bps)",
                 name, lvl, fps, bps);
    } else if (held) {
        /* Deliberately quiet: the transport logs the loss itself, and this only says
         * the ladder is waiting on an actuation it has already asked for. */
        ESP_LOGD(TAG, "'%s' overflow, data lost: holding L%" PRIu32 " until the last "
                      "change can reach the encoder", name, lvl);
    }
}

/* Shared body. @c set_request distinguishes the two callers, and the distinction
 * matters more than it looks: when the LADDER forces a release, the actuator reports
 * that back here, and letting that report also clear fps_pin_wanted destroys the
 * operator's choice - after which nothing ever re-asserts the pin, because the
 * re-assert is conditional on it. Measured exactly that: one forced release and
 * `sink-stats` stopped mentioning the pin at all for the rest of the session. */
static void vrate_set_pin(bool pinned, bool set_request)
{
    if (vrate_lock_init() != ESP_OK) {
        return;
    }
    vrate_lock();
    const bool was = s_agg.fps_pinned;
    if (set_request) {
        s_agg.fps_pin_wanted  = pinned;
        s_agg.pin_released_us = 0;
    } else if (!pinned) {
        /* Start the anti-flap dwell from the moment the release actually took effect. */
        s_agg.pin_released_us = esp_timer_get_time();
    }
    s_agg.fps_pinned = pinned;
    if (s_agg.inited && was != pinned) {
        /* The floor moved, so rungs that were clamped to it are now wrong. Re-apply
         * every enabled controller before re-arbitrating. */
        const int64_t now = esp_timer_get_time();
        for (int i = 0; i < CONFIG_VIDEO_RATE_CTRL_MAX; i++) {
            struct video_rate_ctrl_s *h = &s_ctrl[i];
            if (h->in_use && h->enabled) {
                vrate_apply_level(h, h->level);
            }
        }
        vrate_rearbitrate(now);
    }
    const bool  wanted = s_agg.fps_pin_wanted;
    const uint32_t flr = s_agg.inited ? vrate_floor_bps() : 0;
    const uint32_t pf  = vrate_pinned_floor_raw();
    const uint32_t dp  = s_agg.inited ? vrate_deepest_useful_level() : 0;
    vrate_unlock();
    /* No s_pending_pin store: the caller is the one that owns the actuator, and it is
     * telling US what it just did. Publishing back would loop. */

    if (was != pinned || set_request) {
        ESP_LOGI(TAG, "frame-rate pin %s (requested %s): floor %" PRIu32 " bps, pinned needs %"
                      PRIu32 " bps, ladder stops at L%" PRIu32,
                 pinned ? "held" : "off", wanted ? "on" : "off", flr, pf, dp);
    }
}

void video_rate_ctrl_set_fps_pinned(bool pinned, bool operator_request)
{
    vrate_set_pin(pinned, operator_request);
}

int video_rate_ctrl_pull_fps_pin(void)
{
    return (int) atomic_exchange(&s_pending_pin, -1);
}

void video_rate_ctrl_set_min_bitrate_bps(uint32_t bps)
{
    if (s_lock == NULL) {
        return;
    }
    vrate_lock();
    uint32_t flr = (bps > MIN_BITRATE_BPS) ? bps : MIN_BITRATE_BPS;
    if (s_agg.max_bitrate_bps && flr > s_agg.max_bitrate_bps) {
        flr = s_agg.max_bitrate_bps;
    }
    const bool changed = (s_agg.min_bitrate_bps != flr);
    s_agg.min_bitrate_bps = flr;
    /* Re-apply every enabled controller's level so a rung that now sits below the
     * floor reports the floor instead of a target nothing will honour. */
    if (changed) {
        const int64_t now = esp_timer_get_time();
        for (int i = 0; i < CONFIG_VIDEO_RATE_CTRL_MAX; i++) {
            if (s_ctrl[i].in_use) {
                vrate_apply_level(&s_ctrl[i], s_ctrl[i].level);
            }
        }
        vrate_rearbitrate(now);
    }
    vrate_unlock();

    if (changed) {
        ESP_LOGI(TAG, "bitrate floor raised to %" PRIu32 " bps (the encoder refuses lower)", flr);
    }
}

void video_rate_ctrl_set_actuation_period_ms(uint32_t ms)
{
    if (s_lock == NULL) {
        return;
    }
    vrate_lock();
    const bool changed = (s_agg.actuation_period_ms != ms);
    s_agg.actuation_period_ms = ms;
    vrate_unlock();
    if (changed) {
        ESP_LOGI(TAG, "actuation period %" PRIu32 " ms: the level front-end will not "
                      "step more often than that", ms);
    }
}

void video_rate_ctrl_report_committed_bitrate_bps(uint32_t bps)
{
    if (s_lock == NULL) {
        return;
    }
    vrate_lock();
    s_agg.committed_bitrate = bps;
    vrate_unlock();
}

void video_rate_ctrl_set_bitrate_actuable(bool actuable)
{
    if (s_lock == NULL) {
        return;
    }
    vrate_lock();
    const bool changed = (s_agg.bitrate_actuable != actuable);
    s_agg.bitrate_actuable = actuable;
    const bool inert = s_agg.inited && !s_agg.fps_actuable && !actuable;
    vrate_unlock();

    /* Logged outside the lock: ESP_LOG can block on stdout, and this is called
     * from the encoder task that also dispatches to every sink. */
    if (changed && inert) {
        ESP_LOGW(TAG, "no lever left on this encoder (fps fixed, bitrate not actuable) "
                      "- controllers frozen, rate control is inert");
    } else if (changed && actuable) {
        ESP_LOGI(TAG, "bitrate is actuable again");
    }
}

esp_err_t video_rate_ctrl_get_encoder_stats(video_rate_ctrl_encoder_stats_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_lock == NULL) {
        memset(out, 0, sizeof(*out));
        return ESP_OK;
    }
    /* Third place staleness is noticed, and the only one on the restart actuator's path:
     * in restart mode the grabber never pulls a bitrate, it reads requested_bps from here.
     * Without this a sink that stopped reporting would keep the camera at its rung. */
    vrate_expire_if_due(esp_timer_get_time());
    vrate_lock();
    out->requested_bps    = s_agg.inited ? s_agg.requested_bitrate : 0;
    out->committed_bps    = s_agg.committed_bitrate;
    out->fps_actuable     = s_agg.fps_actuable;
    out->bitrate_actuable = s_agg.bitrate_actuable;
    out->fps_pin_wanted   = s_agg.fps_pin_wanted;
    out->fps_pinned       = s_agg.fps_pinned;
    out->pinned_floor_bps = s_agg.fps_pin_wanted ? vrate_pinned_floor_raw() : 0;
    out->floor_bps        = s_agg.inited ? vrate_floor_bps() : 0;
    out->deepest_level    = s_agg.inited ? vrate_deepest_useful_level() : 0;
    vrate_unlock();
    return ESP_OK;
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
        o->probe_bar_level    = h->probe_bar_level;
    }
    if (enc_fps) {
        uint32_t t = atomic_load(&s_pub_target_fps);
        *enc_fps = t ? t : s_agg.camera_fps;
    }
    if (enc_bps) {
        *enc_bps = s_agg.inited ? s_agg.requested_bitrate : 0;
    }
    vrate_unlock();
    return ESP_OK;
}
