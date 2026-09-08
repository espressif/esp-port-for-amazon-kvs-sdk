/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @brief esp-dl PedestrianDetect behind a C API. See person_detect.h.
 */

#include "esp_log.h"
#include "pedestrian_detect.hpp"
#include "person_detect.h"

static const char *TAG = "person_detect";

static PedestrianDetect *s_detect;

esp_err_t person_detect_init(float score_thr)
{
    if (s_detect != nullptr) {
        return ESP_OK;
    }
    /* lazy_load = false: load the .espdl now. The default (true) defers it to the first run(),
     * which on this pipeline means a ~435 KB allocation landing while the camera, the encoder and
     * two converter pools are already holding PSRAM. */
    s_detect = new (std::nothrow) PedestrianDetect(PedestrianDetect::PICO_S8_V1, false);
    if (s_detect == nullptr) {
        ESP_LOGE(TAG, "failed to construct PedestrianDetect");
        return ESP_FAIL;
    }
    if (score_thr > 0.0f) {
        s_detect->set_score_thr(score_thr, 0);
    }
    ESP_LOGI(TAG, "model loaded (score_thr %.2f)", score_thr > 0.0f ? score_thr : 0.7f);
    return ESP_OK;
}

int person_detect_run(const void *rgb565, int width, int height,
                      person_box_t *out, int max_boxes)
{
    if (s_detect == nullptr || rgb565 == nullptr || out == nullptr || max_boxes <= 0) {
        return -1;
    }

    dl::image::img_t img = {};
    img.data     = const_cast<void *>(rgb565);
    img.width    = static_cast<uint16_t>(width);
    img.height   = static_cast<uint16_t>(height);
    /* The PPA writes native-endian RGB565. The display sink swaps for the panel while copying;
     * this sink takes the unswapped buffer, so LE is correct here. */
    img.pix_type = dl::image::DL_IMAGE_PIX_TYPE_RGB565LE;

    auto &results = s_detect->run(img);

    int n = 0;
    for (const auto &r : results) {
        if (n >= max_boxes) {
            break;
        }
        /* box is [left_up_x, left_up_y, right_down_x, right_down_y], already clamped to the input
         * dimensions by the postprocessor. Copied out because `results` is a reference to state the
         * postprocessor reuses on the next run(). */
        out[n].x1    = r.box[0];
        out[n].y1    = r.box[1];
        out[n].x2    = r.box[2];
        out[n].y2    = r.box[3];
        out[n].score = r.score;
        n++;
    }
    return n;
}

void person_detect_deinit(void)
{
    delete s_detect;
    s_detect = nullptr;
}
