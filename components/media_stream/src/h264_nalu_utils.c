/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esp_log.h"
#include "h264_nalu_utils.h"

static const char *TAG = "h264_nalu";

bool h264_find_next_start_code(const uint8_t *data, size_t len,
                               size_t *start_off, size_t *sc_len)
{
    if (data == NULL || len < 3) {
        return false;
    }
    for (size_t i = 0; i + 2 < len; ++i) {
        if (data[i] != 0 || data[i + 1] != 0) {
            continue;
        }
        if (data[i + 2] == 1) {
            if (start_off) *start_off = i;
            if (sc_len) *sc_len = 3;
            return true;
        }
        if (i + 3 < len && data[i + 2] == 0 && data[i + 3] == 1) {
            if (start_off) *start_off = i;
            if (sc_len) *sc_len = 4;
            return true;
        }
    }
    return false;
}

h264_nal_scan_result_t h264_scan_nals(const uint8_t *data, size_t len,
                                      h264_nal_dir_t dir)
{
    h264_nal_scan_result_t res = { 0 };
    if (data == NULL || len == 0) {
        return res;
    }

    size_t off = 0;
    while (off < len) {
        size_t rel_start = 0;
        size_t sc_len = 0;
        if (!h264_find_next_start_code(data + off, len - off, &rel_start, &sc_len)) {
            break;
        }
        size_t nal_hdr_off = off + rel_start + sc_len;
        if (nal_hdr_off >= len) {
            break;
        }
        switch (h264_nal_type_from_header(data[nal_hdr_off])) {
        case H264_NAL_TYPE_SPS: {
            res.has_sps = true;
            /* Log the SPS header bytes so we can identify the profile each
             * direction is using. SPS RBSP byte layout:
             *   [0] profile_idc  (66=Baseline, 77=Main, 88=Extended, 100=High)
             *   [1] constraint_set_flags + reserved
             *   [2] level_idc
             * Memoize per-direction so RX and TX state can't shadow each
             * other and we only log on first/changed. */
            static uint8_t last_profile_idc[2], last_constraint[2], last_level[2];
            static bool    seen_sps[2];
            const unsigned di = (dir == H264_NAL_DIR_TX) ? 1u : 0u;
            const char    *dir_str = (dir == H264_NAL_DIR_TX) ? "sending" : "received";
            if (nal_hdr_off + 3 < len) {
                uint8_t profile_idc   = data[nal_hdr_off + 1];
                uint8_t constraint    = data[nal_hdr_off + 2];
                uint8_t level_idc     = data[nal_hdr_off + 3];
                if (!seen_sps[di] || profile_idc != last_profile_idc[di] ||
                    constraint != last_constraint[di] || level_idc != last_level[di]) {
                    ESP_LOGI(TAG, "%s SPS: profile_idc=0x%02x constraint=0x%02x level_idc=%u (%s)",
                             dir_str,
                             profile_idc, constraint, (unsigned)level_idc,
                             profile_idc == 66  ? "Baseline" :
                             profile_idc == 77  ? "Main" :
                             profile_idc == 88  ? "Extended" :
                             profile_idc == 100 ? "High" :
                             profile_idc == 110 ? "High10" :
                             profile_idc == 122 ? "High422" : "Unknown");
                    last_profile_idc[di] = profile_idc;
                    last_constraint[di]  = constraint;
                    last_level[di]       = level_idc;
                    seen_sps[di]         = true;
                }
            }
            break;
        }
        case H264_NAL_TYPE_PPS:       res.has_pps = true; break;
        case H264_NAL_TYPE_IDR_SLICE: res.has_idr = true; break;
        default:                      break;
        }
        off = nal_hdr_off + 1;
    }
    return res;
}
