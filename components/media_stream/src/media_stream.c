/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include <stdio.h>
#include <inttypes.h>
#include <pthread.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "media_stream.h"
#include "video_sink_priv.h"
/* bsp_selector is gated to `target not in [esp32c5, esp32c6]` in
 * idf_component.yml — the header is missing on c-series builds. The
 * sole use of a BSP symbol (bsp_i2c_init at l.680) is already inside
 * a `#if CONFIG_IDF_TARGET_ESP32P4 && !CONFIG_BSP_SELECT_NONE` gate. */
#if !CONFIG_IDF_TARGET_ESP32C5 && !CONFIG_IDF_TARGET_ESP32C6
#include "bsp/esp-bsp.h"
#endif
#include "driver/i2c_master.h"

static const char *TAG = "media_stream";

#define NUMBER_OF_H265_FRAME_FILES               1500

#define DEFAULT_FPS_VALUE                        25
#define DEFAULT_VIDEO_HEIGHT_PIXELS              720
#define DEFAULT_VIDEO_WIDTH_PIXELS               1280
#define DEFAULT_AUDIO_OPUS_CHANNELS              2
#define DEFAULT_AUDIO_OPUS_SAMPLE_RATE_HZ        48000
#define DEFAULT_AUDIO_OPUS_BITS_PER_SAMPLE       16

// Static handles for media components
static video_capture_handle_t s_video_handle = NULL;
static audio_capture_handle_t s_audio_handle = NULL;
static video_player_handle_t s_video_player = NULL;
static audio_player_handle_t s_audio_player = NULL;

// Callback for received frames
static media_stream_frame_received_cb_t s_frame_received_cb = NULL;
static void *s_frame_received_user_data = NULL;

// Callback for frames ready to be sent
static media_stream_frame_ready_cb_t s_frame_ready_cb = NULL;
static void *s_frame_ready_user_data = NULL;

// Sample file state
static uint32_t s_video_file_index = 0;
static uint32_t s_audio_file_index = 0;
static uint64_t s_video_ref_time = 0;
static uint64_t s_audio_ref_time = 0;

// Configurable file paths for sample frames
static char s_mount_point[64] = "/spiffs";
static char s_video_file_prefix[64] = "samples/frame";
static char s_audio_file_prefix[64] = "samples/sample";
static char s_video_file_extension[16] = ".h264";
static char s_audio_file_extension[16] = ".opus";

/* One file per medium plus an index, opened once. Per-frame files cost two
 * fopen() per frame, which SPIFFS makes worse as the file count grows. */
#define SAMPLE_PACK_MAGIC 0x31464B50u   /* "PKF1" */

typedef struct {
    FILE     *fp;
    uint32_t  count;
    uint32_t *offset;
    uint32_t *length;
    bool      failure_logged;
} sample_pack_t;

static sample_pack_t s_video_pack;
static sample_pack_t s_audio_pack;

static void pack_path(char *out, size_t out_len, const char *prefix, const char *name)
{
    const char *slash = strrchr(prefix, '/');
    int dir_len = slash ? (int)(slash - prefix) : 0;
    if (dir_len > 0) {
        snprintf(out, out_len, "%s/%.*s/%s", s_mount_point, dir_len, prefix, name);
    } else {
        snprintf(out, out_len, "%s/%s", s_mount_point, name);
    }
}

/* The file source reads only from the pack. An open that fails is retried on the
 * next read, since the pack may not be mounted yet, and the failure is logged once
 * rather than on every frame. */
static esp_err_t pack_open(sample_pack_t *p, const char *prefix, const char *name)
{
    if (p->fp) {
        return ESP_OK;
    }

    char path[160];
    pack_path(path, sizeof(path), prefix, name);

    esp_err_t err;
    const char *why;
    uint32_t *off = NULL;
    uint32_t *len = NULL;
    uint32_t hdr[2];
    uint32_t n = 0;

    FILE *fp = fopen(path, "rb");
    if (!fp) {
        err = ESP_ERR_NOT_FOUND;
        why = "not found";
        goto fail;
    }
    if (fread(hdr, sizeof(hdr), 1, fp) != 1 || hdr[0] != SAMPLE_PACK_MAGIC || hdr[1] == 0) {
        err = ESP_ERR_INVALID_STATE;
        why = "not a sample pack";
        goto fail;
    }
    n = hdr[1];
    off = calloc(n, sizeof(uint32_t));
    len = calloc(n, sizeof(uint32_t));
    if (!off || !len) {
        err = ESP_ERR_NO_MEM;
        why = "no memory for its index";
        goto fail;
    }
    for (uint32_t i = 0; i < n; i++) {
        uint32_t entry[2];
        if (fread(entry, sizeof(entry), 1, fp) != 1) {
            err = ESP_FAIL;
            why = "its index is truncated";
            goto fail;
        }
        off[i] = entry[0];
        len[i] = entry[1];
    }

    p->fp = fp;
    p->count = n;
    p->offset = off;
    p->length = len;
    p->failure_logged = false;
    ESP_LOGI(TAG, "Opened %s: %" PRIu32 " frames", path, n);
    return ESP_OK;

fail:
    free(off);
    free(len);
    if (fp) {
        fclose(fp);
    }
    if (!p->failure_logged) {
        ESP_LOGW(TAG, "Cannot use %s: %s; the file source sends nothing until it can", path, why);
        p->failure_logged = true;
    }
    return err;
}

static esp_err_t pack_read(sample_pack_t *p, uint32_t idx, uint8_t *buf, uint32_t *size)
{
    if (!p->fp || idx >= p->count || size == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint32_t len = p->length[idx];
    if (buf == NULL) {
        *size = len;
        return ESP_OK;
    }
    if (*size < len) {
        *size = len;
        return ESP_ERR_INVALID_SIZE;
    }
    if (fseek(p->fp, (long)p->offset[idx], SEEK_SET) != 0) {
        return ESP_FAIL;
    }
    if (fread(buf, 1, len, p->fp) != len) {
        return ESP_FAIL;
    }
    *size = len;
    return ESP_OK;
}

// File-based capture state structures
typedef struct {
    bool started;
    video_frame_t current_frame;
    uint8_t *frame_buffer;
    uint32_t frame_buffer_size;
    uint32_t current_bitrate_kbps;
    uint64_t start_time;
    uint32_t period_us;
    int64_t next_due_us;
    uint32_t index;               /* get_frame()'s read position (1-based, 0 = before the first) */
    TaskHandle_t pump;            /* publishes to the sink registry while this source owns it */
    volatile bool pump_done;
    uint8_t *pump_buffer;         /* the pump's own frame, so get_frame() never shares a buffer with it */
    uint32_t pump_buffer_size;
    uint32_t pump_index;          /* the pump's read position */
} file_video_capture_state_t;

typedef struct {
    bool started;
    audio_frame_t current_frame;
    uint8_t *frame_buffer;
    uint32_t frame_buffer_size;
    uint64_t start_time;
    uint32_t period_us;
    int64_t next_due_us;
    uint32_t index;               /* this source's read position in the pack */
} file_audio_capture_state_t;

// Default audio capture implementation
static esp_err_t audio_capture_init_impl(audio_capture_config_t *config, audio_capture_handle_t *handle)
{
    return audio_capture_init(config, handle);
}

static esp_err_t audio_capture_start_impl(audio_capture_handle_t handle)
{
    return audio_capture_start(handle);
}

static esp_err_t audio_capture_stop_impl(audio_capture_handle_t handle)
{
    return audio_capture_stop(handle);
}

static esp_err_t audio_capture_get_frame_impl(audio_capture_handle_t handle, audio_frame_t **frame, uint32_t timeout_ms)
{
    return audio_capture_get_frame(handle, frame, timeout_ms);
}

static esp_err_t audio_capture_release_frame_impl(audio_capture_handle_t handle, audio_frame_t *frame)
{
    return audio_capture_release_frame(handle, frame);
}

static esp_err_t audio_capture_deinit_impl(audio_capture_handle_t handle)
{
    return audio_capture_deinit(handle);
}

// Default video capture implementation
static esp_err_t video_capture_init_impl(video_capture_config_t *config, video_capture_handle_t *handle)
{
    return video_capture_init(config, handle);
}

static esp_err_t video_capture_start_impl(video_capture_handle_t handle)
{
    return video_capture_start(handle);
}

static esp_err_t video_capture_stop_impl(video_capture_handle_t handle)
{
    return video_capture_stop(handle);
}

static esp_err_t video_capture_get_frame_impl(video_capture_handle_t handle, video_frame_t **frame, uint32_t timeout_ms)
{
    return video_capture_get_frame(handle, frame, timeout_ms);
}

static esp_err_t video_capture_release_frame_impl(video_capture_handle_t handle, video_frame_t *frame)
{
    return video_capture_release_frame(handle, frame);
}

static esp_err_t video_capture_deinit_impl(video_capture_handle_t handle)
{
    return video_capture_deinit(handle);
}

static esp_err_t video_capture_set_bitrate_impl(video_capture_handle_t handle, uint32_t bitrate_kbps)
{
    return video_capture_set_bitrate(handle, bitrate_kbps);
}

static esp_err_t video_capture_get_bitrate_impl(video_capture_handle_t handle, uint32_t *bitrate_kbps)
{
    return video_capture_get_bitrate(handle, bitrate_kbps);
}

// Static interface structures
static media_stream_audio_capture_t s_audio_capture = {
    .init = audio_capture_init_impl,
    .start = audio_capture_start_impl,
    .stop = audio_capture_stop_impl,
    .get_frame = audio_capture_get_frame_impl,
    .release_frame = audio_capture_release_frame_impl,
    .deinit = audio_capture_deinit_impl
};

static media_stream_video_capture_t s_video_capture = {
    .init = video_capture_init_impl,
    .start = video_capture_start_impl,
    .stop = video_capture_stop_impl,
    .get_frame = video_capture_get_frame_impl,
    .release_frame = video_capture_release_frame_impl,
    .deinit = video_capture_deinit_impl,
    .set_bitrate = video_capture_set_bitrate_impl,
    .get_bitrate = video_capture_get_bitrate_impl
};

// Default audio player implementation
static esp_err_t audio_player_init_impl(audio_player_config_t *config, audio_player_handle_t *handle)
{
    return audio_player_init(config, handle);
}

static esp_err_t audio_player_start_impl(audio_player_handle_t handle)
{
    return audio_player_start(handle);
}

static esp_err_t audio_player_stop_impl(audio_player_handle_t handle)
{
    return audio_player_stop(handle);
}

static esp_err_t audio_player_play_frame_impl(audio_player_handle_t handle, const uint8_t *data, uint32_t len)
{
    return audio_player_play_frame(handle, data, len);
}

static esp_err_t audio_player_deinit_impl(audio_player_handle_t handle)
{
    return audio_player_deinit(handle);
}

// Default video player implementation
static esp_err_t video_player_init_impl(video_player_config_t *config, video_player_handle_t *handle)
{
    return video_player_init(config, handle);
}

static esp_err_t video_player_start_impl(video_player_handle_t handle)
{
    return video_player_start(handle);
}

static esp_err_t video_player_stop_impl(video_player_handle_t handle)
{
    return video_player_stop(handle);
}

static esp_err_t video_player_play_frame_impl(video_player_handle_t handle, const uint8_t *data, uint32_t len, bool is_keyframe)
{
    return video_player_play_frame(handle, data, len, is_keyframe);
}

static esp_err_t video_player_deinit_impl(video_player_handle_t handle)
{
    return video_player_deinit(handle);
}

// Static interface structures for players
static media_stream_audio_player_t s_audio_player_if = {
    .init = audio_player_init_impl,
    .start = audio_player_start_impl,
    .stop = audio_player_stop_impl,
    .play_frame = audio_player_play_frame_impl,
    .deinit = audio_player_deinit_impl
};

static media_stream_video_player_t s_video_player_if = {
    .init = video_player_init_impl,
    .start = video_player_start_impl,
    .stop = video_player_stop_impl,
    .play_frame = video_player_play_frame_impl,
    .deinit = video_player_deinit_impl
};

// =============================================================================
// File-based Video Capture Interface Implementation
// =============================================================================

/* Sample files carry no clock. Unpaced, the faster medium starves the other on
 * the shared transport. Release each frame on its own period. */
static void file_capture_pace(int64_t *next_due_us, uint32_t period_us)
{
    int64_t now = esp_timer_get_time();
    int64_t wait_us = *next_due_us - now;
    if (wait_us > 0) {
        vTaskDelay(pdMS_TO_TICKS((wait_us + 999) / 1000));
    }
    /* A late reader restarts from now, so one slow frame does not bunch the rest. */
    *next_due_us = (wait_us > -(int64_t)period_us) ? *next_due_us + period_us
                                                   : esp_timer_get_time() + period_us;
}

static esp_err_t sample_video_read(uint32_t *index, uint8_t *frame_data, size_t max_size,
                                   size_t *actual_size);
static esp_err_t sample_audio_read(uint32_t *index, uint8_t *frame_data, size_t max_size,
                                   size_t *actual_size);
static bool h264_has_idr(const uint8_t *buf, uint32_t len);

/* Grow *buf to at least need bytes (SPIRAM preferred). Contents are not kept. */
static esp_err_t sample_buffer_reserve(uint8_t **buf, uint32_t *size, uint32_t need)
{
    if (*buf != NULL && *size >= need) {
        return ESP_OK;
    }
    heap_caps_free(*buf);
    *buf = (uint8_t *) heap_caps_malloc_prefer(need, 2,
                                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                                               MALLOC_CAP_DEFAULT | MALLOC_CAP_INTERNAL);
    *size = *buf ? need : 0;
    return *buf ? ESP_OK : ESP_ERR_NO_MEM;
}

static esp_err_t file_video_capture_init_impl(video_capture_config_t *config, video_capture_handle_t *handle)
{
    file_video_capture_state_t *state = NULL;

    if (config == NULL || handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    state = (file_video_capture_state_t *)calloc(1, sizeof(file_video_capture_state_t));
    if (state == NULL) {
        return ESP_ERR_NO_MEM;
    }

    state->started = false;
    state->current_bitrate_kbps = config->bitrate;
    state->frame_buffer = NULL;
    state->frame_buffer_size = 0;
    state->start_time = 0;
    state->period_us = 1000000u / (config->resolution.fps ? config->resolution.fps : DEFAULT_FPS_VALUE);

    *handle = (video_capture_handle_t)state;
    ESP_LOGI(TAG, "File-based video capture initialized");
    return ESP_OK;
}

static esp_err_t file_video_capture_get_frame_impl(video_capture_handle_t handle, video_frame_t **frame, uint32_t timeout_ms);

/* Consumers take encoded video from the sink registry (the WebRTC sender registers
 * a sink), not through get_frame(), so the file source publishes there itself.
 * One pump at a time: every source reads the same sample pack, and two pumps would
 * send each frame twice. The pump belongs to one started source; when that one
 * stops, another still-started source takes the pump over, so a viewer
 * keeps its video when streaming_only's media-selftest ends. get_frame() (the
 * selftest) and the pump each keep their own buffer and read position. */
#define FILE_VIDEO_MAX_SOURCES 4

static pthread_mutex_t s_file_video_lock = PTHREAD_MUTEX_INITIALIZER;
static file_video_capture_state_t *s_file_video_started[FILE_VIDEO_MAX_SOURCES];
static file_video_capture_state_t *s_file_video_pump_owner = NULL;
/* Timestamp origin of the published frames, in 100 ns units. Set when a source
 * takes a free pump and kept across a handover, so a viewer sees no jump. */
static uint64_t s_file_video_pump_epoch;

/* Frames are read only while a sink is enabled: with nobody listening the
 * position stays put. */
static void file_video_pump(void *arg)
{
    file_video_capture_state_t *state = (file_video_capture_state_t *) arg;
    TickType_t interval = pdMS_TO_TICKS(state->period_us / 1000);
    TickType_t last = xTaskGetTickCount();

    if (interval == 0) {
        interval = 1;
    }
    while (state->started) {
        vTaskDelayUntil(&last, interval);
        if (!state->started || !video_sink_any_enabled()) {
            continue;
        }
        /* A failed grow leaves no buffer; get one before reading again. */
        if (sample_buffer_reserve(&state->pump_buffer, &state->pump_buffer_size,
                                  state->pump_buffer ? 0 : 256 * 1024) != ESP_OK) {
            continue;
        }
        size_t len = 0;
        esp_err_t ret = sample_video_read(&state->pump_index, state->pump_buffer,
                                          state->pump_buffer_size, &len);
        if (ret == ESP_ERR_INVALID_SIZE &&
            sample_buffer_reserve(&state->pump_buffer, &state->pump_buffer_size, len) == ESP_OK) {
            ret = sample_video_read(&state->pump_index, state->pump_buffer,
                                    state->pump_buffer_size, &len);
        }
        if (ret != ESP_OK) {
            continue;
        }
        video_frame_t frame = {
            .buffer    = state->pump_buffer,
            .len       = len,
            .timestamp = esp_timer_get_time() * 10 - s_file_video_pump_epoch,
            .type      = h264_has_idr(state->pump_buffer, len) ? VIDEO_FRAME_TYPE_I
                                                               : VIDEO_FRAME_TYPE_P,
        };
        video_sink_dispatch(&frame);   /* sinks copy what they keep */
    }
    state->pump_done = true;
    vTaskDelete(NULL);
}

/* Called with s_file_video_lock held. handover: the pump passes from a source that
 * stopped, so the stream keeps its timestamp origin. */
static esp_err_t file_video_take_pump(file_video_capture_state_t *state, bool handover)
{
    if (sample_buffer_reserve(&state->pump_buffer, &state->pump_buffer_size, 256 * 1024) != ESP_OK) {
        return ESP_ERR_NO_MEM;
    }
    state->pump_index = 0;
    state->pump_done = false;
    if (!handover) {
        s_file_video_pump_epoch = state->start_time;
        s_video_ref_time = state->start_time;
    }
    if (xTaskCreate(file_video_pump, "file_vpump", 4096, state, 5, &state->pump) != pdPASS) {
        state->pump = NULL;
        return ESP_FAIL;
    }
    s_file_video_pump_owner = state;
    return ESP_OK;
}

static esp_err_t file_video_capture_start_impl(video_capture_handle_t handle)
{
    file_video_capture_state_t *state = (file_video_capture_state_t *)handle;
    esp_err_t ret = ESP_OK;

    if (state == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (state->started) {
        return ESP_OK;
    }

    pthread_mutex_lock(&s_file_video_lock);
    int slot = -1;
    for (int i = 0; i < FILE_VIDEO_MAX_SOURCES; i++) {
        if (s_file_video_started[i] == NULL) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        pthread_mutex_unlock(&s_file_video_lock);
        return ESP_ERR_NO_MEM;
    }

    state->started = true;
    state->start_time = esp_timer_get_time() * 10; // Convert to 100ns units
    state->next_due_us = esp_timer_get_time();
    state->index = 0;
    s_file_video_started[slot] = state;

    if (s_file_video_pump_owner == NULL) {
        ret = file_video_take_pump(state, false);
        if (ret != ESP_OK) {
            state->started = false;
            s_file_video_started[slot] = NULL;
        }
    } else {
        ESP_LOGI(TAG, "Another file video source owns the sink pump; this one serves get_frame() "
                 "and takes the pump over if that one stops first");
    }
    pthread_mutex_unlock(&s_file_video_lock);

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "File-based video capture started");
    }
    return ret;
}

static esp_err_t file_video_capture_stop_impl(video_capture_handle_t handle)
{
    file_video_capture_state_t *state = (file_video_capture_state_t *)handle;

    if (state == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!state->started) {
        return ESP_OK;
    }

    pthread_mutex_lock(&s_file_video_lock);
    state->started = false;
    for (int i = 0; i < FILE_VIDEO_MAX_SOURCES; i++) {
        if (s_file_video_started[i] == state) {
            s_file_video_started[i] = NULL;
        }
    }
    bool was_owner = (s_file_video_pump_owner == state);
    pthread_mutex_unlock(&s_file_video_lock);

    if (was_owner) {
        /* The pump may be inside a read; wait for it rather than free its state. */
        while (state->pump != NULL && !state->pump_done) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        state->pump = NULL;

        pthread_mutex_lock(&s_file_video_lock);
        s_file_video_pump_owner = NULL;
        for (int i = 0; i < FILE_VIDEO_MAX_SOURCES; i++) {
            file_video_capture_state_t *next = s_file_video_started[i];
            if (next != NULL && file_video_take_pump(next, true) == ESP_OK) {
                ESP_LOGI(TAG, "File video sink pump handed over to the remaining source");
                break;
            }
        }
        pthread_mutex_unlock(&s_file_video_lock);
    }
    ESP_LOGI(TAG, "File-based video capture stopped");
    return ESP_OK;
}

static esp_err_t file_video_capture_get_frame_impl(video_capture_handle_t handle, video_frame_t **frame, uint32_t timeout_ms)
{
    file_video_capture_state_t *state = (file_video_capture_state_t *)handle;
    esp_err_t ret = ESP_OK;
    size_t actual_size = 0;
    uint64_t timestamp = 0;

    if (state == NULL || frame == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!state->started) {
        return ESP_ERR_INVALID_STATE;
    }

    // Start with 256KB (SPIRAM preferred); grown below when a frame needs more.
    ret = sample_buffer_reserve(&state->frame_buffer, &state->frame_buffer_size, 256 * 1024);
    if (ret != ESP_OK) {
        return ret;
    }

    file_capture_pace(&state->next_due_us, state->period_us);

    ret = sample_video_read(&state->index, state->frame_buffer, state->frame_buffer_size, &actual_size);
    if (ret == ESP_ERR_INVALID_SIZE) {
        /* actual_size holds the size needed; the read position did not move. */
        ret = sample_buffer_reserve(&state->frame_buffer, &state->frame_buffer_size, actual_size);
        if (ret == ESP_OK) {
            ret = sample_video_read(&state->index, state->frame_buffer, state->frame_buffer_size,
                                    &actual_size);
        }
    }
    if (ret != ESP_OK) {
        return ret;
    }
    timestamp = esp_timer_get_time() * 10 - state->start_time; // 100ns units, like start_time

    // Fill frame structure
    state->current_frame.buffer = state->frame_buffer;
    state->current_frame.len = actual_size;
    state->current_frame.timestamp = timestamp;
    /* Typed from the content: the sample pack has one IDR per loop, and taking a
     * P-frame for a keyframe opens a sink's start gate on an undecodable frame. */
    state->current_frame.type = h264_has_idr(state->frame_buffer, actual_size) ? VIDEO_FRAME_TYPE_I
                                                                               : VIDEO_FRAME_TYPE_P;

    *frame = &state->current_frame;
    return ESP_OK;
}

static esp_err_t file_video_capture_release_frame_impl(video_capture_handle_t handle, video_frame_t *frame)
{
    // For file-based capture, frames are managed internally, no action needed
    (void)handle;
    (void)frame;
    return ESP_OK;
}

static esp_err_t file_video_capture_deinit_impl(video_capture_handle_t handle)
{
    file_video_capture_state_t *state = (file_video_capture_state_t *)handle;

    if (state == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* The pump reads this state; it has to be gone before the state is freed. */
    file_video_capture_stop_impl(handle);

    if (state->frame_buffer != NULL) {
        heap_caps_free(state->frame_buffer);
        state->frame_buffer = NULL;
    }
    heap_caps_free(state->pump_buffer);
    state->pump_buffer = NULL;

    free(state);
    ESP_LOGI(TAG, "File-based video capture deinitialized");
    return ESP_OK;
}

static esp_err_t file_video_capture_set_bitrate_impl(video_capture_handle_t handle, uint32_t bitrate_kbps)
{
    file_video_capture_state_t *state = (file_video_capture_state_t *)handle;

    if (state == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    state->current_bitrate_kbps = bitrate_kbps;
    ESP_LOGD(TAG, "File-based video capture bitrate set to %" PRIu32 " kbps", bitrate_kbps);
    return ESP_OK;
}

static esp_err_t file_video_capture_get_bitrate_impl(video_capture_handle_t handle, uint32_t *bitrate_kbps)
{
    file_video_capture_state_t *state = (file_video_capture_state_t *)handle;

    if (state == NULL || bitrate_kbps == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *bitrate_kbps = state->current_bitrate_kbps;
    return ESP_OK;
}

// Static file-based video capture interface
static media_stream_video_capture_t s_file_video_capture = {
    .init = file_video_capture_init_impl,
    .start = file_video_capture_start_impl,
    .stop = file_video_capture_stop_impl,
    .get_frame = file_video_capture_get_frame_impl,
    .release_frame = file_video_capture_release_frame_impl,
    .deinit = file_video_capture_deinit_impl,
    .set_bitrate = file_video_capture_set_bitrate_impl,
    .get_bitrate = file_video_capture_get_bitrate_impl
};

// =============================================================================
// File-based Audio Capture Interface Implementation
// =============================================================================

static esp_err_t file_audio_capture_init_impl(audio_capture_config_t *config, audio_capture_handle_t *handle)
{
    file_audio_capture_state_t *state = NULL;

    if (config == NULL || handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    state = (file_audio_capture_state_t *)calloc(1, sizeof(file_audio_capture_state_t));
    if (state == NULL) {
        return ESP_ERR_NO_MEM;
    }

    state->started = false;
    state->frame_buffer = NULL;
    state->frame_buffer_size = 0;
    state->start_time = 0;
    state->period_us = 1000u * (config->frame_duration_ms ? config->frame_duration_ms : 20);

    *handle = (audio_capture_handle_t)state;
    ESP_LOGI(TAG, "File-based audio capture initialized");
    return ESP_OK;
}

static esp_err_t file_audio_capture_start_impl(audio_capture_handle_t handle)
{
    file_audio_capture_state_t *state = (file_audio_capture_state_t *)handle;

    if (state == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    state->started = true;
    state->start_time = esp_timer_get_time() * 10; // Convert to 100ns units
    state->index = 0;
    state->next_due_us = esp_timer_get_time();
    ESP_LOGI(TAG, "File-based audio capture started");
    return ESP_OK;
}

static esp_err_t file_audio_capture_stop_impl(audio_capture_handle_t handle)
{
    file_audio_capture_state_t *state = (file_audio_capture_state_t *)handle;

    if (state == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    state->started = false;
    ESP_LOGI(TAG, "File-based audio capture stopped");
    return ESP_OK;
}

static esp_err_t file_audio_capture_get_frame_impl(audio_capture_handle_t handle, audio_frame_t **frame, uint32_t timeout_ms)
{
    file_audio_capture_state_t *state = (file_audio_capture_state_t *)handle;
    esp_err_t ret = ESP_OK;
    size_t actual_size = 0;
    uint64_t timestamp = 0;

    if (state == NULL || frame == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!state->started) {
        return ESP_ERR_INVALID_STATE;
    }

    // Start with 4 KB; grown below when a frame needs more.
    ret = sample_buffer_reserve(&state->frame_buffer, &state->frame_buffer_size, 4 * 1024);
    if (ret != ESP_OK) {
        return ret;
    }

    file_capture_pace(&state->next_due_us, state->period_us);

    // Own read position: another file source (media-selftest) does not move it.
    ret = sample_audio_read(&state->index, state->frame_buffer, state->frame_buffer_size, &actual_size);
    if (ret == ESP_ERR_INVALID_SIZE) {
        ret = sample_buffer_reserve(&state->frame_buffer, &state->frame_buffer_size, actual_size);
        if (ret == ESP_OK) {
            ret = sample_audio_read(&state->index, state->frame_buffer, state->frame_buffer_size,
                                    &actual_size);
        }
    }
    if (ret != ESP_OK) {
        return ret;
    }
    timestamp = esp_timer_get_time() * 10 - state->start_time; // 100ns units, like start_time

    // Fill frame structure
    state->current_frame.buffer = state->frame_buffer;
    state->current_frame.len = actual_size;
    state->current_frame.timestamp = timestamp;

    *frame = &state->current_frame;
    return ESP_OK;
}

static esp_err_t file_audio_capture_release_frame_impl(audio_capture_handle_t handle, audio_frame_t *frame)
{
    // For file-based capture, frames are managed internally, no action needed
    (void)handle;
    (void)frame;
    return ESP_OK;
}

static esp_err_t file_audio_capture_deinit_impl(audio_capture_handle_t handle)
{
    file_audio_capture_state_t *state = (file_audio_capture_state_t *)handle;

    if (state == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (state->frame_buffer != NULL) {
        heap_caps_free(state->frame_buffer);
        state->frame_buffer = NULL;
    }

    free(state);
    ESP_LOGI(TAG, "File-based audio capture deinitialized");
    return ESP_OK;
}

// Static file-based audio capture interface
static media_stream_audio_capture_t s_file_audio_capture = {
    .init = file_audio_capture_init_impl,
    .start = file_audio_capture_start_impl,
    .stop = file_audio_capture_stop_impl,
    .get_frame = file_audio_capture_get_frame_impl,
    .release_frame = file_audio_capture_release_frame_impl,
    .deinit = file_audio_capture_deinit_impl
};

// Get interface functions
media_stream_audio_capture_t* media_stream_get_audio_capture_if(void)
{
    return &s_audio_capture;
}

media_stream_video_capture_t* media_stream_get_video_capture_if(void)
{
    return &s_video_capture;
}

media_stream_video_capture_t* media_stream_get_file_video_capture_if(void)
{
    return &s_file_video_capture;
}

media_stream_audio_capture_t* media_stream_get_file_audio_capture_if(void)
{
    return &s_file_audio_capture;
}

// Get player interface functions
media_stream_audio_player_t* media_stream_get_audio_player_if(void)
{
    return &s_audio_player_if;
}

media_stream_video_player_t* media_stream_get_video_player_if(void)
{
    return &s_video_player_if;
}

// Register callback for received frames
void media_stream_register_frame_received_cb(media_stream_frame_received_cb_t callback, void *user_data)
{
    s_frame_received_cb = callback;
    s_frame_received_user_data = user_data;
}

// Register callback for frames ready to be sent
void media_stream_register_frame_ready_cb(media_stream_frame_ready_cb_t callback, void *user_data)
{
    s_frame_ready_cb = callback;
    s_frame_ready_user_data = user_data;
}

esp_err_t media_stream_init(video_capture_handle_t *video_handle, audio_capture_handle_t *audio_handle)
{
    esp_err_t ret = ESP_OK;

    // Initialize video capture with H264 configuration
    video_capture_config_t video_config = {
        .codec = VIDEO_CODEC_H264,
        .resolution = {
            .width = 640,
            .height = 480,
            .fps = 30
        },
        .quality = 80,
        .bitrate = 500, // 500 kbps
        .codec_specific = NULL
    };

    // Initialize audio capture with Opus configuration.
    // 16 kHz mono end-to-end: SDP fmtp `maxplaybackrate=16000` (the Opus
    // narrowing patch) caps the peer at 16 kHz; running the encoder at a
    // higher rate would just waste CPU since the peer downsamples anyway.
    audio_capture_config_t audio_config = {
        .codec = AUDIO_CODEC_OPUS,
        .format = {
            .sample_rate = 16000,
            .channels = 1,
            .bits_per_sample = 16
        },
        .bitrate = 64,  // 64 kbps
        .frame_duration_ms = 20,
        .codec_specific = NULL
    };

    // Initialize video player with H264 configuration
    video_player_config_t video_player_config = {
        .codec = VIDEO_PLAYER_CODEC_H264,
        .format = {
            .width = 640,
            .height = 480,
            .framerate = 30
        },
        .buffer_frames = 10,
        .codec_specific = NULL,
        .display_handle = NULL
    };

    // Initialize audio player with Opus configuration
    audio_player_config_t audio_player_config = {
        .codec = AUDIO_PLAYER_CODEC_OPUS,
        .format = {
            .sample_rate = 48000,
            .channels = 1,
            .bits_per_sample = 16
        },
        .buffer_ms = 500,  // 500 ms buffer
        .codec_specific = NULL
    };

#if CONFIG_IDF_TARGET_ESP32P4 && !CONFIG_BSP_SELECT_NONE
    /* Bring the BSP I2C bus up once, here. Camera SCCB (via
     * bsp_i2c_get_handle()) and the ES8311 codec init both share this bus,
     * and the BSP's own bsp_i2c_init() is idempotent — letting the BSP own
     * the bus avoids the previous wrapper, which tried to second-guess
     * pull-up / handle state and ended up racing with the codec init at
     * session start. */
    ret = bsp_i2c_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "bsp_i2c_init failed: %s", esp_err_to_name(ret));
        return ret;
    }
#endif

    // Initialize video capture
    ESP_LOGI(TAG, "Initializing video capture");
    ret = video_capture_init(&video_config, &s_video_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize video capture: %d", ret);
        return ret;
    }

    // Initialize audio capture
    ESP_LOGI(TAG, "Initializing audio capture");
    ret = audio_capture_init(&audio_config, &s_audio_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize audio capture: %d", ret);
        video_capture_deinit(s_video_handle);
        s_video_handle = NULL;
        return ret;
    }

    // Initialize video player
    ESP_LOGI(TAG, "Initializing video player");
    ret = video_player_init(&video_player_config, &s_video_player);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize video player: %d", ret);
        audio_capture_deinit(s_audio_handle);
        video_capture_deinit(s_video_handle);
        s_audio_handle = NULL;
        s_video_handle = NULL;
        return ret;
    }

    // Initialize audio player
    ESP_LOGI(TAG, "Initializing audio player");
    ret = audio_player_init(&audio_player_config, &s_audio_player);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize audio player: %d", ret);
        video_player_deinit(s_video_player);
        audio_capture_deinit(s_audio_handle);
        video_capture_deinit(s_video_handle);
        s_video_player = NULL;
        s_audio_handle = NULL;
        s_video_handle = NULL;
        return ret;
    }

    // Initialize reference times (TODO: is this good way for reference time?)
    s_video_ref_time = esp_timer_get_time() * 10; // Convert to 100ns units
    s_audio_ref_time = s_video_ref_time;

    // Set the output parameters
    if (video_handle) {
        *video_handle = s_video_handle;
    }

    if (audio_handle) {
        *audio_handle = s_audio_handle;
    }

    return ESP_OK;
}

esp_err_t media_stream_start(video_capture_handle_t video_handle, audio_capture_handle_t audio_handle)
{
    esp_err_t ret = ESP_OK;

    // Use passed handles or defaults if NULL
    video_capture_handle_t vid_handle = video_handle ? video_handle : s_video_handle;
    audio_capture_handle_t aud_handle = audio_handle ? audio_handle : s_audio_handle;

    if (vid_handle) {
        ret = video_capture_start(vid_handle);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to start video capture");
            return ret;
        }
    }

    if (aud_handle) {
        ret = audio_capture_start(aud_handle);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to start audio capture");
            if (vid_handle) {
                video_capture_stop(vid_handle);
            }
            return ret;
        }
    }

    // Start the players if they exist
    if (s_video_player) {
        ret = video_player_start(s_video_player);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to start video player");
            if (aud_handle) {
                audio_capture_stop(aud_handle);
            }
            if (vid_handle) {
                video_capture_stop(vid_handle);
            }
            return ret;
        }
    }

    if (s_audio_player) {
        ret = audio_player_start(s_audio_player);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to start audio player");
            if (s_video_player) {
                video_player_stop(s_video_player);
            }
            if (aud_handle) {
                audio_capture_stop(aud_handle);
            }
            if (vid_handle) {
                video_capture_stop(vid_handle);
            }
            return ret;
        }
    }

    return ESP_OK;
}

esp_err_t media_stream_stop(video_capture_handle_t video_handle, audio_capture_handle_t audio_handle)
{
    // Use passed handles or defaults if NULL
    video_capture_handle_t vid_handle = video_handle ? video_handle : s_video_handle;
    audio_capture_handle_t aud_handle = audio_handle ? audio_handle : s_audio_handle;

    // Stop players first
    if (s_audio_player) {
        audio_player_stop(s_audio_player);
    }

    if (s_video_player) {
        video_player_stop(s_video_player);
    }

    // Then stop capture devices
    if (aud_handle) {
        audio_capture_stop(aud_handle);
    }

    if (vid_handle) {
        video_capture_stop(vid_handle);
    }

    return ESP_OK;
}

esp_err_t media_stream_deinit(video_capture_handle_t video_handle, audio_capture_handle_t audio_handle)
{
    // Use passed handles or defaults if NULL
    video_capture_handle_t vid_handle = video_handle ? video_handle : s_video_handle;
    audio_capture_handle_t aud_handle = audio_handle ? audio_handle : s_audio_handle;

    // Deinitialize players first
    if (s_audio_player) {
        audio_player_deinit(s_audio_player);
        s_audio_player = NULL;
    }

    if (s_video_player) {
        video_player_deinit(s_video_player);
        s_video_player = NULL;
    }

    // Then deinitialize capture devices
    if (aud_handle) {
        audio_capture_deinit(aud_handle);
        if (aud_handle == s_audio_handle) {
            s_audio_handle = NULL;
        }
    }

    if (vid_handle) {
        video_capture_deinit(vid_handle);
        if (vid_handle == s_video_handle) {
            s_video_handle = NULL;
        }
    }

    return ESP_OK;
}

void media_stream_handle_video_frame(uint32_t stream_id, uint8_t *frame_data, size_t frame_size,
                                     uint64_t timestamp, bool is_key_frame)
{
    ESP_LOGD(TAG, "Received video frame: stream_id=%" PRIu32 ", size=%zu, ts=%" PRIu64 ", key=%d",
             stream_id, frame_size, timestamp, is_key_frame);

    // Call the registered callback if any
    if (s_frame_received_cb != NULL) {
        s_frame_received_cb(stream_id, MEDIA_STREAM_VIDEO, frame_data, frame_size,
                           timestamp, is_key_frame, s_frame_received_user_data);
    }

    // Play the frame if we have a player initialized
    if (s_video_player != NULL) {
        esp_err_t ret = video_player_play_frame(s_video_player, frame_data, frame_size, is_key_frame);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to play video frame: %d", ret);
        }
    }
}

void media_stream_handle_audio_frame(uint32_t stream_id, uint8_t *frame_data, size_t frame_size,
                                  uint64_t timestamp)
{
    ESP_LOGD(TAG, "Received audio frame: stream_id=%" PRIu32 ", size=%zu, ts=%" PRIu64,
             stream_id, frame_size, timestamp);

    // Call the registered callback if any
    if (s_frame_received_cb != NULL) {
        s_frame_received_cb(stream_id, MEDIA_STREAM_AUDIO, frame_data, frame_size,
                           timestamp, false, s_frame_received_user_data);
    }

    // Play the frame if we have a player initialized
    if (s_audio_player != NULL) {
        esp_err_t ret = audio_player_play_frame(s_audio_player, frame_data, frame_size);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to play audio frame: %d", ret);
        }
    }
}

esp_err_t media_stream_configure_file_paths(const media_stream_file_config_t *config)
{
    int needed_size;

    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    // Update mount point if provided
    if (config->mount_point != NULL) {
        needed_size = snprintf(s_mount_point, sizeof(s_mount_point), "%s", config->mount_point);
        if (needed_size < 0) {
            ESP_LOGE(TAG, "Failed to format mount point");
            return ESP_FAIL;
        } else if (needed_size >= sizeof(s_mount_point)) {
            ESP_LOGE(TAG, "Mount point too long: %s", config->mount_point);
            return ESP_ERR_NO_MEM;
        }
    }

    // Update video file prefix if provided
    if (config->video_file_prefix != NULL) {
        needed_size = snprintf(s_video_file_prefix, sizeof(s_video_file_prefix), "%s", config->video_file_prefix);
        if (needed_size < 0) {
            ESP_LOGE(TAG, "Failed to format video file prefix");
            return ESP_FAIL;
        } else if (needed_size >= sizeof(s_video_file_prefix)) {
            ESP_LOGE(TAG, "Video file prefix too long: %s", config->video_file_prefix);
            return ESP_ERR_NO_MEM;
        }
    }

    // Update audio file prefix if provided
    if (config->audio_file_prefix != NULL) {
        needed_size = snprintf(s_audio_file_prefix, sizeof(s_audio_file_prefix), "%s", config->audio_file_prefix);
        if (needed_size < 0) {
            ESP_LOGE(TAG, "Failed to format audio file prefix");
            return ESP_FAIL;
        } else if (needed_size >= sizeof(s_audio_file_prefix)) {
            ESP_LOGE(TAG, "Audio file prefix too long: %s", config->audio_file_prefix);
            return ESP_ERR_NO_MEM;
        }
    }

    // Update video file extension if provided
    if (config->video_file_extension != NULL) {
        needed_size = snprintf(s_video_file_extension, sizeof(s_video_file_extension), "%s", config->video_file_extension);
        if (needed_size < 0) {
            ESP_LOGE(TAG, "Failed to format video file extension");
            return ESP_FAIL;
        } else if (needed_size >= sizeof(s_video_file_extension)) {
            ESP_LOGE(TAG, "Video file extension too long: %s", config->video_file_extension);
            return ESP_ERR_NO_MEM;
        }
    }

    // Update audio file extension if provided
    if (config->audio_file_extension != NULL) {
        needed_size = snprintf(s_audio_file_extension, sizeof(s_audio_file_extension), "%s", config->audio_file_extension);
        if (needed_size < 0) {
            ESP_LOGE(TAG, "Failed to format audio file extension");
            return ESP_FAIL;
        } else if (needed_size >= sizeof(s_audio_file_extension)) {
            ESP_LOGE(TAG, "Audio file extension too long: %s", config->audio_file_extension);
            return ESP_ERR_NO_MEM;
        }
    }

    ESP_LOGI(TAG, "File paths configured: mount=%s, video=%s-XXXX%s, audio=%s-XXXX%s",
             s_mount_point, s_video_file_prefix, s_video_file_extension,
             s_audio_file_prefix, s_audio_file_extension);

    return ESP_OK;
}

esp_err_t media_stream_read_frame_from_disk(uint8_t *frame_data, uint32_t *size, const char *frame_path)
{
    if (size == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    FILE *fp = fopen(frame_path, "rb");
    if (fp == NULL) {
        ESP_LOGE(TAG, "Failed to open file: %s", frame_path);
        return ESP_ERR_NOT_FOUND;
    }

    // Get file size
    fseek(fp, 0, SEEK_END);
    long file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    // If frame_data is NULL, just return the size
    if (frame_data == NULL) {
        *size = (uint32_t)file_size;
        fclose(fp);
        return ESP_OK;
    }

    // Check if buffer is large enough
    if (*size < file_size) {
        fclose(fp);
        *size = (uint32_t)file_size;
        return ESP_ERR_INVALID_SIZE;
    }

    // Read the file
    size_t bytes_read = fread(frame_data, 1, file_size, fp);
    fclose(fp);

    if (bytes_read != file_size) {
        ESP_LOGE(TAG, "Failed to read file: %s", frame_path);
        return ESP_FAIL;
    }

    *size = (uint32_t)bytes_read;
    return ESP_OK;
}

/* Reads the sample after *index (1-based, wrapping) from a medium's pack into
 * frame_data. *index moves only on success, so a caller that grows its buffer after
 * ESP_ERR_INVALID_SIZE (*actual_size then holds the size needed) reads the same
 * frame again. Reads are serialised: every file source shares the open packs. */
static pthread_mutex_t s_sample_lock = PTHREAD_MUTEX_INITIALIZER;

static esp_err_t sample_read(sample_pack_t *p, const char *prefix, const char *name,
                             uint32_t *index, uint8_t *frame_data, size_t max_size,
                             size_t *actual_size)
{
    if (frame_data == NULL) {
        return ESP_ERR_INVALID_ARG;   /* pack_read() takes a NULL buffer as a size probe */
    }

    pthread_mutex_lock(&s_sample_lock);
    esp_err_t ret = pack_open(p, prefix, name);
    if (ret == ESP_OK) {
        uint32_t next = *index % p->count + 1;
        uint32_t size = (uint32_t) max_size;
        ret = pack_read(p, next - 1, frame_data, &size);
        if (ret == ESP_OK || ret == ESP_ERR_INVALID_SIZE) {
            *actual_size = size;
        }
        if (ret == ESP_OK) {
            *index = next;
        }
    }
    pthread_mutex_unlock(&s_sample_lock);
    return ret;
}

static esp_err_t sample_video_read(uint32_t *index, uint8_t *frame_data, size_t max_size,
                                   size_t *actual_size)
{
    return sample_read(&s_video_pack, s_video_file_prefix, "video.pack",
                       index, frame_data, max_size, actual_size);
}

static esp_err_t sample_audio_read(uint32_t *index, uint8_t *frame_data, size_t max_size,
                                   size_t *actual_size)
{
    return sample_read(&s_audio_pack, s_audio_file_prefix, "audio.pack",
                       index, frame_data, max_size, actual_size);
}

/* An access unit with an IDR slice or an SPS starts a decodable stream. The sample
 * pack holds one IDR per loop; every other frame is a P-slice. */
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

esp_err_t media_stream_get_sample_video_frame(uint8_t *frame_data, size_t max_size,
                                              size_t *actual_size, uint64_t *timestamp)
{
    if (!frame_data || !actual_size || !timestamp) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = sample_video_read(&s_video_file_index, frame_data, max_size, actual_size);
    if (ret != ESP_OK) {
        return ret;
    }
    // TODO: is this good way for reference time?
    *timestamp = esp_timer_get_time() * 10 - s_video_ref_time; // Convert to 100ns units

    // Notify frame ready callback if registered
    if (s_frame_ready_cb != NULL) {
        s_frame_ready_cb(
            MEDIA_STREAM_VIDEO,
            frame_data,
            *actual_size,
            *timestamp,
            h264_has_idr(frame_data, *actual_size),
            s_frame_ready_user_data
        );
    }

    return ESP_OK;
}

esp_err_t media_stream_get_sample_audio_frame(uint8_t *frame_data, size_t max_size,
                                              size_t *actual_size, uint64_t *timestamp)
{
    if (!frame_data || !actual_size || !timestamp) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = sample_audio_read(&s_audio_file_index, frame_data, max_size, actual_size);
    if (ret != ESP_OK) {
        return ret;
    }
    // TODO: is this good way for reference time?
    *timestamp = esp_timer_get_time() * 10 - s_audio_ref_time; // Convert to 100ns units

    // Notify frame ready callback if registered
    if (s_frame_ready_cb != NULL) {
        s_frame_ready_cb(
            MEDIA_STREAM_AUDIO,
            frame_data,
            *actual_size,
            *timestamp,
            false, // Audio frames are never key frames
            s_frame_ready_user_data
        );
    }

    return ESP_OK;
}
