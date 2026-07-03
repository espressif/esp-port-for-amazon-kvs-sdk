/*
 * SPDX-FileCopyrightText: 2024-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <inttypes.h>
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <freertos/stream_buffer.h>
#include "esp_err.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_opus_enc.h"
#include "webrtc_mem_utils.h"

#include "OpusFrameGrabber.h"

/* Declared outside the per-target #if blocks below so unconditional helpers
 * (opus_frame_grabber_set_mute / _is_muted at the bottom of this file) can
 * link on any target. ESP32 (xtensa, no audio codec) builds get the stubs
 * without missing-identifier errors. */
static const char *TAG = "OpusFrameGrabber";
static volatile bool s_mic_muted = false;  /* TEMP: was true (push-to-talk via GPIO3) — unmuted by default so device-side audio TX flows without button press. Restore to true for production push-to-talk. */

#if CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32P4
#if CONFIG_IDF_TARGET_ESP32P4
#include "bsp/esp-bsp.h"
#include "esp_codec_dev.h"
#endif

/* Mic open rate. Must match the Opus encoder's sample_rate passed in via
 * audio_capture_config. We use 16 kHz mono end-to-end because the SDP
 * fmtp `maxplaybackrate=16000` (Opus narrowing patch) caps the peer Opus
 * at 16 kHz anyway — higher rates here would just waste CPU and the peer
 * would downsample on decode. */
#define SAMPLE_RATE 16000
#define CHANNELS 1
#define BITRATE 16000

typedef struct {
    QueueHandle_t frame_queue;
    /* SPIRAM-resident backing for the static frame_queue (xQueueCreateStatic).
     * Single one-time allocation at init; never touched per-frame. */
    void *frame_queue_storage;
    TaskHandle_t encoder_task_handle;
    void *encoder_handle;
    StaticTask_t *task_buffer;
    void *task_stack;
    bool encoder_initialized;
    bool running;
    SemaphoreHandle_t run_semaphore;
    uint8_t *inbuf;
    uint8_t *outbuf;
    int insize;
    int outsize;
    /* Mic-read pipeline split: dedicated high-prio i2s_read_task pulls raw PCM
     * from the codec into mic_pcm_ring; audio_encoder_task drains the ring and
     * Opus-encodes at its own (low) priority. Decouples I/O timing from CPU work. */
    StreamBufferHandle_t mic_pcm_ring;
    TaskHandle_t i2s_read_task_handle;
    StaticTask_t *i2s_read_task_buffer;
    void *i2s_read_task_stack;
} opus_encoder_data_t;

static opus_encoder_data_t s_enc_data = {0};

#if CONFIG_IDF_TARGET_ESP32P4
static esp_codec_dev_handle_t mic_codec_dev = NULL;
#endif

#ifndef CONFIG_IDF_TARGET_ESP32P4
#include "driver/i2s_std.h"
#include "driver/gpio.h"

static i2s_chan_handle_t i2s_rx_chan = NULL;

static void i2s_init(void)
{
    // Start listening for audio: MONO @ 16KHz
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 3;
    chan_cfg.dma_frame_num = 300;
    chan_cfg.auto_clear = true;

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = GPIO_NUM_NC,
            .ws = GPIO_NUM_NC,
            .dout = I2S_GPIO_UNUSED,
            .din = GPIO_NUM_NC,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

#if CONFIG_IDF_TARGET_ESP32S3
    std_cfg.gpio_cfg.bclk = GPIO_NUM_41;    // IIS_SCLK
    std_cfg.gpio_cfg.ws = GPIO_NUM_42;      // IIS_LCLK
    std_cfg.gpio_cfg.din = GPIO_NUM_2;      // IIS_DOUT
// #define READ_SAMPLE_SIZE_30  1
#if READ_SAMPLE_SIZE_30
    std_cfg.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO);
#endif
#else
    // For esp32-eye and other non-S3 targets
    std_cfg.gpio_cfg.bclk = GPIO_NUM_26;    // IIS_SCLK
    std_cfg.gpio_cfg.ws = GPIO_NUM_32;      // IIS_LCLK
    std_cfg.gpio_cfg.din = GPIO_NUM_33;     // IIS_DOUT
    // Use I2S_NUM_1 for esp32-eye
    chan_cfg.id = I2S_NUM_1;
#endif

    esp_err_t ret = i2s_new_channel(&chan_cfg, NULL, &i2s_rx_chan);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error in i2s_new_channel: %s", esp_err_to_name(ret));
        return;
    }

    ret = i2s_channel_init_std_mode(i2s_rx_chan, &std_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error in i2s_channel_init_std_mode: %s", esp_err_to_name(ret));
        return;
    }

    ret = i2s_channel_enable(i2s_rx_chan);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error in i2s_channel_enable: %s", esp_err_to_name(ret));
    }
}
#endif

#if CONFIG_IDF_TARGET_ESP32P4
/* High-priority I/O task: pulls raw PCM from the mic codec into the ring buffer.
 * Decoupled from Opus encode so I/O timing isn\'t held hostage by CPU work or
 * audio playback priority spikes. */
static void i2s_read_task(void *arg)
{
    (void) arg;
    ESP_LOGD(TAG, "i2s_read_task started (prio %d)", uxTaskPriorityGet(NULL));
    uint8_t *tmp = heap_caps_calloc(1, s_enc_data.insize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!tmp) {
        ESP_LOGE(TAG, "i2s_read_task: failed to alloc tmp buffer");
        vTaskDelete(NULL);
        return;
    }

    while (1) {
        /* Pause mic reads when no streaming session is active. Without
         * this, audio_encoder_task blocks on run_semaphore (so it stops
         * draining mic_pcm_ring), but this task keeps reading from the
         * codec and pushing into the ring -> ring fills within one
         * encode-period and we spew "ring backpressure timeout, dropped
         * frame" warnings forever after the WebRTC session is destroyed.
         * Idle at 50 ms granularity matches the codec_dev_read window
         * latency on resume. */
        if (!s_enc_data.running) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        if (!mic_codec_dev || !s_enc_data.mic_pcm_ring) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        esp_err_t r = esp_codec_dev_read(mic_codec_dev, tmp, s_enc_data.insize);
        if (r != ESP_OK) {
            ESP_LOGE(TAG, "i2s_read_task: codec_read err %s", esp_err_to_name(r));
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        /* 20 ms wait — one frame period. If the encoder is more than a frame
         * behind, drop this sample (audio TX wants current, not stale). */
        size_t sent = xStreamBufferSend(s_enc_data.mic_pcm_ring, tmp, s_enc_data.insize, pdMS_TO_TICKS(20));
        if (sent != (size_t) s_enc_data.insize) {
            static uint32_t drop_count;
            drop_count++;
            if ((drop_count & 0x3F) == 1) {
                ESP_LOGW(TAG, "i2s_read: ring backpressure timeout, dropped frame (total=%" PRIu32 ")", drop_count);
            }
        }
    }
}
#endif

static void audio_encoder_task(void *arg)
{
    void *enc_handle = arg;
    ESP_LOGD(TAG, "Audio encoder task started (singleton mode - runs continuously)");

    /* CPU-cost instrumentation — proof for the Opus library owner that
     * the 16 kHz mono encoder is ~30% of one core on P4. Logged every 5s. */
    uint32_t enc_count = 0;
    uint64_t enc_total_us = 0;
    uint64_t enc_max_us = 0;
    uint64_t enc_min_us = UINT64_MAX;
    uint64_t enc_stats_last_us = esp_timer_get_time();

    while (1) {
        // Check if encoding should be running
        if (!s_enc_data.running) {
            ESP_LOGD(TAG, "Audio encoder paused, waiting for start signal...");
            // Wait for start signal (blocking)
            xSemaphoreTake(s_enc_data.run_semaphore, portMAX_DELAY);
            ESP_LOGD(TAG, "Audio encoder resumed");
        }

        // Encode process
        esp_audio_enc_in_frame_t in_frame = { 0 };
        esp_audio_enc_out_frame_t out_frame = { 0 };

        in_frame.buffer = s_enc_data.inbuf;
        in_frame.len = s_enc_data.insize;
        out_frame.buffer = s_enc_data.outbuf;
        out_frame.len = s_enc_data.outsize;

#define I2S_READ_WAIT_MS CONFIG_AUDIO_QUEUE_WAIT_MS
#if CONFIG_IDF_TARGET_ESP32P4
        /* Drain the mic ring buffer instead of reading the codec inline.
         * The dedicated i2s_read_task (high prio) fills this ring; we just
         * encode what's there. Block until a full Opus frame's worth of PCM
         * is available so the encoder always sees aligned input. */
        if (s_enc_data.mic_pcm_ring) {
            size_t got = xStreamBufferReceive(s_enc_data.mic_pcm_ring,
                                              s_enc_data.inbuf, s_enc_data.insize,
                                              pdMS_TO_TICKS(I2S_READ_WAIT_MS));
            if (got != (size_t) s_enc_data.insize) {
                /* I/O task lagging or ring empty — skip this 20ms encode slot. */
                continue;
            }
        } else {
            ESP_LOGE(TAG, "Mic PCM ring not initialized");
            vTaskDelay(pdMS_TO_TICKS(I2S_READ_WAIT_MS));
            continue;
        }
#else // CONFIG_IDF_TARGET_ESP32P4
        esp_err_t i2s_ret = ESP_OK;
        size_t bytes_read = 0;
        size_t bytes_to_read = s_enc_data.insize;
#if READ_SAMPLE_SIZE_30
        bytes_to_read = s_enc_data.insize * 2;
#endif
        i2s_ret = i2s_channel_read(i2s_rx_chan, (void*)s_enc_data.inbuf, bytes_to_read, &bytes_read, pdMS_TO_TICKS(I2S_READ_WAIT_MS));
        if (i2s_ret != ESP_OK) {
            ESP_LOGE(TAG, "I2S read error: %s", esp_err_to_name(i2s_ret));
            vTaskDelay(pdMS_TO_TICKS(I2S_READ_WAIT_MS));
            continue;
        }
#if READ_SAMPLE_SIZE_30
        // rescale the data (Actual data is 30 bits, use higher 16 bits out of those)
        for (int i = 0; i < bytes_read / 4; ++i) {
            ((uint16_t *) s_enc_data.inbuf)[i] = (((uint32_t *) s_enc_data.inbuf)[i] >> 14) & 0xffff;
        }
        bytes_read = bytes_read / 2; // Adjust bytes_read to reflect 16-bit samples after rescaling
#endif
#endif

        /* Push-to-talk: when muted, zero the captured audio to send silence.
         * Keeps the RTP stream alive while preventing echo feedback. */
        if (s_mic_muted) {
            memset(s_enc_data.inbuf, 0, s_enc_data.insize);
        }

#define OPUS_ENCODE_WAIT_MS CONFIG_AUDIO_QUEUE_WAIT_MS

        esp_opus_out_buf_t opus_frame;  /* ~260 B stack — no heap alloc */
        esp_audio_err_t ret = ESP_AUDIO_ERR_OK;
        uint64_t enc_t0 = esp_timer_get_time();
        ret = esp_opus_enc_process(enc_handle, &in_frame, &out_frame);
        uint64_t enc_t1 = esp_timer_get_time();
        if (ret != ESP_AUDIO_ERR_OK) {
            ESP_LOGE(TAG, "audio encoder process failed.");
            vTaskDelay(pdMS_TO_TICKS(OPUS_ENCODE_WAIT_MS));
            continue;
        }
        {
            uint64_t dt = enc_t1 - enc_t0;
            enc_count++;
            enc_total_us += dt;
            if (dt > enc_max_us) {
                enc_max_us = dt;
            }
            if (dt < enc_min_us) {
                enc_min_us = dt;
            }
            uint64_t window_us = enc_t1 - enc_stats_last_us;
            if (window_us > 5000000) {
                /* min_us  = least-preempted frame in the window ≈ pure
                 *           esp_opus_enc_process() compute (use THIS to compare
                 *           codecs/app-modes; avg/max are inflated by preemption
                 *           from the video pipeline sharing the core).
                 * avg_us  = mean wall-clock per frame (compute + preemption).
                 * max_us  = worst-case wall-clock (compute + worst preemption). */
                uint32_t avg_us = (uint32_t)(enc_total_us / enc_count);
                uint32_t budget_pct = (uint32_t)(enc_total_us * 100 / ((uint64_t)enc_count * 20000));
                uint32_t cpu_pct = (uint32_t)(enc_total_us * 100 / window_us);
                ESP_LOGI(TAG, "Encode CPU: %lu frames in %llums | min=%lluus avg=%luus max=%lluus | %lu%% of frame budget | %lu%% of one core",
                         (unsigned long)enc_count,
                         (unsigned long long)(window_us / 1000),
                         (unsigned long long)enc_min_us,
                         (unsigned long)avg_us,
                         (unsigned long long)enc_max_us,
                         (unsigned long)budget_pct,
                         (unsigned long)cpu_pct);
                enc_count = 0;
                enc_total_us = 0;
                enc_max_us = 0;
                enc_min_us = UINT64_MAX;
                enc_stats_last_us = enc_t1;
            }
        }
        /* Skip empty-encoder-output frames. The Opus encoder can emit
         * 0-byte frames during DTX or silence; pushing one downstream
         * crashes the KVS SDK's writeFrame() with a heap double-free
         * (calloc(1, 0) returns a tiny pointer that later free()s
         * corrupt the TLSF free list — see plan memo
         * "project_kvs_zero_frame_bug"). Guard at producer side. */
        if (out_frame.encoded_bytes == 0) {
            continue;
        }
        opus_frame.len = out_frame.encoded_bytes;
        if (opus_frame.len > OPUS_MAX_FRAME_BYTES) {
            ESP_LOGW(TAG, "Opus frame %u B exceeds OPUS_MAX_FRAME_BYTES (%d); dropping",
                     (unsigned)opus_frame.len, OPUS_MAX_FRAME_BYTES);
            continue;
        }
        memcpy(opus_frame.buffer, out_frame.buffer, opus_frame.len);

        /* 50 ms send-wait absorbs brief consumer backpressure under bidir
         * A/V load. The encoder just spent ~5-10 ms producing this frame;
         * dropping it on a 20 ms timeout would waste that CPU. Better to
         * stall the encoder slot here — i2s_read_task will drop raw PCM
         * upstream (cheaper) if mic_pcm_ring fills. */
#define OPUS_FRAME_QUEUE_SEND_WAIT_MS 50
        if (xQueueSend(s_enc_data.frame_queue, &opus_frame,
                       pdMS_TO_TICKS(OPUS_FRAME_QUEUE_SEND_WAIT_MS)) != pdTRUE) {
            static uint32_t qdrop_count;
            qdrop_count++;
            if ((qdrop_count & 0x1F) == 1) {
                ESP_LOGW(TAG, "frame_queue backpressure >50ms, dropped Opus frame (total=%" PRIu32 ")",
                         qdrop_count);
            }
        }
    }

    // This point should never be reached in singleton mode
    ESP_LOGE(TAG, "Audio encoder task unexpectedly exited!");
}

/* returns handle */
void *opus_encoder_init_internal(audio_capture_config_t *config)
{
    if (config == NULL) {
        return NULL;
    }

    // Singleton pattern: return existing handle if already initialized
    if (s_enc_data.encoder_initialized) {
        ESP_LOGD(TAG, "Opus encoder already initialized (singleton), returning existing handle");
        return s_enc_data.encoder_handle;
    }

    esp_audio_err_t ret = ESP_AUDIO_ERR_OK;
    esp_opus_enc_config_t enc_config = ESP_OPUS_ENC_CONFIG_DEFAULT();
    enc_config.sample_rate = config->format.sample_rate;
    enc_config.channel = config->format.channels;
    enc_config.bitrate = config->bitrate;
    enc_config.enable_dtx = true;
    ret = esp_opus_enc_open(&enc_config, sizeof(esp_opus_enc_config_t), &s_enc_data.encoder_handle);
    if (ret != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "Failed to initialize Opus encoder");
        goto cleanup;
    }

    /* Static queue with SPIRAM-backed storage. The queue control block
     * is tiny (~80 B) and stays in internal RAM via the static decl;
     * the per-slot storage (CONFIG_AUDIO_FRAME_QUEUE_SIZE × ~256 B) is
     * one-time allocated in SPIRAM at init. Per-frame work is just a
     * memcpy into the next slot — no heap_caps_calloc/free in the hot
     * path (eliminates the SPIRAM churn that bled audio_encoder CPU
     * under bidir A/V load). */
    static StaticQueue_t s_frame_queue_ctrl;
    s_enc_data.frame_queue_storage = heap_caps_calloc(CONFIG_AUDIO_FRAME_QUEUE_SIZE,
                                                      sizeof(esp_opus_out_buf_t),
                                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_enc_data.frame_queue_storage) {
        ESP_LOGE(TAG, "Failed to alloc SPIRAM queue storage (%u B)",
                 (unsigned)(CONFIG_AUDIO_FRAME_QUEUE_SIZE * sizeof(esp_opus_out_buf_t)));
        goto cleanup;
    }
    s_enc_data.frame_queue = xQueueCreateStatic(CONFIG_AUDIO_FRAME_QUEUE_SIZE,
                                                sizeof(esp_opus_out_buf_t),
                                                s_enc_data.frame_queue_storage,
                                                &s_frame_queue_ctrl);
    if (!s_enc_data.frame_queue) {
        ESP_LOGE(TAG, "Failed to create frame queue");
        goto cleanup;
    }

    // Create semaphore for start/stop control
    s_enc_data.run_semaphore = xSemaphoreCreateBinary();
    if (!s_enc_data.run_semaphore) {
        ESP_LOGE(TAG, "Failed to create run semaphore");
        goto cleanup;
    }

    // Get frame sizes and allocate buffers
    esp_opus_enc_get_frame_size(s_enc_data.encoder_handle, &s_enc_data.insize, &s_enc_data.outsize);
#if READ_SAMPLE_SIZE_30
    s_enc_data.inbuf = heap_caps_calloc(1, s_enc_data.insize * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); // 20ms mono
#else
    s_enc_data.inbuf = heap_caps_calloc(1, s_enc_data.insize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); // 20ms mono
#endif
    s_enc_data.outbuf = heap_caps_calloc(1, s_enc_data.outsize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if (!s_enc_data.inbuf || !s_enc_data.outbuf) {
        ESP_LOGE(TAG, "Failed to allocate audio buffers");
        goto cleanup;
    }

    // Initialize audio hardware
#if CONFIG_IDF_TARGET_ESP32P4
    if (mic_codec_dev == NULL) {
        /* bsp_audio_codec_microphone_init() runs bsp_i2c_init() internally
         * (idempotent); media_stream_init() also brings the bus up once before
         * we get here. No extra I2C wrapper needed. */
        mic_codec_dev = bsp_audio_codec_microphone_init();
        if (mic_codec_dev == NULL) {
            ESP_LOGE(TAG, "Failed to initialize microphone codec");
            goto cleanup;
        }

        esp_codec_dev_sample_info_t fs = {
            .sample_rate = SAMPLE_RATE,
            .channel = CHANNELS,
            .bits_per_sample = 16,
            /* When channel_mask is left at 0, esp_codec_dev's I2S backend
             * (audio_codec_data_i2s.c:250) defaults the I2S slot_mask to
             * I2S_STD_SLOT_BOTH (both L+R slots active), even when channel
             * is 1 (mono). That reads BOTH slots per frame -> 2× the
             * samples per real-time interval -> Opus encoder ships frames
             * at 2× wall-clock cadence -> phone hears 2× stretched audio.
             * Explicitly select slot 0 (LEFT) so the mask is single-slot. */
            .channel_mask = ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0),
        };
        esp_err_t ret = esp_codec_dev_open(mic_codec_dev, &fs);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to open microphone codec device: %s", esp_err_to_name(ret));
            mic_codec_dev = NULL;
            goto cleanup;
        }

        // Give some time for the I2S channel to be properly enabled
        vTaskDelay(pdMS_TO_TICKS(100));
        ESP_LOGD(TAG, "ESP32P4 audio codec initialized");

        /* Mic-read split: stream buffer + dedicated high-prio i2s_read task.
         * Capacity = 8 Opus frames of PCM; trigger = 1 frame (encoder drains
         * one at a time). I2S_READ_TASK_PRIO is intentionally high (9) so
         * mic sample timing is preserved when audio playback (prio 9) and
         * encoders (prio 4) are competing for CPU. */
        #define I2S_READ_TASK_PRIO        (9)
        #define I2S_READ_TASK_STACK_SIZE  (4096)
        /* Ring holds ~32 frames (640 ms @ 20 ms/frame). 8 was too tight — encoder
         * stalls of >160 ms (e.g. during audio playback bursts on prio-9 i2s_write)
         * could overflow the ring, drop mic samples, and pull TX audio fps below 50. */
        s_enc_data.mic_pcm_ring = xStreamBufferCreate(s_enc_data.insize * 32, s_enc_data.insize);
        if (!s_enc_data.mic_pcm_ring) {
            ESP_LOGE(TAG, "Failed to create mic PCM ring buffer");
            goto cleanup;
        }
        s_enc_data.i2s_read_task_buffer = heap_caps_calloc(1, sizeof(StaticTask_t), MALLOC_CAP_INTERNAL);
        s_enc_data.i2s_read_task_stack = heap_caps_calloc(1, I2S_READ_TASK_STACK_SIZE, MALLOC_CAP_SPIRAM);
        if (!s_enc_data.i2s_read_task_buffer || !s_enc_data.i2s_read_task_stack) {
            ESP_LOGE(TAG, "Failed to alloc i2s_read_task buffers");
            goto cleanup;
        }
        /* Pin audio I/O to core 0 so it shares a cache neighborhood with
         * audio_encoder and opus_player (also core 0). Video path is on core 1. */
        s_enc_data.i2s_read_task_handle = xTaskCreateStatic(
            i2s_read_task, "i2s_read", I2S_READ_TASK_STACK_SIZE,
            NULL, I2S_READ_TASK_PRIO,
            s_enc_data.i2s_read_task_stack, s_enc_data.i2s_read_task_buffer);
    }
#else
    i2s_init();
#endif

#define ENC_TASK_STACK_SIZE     CONFIG_AUDIO_ENCODER_TASK_STACK_SIZE
#define ENC_TASK_PRIO           CONFIG_AUDIO_ENCODER_TASK_PRIORITY
    s_enc_data.task_buffer = heap_caps_calloc(1, sizeof(StaticTask_t), MALLOC_CAP_INTERNAL);
    s_enc_data.task_stack = heap_caps_calloc(1, ENC_TASK_STACK_SIZE, MALLOC_CAP_SPIRAM);
    if (!s_enc_data.task_buffer || !s_enc_data.task_stack) {
        ESP_LOGE(TAG, "Failed to allocate task buffers");
        goto cleanup;
    }

    s_enc_data.running = false;  // Start in stopped state

    s_enc_data.encoder_task_handle =
    xTaskCreateStatic(audio_encoder_task, "audio_encoder", ENC_TASK_STACK_SIZE,
        s_enc_data.encoder_handle, ENC_TASK_PRIO, s_enc_data.task_stack, s_enc_data.task_buffer);

    s_enc_data.encoder_initialized = true;

    ESP_LOGD(TAG, "Opus encoder initialized as singleton (stopped, use start() to begin)");
    webrtc_mem_utils_print_stats(TAG);

    return s_enc_data.encoder_handle;

cleanup:
    // Conditional cleanup based on what was allocated
    if (s_enc_data.task_buffer != NULL) {
        heap_caps_free(s_enc_data.task_buffer);
        s_enc_data.task_buffer = NULL;
    }
    if (s_enc_data.task_stack != NULL) {
        heap_caps_free(s_enc_data.task_stack);
        s_enc_data.task_stack = NULL;
    }
    if (s_enc_data.inbuf != NULL) {
        heap_caps_free(s_enc_data.inbuf);
        s_enc_data.inbuf = NULL;
    }
    if (s_enc_data.outbuf != NULL) {
        heap_caps_free(s_enc_data.outbuf);
        s_enc_data.outbuf = NULL;
    }
    s_enc_data.insize = 0;
    s_enc_data.outsize = 0;

    if (s_enc_data.run_semaphore != NULL) {
        vSemaphoreDelete(s_enc_data.run_semaphore);
        s_enc_data.run_semaphore = NULL;
    }
    if (s_enc_data.frame_queue != NULL) {
        vQueueDelete(s_enc_data.frame_queue);
        s_enc_data.frame_queue = NULL;
    }
    if (s_enc_data.frame_queue_storage != NULL) {
        heap_caps_free(s_enc_data.frame_queue_storage);
        s_enc_data.frame_queue_storage = NULL;
    }
    if (s_enc_data.encoder_handle != NULL) {
        esp_opus_enc_close(s_enc_data.encoder_handle);
        s_enc_data.encoder_handle = NULL;
    }

    return NULL;
}

bool get_opus_encoded_frame(esp_opus_out_buf_t *out_frame)
{
    if (!out_frame) {
        return false;
    }
    return xQueueReceive(s_enc_data.frame_queue, out_frame,
                         pdMS_TO_TICKS(CONFIG_AUDIO_QUEUE_WAIT_MS)) == pdTRUE;
}

esp_err_t opus_encoder_start_internal(void)
{
    if (!s_enc_data.encoder_initialized) {
        ESP_LOGE(TAG, "Opus encoder not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (s_enc_data.running) {
        ESP_LOGD(TAG, "Opus encoder already running");
        return ESP_OK;
    }

    s_enc_data.running = true;
    xSemaphoreGive(s_enc_data.run_semaphore);  // Signal encoder to start
    ESP_LOGD(TAG, "Opus encoder started");

    return ESP_OK;
}

esp_err_t opus_encoder_stop_internal(void)
{
    if (!s_enc_data.encoder_initialized) {
        ESP_LOGE(TAG, "Opus encoder not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (!s_enc_data.running) {
        ESP_LOGD(TAG, "Opus encoder already stopped");
        return ESP_OK;
    }

    s_enc_data.running = false;
    ESP_LOGD(TAG, "Opus encoder stopped");

    return ESP_OK;
}

esp_err_t opus_encoder_deinit_internal(void)
{
    if (!s_enc_data.encoder_initialized) {
        ESP_LOGD(TAG, "Opus encoder not initialized, nothing to deinitialize");
        return ESP_OK;
    }

    // Stop encoding first
    opus_encoder_stop_internal();

    // Drain the frame queue with limited iterations to prevent infinite loop
    if (s_enc_data.frame_queue != NULL) {
        ESP_LOGD(TAG, "Draining frame queue...");
        esp_opus_out_buf_t opus_frame;
        int drained_count = 0;
        const int max_drain_iterations = CONFIG_AUDIO_FRAME_QUEUE_SIZE;  // Prevent infinite loop

        while (xQueueReceive(s_enc_data.frame_queue, &opus_frame, 0) == pdTRUE &&
               drained_count < max_drain_iterations) {
            /* Buffer is embedded in opus_frame and lives inside the queue
             * slot — no per-frame heap free needed. Just drain the queue. */
            drained_count++;
        }

        if (drained_count > 0) {
            ESP_LOGD(TAG, "Drained %d frames from queue", drained_count);
        }
        if (drained_count >= max_drain_iterations) {
            ESP_LOGI(TAG, "Reached max drain limit, queue may still contain frames");
        }
    }

    // Singleton pattern: encoder task remains running but paused
    ESP_LOGD(TAG, "Opus encoder is singleton - task remains paused until start() is called");

    return ESP_OK;
}
#else
void *opus_encoder_init_internal(audio_capture_config_t *config)
{
    (void) config;
    return NULL;
}

bool get_opus_encoded_frame(esp_opus_out_buf_t *out_frame)
{
    (void)out_frame;
    return false;
}

esp_err_t opus_encoder_start_internal(void)
{
    // No-op for unsupported targets
    return ESP_OK;
}

esp_err_t opus_encoder_stop_internal(void)
{
    // No-op for unsupported targets
    return ESP_OK;
}

esp_err_t opus_encoder_deinit_internal(void)
{
    // No-op for unsupported targets
    return ESP_OK;
}
#endif

void opus_frame_grabber_set_mute(bool mute)
{
    s_mic_muted = mute;
    ESP_LOGI(TAG, "Microphone %s", mute ? "muted" : "unmuted");
}

bool opus_frame_grabber_is_muted(void)
{
    return s_mic_muted;
}
