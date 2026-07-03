/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file h264_nalu_utils.h
 * @brief Small helpers for scanning H.264 Annex-B byte streams.
 *
 * The video player receives H.264 frames (possibly multiple NAL units per
 * buffer) in Annex-B format over the WebRTC depayloader. These helpers
 * locate start codes and classify NAL types so the player can decide when
 * to (re)initialize the decoder and when a new GOP has arrived.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** H.264 NAL unit types we care about. Other values are valid per spec
 *  but not interpreted by this module. */
typedef enum {
    H264_NAL_TYPE_UNSPECIFIED   = 0,
    H264_NAL_TYPE_NON_IDR_SLICE = 1,
    H264_NAL_TYPE_IDR_SLICE     = 5,
    H264_NAL_TYPE_SEI           = 6,
    H264_NAL_TYPE_SPS           = 7,
    H264_NAL_TYPE_PPS           = 8,
    H264_NAL_TYPE_AUD           = 9,
} h264_nal_type_t;

/**
 * @brief Locate the next Annex-B start code (0x000001 or 0x00000001).
 *
 * @param data       Buffer to scan.
 * @param len        Buffer length in bytes.
 * @param start_off  [out, optional] Offset (within @p data) of the first
 *                   byte of the start code.
 * @param sc_len     [out, optional] 3 for 0x000001, 4 for 0x00000001.
 * @return true if a start code was found, false otherwise.
 */
bool h264_find_next_start_code(const uint8_t *data, size_t len,
                               size_t *start_off, size_t *sc_len);

/**
 * @brief Extract the NAL unit type from a NAL header byte.
 *
 * The caller is expected to pass the first payload byte that follows an
 * Annex-B start code.
 */
static inline h264_nal_type_t h264_nal_type_from_header(uint8_t nal_hdr_byte)
{
    return (h264_nal_type_t)(nal_hdr_byte & 0x1F);
}

/** Result of a scan across a (possibly multi-NAL) Annex-B buffer. */
typedef struct {
    bool has_sps;  /*!< At least one SPS NAL was found. */
    bool has_pps;  /*!< At least one PPS NAL was found. */
    bool has_idr;  /*!< At least one IDR slice NAL was found. */
} h264_nal_scan_result_t;

/** Direction tag for the SPS profile log inside h264_scan_nals.
 *  Two independent state slots so an RX-side SPS doesn't shadow the
 *  TX-side memo (and vice-versa). */
typedef enum {
    H264_NAL_DIR_RX = 0,
    H264_NAL_DIR_TX = 1,
} h264_nal_dir_t;

/**
 * @brief Scan an Annex-B buffer and flag whether SPS/PPS/IDR are present.
 *
 * Tolerant of a missing leading start code: scanning resumes at the first
 * found start code. Returns all-false on NULL/empty input. When an SPS
 * is encountered, logs its profile/level once per direction, and again
 * any time the profile/level changes.
 */
h264_nal_scan_result_t h264_scan_nals(const uint8_t *data, size_t len,
                                      h264_nal_dir_t dir);

#ifdef __cplusplus
}
#endif
