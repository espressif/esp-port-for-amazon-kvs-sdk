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
// One GetIceServerConfig call outside the state machine. The caller holds the client's stateLock.
STATUS signaling_fetch_ice_config(PSignalingClient pSignalingClient);
// Copy of one cached ICE config entry, taken under the ICE config lock. Fails without a valid config.
STATUS signaling_copy_ice_config(PSignalingClient pSignalingClient, UINT32 index, PIceConfigInfo pIceConfigInfo,
                                 PUINT32 pIceConfigCount);
// Held by every writer of iceConfigs / iceConfigCount, and by the readers that copy or encode them
VOID signalingIceConfigLock(VOID);
VOID signalingIceConfigUnlock(VOID);

// True on one of the tasks the client's callbacks run on (esp_websocket's, the work queue's)
BOOL isEspSignalingTask(PSignalingClient pSignalingClient);
// Wake a signaling call waiting for the network, so it re-checks the client's shutdown flag
VOID wakeEspSignalingWaits(VOID);
// A state's backoff sleep, cut short once the client is shutting down
VOID signalingSleepUntilUnlessShutdown(PSignalingClient pSignalingClient, UINT64 time);

#ifdef __cplusplus
}
#endif
