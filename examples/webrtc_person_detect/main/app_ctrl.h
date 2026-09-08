/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @brief The network consumer of the shared capture pipeline, start/stoppable on its own.
 *        It does not run until asked.
 *
 * The consumers of this one camera divide into two kinds:
 *
 *   - the LOCAL ones - the LCD preview and the person detector - are raw sinks on
 *     media_stream's raw-frame bus, and they run from boot. They need no network.
 *   - the NETWORK one - WebRTC - is an ENCODED sink (video_sink.h), started from the
 *     console. It attaches to the H.264 encoder and receives borrowed buffers on the
 *     encoder task.
 *
 * WebRTC copies the frame onto its own queue and returns; the RTP send happens on its own
 * thread, because writeFrame() packetizes inline and would otherwise stall capture.
 *
 * video_capture_init/start/stop/deinit are reference counted, so any order works: the
 * camera is already up for the preview, and this side simply joins it.
 */

#pragma once

#include <stdbool.h>
#include "sdkconfig.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The ONE camera profile for this example.
 *
 * kvs_combined gives its uploader a Kconfig'd profile of its own because nothing else
 * there owns the camera. Here the preview brings the camera up at boot, so a second
 * profile would only be a way for the two to disagree - video_capture warns and keeps the
 * first one. One definition, used by main.c's camera_start(). */
#define APP_VIDEO_WIDTH    CONFIG_PERSON_DETECT_VIDEO_WIDTH
#define APP_VIDEO_HEIGHT   CONFIG_PERSON_DETECT_VIDEO_HEIGHT
#define APP_VIDEO_FPS      CONFIG_PERSON_DETECT_VIDEO_FPS
#define APP_VIDEO_BITRATE  CONFIG_PERSON_DETECT_VIDEO_BITRATE   /* kbps, as video_capture_config_t wants it */

/* WebRTC: KVS signaling + peer connections, streaming the encoded video. */
esp_err_t webrtc_side_start(void);
esp_err_t webrtc_side_stop(void);
bool      webrtc_side_is_running(void);

#ifdef __cplusplus
}
#endif
