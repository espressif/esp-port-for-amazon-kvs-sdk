/**
 * SPDX-FileCopyrightText: 2024-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include "esp_err.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"

#include "esp_audio_dec_default.h"
#include "esp_audio_dec.h"
#include "ringbuf.h"
#include "sdkconfig.h"

/* bsp_selector (which provides bsp/esp-bsp.h) is gated in
 * media_stream/idf_component.yml to `target not in [esp32c5, esp32c6]`,
 * so the header is missing on c-series builds. BSP_CAPS_* macros it
 * defines fall through to undefined (= 0), correctly disabling the
 * codec/display blocks below. */
#if !CONFIG_IDF_TARGET_ESP32C5 && !CONFIG_IDF_TARGET_ESP32C6
#include "bsp/esp-bsp.h"
#if CONFIG_IDF_TARGET_ESP32P4
#include "esp_codec_dev.h"
#endif
#endif

static const char *TAG = "OPUS_AUDIO_PLAYER";

typedef struct {
    bool                      use_common_api;
    esp_audio_dec_handle_t    decoder;
    esp_audio_dec_out_frame_t out_frame;
    bool                      decode_err;
} write_ctx_t;

static write_ctx_t write_ctx;
static rb_handle_t rb_handle;
/* Only declared when the BSP advertises a speaker: the P4 playback task and the
 * codec init below are both gated on BSP_CAPS_AUDIO_SPEAKER; boards without one
 * (e.g. P4-EYE) use the no-op playback stub and never touch this handle. */
#if BSP_CAPS_AUDIO_SPEAKER
static esp_codec_dev_handle_t spk_codec_dev = NULL;
#endif

#define OPUS_PLAYER_TASK        (1)
#define CODEC_SAMPLE_RATE       (16000)
#define OPUS_OUTPUT_BUFFER_SIZE (4096)
#define OPUS_PLAYER_OUT_RB_SIZE (128000)
/* Speaker codec output channels. ES8311 is a mono codec on both EV-board
 * and P4-EYE; we open it with channel=1 (see bsp_audio_codec_speaker_init
 * call below). Tell the Opus decoder to emit mono so the sample count per
 * frame matches what the codec consumes per real-time interval — otherwise
 * a 2-channel decoder pushes 2× samples per frame and the speaker plays
 * back at 2× wall time (audio sounds stretched). Override via Kconfig
 * only for boards with a genuinely stereo speaker codec. */
#ifdef CONFIG_AUDIO_PLAYER_CODEC_CHANNELS
#define CODEC_CHANNELS          CONFIG_AUDIO_PLAYER_CODEC_CHANNELS
#else
#define CODEC_CHANNELS          (1)
#endif

#include "OpusAudioPlayer.h"

/* Cached audio_info that media_playback uses as its input format.
 *
 * The decoder always emits at its configured output rate/channels — so as
 * long as we configure the decoder for CODEC_SAMPLE_RATE/CODEC_CHANNELS,
 * media_playback's input == codec target and the resample path collapses
 * to a memcpy (pass-through). The plumbing below stays in place so that
 * if a future decoder reports bitstream-native info (and we re-emit
 * non-codec-rate PCM) the playback task adapts without code changes. */
/* Only read inside the P4 media_playback_task body (gated below with
 * CONFIG_IDF_TARGET_ESP32P4); kept at file scope so the audio_info
 * publish/read share a single static. */
static media_audio_info_t       g_current_info __attribute__((unused)) = {.sample_rate = CODEC_SAMPLE_RATE, .channels = CODEC_CHANNELS};
static media_audio_info_t       g_pending_info;
static volatile bool            g_info_dirty   = false;
static portMUX_TYPE             g_info_lock    = portMUX_INITIALIZER_UNLOCKED;
static media_audio_info_cb_t    g_info_cb      = NULL;
static void                    *g_info_cb_ctx  = NULL;

esp_err_t MediaPlaybackRegisterInfoCallback(media_audio_info_cb_t cb, void *user_ctx)
{
    g_info_cb     = cb;
    g_info_cb_ctx = user_ctx;
    return ESP_OK;
}

/* Called by the decoder after each successful decode. Reads aud_info via
 * esp_audio_dec_get_info() (free) and stages an update under spinlock if
 * the value differs from the last observation. media_playback_task picks
 * up the change at the top of its next iteration. */
static void media_audio_info_maybe_update(void)
{
    static uint32_t last_rate = 0;
    static uint8_t  last_ch   = 0;

    esp_audio_dec_info_t info = {0};
    if (esp_audio_dec_get_info(write_ctx.decoder, &info) != ESP_AUDIO_ERR_OK) {
        return;
    }
    if (info.sample_rate == last_rate && info.channel == last_ch) {
        return;
    }
    last_rate = info.sample_rate;
    last_ch   = info.channel;

    taskENTER_CRITICAL(&g_info_lock);
    g_pending_info.sample_rate = info.sample_rate;
    g_pending_info.channels    = info.channel;
    g_info_dirty               = true;
    taskEXIT_CRITICAL(&g_info_lock);

    ESP_LOGI(TAG, "decoder audio_info changed: rate=%" PRIu32 " ch=%u",
             info.sample_rate, info.channel);
}

static int decode_one_frame(uint8_t *data, int size)
{
    esp_audio_dec_in_raw_t raw = {
        .buffer = data,
        .len = size,
    };
    esp_audio_dec_out_frame_t *out_frame = &write_ctx.out_frame;

    int ret = 0;
    // Input data may contain multiple frames, each call of process decode only one frame
    while (raw.len) {
        if (write_ctx.use_common_api) {
            ret = esp_audio_dec_process(write_ctx.decoder, &raw, out_frame);
        } else {
            esp_audio_dec_info_t aud_info;
            ret = esp_opus_dec_decode(write_ctx.decoder, &raw, out_frame, &aud_info);
        }
        if (ret == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
            ESP_LOGW(TAG, "Output buffer not enough, reallocating to %d", (int)out_frame->needed_size);
            // When output buffer for pcm is not enough, need reallocate it according reported `needed_size` and retry
            uint8_t *new_buf = heap_caps_realloc(out_frame->buffer, out_frame->needed_size, MALLOC_CAP_SPIRAM);
            if (new_buf == NULL) {
                ESP_LOGE(TAG, "Failed to reallocate buffer for pcm");
                break; // skips this frame
            }
            out_frame->buffer = new_buf;
            out_frame->len = out_frame->needed_size;
            continue;
        }
        if (ret != ESP_AUDIO_ERR_OK) {
            ESP_LOGE(TAG, "Failed to decode data ret %d", ret);
            write_ctx.decode_err = true;
            break;
        } else {
            ESP_LOGD(TAG, "Decoded the frame. Output buffer size: %d", (int) out_frame->decoded_size);
            raw.len -= raw.consumed;
            raw.buffer += raw.consumed;
            /* Surface any audio_info change to media_playback (no-op when unchanged). */
            media_audio_info_maybe_update();
        }

        if (raw.len != 0) {
            ESP_LOGW(TAG, "More data to decode %d", (int) raw.len);
            break;
        }
    }
    return ret;
}

/* The P4 playback task feeds decoded PCM to the speaker codec (spk_codec_dev),
 * which only exists when the BSP advertises a speaker. Some P4 boards have no
 * speaker (e.g. ESP32-P4-EYE: BSP_CAPS_AUDIO_SPEAKER == 0) — there we fall
 * through to the no-op stub below rather than reference an undeclared handle. */
#if CONFIG_IDF_TARGET_ESP32P4 && BSP_CAPS_AUDIO_SPEAKER
#include "resampling.h"
#include <math.h>

/* Set to 1 to play a 440Hz sine wave instead of decoded audio (speaker hardware test) */
#define AUDIO_PLAYER_SINE_TEST  0

#if AUDIO_PLAYER_SINE_TEST
static void media_playback_task(void *arg)
{
    /* 440Hz sine wave, 16kHz stereo, 20ms frames */
    const int sample_rate = CODEC_SAMPLE_RATE;
    const int channels = CODEC_CHANNELS;
    const int frame_ms = 20;
    const int frame_samples = sample_rate * frame_ms / 1000;
    const float freq = 440.0f;
    int16_t buf[frame_samples * channels];
    uint32_t phase = 0;

    ESP_LOGW(TAG, "*** SINE WAVE TEST MODE (440Hz) ***");
    while (1) {
        for (int i = 0; i < frame_samples; i++) {
            int16_t sample = (int16_t)(sinf(2.0f * M_PI * freq * (float)(phase + i) / sample_rate) * 16000);
            for (int ch = 0; ch < channels; ch++) {
                buf[i * channels + ch] = sample;
            }
        }
        phase += frame_samples;

        if (spk_codec_dev) {
            esp_codec_dev_write(spk_codec_dev, buf, sizeof(buf));
        } else {
            ESP_LOGW(TAG, "Codec device not initialized");
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
}
#else /* !AUDIO_PLAYER_SINE_TEST */
/* media_playback_task — was i2s_write_task. Pulls decoded PCM out of
 * the ringbuf (written by opus_player_task) and feeds the speaker codec.
 *
 * Hot path: when the decoder's current audio_info matches CODEC_SAMPLE_RATE
 * and CODEC_CHANNELS, we pass through directly — no resample, no extra
 * copy. This is the common case once we configure the Opus decoder to
 * emit at codec rate.
 *
 * Cold path: when audio_info diverges (a future decoder reports
 * bitstream-native rate/channels), the existing resample/up-channel/
 * down-channel paths kick in with rates derived from the cached
 * audio_info. media_audio_info_maybe_update() (called from the decoder)
 * stages updates; we apply them at the top of each iteration. */
static void media_playback_task(void *arg)
{
    audio_resample_config_t resample_cfg = {0};
    media_audio_info_t cached = g_current_info;

    /* Output buffer sized for one 20 ms codec-rate frame.
     * Input buffer sized for the worst-case incoming frame we still
     * support (48 kHz stereo 20 ms = 3840 bytes); reads are bounded
     * by frame_bytes computed from `cached`. */
    const int out_samples_per_frame = CODEC_SAMPLE_RATE / 1000 * 20 * CODEC_CHANNELS;
    int16_t out_buf[out_samples_per_frame];
    static int16_t in_buf[48000 / 1000 * 20 * 2]; /* worst-case 48k stereo 20 ms */

    while (1) {
        /* Apply any pending audio_info change staged by the decoder. */
        if (g_info_dirty) {
            taskENTER_CRITICAL(&g_info_lock);
            cached = g_pending_info;
            g_info_dirty = false;
            taskEXIT_CRITICAL(&g_info_lock);
            g_current_info = cached; /* publish snapshot for read-only consumers */
            memset(&resample_cfg, 0, sizeof(resample_cfg)); /* re-init on next call */
            ESP_LOGI(TAG, "media_playback: audio_info applied rate=%" PRIu32 " ch=%u (codec target %d/%d)",
                     cached.sample_rate, cached.channels, CODEC_SAMPLE_RATE, CODEC_CHANNELS);
            if (g_info_cb) {
                g_info_cb(&cached, g_info_cb_ctx);
            }
        }

        const int in_samples_per_ch = cached.sample_rate / 1000 * 20;
        const int in_bytes = in_samples_per_ch * cached.channels * (int)sizeof(int16_t);
        if (in_bytes <= 0 || in_bytes > (int)sizeof(in_buf)) {
            ESP_LOGE(TAG, "incoming frame size %d outside supported range", in_bytes);
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        int bytes_read = rb_read(rb_handle, (void *)in_buf, in_bytes, portMAX_DELAY);
        if (bytes_read <= 0) {
            continue;
        }
        if (!spk_codec_dev) {
            /* No speaker subboard on this board — drain RX audio quietly.
             * Warn once instead of per-frame (every incoming Opus frame hits
             * this on speaker-less boards like the production P4-EYE). */
            static bool s_no_codec_warned = false;
            if (!s_no_codec_warned) {
                ESP_LOGW(TAG, "No speaker codec on this board; discarding received audio");
                s_no_codec_warned = true;
            }
            continue;
        }

        /* Fast path: decoder output already matches codec target. */
        if (cached.sample_rate == CODEC_SAMPLE_RATE && cached.channels == CODEC_CHANNELS) {
            esp_err_t ret = esp_codec_dev_write(spk_codec_dev, in_buf, bytes_read);
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "Failed to write to codec: %s", esp_err_to_name(ret));
            }
            continue;
        }

        /* Mismatch path: resample (and channel-convert if needed) into out_buf. */
        const int in_total_samples = in_samples_per_ch * cached.channels;
        int outnum = 0;
        if (cached.channels == CODEC_CHANNELS) {
            outnum = audio_resample((short *)in_buf, (short *)out_buf,
                                    cached.sample_rate, CODEC_SAMPLE_RATE,
                                    in_total_samples, out_samples_per_frame,
                                    cached.channels, &resample_cfg);
        } else if (cached.channels > CODEC_CHANNELS) {
            outnum = audio_resample_down_channel((short *)in_buf, (short *)out_buf,
                                                 cached.sample_rate, CODEC_SAMPLE_RATE,
                                                 in_total_samples, out_samples_per_frame,
                                                 0, &resample_cfg);
        } else {
            outnum = audio_resample_up_channel((short *)in_buf, (short *)out_buf,
                                               cached.sample_rate, CODEC_SAMPLE_RATE,
                                               in_total_samples, out_samples_per_frame,
                                               &resample_cfg);
        }

        const size_t bytes_to_write = (size_t)outnum * sizeof(int16_t);
        esp_err_t ret = esp_codec_dev_write(spk_codec_dev, out_buf, bytes_to_write);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to write to codec: %s", esp_err_to_name(ret));
        }
    }
}
#endif /* AUDIO_PLAYER_SINE_TEST */
#else /* !CONFIG_IDF_TARGET_ESP32P4 || !BSP_CAPS_AUDIO_SPEAKER */
static void media_playback_task(void *arg)
{
    while (1) {
        ESP_LOGW(TAG, "media playback task not implemented (no speaker on this board/target)");
        vTaskDelay(pdMS_TO_TICKS(5 * 1000));
    }
}
#endif /* CONFIG_IDF_TARGET_ESP32P4 && BSP_CAPS_AUDIO_SPEAKER */
esp_err_t opus_player_decode_and_play_one_frame(uint8_t *data, size_t size)
{
    // printf("Decoding and playing one frame %p of size %zu\n", data, size);

    static uint64_t total_time = 0, fc = 0;
    static const int BM_FRAMES = 10;

    uint64_t st = esp_timer_get_time();
    decode_one_frame(data, size);
    uint64_t et = esp_timer_get_time();

    total_time += et - st;

    if (fc++ % BM_FRAMES == 0) {
        ESP_LOGD(TAG, "time per frame: %" PRIu64 "ms", (total_time / BM_FRAMES) / 1000);
        total_time = 0;
    }

    /* Use short timeout instead of portMAX_DELAY to avoid blocking the
     * jitter buffer's frame delivery callback. If the ring buffer is full,
     * drop this decoded frame rather than stalling the entire receive path
     * which causes the jitter buffer to accumulate → hit maxLatency → drop
     * subsequent frames in a cascade. */
    if (rb_write(rb_handle, write_ctx.out_frame.buffer, write_ctx.out_frame.decoded_size, pdMS_TO_TICKS(50)) <= 0) {
        ESP_LOGD(TAG, "Audio ring buffer full, dropping decoded frame");
    }
    return ESP_OK;
}

static esp_err_t init_decoder()
{
    // Register OPUS decoder, or you can call `esp_audio_dec_register_default` to register all supported decoder
    if (esp_opus_dec_register() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register OPUS decoder");
        return ESP_FAIL;
    }

    /* Configuration for OPUS decoder. We pin the output to CODEC_SAMPLE_RATE /
     * CODEC_CHANNELS so the decoder normalises any inbound bitstream
     * (e.g. peer-sent 48 kHz stereo) to the playback target — keeping
     * media_playback on the pass-through fast path. */
    esp_opus_dec_cfg_t opus_cfg = {
        .sample_rate    = CODEC_SAMPLE_RATE,
        .channel        = CODEC_CHANNELS,
        .self_delimited = false,
    };
    memset(&write_ctx, 0, sizeof(write_ctx));
    write_ctx.use_common_api = true;
    // Allocate buffer to hold output PCM data
    write_ctx.out_frame.len = OPUS_OUTPUT_BUFFER_SIZE;
    write_ctx.out_frame.buffer = heap_caps_malloc(write_ctx.out_frame.len, MALLOC_CAP_SPIRAM);
    if (write_ctx.out_frame.buffer == NULL) {
        ESP_LOGE(TAG, "Failed to allocate buffer for OPUS decoder");
        return ESP_FAIL;
    }
    esp_audio_dec_cfg_t dec_cfg = {
        .type = ESP_AUDIO_TYPE_OPUS,
        .cfg = &opus_cfg,
        .cfg_sz = sizeof(opus_cfg),
    };

    // Open decoder
    if (esp_audio_dec_open(&dec_cfg, &write_ctx.decoder) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open OPUS decoder");
        return ESP_FAIL;
    }
    return ESP_OK;
}

#if OPUS_PLAYER_TASK
typedef struct frame_data_t {
    void *data;
    size_t len; // size of the data
} frame_data_t;

#define MAX_QUEUE_SIZE 50
#define QUEUE_RECV_TIMEOUT pdMS_TO_TICKS(20)
#define QUEUE_SEND_TIMEOUT pdMS_TO_TICKS(20)

static QueueHandle_t frame_queue = NULL;

static void opus_player_task(void *arg)
{
    frame_data_t frame = {0};
    uint32_t decode_count = 0;
    uint64_t decode_total_us = 0;
    uint64_t decode_max_us = 0;
    uint64_t stats_last_us = esp_timer_get_time();

    while(1) {
        if (xQueueReceive(frame_queue, &frame, QUEUE_RECV_TIMEOUT) != pdTRUE) {
            continue;
        }

        uint64_t t0 = esp_timer_get_time();
        opus_player_decode_and_play_one_frame(frame.data, frame.len);
        uint64_t t1 = esp_timer_get_time();

        heap_caps_free(frame.data);
        decode_count++;
        uint64_t dt = t1 - t0;
        decode_total_us += dt;
        if (dt > decode_max_us) {
            decode_max_us = dt;
        }

        /* Log stats every 5 seconds */
        uint64_t window_us = t1 - stats_last_us;
        if (window_us > 5000000) {
            if (decode_count > 0) {
                uint32_t avg_us = (uint32_t)(decode_total_us / decode_count);
                uint32_t budget_pct = (uint32_t)(decode_total_us * 100 / ((uint64_t)decode_count * 20000));
                uint32_t cpu_pct = (uint32_t)(decode_total_us * 100 / window_us);
                ESP_LOGI(TAG, "Decode CPU: %lu frames in %llums | avg=%luus max=%lluus | %lu%% of frame budget | %lu%% of one core | q=%u/%d",
                         (unsigned long)decode_count,
                         (unsigned long long)(window_us / 1000),
                         (unsigned long)avg_us,
                         (unsigned long long)decode_max_us,
                         (unsigned long)budget_pct,
                         (unsigned long)cpu_pct,
                         (unsigned)uxQueueMessagesWaiting(frame_queue),
                         MAX_QUEUE_SIZE);
            }
            decode_count = 0;
            decode_total_us = 0;
            decode_max_us = 0;
            stats_last_us = t1;
        }
    }
}
#endif

esp_err_t OpusAudioPlayerInit()
{
    static bool initialized = false; // allow only one initialization
    if (initialized) {
        ESP_LOGI(TAG, "OPUS audio player already initialized");
        return ESP_OK;
    }

#if OPUS_PLAYER_TASK
    frame_queue = xQueueCreate(MAX_QUEUE_SIZE, sizeof(frame_data_t));
    if (!frame_queue) {
        ESP_LOGE(TAG, "Failed to create frame queue");
        return ESP_FAIL;
    }
#endif

#if BSP_CAPS_AUDIO_SPEAKER
    if (spk_codec_dev == NULL) {
        /* bsp_audio_codec_speaker_init() runs bsp_i2c_init() internally (idempotent
         * across the BSP); media_stream_init() also brings the bus up once before
         * we get here. No extra I2C wrapper needed. */
        spk_codec_dev = bsp_audio_codec_speaker_init();
        if (spk_codec_dev == NULL) {
            ESP_LOGE(TAG, "Failed to initialize speaker codec");
            return ESP_FAIL;
        } else {
            /* Duplex I2S on EV-board: mic + speaker share the same I2S
             * channel via the ES8311 codec. Both sides must agree on
             * slot config — opening the speaker as stereo (channel=2,
             * default channel_mask=BOTH) flips the underlying I2S slot
             * back to STEREO after the mic's MONO init, doubling the
             * sample rate the mic reads -> Opus encoder ships frames
             * faster than wall -> phone hears stretched audio.
             *
             * Match the mic: channel=1 mono with explicit channel_mask
             * for slot 0 (LEFT). Decoder still produces stereo output
             * when the peer sends 2ch Opus; the I2S writer downmixes
             * to one slot at the codec layer. */
            esp_codec_dev_sample_info_t fs = {
                .sample_rate = 16000,
                .channel = 1,
                .bits_per_sample = 16,
                .channel_mask = ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0),
            };
            esp_err_t ret = esp_codec_dev_open(spk_codec_dev, &fs);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Failed to open codec device: %s", esp_err_to_name(ret));
                return ESP_FAIL;
            }
            /* Volume and mute must be set AFTER open — the codec driver
             * enables PA and unmutes during open/enable sequence. */
            esp_codec_dev_set_out_vol(spk_codec_dev, 90);
        }
    }
#endif

    if (init_decoder() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize decoder");
        return ESP_FAIL;
    }

#if OPUS_PLAYER_TASK
    rb_handle = rb_init("opus_player", OPUS_PLAYER_OUT_RB_SIZE);
    if (rb_handle == NULL) {
        ESP_LOGE(TAG, "Failed to initialize ring buffer");
        return ESP_FAIL;
    }

#define OPUS_TASK_STACK_SIZE     (12 * 1024)
/* Higher than the encoder tasks (default 4) so the RX-audio decoder
 * preempts TX encode when both want the same core. Stays below the I2S
 * playback task (10) so speaker IO still wins. Without this, encoder
 * and decoder split a core 50/50, decode falls behind (~24ms/frame
 * observed vs ~6ms baseline), the 50-deep queue saturates, and every
 * Opus frame past that is dropped. */
#define OPUS_TASK_PRIO           (4)
    StaticTask_t *task_buffer = heap_caps_calloc(1, sizeof(StaticTask_t), MALLOC_CAP_INTERNAL);
    void *task_stack = heap_caps_calloc(1, OPUS_TASK_STACK_SIZE, MALLOC_CAP_SPIRAM);
    if (task_buffer == NULL || task_stack == NULL) {
        ESP_LOGE(TAG, "Failed to allocate opus player task buffers");
        if (task_buffer) {
            heap_caps_free(task_buffer);
        }
        if (task_stack) {
            heap_caps_free(task_stack);
        }
        if (rb_handle) {
            rb_cleanup(rb_handle);
            rb_handle = NULL;
        }
        return ESP_FAIL;
    }

    /* the task never exits, so do not bother to free the buffers */
    TaskHandle_t opus_task_handle = xTaskCreateStatic(opus_player_task, "opus_player", OPUS_TASK_STACK_SIZE,
                                                     NULL, OPUS_TASK_PRIO, task_stack, task_buffer);
    if (opus_task_handle == NULL) {
        ESP_LOGE(TAG, "failed to create opus player task!");
        heap_caps_free(task_buffer);
        heap_caps_free(task_stack);
        if (rb_handle) {
            rb_cleanup(rb_handle);
            rb_handle = NULL;
        }
        return ESP_FAIL;
    }

#define MEDIA_PLAYBACK_TASK_STACK_SIZE     (8 * 1024)
#define MEDIA_PLAYBACK_TASK_PRIO           (9) // IO sensitive task
    /* TCB stays in INTERNAL (scheduler ISR touches it); stack moves to
     * SPIRAM to save ~8 KB of internal RAM. */
    StaticTask_t *media_playback_task_buffer = heap_caps_calloc(1, sizeof(StaticTask_t), MALLOC_CAP_INTERNAL);
    void *media_playback_task_stack = heap_caps_calloc(1, MEDIA_PLAYBACK_TASK_STACK_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (media_playback_task_buffer == NULL || media_playback_task_stack == NULL) {
        ESP_LOGE(TAG, "Failed to allocate media_playback task buffers");
        if (task_buffer) {
            heap_caps_free(task_buffer);
        }
        if (task_stack) {
            heap_caps_free(task_stack);
        }
        if (media_playback_task_buffer) {
            heap_caps_free(media_playback_task_buffer);
        }
        if (media_playback_task_stack) {
            heap_caps_free(media_playback_task_stack);
        }
        if (rb_handle) {
            rb_cleanup(rb_handle);
            rb_handle = NULL;
        }
        return ESP_FAIL;
    }

    /* the task never exits, so do not bother to free the buffers */
    TaskHandle_t media_playback_task_handle = xTaskCreateStatic(media_playback_task, "media_playback", MEDIA_PLAYBACK_TASK_STACK_SIZE,
                                                     NULL, MEDIA_PLAYBACK_TASK_PRIO, media_playback_task_stack, media_playback_task_buffer);
    if (media_playback_task_handle == NULL) {
        ESP_LOGE(TAG, "failed to create media_playback task!");
    }
#endif


    ESP_LOGI(TAG, "OPUS audio player initialized");

    initialized = true;
    return ESP_OK;
}

esp_err_t OpusAudioPlayerDeinit()
{
#if 0 // Currently, we do not really deinit the player
    // Clear up resources
    esp_audio_dec_close(write_ctx.decoder);
    free(write_ctx.out_frame.buffer);
    esp_audio_dec_unregister(ESP_AUDIO_TYPE_OPUS);
#endif
    ESP_LOGI(TAG, "OpusAudioPlayerDeinit: Kept running in the background");
    return ESP_OK;
}

esp_err_t OpusAudioPlayerDecode(uint8_t *data, size_t size)
{
#if OPUS_PLAYER_TASK
    uint8_t *data_dup = heap_caps_calloc(1, size, MALLOC_CAP_SPIRAM);
    if (data_dup == NULL) {
        ESP_LOGE(TAG, "Failed to allocate buffer for OPUS decoder");
        return ESP_FAIL;
    }
    memcpy(data_dup, data, size);
    frame_data_t frame = {0};
    frame.data = data_dup;
    frame.len = size;
    if (xQueueSend(frame_queue, &frame, QUEUE_SEND_TIMEOUT) != pdTRUE) {
        /* Drop the NEW frame, not the oldest. Oldest frames are what the
         * listener expects to hear next — evicting them creates audible gaps.
         * Dropping new arrivals during congestion gracefully skips forward. */
        ESP_LOGW(TAG, "Audio decode queue full, dropping incoming frame");
        heap_caps_free(data_dup);
        return ESP_ERR_NO_MEM;
    }
#else
    return opus_player_decode_and_play_one_frame(data, size);
#endif
    return ESP_OK;
}
