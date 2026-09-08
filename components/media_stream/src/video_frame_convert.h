/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @brief Hardware frame conversion for raw sinks: colour convert + crop + scale in one PPA pass.
 *
 * This is what makes VIDEO_RAW_MODE_CONVERTED worth having. A consumer that wants a small RGB
 * image does not have to pin a 3 MiB camera buffer for the length of its work: the PPA writes a small
 * private copy and the camera buffer goes straight back to the driver.
 * Converting 1920x1080 to 224x224 moves ~100 KiB instead of holding 2.97 MiB, and costs no CPU.
 *
 * Aspect Ratio is preserved. The largest centred block matching the destination aspect is cropped
 * out of the source in the same pass.
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "video_capture.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Opaque converter, one per sink
 */
typedef struct video_frame_convert_s *video_frame_convert_handle_t;

/**
 * @brief Converter configuration
 */
typedef struct {
    uint32_t src_fourcc;  /* VIDEO_FOURCC_* the camera produces */
    uint32_t dst_fourcc;  /* VIDEO_FOURCC_* the sink wants */
    uint16_t dst_width;
    uint16_t dst_height;
    uint8_t  pool_depth;  /* Output buffers to rotate through; 0 => 2 */
} video_frame_convert_cfg_t;

/**
 * @brief Create the shared state every converter needs
 *
 * Idempotent, and must run before the first video_frame_convert_create(). Converters are created
 * lazily on each sink's own task, so the PPA client they share cannot safely be reference-counted
 * without a lock that already exists by then. video_raw_bus_init() calls this.
 *
 * @return ESP_OK or ESP_ERR_NO_MEM
 */
esp_err_t video_frame_convert_init(void);

/**
 * @brief Whether a destination format can be a PPA output at all
 *
 * Split from the full check because a sink registers before the camera starts, so its source format
 * is not known yet. This catches the half that is knowable at registration; the source half is
 * checked when the first frame arrives.
 *
 * @return ESP_OK or ESP_ERR_NOT_SUPPORTED
 */
esp_err_t video_frame_convert_check_dst(uint32_t dst_fourcc);

/**
 * @brief Whether a conversion is supported before anything is allocated
 *
 * Called at sink registration so an impossible request fails loudly there rather than turning into
 * a per-frame error nobody reads.
 *
 * @return ESP_OK or ESP_ERR_NOT_SUPPORTED
 */
esp_err_t video_frame_convert_check(uint32_t src_fourcc, uint32_t dst_fourcc);

/**
 * @brief Create a converter and its output pool
 *
 * @param cfg Configuration
 * @param out Receives the handle
 * @return ESP_OK, ESP_ERR_INVALID_ARG, ESP_ERR_NO_MEM, or ESP_ERR_NOT_SUPPORTED
 */
esp_err_t video_frame_convert_create(const video_frame_convert_cfg_t *cfg,
                                     video_frame_convert_handle_t *out);

/**
 * @brief Convert one frame into a pool buffer
 *
 * On success @p dst describes the converted frame with @c owned set, and @c slot carrying the pool
 * index that video_frame_convert_release() needs back.
 *
 * @param handle Converter
 * @param src    Source frame (borrowed; not retained)
 * @param dst    Receives the converted frame descriptor
 * @return ESP_OK, ESP_ERR_NO_MEM if the pool is exhausted, or a PPA error
 */
esp_err_t video_frame_convert_run(video_frame_convert_handle_t handle,
                                  const video_raw_frame_t *src, video_raw_frame_t *dst);

/**
 * @brief Return a pool buffer
 *
 * @param handle Converter
 * @param index  The @c slot field of the descriptor from video_frame_convert_run()
 */
void video_frame_convert_release(video_frame_convert_handle_t handle, uint8_t index);

/**
 * @brief Destroy a converter and free its pool
 */
void video_frame_convert_destroy(video_frame_convert_handle_t handle);

#ifdef __cplusplus
}
#endif
