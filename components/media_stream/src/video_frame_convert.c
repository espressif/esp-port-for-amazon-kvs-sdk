/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @brief PPA-backed frame conversion for VIDEO_RAW_MODE_CONVERTED sinks.
 *
 * See video_frame_convert.h for why this exists. Implementation notes worth knowing:
 *
 *  - ONE PPA client for the whole component, shared by every converter. The PPA is a single
 *    peripheral; a client per sink would just queue against the same hardware while costing another
 *    transaction ring. Blocking mode, because a completion callback buys nothing when the caller
 *    has nothing else to do with the frame until the pass finishes. The client is sized for
 *    several concurrent callers - see PPA_PENDING_TRANS.
 *
 *  - The output pool free-list is a FreeRTOS QUEUE OF INDICES rather than a lock-protected array.
 *    Buffers are taken by the pump and returned by the consumer's task - and sometimes by the pump
 *    itself while it already holds the bus slot lock, when it evicts a queued frame. A queue is
 *    intrinsically safe from any of those without adding a second lock to reason about against the
 *    bus's own.
 *
 *  - PPA's byte_swap applies to the INPUT picture and only for ARGB8888/RGB565 inputs, so it cannot
 *    be used to produce big-endian RGB565 from a YUV camera frame. A consumer that needs the
 *    panel's byte order does the swap while it copies the frame out.
 */

#include "sdkconfig.h"

/* Gated on the PPA itself rather than on a target, so VIDEO_RAW_MODE_CONVERTED follows
 * the hardware wherever it turns up. A target without one compiles this to empty, and
 * the bus refuses a CONVERTED registration. */
#include "media_stream_caps.h"
#if MEDIA_STREAM_HAS_PPA

#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_cache.h"
#include "esp_timer.h"
#include "driver/ppa.h"
#include "video_frame_convert.h"

static const char *TAG = "video_convert";

/* Transaction slots on the shared PPA client: one per task that can have a conversion in
 * flight at the same moment.
 *
 * One is not enough, even though every call here is blocking: each converted sink runs
 * its pass on its OWN task, so two of them submit concurrently, the second gets
 * ESP_ERR_INVALID_STATE, and the driver says "exceed maximum pending transactions for
 * the client". At 1080p, where a pass runs long enough for the overlap to be the common
 * case rather than a race, that would be every frame.
 *
 * Sized for the pump plus every registered sink, since any of them may be the one holding
 * the client. Slots are a small descriptor each, not a buffer. */
#ifndef CONFIG_VIDEO_RAW_SINK_MAX
#define CONFIG_VIDEO_RAW_SINK_MAX 4
#endif
#define PPA_PENDING_TRANS   (CONFIG_VIDEO_RAW_SINK_MAX + 1)

/* Both the address and the size handed to the PPA must be cache-line aligned, and esp_cache_msync()
 * enforces the same on the invalidate below. 64 matches CONFIG_CACHE_L2_CACHE_LINE_64B, which the
 * rest of this component already assumes throughout. */
#define CONV_ALIGN      64
#define ALIGN_UP_64(x)  (((x) + (CONV_ALIGN - 1)) & ~((size_t)(CONV_ALIGN) - 1))
#define MAX_POOL_DEPTH  4

struct video_frame_convert_s {
    video_frame_convert_cfg_t cfg;
    ppa_srm_color_mode_t      src_cm;
    ppa_srm_color_mode_t      dst_cm;
    uint8_t                   bpp;
    size_t                    buf_size;              /* Already rounded up to CONV_ALIGN */
    uint8_t                  *buf[MAX_POOL_DEPTH];
    QueueHandle_t             free_idx;              /* Indices of buffers nobody holds */
    bool                      has_ppa;               /* This converter holds a share of s_ppa */

    /* Crop and scale, from pick_geometry(). Recomputed only when the source resolution
     * changes, which for a camera is never after the first frame. Owned by the sink's own
     * task, like everything else in a conversion, so no lock. */
    uint16_t                  geo_src_w;
    uint16_t                  geo_src_h;
    uint16_t                  block_w;
    uint16_t                  block_h;
    uint16_t                  off_x;
    uint16_t                  off_y;
    float                     scale;

    /* Timing, because "the pump spends 600 ms of every second in the bus" has two very
     * different causes and the fix differs: PPA_TRANS_MODE_BLOCKING means the caller waits,
     * and that wait covers both the hardware pass AND however long the scheduler takes to
     * run this task again. ppa_us is the pass; total_us includes the wait and the
     * invalidate. Pump task only, so no lock. */
    uint32_t                  ppa_us_sum;
    uint32_t                  total_us_sum;
    uint32_t                  ppa_us_max;
    uint32_t                  samples;
};

/* At 15 fps this is a line every four seconds. */
#define CONV_TIMING_EVERY  64

static ppa_client_handle_t s_ppa;      /* Shared by every converter; see the file comment. */
static uint32_t            s_ppa_refs;
/* Guards the two above. Converters are created LAZILY, on the first frame, on each sink's
 * OWN task - so two converted sinks with the same fps limit (the example's preview and
 * detect both default to 5) see the same first frame and create at the same instant on
 * two cores. Without this both find s_ppa == NULL and register a client, one handle and
 * its transaction ring leak, and s_ppa_refs ends at 1 for two users - so unregistering
 * either one pulls the client out from under the other. */
static SemaphoreHandle_t   s_ppa_lock;

/* ------------------------------------------------------------------ format mapping */

static bool fourcc_to_srm_cm(uint32_t fourcc, ppa_srm_color_mode_t *cm, uint8_t *bpp)
{
    switch (fourcc) {
    /* The P4 ISP's semi-packed YUV420 is exactly what the PPA calls YUV420, which is why camera
     * output feeds it with no repack. */
    case VIDEO_FOURCC_O_UYY_E_VYY: *cm = PPA_SRM_COLOR_MODE_YUV420;      *bpp = 0; return true;
    case VIDEO_FOURCC_RGB565:      *cm = PPA_SRM_COLOR_MODE_RGB565;      *bpp = 2; return true;
    case VIDEO_FOURCC_RGB24:       *cm = PPA_SRM_COLOR_MODE_RGB888;      *bpp = 3; return true;
    case VIDEO_FOURCC_YUYV:        *cm = PPA_SRM_COLOR_MODE_YUV422_YUYV; *bpp = 2; return true;
    case VIDEO_FOURCC_UYVY:        *cm = PPA_SRM_COLOR_MODE_YUV422_UYVY; *bpp = 2; return true;
    default:                                                                       return false;
    }
}

esp_err_t video_frame_convert_check_dst(uint32_t dst_fourcc)
{
    ppa_srm_color_mode_t dst_cm;
    uint8_t dst_bpp;

    if (!fourcc_to_srm_cm(dst_fourcc, &dst_cm, &dst_bpp)) {
        ESP_LOGE(TAG, "target format '%.4s' is not a PPA output mode", (const char *)&dst_fourcc);
        return ESP_ERR_NOT_SUPPORTED;
    }
    /* YUV422 and YUV420 are input-only on the SRM output path, and a planar destination would need
     * a stride the raw-frame descriptor cannot express anyway. */
    if (dst_bpp == 0 || dst_cm == PPA_SRM_COLOR_MODE_YUV422_YUYV ||
        dst_cm == PPA_SRM_COLOR_MODE_YUV422_UYVY) {
        ESP_LOGE(TAG, "target format '%.4s' cannot be a PPA output", (const char *)&dst_fourcc);
        return ESP_ERR_NOT_SUPPORTED;
    }
    return ESP_OK;
}

esp_err_t video_frame_convert_check(uint32_t src_fourcc, uint32_t dst_fourcc)
{
    ppa_srm_color_mode_t src_cm;
    uint8_t src_bpp;

    if (!fourcc_to_srm_cm(src_fourcc, &src_cm, &src_bpp)) {
        ESP_LOGE(TAG, "source format '%.4s' is not a PPA input mode", (const char *)&src_fourcc);
        return ESP_ERR_NOT_SUPPORTED;
    }
    return video_frame_convert_check_dst(dst_fourcc);
}

/* ------------------------------------------------------------------ lifecycle */

esp_err_t video_frame_convert_init(void)
{
    if (s_ppa_lock != NULL) {
        return ESP_OK;
    }
    s_ppa_lock = xSemaphoreCreateMutex();
    if (s_ppa_lock == NULL) {
        ESP_LOGE(TAG, "failed to create the PPA client lock");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static esp_err_t ppa_acquire(void)
{
    if (s_ppa_lock == NULL) {
        ESP_LOGE(TAG, "video_frame_convert_init() was never called");
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_ppa_lock, portMAX_DELAY);

    esp_err_t err = ESP_OK;
    if (s_ppa != NULL) {
        s_ppa_refs++;
    } else {
        const ppa_client_config_t cfg = {
            .oper_type             = PPA_OPERATION_SRM,
            .max_pending_trans_num = PPA_PENDING_TRANS,   /* See PPA_PENDING_TRANS. */
        };
        err = ppa_register_client(&cfg, &s_ppa);
        if (err == ESP_OK) {
            s_ppa_refs = 1;
        } else {
            ESP_LOGE(TAG, "ppa_register_client() failed: %s", esp_err_to_name(err));
        }
    }

    xSemaphoreGive(s_ppa_lock);
    return err;
}

static void ppa_give(void)
{
    if (s_ppa_lock == NULL) {
        return;
    }
    xSemaphoreTake(s_ppa_lock, portMAX_DELAY);
    if (s_ppa != NULL && s_ppa_refs > 0 && --s_ppa_refs == 0) {
        ppa_unregister_client(s_ppa);
        s_ppa = NULL;
    }
    xSemaphoreGive(s_ppa_lock);
}

esp_err_t video_frame_convert_create(const video_frame_convert_cfg_t *cfg,
                                     video_frame_convert_handle_t *out)
{
    if (cfg == NULL || out == NULL || cfg->dst_width == 0 || cfg->dst_height == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = video_frame_convert_check(cfg->src_fourcc, cfg->dst_fourcc);
    if (err != ESP_OK) {
        return err;
    }

    const uint8_t depth = cfg->pool_depth ? cfg->pool_depth : 2;
    if (depth > MAX_POOL_DEPTH) {
        ESP_LOGE(TAG, "pool depth %u exceeds %d", depth, MAX_POOL_DEPTH);
        return ESP_ERR_INVALID_ARG;
    }

    struct video_frame_convert_s *c = calloc(1, sizeof(*c));
    if (c == NULL) {
        return ESP_ERR_NO_MEM;
    }
    c->cfg = *cfg;
    c->cfg.pool_depth = depth;

    uint8_t src_bpp;
    fourcc_to_srm_cm(cfg->src_fourcc, &c->src_cm, &src_bpp);
    fourcc_to_srm_cm(cfg->dst_fourcc, &c->dst_cm, &c->bpp);

    c->buf_size = ALIGN_UP_64((size_t)cfg->dst_width * cfg->dst_height * c->bpp);

    c->free_idx = xQueueCreate(depth, sizeof(uint8_t));
    if (c->free_idx == NULL) {
        free(c);
        return ESP_ERR_NO_MEM;
    }
    for (uint8_t i = 0; i < depth; i++) {
        c->buf[i] = heap_caps_aligned_calloc(CONV_ALIGN, 1, c->buf_size, MALLOC_CAP_SPIRAM);
        if (c->buf[i] == NULL) {
            ESP_LOGE(TAG, "out of PSRAM for a %u-buffer pool of %u bytes", depth,
                     (unsigned)c->buf_size);
            video_frame_convert_destroy(c);
            return ESP_ERR_NO_MEM;
        }
        xQueueSend(c->free_idx, &i, 0);
    }

    err = ppa_acquire();
    if (err != ESP_OK) {
        video_frame_convert_destroy(c);
        return err;
    }
    c->has_ppa = true;

    ESP_LOGI(TAG, "converter '%.4s' -> '%.4s' %ux%u, %u buffers of %u bytes",
             (const char *)&cfg->src_fourcc, (const char *)&cfg->dst_fourcc,
             cfg->dst_width, cfg->dst_height, depth, (unsigned)c->buf_size);
    *out = c;
    return ESP_OK;
}

void video_frame_convert_destroy(video_frame_convert_handle_t c)
{
    if (c == NULL) {
        return;
    }
    /* Only if THIS converter took a share. destroy() is also the cleanup path for a create
     * that failed before ppa_acquire() - a pool that would not fit in PSRAM, say - and
     * giving back a reference we never took drops the client under whichever converter
     * does hold it. That surfaced as the preview freezing with PPA errors when the detect
     * sink ran out of memory, which points at entirely the wrong sink. */
    if (c->has_ppa) {
        ppa_give();
    }
    for (int i = 0; i < MAX_POOL_DEPTH; i++) {
        if (c->buf[i]) {
            heap_caps_free(c->buf[i]);
        }
    }
    if (c->free_idx) {
        vQueueDelete(c->free_idx);
    }
    free(c);
}

/* ------------------------------------------------------------------ geometry */

/* Largest even block in [lo, hi] that the crop can afford, where [lo, hi] is the set of block
 * sizes the hardware turns into exactly `dst` output pixels. False when there is none. */
static bool solve_axis(uint32_t dst, uint32_t bmax, uint32_t k, uint32_t step, uint32_t *out)
{
    /* written == dst  <=>  dst <= block*k/step < dst+1  <=>  step*dst/k <= block < step*(dst+1)/k */
    const uint32_t lo = (step * dst + k - 1u) / k;                 /* ceil */
    const uint32_t hi = (step * (dst + 1u) + k - 1u) / k - 1u;     /* ceil - 1 */

    uint32_t b = (hi < bmax) ? hi : bmax;
    b &= ~1u;
    if (b == 0u || b < lo) {
        return false;
    }
    *out = b;
    return true;
}

/* Choose the crop block, the offsets and the scale factor for one source resolution.
 *
 * THE PPA DOES NOT SCALE TO A SIZE YOU ASK FOR. It scales by a factor it can represent, and
 * whatever that produces is what lands in the destination. ppa_srm.c truncates the factor to an
 * integer part plus a four-bit fraction, and for YUV420 on either side it also clears the low
 * fraction bit:
 *
 *     scale_int  = (uint32_t)scale;
 *     scale_frag = (uint32_t)(scale * 16) & 15;        // &= ~1 when YUV420 is in or out
 *     written    = block * scale_int + block * scale_frag / 16;
 *
 * and the only range check is `written <= out.pic_w`. So the obvious scale = dst/block leaves the
 * right and bottom of the destination UNWRITTEN - whatever the pool buffer held two frames ago.
 * 1280x720 into 224x224 wrote a 180x180 corner and left a 44-pixel band of stale pixels on two
 * sides of every model input; 1920x1080 into the same size wrote 135x135, so well over half the
 * model input was whatever the buffer held before. Nothing errors, so it reads as a bad model.
 *
 * The fix is to pick the FACTOR first, off the k/step grid the hardware can represent, and derive
 * the block from it - block = step*dst/k makes written == dst exactly. The block still has to fit
 * inside the aspect-preserving centre crop, so k walks upward from the smallest that fits until
 * both axes land exactly.
 *
 * Worked cases, all with step 8 (YUV420 in): 720 -> 224 picks k=3, block 598, 598*3/8 = 224.25 ->
 * 224. 720 -> 240 picks k=3, block 642, 642*3/8 = 240.75 -> 240. 1080 -> 224 picks k=2, block 898,
 * 898*2/8 = 224.5 -> 224.
 */
static void pick_geometry(struct video_frame_convert_s *c, uint32_t src_w, uint32_t src_h)
{
    /* Largest centred block matching the destination aspect, so nothing is squashed. Dimensions and
     * offsets are forced even: the PPA requires it for YUV420 input, and it costs at most one row
     * or column of the crop. */
    uint32_t bw_max = src_w;
    uint32_t bh_max = src_h;
    if (src_w * c->cfg.dst_height > src_h * c->cfg.dst_width) {
        bw_max = src_h * c->cfg.dst_width / c->cfg.dst_height;
    } else {
        bh_max = src_w * c->cfg.dst_height / c->cfg.dst_width;
    }
    bw_max &= ~1u;
    bh_max &= ~1u;

    /* Sixteenths, or eighths when YUV420 is on either side and the driver drops the low bit. */
    const uint32_t step = (c->src_cm == PPA_SRM_COLOR_MODE_YUV420 ||
                           c->dst_cm == PPA_SRM_COLOR_MODE_YUV420) ? 8u : 16u;

    uint32_t block_w = 0;
    uint32_t block_h = 0;
    bool     found   = false;

    if (bw_max >= 2u && bh_max >= 2u) {
        const uint32_t kw = (step * c->cfg.dst_width  + bw_max - 1u) / bw_max;
        const uint32_t kh = (step * c->cfg.dst_height + bh_max - 1u) / bh_max;
        uint32_t k = (kw > kh) ? kw : kh;
        if (k == 0u) {
            k = 1u;
        }
        /* One k for both axes, so the crop keeps the destination's aspect. The upper bound is the
         * driver's own (scale < PPA_LL_SRM_SCALING_INT_MAX, which is 256). */
        for (; k < step * 256u; k++) {
            if (solve_axis(c->cfg.dst_width, bw_max, k, step, &block_w) &&
                solve_axis(c->cfg.dst_height, bh_max, k, step, &block_h)) {
                c->scale = (float)k / (float)step;
                found    = true;
                break;
            }
            /* Once the block needed for either axis has shrunk below the minimum, a larger k
             * only shrinks it further. */
            if (step * c->cfg.dst_width / k < 2u || step * c->cfg.dst_height / k < 2u) {
                break;
            }
        }
    }

    if (!found) {
        /* No representable factor lands both axes exactly - possible for odd destination sizes
         * very close to 1:1. Fall back to the old behaviour, which is at least no worse. */
        static bool warned;
        if (!warned) {
            warned = true;
            ESP_LOGW(TAG, "%ux%u -> %ux%u has no exact PPA scale; the destination edge will "
                          "keep stale pixels", (unsigned)src_w, (unsigned)src_h,
                     c->cfg.dst_width, c->cfg.dst_height);
        }
        block_w  = bw_max;
        block_h  = bh_max;
        c->scale = (float)c->cfg.dst_width / (float)bw_max;
    }

    c->block_w   = (uint16_t)block_w;
    c->block_h   = (uint16_t)block_h;
    c->off_x     = (uint16_t)(((src_w - block_w) / 2u) & ~1u);
    c->off_y     = (uint16_t)(((src_h - block_h) / 2u) & ~1u);
    c->geo_src_w = (uint16_t)src_w;
    c->geo_src_h = (uint16_t)src_h;

    ESP_LOGI(TAG, "%ux%u -> %ux%u: crop %ux%u at (%u,%u), scale %u/%u",
             (unsigned)src_w, (unsigned)src_h, c->cfg.dst_width, c->cfg.dst_height,
             c->block_w, c->block_h, c->off_x, c->off_y,
             (unsigned)(c->scale * step + 0.5f), (unsigned)step);
}

/* ------------------------------------------------------------------ the conversion */

esp_err_t video_frame_convert_run(video_frame_convert_handle_t c,
                                  const video_raw_frame_t *src, video_raw_frame_t *dst)
{
    if (c == NULL || src == NULL || dst == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (src->fourcc != c->cfg.src_fourcc) {
        /* The camera changed format under us - a resolution or sensor-mode switch. Converting with
         * the old mapping would produce garbage that looks like a bug somewhere else. */
        ESP_LOGE(TAG, "source is '%.4s', converter was built for '%.4s'",
                 (const char *)&src->fourcc, (const char *)&c->cfg.src_fourcc);
        return ESP_ERR_INVALID_STATE;
    }

    const int64_t t_start = esp_timer_get_time();

    uint8_t idx;
    if (xQueueReceive(c->free_idx, &idx, 0) != pdTRUE) {
        return ESP_ERR_NO_MEM;   /* Every buffer is out with the consumer. Caller counts the drop. */
    }

    /* Crop and factor are a property of the resolution pair, not of the frame, so they are worked
     * out once and reused - see pick_geometry() for why they are not simply dst/block. */
    if (c->geo_src_w != src->width || c->geo_src_h != src->height) {
        pick_geometry(c, src->width, src->height);
    }
    const uint32_t block_w = c->block_w;
    const uint32_t block_h = c->block_h;
    const uint32_t off_x   = c->off_x;
    const uint32_t off_y   = c->off_y;

    const ppa_srm_oper_config_t op = {
        .in = {
            .buffer         = src->buffer,
            .pic_w          = src->width,
            .pic_h          = src->height,
            .block_w        = block_w,
            .block_h        = block_h,
            .block_offset_x = off_x,
            .block_offset_y = off_y,
            .srm_cm         = c->src_cm,
            /* The ISP emits limited-range BT.601 by default; saying otherwise here is what makes
             * colours look washed out or crushed. */
            .yuv_range      = PPA_COLOR_RANGE_LIMIT,
            .yuv_std        = PPA_COLOR_CONV_STD_RGB_YUV_BT601,
        },
        .out = {
            .buffer         = c->buf[idx],
            .buffer_size    = c->buf_size,
            .pic_w          = c->cfg.dst_width,
            .pic_h          = c->cfg.dst_height,
            .block_offset_x = 0,
            .block_offset_y = 0,
            .srm_cm         = c->dst_cm,
            .yuv_range      = PPA_COLOR_RANGE_LIMIT,
            .yuv_std        = PPA_COLOR_CONV_STD_RGB_YUV_BT601,
        },
        .rotation_angle = PPA_SRM_ROTATION_ANGLE_0,
        /* One factor for both axes: the crop was derived from it, so each axis lands exactly. */
        .scale_x        = c->scale,
        .scale_y        = c->scale,
        .mode           = PPA_TRANS_MODE_BLOCKING,
    };

    const int64_t t_ppa0 = esp_timer_get_time();
    esp_err_t err = ppa_do_scale_rotate_mirror(s_ppa, &op);
    const uint32_t ppa_us = (uint32_t)(esp_timer_get_time() - t_ppa0);
    if (err != ESP_OK) {
        static uint32_t fail_count;
        fail_count++;
        if (fail_count == 1 || (fail_count & 0x1F) == 0) {
            ESP_LOGE(TAG, "ppa_do_scale_rotate_mirror() failed: %s (count=%" PRIu32 ")",
                     esp_err_to_name(err), fail_count);
        }
        xQueueSend(c->free_idx, &idx, 0);
        return err;
    }

    /* The PPA wrote through DMA; invalidate so the consumer's reads see it rather than stale cache. */
    esp_cache_msync(c->buf[idx], c->buf_size, ESP_CACHE_MSYNC_FLAG_DIR_M2C);

    c->ppa_us_sum   += ppa_us;
    c->total_us_sum += (uint32_t)(esp_timer_get_time() - t_start);
    if (ppa_us > c->ppa_us_max) {
        c->ppa_us_max = ppa_us;
    }
    if (++c->samples >= CONV_TIMING_EVERY) {
        ESP_LOGI(TAG, "'%.4s'->'%.4s' %ux%u: ppa avg %" PRIu32 " us (max %" PRIu32
                 "), total avg %" PRIu32 " us over %" PRIu32 " frames",
                 (const char *)&c->cfg.src_fourcc, (const char *)&c->cfg.dst_fourcc,
                 c->cfg.dst_width, c->cfg.dst_height,
                 c->ppa_us_sum / c->samples, c->ppa_us_max,
                 c->total_us_sum / c->samples, c->samples);
        c->ppa_us_sum = c->total_us_sum = c->ppa_us_max = c->samples = 0;
    }

    *dst = (video_raw_frame_t){
        .buffer       = c->buf[idx],
        .len          = (size_t)c->cfg.dst_width * c->cfg.dst_height * c->bpp,
        .width        = c->cfg.dst_width,
        .height       = c->cfg.dst_height,
        .fourcc       = c->cfg.dst_fourcc,
        .timestamp_us = src->timestamp_us,
        .seq          = src->seq,
        .slot         = idx,      /* Pool index, not a camera slot - owned says which. */
        .generation   = 0,
        .owned        = true,
    };
    return ESP_OK;
}

void video_frame_convert_release(video_frame_convert_handle_t c, uint8_t index)
{
    if (c == NULL || index >= c->cfg.pool_depth) {
        return;
    }
    xQueueSend(c->free_idx, &index, 0);
}

#endif /* MEDIA_STREAM_HAS_PPA */
