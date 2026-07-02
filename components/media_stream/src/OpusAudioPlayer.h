/**
 * SPDX-FileCopyrightText: 2024-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esp_err.h"
#include "inttypes.h"
#include <stdint.h>

#pragma once

/**
 * @brief Initialize OPUS audio player
 *
 * @return esp_err_t ESP_OK on success, otherwise an error code
 */
esp_err_t OpusAudioPlayerInit();

/**
 * @brief Deinitialize OPUS audio player
 *
 * @return esp_err_t ESP_OK on success, otherwise an error code
 */
esp_err_t OpusAudioPlayerDeinit();

/**
 * @brief Decode one frame of OPUS to PCM
 *
 * @param data Pointer to encoded OPUS data
 * @param size Size of encoded OPUS data
 * @return esp_err_t ESP_OK on success, otherwise an error code
 */
esp_err_t OpusAudioPlayerDecode(uint8_t *data, size_t size);

/**
 * @brief Audio format that the decoder is currently producing.
 *
 * media_playback_task uses this as its "input format" and resamples
 * to the codec target when the two differ. Reported on every change
 * via the registered callback (see MediaPlaybackRegisterInfoCallback).
 */
typedef struct {
    uint32_t sample_rate;
    uint8_t  channels;
} media_audio_info_t;

/**
 * @brief Callback fired by media_playback when the decoder's reported
 *        audio format changes vs. the previously cached value.
 *
 * @param info     New audio_info (sample_rate + channels).
 * @param user_ctx Opaque pointer passed at registration.
 */
typedef void (*media_audio_info_cb_t)(const media_audio_info_t *info, void *user_ctx);

/**
 * @brief Register a listener that is notified when the decoder's audio_info
 *        changes (rate or channel count). Pass cb = NULL to deregister.
 *
 * @return ESP_OK on success.
 */
esp_err_t MediaPlaybackRegisterInfoCallback(media_audio_info_cb_t cb, void *user_ctx);
