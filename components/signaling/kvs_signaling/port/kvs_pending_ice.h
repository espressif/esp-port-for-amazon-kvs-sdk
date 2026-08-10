/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

/*
 * Bounded buffer for ICE candidates whose signaling send failed because the
 * websocket was momentarily down (AWS FINs the signaling socket shortly after
 * the SDP answer, while relay candidates are still gathering). Buffered here
 * and re-sent once the signaling client reconnects, so a viewer on a relay-only
 * path (e.g. cellular) still learns the device's relay candidate instead of
 * failing with NO_CONNECTED_CANDIDATE_PAIR.
 *
 * Kept free of KVS/lws types so it can be unit-tested on the host. The owner
 * (kvs_signaling) lazily allocates the slot array, serialises access under its
 * send mutex, and drains outside that mutex.
 */
#define KVS_PENDING_ICE_MAX          16
#define KVS_PENDING_ICE_PAYLOAD_LEN  512
#define KVS_PENDING_ICE_PEERID_LEN   256   /* >= MAX_SIGNALING_CLIENT_ID_LEN */
#define KVS_PENDING_ICE_MAX_ATTEMPTS 5     /* drop a candidate after this many failed re-sends */

typedef struct {
    char     peer_client_id[KVS_PENDING_ICE_PEERID_LEN];
    char     payload[KVS_PENDING_ICE_PAYLOAD_LEN];
    uint32_t payload_len;
    uint32_t version;
    uint32_t attempts;   /* failed re-send attempts so far */
} KvsPendingIceCandidate;

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Append one ICE candidate to the pending buffer. attempts is reset to 0.
 * Returns true if buffered; false if dropped (buffer full, oversized payload,
 * or invalid args). On success the payload is copied and null-terminated.
 */
bool kvsPendingIcePush(KvsPendingIceCandidate slots[], uint32_t *pCount,
                       const char *peerId, const char *payload,
                       uint32_t payloadLen, uint32_t version);

/*
 * Callback used by kvsPendingIceDrain to re-send one buffered candidate.
 * Returns true if the send succeeded (candidate is dropped), false otherwise.
 */
typedef bool (*kvs_pending_ice_send_fn)(void *ctx, const char *peerId,
                                        const char *payload, uint32_t payloadLen,
                                        uint32_t version);

/*
 * Re-send every buffered candidate through send_fn in FIFO order. Entries whose
 * send succeeds are removed; entries whose send fails have attempts incremented
 * and are compacted back into the buffer to be retried on the next reconnect —
 * until KVS_PENDING_ICE_MAX_ATTEMPTS, after which they are dropped so a
 * permanently-unreachable peer can't pin slots forever. Returns the number of
 * candidates still buffered afterwards.
 */
uint32_t kvsPendingIceDrain(KvsPendingIceCandidate slots[], uint32_t *pCount,
                            kvs_pending_ice_send_fn send_fn, void *ctx);

/*
 * Drop all buffered candidates for a departed peer (compacts the buffer).
 * Returns the number removed.
 */
uint32_t kvsPendingIcePurgePeer(KvsPendingIceCandidate slots[], uint32_t *pCount,
                                const char *peerId);

#ifdef __cplusplus
}
#endif
