/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @brief grab mic data encode them with opus encoder
 *
 */

#pragma once

#include <stdint.h>
#include "audio_capture.h"

#include <stdbool.h>

/* Embedded buffer keeps the frame queue zero-alloc on the hot path.
 * 256 B accommodates the worst-case Opus frame at 16 kHz mono / 16 kbps
 * with comfortable headroom (typical encoded frame is 30-100 B). */
#define OPUS_MAX_FRAME_BYTES 256

typedef struct {
    uint32_t len;
    uint8_t  buffer[OPUS_MAX_FRAME_BYTES];
} esp_opus_out_buf_t;

/* Dequeue one Opus frame into caller-provided storage. Returns true if
 * a frame was received within CONFIG_AUDIO_QUEUE_WAIT_MS, false on
 * timeout. No heap allocation. */
bool get_opus_encoded_frame(esp_opus_out_buf_t *out_frame);

void *opus_encoder_init_internal(audio_capture_config_t *config);

esp_err_t opus_encoder_start_internal(void);

esp_err_t opus_encoder_stop_internal(void);

esp_err_t opus_encoder_deinit_internal(void);
