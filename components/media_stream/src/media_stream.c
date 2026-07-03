/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include <stdio.h>
#include <inttypes.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "media_stream.h"
/* bsp_selector is gated to `target not in [esp32c5, esp32c6]` in
 * idf_component.yml — the header is missing on c-series builds. The
 * sole use of a BSP symbol (bsp_i2c_init at l.680) is already inside
 * a `#if CONFIG_IDF_TARGET_ESP32P4 && !CONFIG_BSP_SELECT_NONE` gate. */
#if !CONFIG_IDF_TARGET_ESP32C5 && !CONFIG_IDF_TARGET_ESP32C6
#include "bsp/esp-bsp.h"
#endif
#include "driver/i2c_master.h"

static const char *TAG = "media_stream";

#define NUMBER_OF_H264_FRAME_FILES               60 //1500
#define NUMBER_OF_H265_FRAME_FILES               1500
#define NUMBER_OF_OPUS_FRAME_FILES               618

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

// File-based capture state structures
typedef struct {
    bool started;
    video_frame_t current_frame;
    uint8_t *frame_buffer;
    uint32_t frame_buffer_size;
    uint32_t current_bitrate_kbps;
    uint64_t start_time;
} file_video_capture_state_t;

typedef struct {
    bool started;
    audio_frame_t current_frame;
    uint8_t *frame_buffer;
    uint32_t frame_buffer_size;
    uint64_t start_time;
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

    *handle = (video_capture_handle_t)state;
    ESP_LOGI(TAG, "File-based video capture initialized");
    return ESP_OK;
}

static esp_err_t file_video_capture_start_impl(video_capture_handle_t handle)
{
    file_video_capture_state_t *state = (file_video_capture_state_t *)handle;

    if (state == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    state->started = true;
    state->start_time = esp_timer_get_time() * 10; // Convert to 100ns units
    s_video_ref_time = state->start_time;
    s_video_file_index = 0;
    ESP_LOGI(TAG, "File-based video capture started");
    return ESP_OK;
}

static esp_err_t file_video_capture_stop_impl(video_capture_handle_t handle)
{
    file_video_capture_state_t *state = (file_video_capture_state_t *)handle;

    if (state == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    state->started = false;
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

    // Allocate buffer if needed (start with 256KB, will grow if needed)
    // Prefer SPIRAM, fallback to internal memory
    if (state->frame_buffer == NULL) {
        state->frame_buffer_size = 256 * 1024;
        state->frame_buffer = (uint8_t *) heap_caps_malloc_prefer(state->frame_buffer_size, 2,
                                                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                                                                  MALLOC_CAP_DEFAULT | MALLOC_CAP_INTERNAL);
        if (state->frame_buffer == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    // Get frame from file
    ret = media_stream_get_sample_video_frame(state->frame_buffer, state->frame_buffer_size,
                                              &actual_size, &timestamp);
    if (ret != ESP_OK) {
        if (ret == ESP_ERR_INVALID_SIZE) {
            // Buffer too small, reallocate - prefer SPIRAM, fallback to internal memory
            heap_caps_free(state->frame_buffer);
            state->frame_buffer_size = actual_size;
            state->frame_buffer = (uint8_t *) heap_caps_malloc_prefer(state->frame_buffer_size, 2,
                                                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                                                                      MALLOC_CAP_DEFAULT | MALLOC_CAP_INTERNAL);
            if (state->frame_buffer == NULL) {
                return ESP_ERR_NO_MEM;
            }
            ret = media_stream_get_sample_video_frame(state->frame_buffer, state->frame_buffer_size,
                                                      &actual_size, &timestamp);
        }
        if (ret != ESP_OK) {
            return ret;
        }
    }

    // Fill frame structure
    state->current_frame.buffer = state->frame_buffer;
    state->current_frame.len = actual_size;
    state->current_frame.timestamp = timestamp;
    state->current_frame.type = VIDEO_FRAME_TYPE_I; // Sample frames are treated as I-frames

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

    if (state->frame_buffer != NULL) {
        heap_caps_free(state->frame_buffer);
        state->frame_buffer = NULL;
    }

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
    s_audio_ref_time = state->start_time;
    s_audio_file_index = 0;
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

    // Allocate buffer if needed (start with 4KB, will grow if needed)
    // Prefer SPIRAM, fallback to internal memory
    if (state->frame_buffer == NULL) {
        state->frame_buffer_size = 4 * 1024;
        state->frame_buffer = (uint8_t *) heap_caps_malloc_prefer(state->frame_buffer_size, 2,
                                                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                                                                  MALLOC_CAP_DEFAULT | MALLOC_CAP_INTERNAL);
        if (state->frame_buffer == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    // Get frame from file
    ret = media_stream_get_sample_audio_frame(state->frame_buffer, state->frame_buffer_size,
                                              &actual_size, &timestamp);
    if (ret != ESP_OK) {
        if (ret == ESP_ERR_INVALID_SIZE) {
            // Buffer too small, reallocate - prefer SPIRAM, fallback to internal memory
            heap_caps_free(state->frame_buffer);
            state->frame_buffer_size = actual_size;
            state->frame_buffer = (uint8_t *) heap_caps_malloc_prefer(state->frame_buffer_size, 2,
                                                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                                                                      MALLOC_CAP_DEFAULT | MALLOC_CAP_INTERNAL);
            if (state->frame_buffer == NULL) {
                return ESP_ERR_NO_MEM;
            }
            ret = media_stream_get_sample_audio_frame(state->frame_buffer, state->frame_buffer_size,
                                                      &actual_size, &timestamp);
        }
        if (ret != ESP_OK) {
            return ret;
        }
    }

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

esp_err_t media_stream_get_sample_video_frame(uint8_t *frame_data, size_t max_size,
                                              size_t *actual_size, uint64_t *timestamp)
{
    if (!frame_data || !actual_size || !timestamp) {
        return ESP_ERR_INVALID_ARG;
    }

    // Update file index and get next frame
    s_video_file_index = s_video_file_index % NUMBER_OF_H264_FRAME_FILES + 1;
    ESP_LOGD(TAG, "Getting H264 Sample: %" PRIu32, s_video_file_index);

    char filePath[256];
    int needed_size = snprintf(filePath, sizeof(filePath), "%s/%s-%04" PRIu32 "%s",
                               s_mount_point, s_video_file_prefix, s_video_file_index, s_video_file_extension);

    if (needed_size < 0) {
        ESP_LOGE(TAG, "Failed to format file path: %s", filePath);
        return ESP_FAIL;
    } else if (needed_size >= sizeof(filePath)) {
        ESP_LOGE(TAG, "File path too long: %s", filePath);
        return ESP_ERR_NO_MEM;
    }

    // Get frame size first
    uint32_t frameSize = max_size;
    esp_err_t ret = media_stream_read_frame_from_disk(NULL, &frameSize, filePath);
    if (ret != ESP_OK) {
        return ret;
    }

    if (frameSize > max_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    // Read the actual frame
    ret = media_stream_read_frame_from_disk(frame_data, &frameSize, filePath);
    if (ret != ESP_OK) {
        return ret;
    }

    *actual_size = frameSize;
    // TODO: is this good way for reference time?
    *timestamp = esp_timer_get_time() * 10 - s_video_ref_time; // Convert to 100ns units

    // Notify frame ready callback if registered
    if (s_frame_ready_cb != NULL) {
        s_frame_ready_cb(
            MEDIA_STREAM_VIDEO,
            frame_data,
            *actual_size,
            *timestamp,
            true, // Assume sample frames are key frames
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

    // Update file index and get next frame
    s_audio_file_index = s_audio_file_index % NUMBER_OF_OPUS_FRAME_FILES + 1;
    ESP_LOGD(TAG, "Getting Opus Sample: %" PRIu32, s_audio_file_index);

    char filePath[256];
    int needed_size = snprintf(filePath, sizeof(filePath), "%s/%s-%04" PRIu32 "%s",
                               s_mount_point, s_audio_file_prefix, s_audio_file_index, s_audio_file_extension);
    if (needed_size < 0) {
        ESP_LOGE(TAG, "Failed to format file path: %s", filePath);
        return ESP_FAIL;
    } else if (needed_size >= sizeof(filePath)) {
        ESP_LOGE(TAG, "File path too long: %s", filePath);
        return ESP_ERR_NO_MEM;
    }

    // Get frame size first
    uint32_t frameSize = max_size;
    esp_err_t ret = media_stream_read_frame_from_disk(NULL, &frameSize, filePath);
    if (ret != ESP_OK) {
        return ret;
    }

    if (frameSize > max_size) {
        *actual_size = frameSize;
        return ESP_ERR_INVALID_SIZE;
    }

    // Read the actual frame
    ret = media_stream_read_frame_from_disk(frame_data, &frameSize, filePath);
    if (ret != ESP_OK) {
        return ret;
    }

    *actual_size = frameSize;
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
