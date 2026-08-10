/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "kvs_pending_ice.h"
#include <string.h>

bool kvsPendingIcePush(KvsPendingIceCandidate slots[], uint32_t *pCount,
                       const char *peerId, const char *payload,
                       uint32_t payloadLen, uint32_t version)
{
    if (slots == NULL || pCount == NULL || payload == NULL) {
        return false;
    }
    if (*pCount >= KVS_PENDING_ICE_MAX) {
        return false;
    }
    if (payloadLen == 0 || payloadLen >= KVS_PENDING_ICE_PAYLOAD_LEN) {
        return false;
    }

    KvsPendingIceCandidate *slot = &slots[*pCount];

    if (peerId != NULL) {
        strncpy(slot->peer_client_id, peerId, KVS_PENDING_ICE_PEERID_LEN - 1);
        slot->peer_client_id[KVS_PENDING_ICE_PEERID_LEN - 1] = '\0';
    } else {
        slot->peer_client_id[0] = '\0';
    }

    memcpy(slot->payload, payload, payloadLen);
    slot->payload[payloadLen] = '\0';
    slot->payload_len = payloadLen;
    slot->version = version;
    slot->attempts = 0;

    (*pCount)++;
    return true;
}

uint32_t kvsPendingIceDrain(KvsPendingIceCandidate slots[], uint32_t *pCount,
                            kvs_pending_ice_send_fn send_fn, void *ctx)
{
    if (slots == NULL || pCount == NULL || send_fn == NULL) {
        return (pCount != NULL) ? *pCount : 0;
    }

    uint32_t n = *pCount;
    uint32_t w = 0;   /* write index for survivors (compaction) */

    for (uint32_t r = 0; r < n; r++) {
        if (send_fn(ctx, slots[r].peer_client_id, slots[r].payload,
                    slots[r].payload_len, slots[r].version)) {
            continue;  /* sent successfully -> drop */
        }
        /* send failed: keep for the next reconnect, up to the retry cap so a
         * permanently-unreachable peer can't pin a slot forever. */
        if (++slots[r].attempts >= KVS_PENDING_ICE_MAX_ATTEMPTS) {
            continue;  /* give up -> drop */
        }
        if (w != r) {
            slots[w] = slots[r];
        }
        w++;
    }

    *pCount = w;
    return w;
}

uint32_t kvsPendingIcePurgePeer(KvsPendingIceCandidate slots[], uint32_t *pCount,
                                const char *peerId)
{
    if (slots == NULL || pCount == NULL || peerId == NULL) {
        return 0;
    }

    uint32_t n = *pCount;
    uint32_t w = 0;
    uint32_t removed = 0;

    for (uint32_t r = 0; r < n; r++) {
        if (strncmp(slots[r].peer_client_id, peerId, KVS_PENDING_ICE_PEERID_LEN) == 0) {
            removed++;
            continue;
        }
        if (w != r) {
            slots[w] = slots[r];
        }
        w++;
    }

    *pCount = w;
    return removed;
}
