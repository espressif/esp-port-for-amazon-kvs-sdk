/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/time.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdbool.h>

#include "esp_err.h"
#include "esp_log.h"
#include "file_capture.h"

static const char *TAG = "file_capture";

/* Hard upper bound — upstream samples ship 1500 H.264 + 618 Opus. */
#define MAX_VIDEO_FRAMES 1500
#define MAX_AUDIO_FRAMES 618

static char s_frames_dir[256] = "samples";

void file_capture_set_frames_dir(const char *dir)
{
    if (!dir || !*dir) {
        return;
    }
    snprintf(s_frames_dir, sizeof(s_frames_dir), "%s", dir);
}

/* --- Shared file-loading helpers ------------------------------------ */

static uint8_t *load_file(const char *path, uint32_t *out_len)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        return NULL;
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return NULL;
    }
    long sz = ftell(fp);
    if (sz <= 0) {
        fclose(fp);
        return NULL;
    }
    rewind(fp);
    uint8_t *buf = (uint8_t *)malloc((size_t)sz);
    if (!buf) {
        fclose(fp);
        return NULL;
    }
    if (fread(buf, 1, (size_t)sz, fp) != (size_t)sz) {
        free(buf);
        fclose(fp);
        return NULL;
    }
    fclose(fp);
    *out_len = (uint32_t)sz;
    return buf;
}

static uint64_t now_us(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000ULL + (uint64_t)tv.tv_usec;
}

/* --- Video capture --------------------------------------------------- */

/* Annex B scan: true when the access unit carries an IDR slice (NAL 5) or an SPS (NAL 7) */
static bool h264_has_idr(const uint8_t *buf, uint32_t len)
{
    for (uint32_t i = 0; i + 3 < len; i++) {
        if (buf[i] == 0 && buf[i + 1] == 0 && buf[i + 2] == 1) {
            uint8_t nal = buf[i + 3] & 0x1f;
            if (nal == 5 || nal == 7) {
                return true;
            }
            i += 2;
        }
    }
    return false;
}

typedef struct {
    int next_index;          /* 1-based; loops 1..MAX_VIDEO_FRAMES */
    uint32_t bitrate_kbps;
    uint8_t fps;
    uint64_t next_us;        /* wall-clock deadline for the next frame (pacing) */
    bool running;            /* shared with the pump: __atomic accesses only */
    bool stopped;
} fc_video_state_t;

/* The pump task is created once, from the app task that asks for the interface, and
 * idles until a capture starts. start() and stop() run on whatever thread the SDK
 * uses, a plain pthread on the host, which must not call FreeRTOS (xTaskCreate
 * there re-enables the tick on that thread), so they only hand the pump a stream. */
static TaskHandle_t s_pump;
static fc_video_state_t *s_pump_st;   /* the stream being fed; NULL when idle */

/* Encoded video reaches the WebRTC sender through media_stream's sink bus (the
 * hardware encoders publish there; kvs_media registers a sink), not through
 * get_frame(). The bus entry point is internal to media_stream, so declare it
 * here rather than reaching into its private header. */
void video_sink_dispatch(const video_frame_t *frame);

static esp_err_t fc_video_get_frame(video_capture_handle_t h, video_frame_t **out, uint32_t timeout_ms);
static esp_err_t fc_video_release_frame(video_capture_handle_t h, video_frame_t *frame);

static esp_err_t fc_video_read(fc_video_state_t *st, video_frame_t **out);

/* Runs as a FreeRTOS task: on the IDF linux target the scheduler only knows FreeRTOS
 * tasks, so the frames must not be pushed from a raw pthread. Pacing uses vTaskDelay
 * for the same reason. */
static void fc_video_pump(void *arg)
{
    (void) arg;
    fc_video_state_t *fed = NULL;
    TickType_t interval = 1;
    TickType_t last = 0;

    for (;;) {
        fc_video_state_t *st = __atomic_load_n(&s_pump_st, __ATOMIC_ACQUIRE);
        if (st == NULL) {
            fed = NULL;
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        if (!__atomic_load_n(&st->running, __ATOMIC_ACQUIRE)) {
            fed = NULL;
            __atomic_store_n(&s_pump_st, NULL, __ATOMIC_RELEASE);
            /* stop() returns once the pump has let go of it */
            __atomic_store_n(&st->stopped, true, __ATOMIC_RELEASE);
            continue;
        }
        if (st != fed) {
            fed = st;
            interval = pdMS_TO_TICKS(1000 / (st->fps ? st->fps : 30));
            interval = interval ? interval : 1;
            last = xTaskGetTickCount();
        }
        vTaskDelayUntil(&last, interval);
        video_frame_t *vf = NULL;
        if (fc_video_read(st, &vf) != ESP_OK) {
            continue;
        }
        video_sink_dispatch(vf);   /* sinks borrow the buffer for the duration of the call */
        fc_video_release_frame(st, vf);
    }
}

static esp_err_t fc_video_init(video_capture_config_t *cfg, video_capture_handle_t *out_handle)
{
    if (!cfg || !out_handle) {
        return ESP_ERR_INVALID_ARG;
    }
    fc_video_state_t *st = (fc_video_state_t *)calloc(1, sizeof(*st));
    if (!st) {
        return ESP_ERR_NO_MEM;
    }
    st->next_index   = 1;
    st->bitrate_kbps = cfg->bitrate ? cfg->bitrate : 1000;
    st->fps          = cfg->resolution.fps ? cfg->resolution.fps : 30;
    *out_handle = (video_capture_handle_t)st;
    ESP_LOGI(TAG, "video capture init: fps=%u, dir=%s/h264SampleFrames",
             (unsigned)st->fps, s_frames_dir);
    return ESP_OK;
}

static esp_err_t fc_video_start(video_capture_handle_t h)
{
    fc_video_state_t *st = (fc_video_state_t *) h;
    if (!st) {
        return ESP_ERR_INVALID_ARG;
    }
    if (__atomic_load_n(&st->running, __ATOMIC_ACQUIRE)) {
        return ESP_OK;
    }
    if (s_pump == NULL || __atomic_load_n(&s_pump_st, __ATOMIC_ACQUIRE) != NULL) {
        return ESP_ERR_INVALID_STATE;   /* no pump, or it is feeding another stream */
    }
    __atomic_store_n(&st->stopped, false, __ATOMIC_RELAXED);
    __atomic_store_n(&st->running, true, __ATOMIC_RELAXED);
    __atomic_store_n(&s_pump_st, st, __ATOMIC_RELEASE);   /* publishes the two above */
    return ESP_OK;
}

static esp_err_t fc_video_stop(video_capture_handle_t h)
{
    fc_video_state_t *st = (fc_video_state_t *) h;
    if (!st) {
        return ESP_ERR_INVALID_ARG;
    }
    if (__atomic_load_n(&st->running, __ATOMIC_ACQUIRE)) {
        __atomic_store_n(&st->running, false, __ATOMIC_RELEASE);
        /* Host sleep: the caller may be one of the SDK's plain pthreads */
        while (!__atomic_load_n(&st->stopped, __ATOMIC_ACQUIRE)) {
            usleep(10 * 1000);
        }
    }
    return ESP_OK;
}

static esp_err_t fc_video_get_frame(video_capture_handle_t h, video_frame_t **out, uint32_t timeout_ms)
{
    (void)timeout_ms;
    if (!h || !out) {
        return ESP_ERR_INVALID_ARG;
    }
    fc_video_state_t *st = (fc_video_state_t *)h;

    /* Pace to fps — a pull-based caller calls this back-to-back, so without a sleep
     * frames blast out and the viewer plays fast-forward. */
    uint64_t interval = 1000000ULL / (st->fps ? st->fps : 30);
    uint64_t now = now_us();
    if (st->next_us == 0) {
        st->next_us = now;
    } else if (now < st->next_us) {
        usleep((useconds_t)(st->next_us - now));
    }
    st->next_us += interval;

    return fc_video_read(st, out);
}

/* Log a missing frame once, not at the frame rate */
static void log_read_failure(const char *path)
{
    static bool s_logged;
    if (!s_logged) {
        s_logged = true;
        ESP_LOGE(TAG, "failed to read %s (check KVS_FRAMES_DIR); further failures not logged", path);
    }
}

/* Read the next sample frame, no pacing */
static esp_err_t fc_video_read(fc_video_state_t *st, video_frame_t **out)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/h264SampleFrames/frame-%04d.h264",
             s_frames_dir, st->next_index);

    uint32_t len = 0;
    uint8_t *buf = load_file(path, &len);
    if (!buf) {
        log_read_failure(path);
        return ESP_ERR_NOT_FOUND;
    }

    video_frame_t *vf = (video_frame_t *)calloc(1, sizeof(*vf));
    if (!vf) {
        free(buf);
        return ESP_ERR_NO_MEM;
    }
    vf->buffer    = buf;
    vf->len       = len;
    vf->timestamp = now_us();
    /* Flag I only on a real IDR/SPS: the sender starts and resyncs on I frames */
    vf->type = h264_has_idr(buf, len) ? VIDEO_FRAME_TYPE_I : VIDEO_FRAME_TYPE_P;

    st->next_index++;
    if (st->next_index > MAX_VIDEO_FRAMES) {
        st->next_index = 1;
    }

    *out = vf;
    return ESP_OK;
}

static esp_err_t fc_video_release_frame(video_capture_handle_t h, video_frame_t *frame)
{
    (void)h;
    if (!frame) {
        return ESP_ERR_INVALID_ARG;
    }
    free(frame->buffer);
    free(frame);
    return ESP_OK;
}

static esp_err_t fc_video_deinit(video_capture_handle_t h)
{
    fc_video_stop(h);
    free(h);
    return ESP_OK;
}

static esp_err_t fc_video_set_bitrate(video_capture_handle_t h, uint32_t bitrate_kbps)
{
    if (!h) return ESP_ERR_INVALID_ARG;
    ((fc_video_state_t *)h)->bitrate_kbps = bitrate_kbps;
    return ESP_OK;
}

static esp_err_t fc_video_get_bitrate(video_capture_handle_t h, uint32_t *bitrate_kbps)
{
    if (!h || !bitrate_kbps) return ESP_ERR_INVALID_ARG;
    *bitrate_kbps = ((fc_video_state_t *)h)->bitrate_kbps;
    return ESP_OK;
}

static media_stream_video_capture_t s_video_if = {
    .init          = fc_video_init,
    .start         = fc_video_start,
    .stop          = fc_video_stop,
    .get_frame     = fc_video_get_frame,
    .release_frame = fc_video_release_frame,
    .deinit        = fc_video_deinit,
    .set_bitrate   = fc_video_set_bitrate,
    .get_bitrate   = fc_video_get_bitrate,
};

media_stream_video_capture_t *file_capture_get_video_if(void)
{
    if (s_pump == NULL &&
        xTaskCreate(fc_video_pump, "fc_video_pump", 8192, NULL, 5, &s_pump) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create the video pump task");
        s_pump = NULL;
    }
    return &s_video_if;
}

/* --- Audio capture --------------------------------------------------- */

typedef struct {
    int next_index;      /* 1-based; loops 1..MAX_AUDIO_FRAMES */
    uint16_t frame_ms;
    uint64_t next_us;    /* wall-clock deadline for the next frame (pacing) */
} fc_audio_state_t;

static esp_err_t fc_audio_init(audio_capture_config_t *cfg, audio_capture_handle_t *out_handle)
{
    if (!cfg || !out_handle) {
        return ESP_ERR_INVALID_ARG;
    }
    fc_audio_state_t *st = (fc_audio_state_t *)calloc(1, sizeof(*st));
    if (!st) {
        return ESP_ERR_NO_MEM;
    }
    st->next_index = 1;
    st->frame_ms   = cfg->frame_duration_ms ? cfg->frame_duration_ms : 20;
    *out_handle = (audio_capture_handle_t)st;
    ESP_LOGI(TAG, "audio capture init: frame_ms=%u, dir=%s/opusSampleFrames",
             (unsigned)st->frame_ms, s_frames_dir);
    return ESP_OK;
}

static esp_err_t fc_audio_start(audio_capture_handle_t h)    { (void)h; return ESP_OK; }
static esp_err_t fc_audio_stop(audio_capture_handle_t h)     { (void)h; return ESP_OK; }

static esp_err_t fc_audio_get_frame(audio_capture_handle_t h, audio_frame_t **out, uint32_t timeout_ms)
{
    (void)timeout_ms;
    if (!h || !out) {
        return ESP_ERR_INVALID_ARG;
    }
    fc_audio_state_t *st = (fc_audio_state_t *)h;

    /* Pace to frame_ms — same reason as video (see fc_video_get_frame). */
    uint64_t interval = (uint64_t)(st->frame_ms ? st->frame_ms : 20) * 1000ULL;
    uint64_t now = now_us();
    if (st->next_us == 0) {
        st->next_us = now;
    } else if (now < st->next_us) {
        /* The audio sender is a plain pthread on the host, so a raw sleep is the right call here */
        usleep((useconds_t)(st->next_us - now));
    }
    st->next_us += interval;

    char path[512];
    snprintf(path, sizeof(path), "%s/opusSampleFrames/sample-%03d.opus",
             s_frames_dir, st->next_index);

    uint32_t len = 0;
    uint8_t *buf = load_file(path, &len);
    if (!buf) {
        log_read_failure(path);
        return ESP_ERR_NOT_FOUND;
    }

    audio_frame_t *af = (audio_frame_t *)calloc(1, sizeof(*af));
    if (!af) {
        free(buf);
        return ESP_ERR_NO_MEM;
    }
    af->buffer    = buf;
    af->len       = len;
    af->timestamp = now_us();

    st->next_index++;
    if (st->next_index > MAX_AUDIO_FRAMES) {
        st->next_index = 1;
    }

    *out = af;
    return ESP_OK;
}

static esp_err_t fc_audio_release_frame(audio_capture_handle_t h, audio_frame_t *frame)
{
    (void)h;
    if (!frame) {
        return ESP_ERR_INVALID_ARG;
    }
    free(frame->buffer);
    free(frame);
    return ESP_OK;
}

static esp_err_t fc_audio_deinit(audio_capture_handle_t h)
{
    free(h);
    return ESP_OK;
}

static media_stream_audio_capture_t s_audio_if = {
    .init          = fc_audio_init,
    .start         = fc_audio_start,
    .stop          = fc_audio_stop,
    .get_frame     = fc_audio_get_frame,
    .release_frame = fc_audio_release_frame,
    .deinit        = fc_audio_deinit,
};

media_stream_audio_capture_t *file_capture_get_audio_if(void)
{
    return &s_audio_if;
}
