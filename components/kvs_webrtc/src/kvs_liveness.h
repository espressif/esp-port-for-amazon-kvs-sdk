/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

/*
 * Per-session liveness watchdog for kvs_webrtc peer connections.
 *
 * kvs_webrtc tears a session down only when the SDK's peer-connection state
 * callback delivers DISCONNECTED/FAILED, which is gated on a passive
 * lastDataReceivedTime timer a dead relay/TURN peer can keep "fresh" with stray
 * inbound bytes. When that callback never fires, the dead peer holds its viewer
 * slot forever: iceAgentSendPacket spams "Invalid state for data sending
 * candidate pair", video stops, and only a device reboot clears it.
 *
 * This is the pure decision half. Given a small liveness snapshot it decides
 * whether a session is dead so kvs_webrtc can drive its own DISCONNECTED
 * teardown. Free of SDK/KVS types so it is unit-tested on host. All
 * timestamps/deadlines share one caller-chosen unit (the firmware uses the
 * SDK's 100ns GETTIME ticks).
 *
 * Liveness signal (connected case): a live session moves bytes over the selected
 * ICE candidate pair every metrics tick; a dead path freezes the counter.
 *
 * Send and receive are tracked separately because the two counters are bumped
 * under different conditions. bytesSent only advances once the pair is
 * SUCCEEDED — on the failure path iceAgentSendPacket() bails out before it — so
 * a frozen send counter is exactly the failure this watchdog exists for.
 * bytesReceived advances for any non-STUN packet on the pair's 5-tuple with no
 * pair-state check, so inbound RTCP from a still-live viewer keeps climbing
 * while our sends are being rejected. Folding the two together would let that
 * inbound traffic mask the stall, so a session that has ever sent is judged on
 * its send counter alone; receive is only consulted for a session that has
 * never sent (receive-only, data-channel-only).
 *
 * A CONNECTED session that is simply quiet is indistinguishable from a dead one
 * by that signal alone, so the stall check is armed only once traffic has been
 * seen at least once. That keeps sessions that legitimately never move bytes —
 * camera init failed and the app continued without video — out of the
 * watchdog's reach, instead of evicting them one stall_deadline after connect.
 */

typedef struct {
    bool     terminate_flag;    /* already flagged for teardown (being reclaimed) */
    bool     ever_connected;    /* latched: reached CONNECTED at least once */
    bool     ever_sent;         /* send counter advanced at least once while connected */
    bool     ever_recvd;        /* receive counter advanced at least once while connected */
    uint64_t offer_time;        /* when the session/offer started */
    uint64_t last_send_progress;/* last time bytesSent advanced (valid iff ever_sent) */
    uint64_t last_recv_progress;/* last time bytesReceived advanced (valid iff ever_recvd) */
} kvs_liveness_health_t;

typedef struct {
    uint64_t connect_deadline; /* must reach CONNECTED within this of offer_time */
    uint64_t stall_deadline;   /* once connected, evict if send bytes flat this long */
} kvs_liveness_cfg_t;

#ifdef __cplusplus
extern "C" {
#endif

/*
 * True if the session is dead and should be force-disconnected (its slot freed):
 *  - never reached CONNECTED and it is past connect_deadline since offer_time, or
 *  - was CONNECTED and has sent at least once, and bytesSent has been flat past
 *    stall_deadline, or
 *  - was CONNECTED, has never sent, but has received at least once, and
 *    bytesReceived has been flat past stall_deadline.
 * A session already flagged for teardown returns false (it is being reclaimed),
 * as does a connected session that has never moved traffic (nothing to judge).
 * Timestamps ahead of `now` (a backward wall-clock step) also return false
 * rather than underflowing into an immediate eviction.
 */
bool kvs_liveness_should_evict(const kvs_liveness_health_t *h,
                               const kvs_liveness_cfg_t *cfg,
                               uint64_t now);

#ifdef __cplusplus
}
#endif
