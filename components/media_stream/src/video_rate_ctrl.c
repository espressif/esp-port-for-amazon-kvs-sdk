/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "video_rate_ctrl.h"
#include "esp_log.h"

static const char *TAG = "vrate";

/* Operating-point ladder.
 *
 * fps and bitrate are NOT tracked as two free counters — they're tied to a
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
    { 27, 100 },  /* L0 - full quality (uplink must prove it can take this) */
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

/* Sustained healthy reports before a one-rung recovery step. Recovery is
 * deliberately slower than degradation (AIMD: back off fast, recover slow). */
#define RECOVER_REPORTS      60

/* Absolute bitrate floor (bps). Matches esp_h264_hw_enc.c MIN_BITRATE — sub-
 * floor targets there are clamped, so the ladder's lowest rung must not ask for
 * less than the encoder will honor. The ladder L7 bitrate is clamped up to this. */
#define MIN_BITRATE_BPS      (100 * 1024)

static struct {
    bool     inited;
    uint32_t camera_fps;
    uint32_t width;
    uint32_t height;
    uint32_t max_bitrate_bps;
    uint32_t min_bitrate_bps;
    uint32_t level;            /* current ladder index */
    uint32_t target_fps;       /* derived from level */
    uint32_t target_bitrate_bps;
    uint32_t ema_ms;
    uint32_t good_run;
    uint32_t accum;            /* fractional frame-skip accumulator */
    uint32_t pending_bitrate;  /* 0 = nothing to apply */
    uint32_t net_ceiling_bps;  /* TWCC network ceiling; 0 = no network limit */
} s;

/* Effective bitrate ceiling = min(configured max, TWCC network ceiling),
 * never below the survival floor. */
static uint32_t vrate_eff_max(void)
{
    uint32_t eff = s.max_bitrate_bps;
    if (s.net_ceiling_bps != 0 && s.net_ceiling_bps < eff) {
        eff = s.net_ceiling_bps;
    }
    return (eff < s.min_bitrate_bps) ? s.min_bitrate_bps : eff;
}

static uint32_t vrate_rung_bitrate(uint32_t level)
{
    uint32_t b = (uint32_t)(((uint64_t)s.max_bitrate_bps * LADDER[level].pct) / 100);
    return (b < s.min_bitrate_bps) ? s.min_bitrate_bps : b;
}

static uint32_t vrate_rung_fps(uint32_t level)
{
    uint32_t f = LADDER[level].fps;
    if (f > s.camera_fps) {
        f = s.camera_fps;
    }
    return (f < FPS_FLOOR) ? FPS_FLOOR : f;
}

/* Highest-quality (lowest-index) rung whose bitrate fits the effective ceiling.
 * Recovery can't climb above this; a low TWCC estimate raises it, snapping us
 * down the ladder (dropping fps and bitrate together, as the rung dictates). */
static uint32_t vrate_ceiling_level(void)
{
    uint32_t eff = vrate_eff_max();
    for (uint32_t i = 0; i < LADDER_N; i++) {
        if (vrate_rung_bitrate(i) <= eff) {
            return i;
        }
    }
    return LADDER_N - 1;
}

/* Move to a ladder level, clamped to the network-affordable range. Sets the
 * derived fps and queues the bitrate for the grabber to apply. */
static void vrate_apply_level(uint32_t level)
{
    uint32_t floor_lvl = vrate_ceiling_level();
    if (level < floor_lvl) {
        level = floor_lvl;
    }
    if (level > LADDER_N - 1) {
        level = LADDER_N - 1;
    }
    s.level      = level;
    s.target_fps = vrate_rung_fps(level);
    uint32_t b   = vrate_rung_bitrate(level);
    if (b != s.target_bitrate_bps) {
        s.target_bitrate_bps = b;
        s.pending_bitrate    = b;
    }
}

void video_rate_ctrl_init(uint32_t camera_fps, uint32_t max_bitrate_bps,
                          uint32_t width, uint32_t height)
{
    s.camera_fps         = camera_fps ? camera_fps : 25;
    s.width              = width;
    s.height             = height;
    s.max_bitrate_bps    = max_bitrate_bps;
    s.min_bitrate_bps    = (max_bitrate_bps > MIN_BITRATE_BPS) ? MIN_BITRATE_BPS
                                                               : max_bitrate_bps;
    s.ema_ms             = 0;
    s.good_run           = 0;
    s.accum              = 0;
    s.net_ceiling_bps    = 0;
    s.target_bitrate_bps = 0;   /* force pending_bitrate on the first apply */
    s.pending_bitrate    = 0;
    s.inited             = true;

    vrate_apply_level(START_LEVEL);

    ESP_LOGI(TAG, "init: cam_fps=%u res=%ux%u max=%ubps floor=%ubps start=L%u(%ufps/%ubps)",
             (unsigned)s.camera_fps, (unsigned)width, (unsigned)height,
             (unsigned)max_bitrate_bps, (unsigned)s.min_bitrate_bps,
             (unsigned)s.level, (unsigned)s.target_fps, (unsigned)s.target_bitrate_bps);
}

void video_rate_ctrl_report_send_ms(uint32_t send_ms)
{
    if (!s.inited) {
        return;
    }
    s.ema_ms = (s.ema_ms * (EMA_A_DEN - EMA_A_NUM) + send_ms * EMA_A_NUM) / EMA_A_DEN;
    uint32_t budget_ms = 1000u / (s.target_fps ? s.target_fps : 1u);

    if (s.ema_ms * CONGEST_DEN > (uint32_t)budget_ms * CONGEST_NUM) {
        /* Congested — step down the ladder (fps + bitrate together). Severity-
         * scaled: the further over budget we are, the more rungs we drop in one
         * report, so a single sparse report during a bad spell still does real
         * work (reports arrive only as fast as frames send, which collapses
         * exactly when congested). Self-arrests as ema falls back under budget. */
        s.good_run = 0;
        uint32_t jump = 1;
        if (s.ema_ms >= (uint32_t)budget_ms * 3) {
            jump = 3;
        } else if (s.ema_ms >= (uint32_t)budget_ms * 2) {
            jump = 2;
        }
        if (s.level < LADDER_N - 1) {
            uint32_t nl = (s.level + jump < LADDER_N) ? s.level + jump : LADDER_N - 1;
            vrate_apply_level(nl);
            ESP_LOGI(TAG, "congested (ema=%ums > budget=%ums): L%u (%ufps/%ubps)",
                     (unsigned)s.ema_ms, (unsigned)budget_ms,
                     (unsigned)s.level, (unsigned)s.target_fps, (unsigned)s.target_bitrate_bps);
        }
    } else if (s.ema_ms * HEALTHY_DEN < (uint32_t)budget_ms * HEALTHY_NUM) {
        /* Healthy — step one rung up, gated so the ramp is slow and careful and
         * clamped to what the TWCC ceiling allows. fps tops out before bitrate
         * (fewer rungs to its cap), so motion smooths first and detail fills in. */
        if (++s.good_run >= RECOVER_REPORTS) {
            s.good_run = 0;
            if (s.level > 0) {
                uint32_t prev = s.level;
                vrate_apply_level(s.level - 1);
                if (s.level != prev) {
                    ESP_LOGI(TAG, "recovering: L%u (%ufps/%ubps)", (unsigned)s.level,
                             (unsigned)s.target_fps, (unsigned)s.target_bitrate_bps);
                }
            }
        }
    } else {
        s.good_run = 0;
    }
}

bool video_rate_ctrl_should_encode(void)
{
    if (!s.inited) {
        return true;
    }
    /* Bresenham-style even distribution: keep target_fps frames out of
     * every camera_fps frames. */
    s.accum += s.target_fps;
    if (s.accum >= s.camera_fps) {
        s.accum -= s.camera_fps;
        return true;
    }
    return false;
}

uint32_t video_rate_ctrl_pull_bitrate_bps(void)
{
    uint32_t b = s.pending_bitrate;
    s.pending_bitrate = 0;
    return b;
}

void video_rate_ctrl_set_network_ceiling_bps(uint32_t bps)
{
    if (!s.inited) {
        return;
    }
    s.net_ceiling_bps = bps;
    /* Re-clamp the current level to the new ceiling. If the estimate dropped
     * below our current rung, this snaps us down the ladder now (fps + bitrate
     * together) instead of waiting for the local send-latency loop to notice. */
    uint32_t prev = s.level;
    vrate_apply_level(s.level);
    if (s.level != prev) {
        ESP_LOGI(TAG, "net ceiling=%ubps: L%u (%ufps/%ubps)", (unsigned)bps,
                 (unsigned)s.level, (unsigned)s.target_fps, (unsigned)s.target_bitrate_bps);
    }
}
