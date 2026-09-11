/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * ESP-IDF WebSocket implementation for KVS WebRTC signaling
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <com/amazonaws/kinesis/video/webrtcclient/Include.h>
#include "Signaling/Signaling.h"

// ICE configuration helper functions
BOOL signaling_is_ice_config_refresh_needed(PSignalingClient pSignalingClient);
BOOL signaling_has_valid_ice_config(PSignalingClient pSignalingClient);
STATUS refresh_ice_configuration(PSignalingClient pSignalingClient);

#ifdef __cplusplus
}
#endif
