/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "kvs_pending_ice.h"
#include <stdlib.h>
#include <string.h>

/* Re-sends one ICE slot; lets kvsPendingIceDrain and kvsPendingSignalingDrain
 * share the compaction loop despite their different send callbacks. */
typedef bool (*ice_slot_send_fn)(void *ctx, const KvsPendingIceCandidate *slot);
/* Returns true if this peer's candidates must stay buffered untouched. */
typedef bool (*ice_hold_fn)(void *ctx, const char *peerId);

static void copyPeerId(char dst[KVS_PENDING_ICE_PEERID_LEN], const char *peerId)
{
    if (peerId != NULL) {
        strncpy(dst, peerId, KVS_PENDING_ICE_PEERID_LEN - 1);
        dst[KVS_PENDING_ICE_PEERID_LEN - 1] = '\0';
    } else {
        dst[0] = '\0';
    }
}

static bool samePeer(const char *slotPeerId, const char *peerId)
{
    return strncmp(slotPeerId, peerId != NULL ? peerId : "", KVS_PENDING_ICE_PEERID_LEN - 1) == 0;
}

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

    copyPeerId(slot->peer_client_id, peerId);
    memcpy(slot->payload, payload, payloadLen);
    slot->payload[payloadLen] = '\0';
    slot->payload_len = payloadLen;
    slot->version = version;
    slot->attempts = 0;

    (*pCount)++;
    return true;
}

static uint32_t iceDrain(KvsPendingIceCandidate slots[], uint32_t *pCount,
                         ice_slot_send_fn send_fn, void *ctx,
                         ice_hold_fn hold_fn, void *holdCtx,
                         uint32_t *pSent, uint32_t *pDropped)
{
    uint32_t n = *pCount;
    uint32_t w = 0;   /* write index for survivors (compaction) */

    for (uint32_t r = 0; r < n; r++) {
        if (hold_fn != NULL && hold_fn(holdCtx, slots[r].peer_client_id)) {
            /* waiting behind its peer's answer: keep, no attempt used */
        } else if (send_fn(ctx, &slots[r])) {
            (*pSent)++;
            continue;  /* sent successfully -> drop */
        } else if (++slots[r].attempts >= KVS_PENDING_ICE_MAX_ATTEMPTS) {
            /* send failed too often: give up so a permanently-unreachable peer
             * can't pin a slot forever */
            (*pDropped)++;
            continue;
        }
        /* keep for the next reconnect */
        if (w != r) {
            slots[w] = slots[r];
        }
        w++;
    }

    *pCount = w;
    return w;
}

typedef struct {
    kvs_pending_ice_send_fn fn;
    void *ctx;
} ice_send_adapter_t;

static bool iceSendAdapter(void *ctx, const KvsPendingIceCandidate *slot)
{
    ice_send_adapter_t *a = (ice_send_adapter_t *) ctx;
    return a->fn(a->ctx, slot->peer_client_id, slot->payload, slot->payload_len, slot->version);
}

uint32_t kvsPendingIceDrain(KvsPendingIceCandidate slots[], uint32_t *pCount,
                            kvs_pending_ice_send_fn send_fn, void *ctx)
{
    if (slots == NULL || pCount == NULL || send_fn == NULL) {
        return (pCount != NULL) ? *pCount : 0;
    }

    ice_send_adapter_t adapter = { .fn = send_fn, .ctx = ctx };
    uint32_t sent = 0;
    uint32_t dropped = 0;

    return iceDrain(slots, pCount, iceSendAdapter, &adapter, NULL, NULL, &sent, &dropped);
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

/* ---- Answers + candidates, with lazy allocation ------------------------- */

static void *pendingCalloc(const KvsPendingSignaling *p, size_t num, size_t size)
{
    return (p->calloc_fn != NULL) ? p->calloc_fn(num, size) : calloc(num, size);
}

static void pendingFree(const KvsPendingSignaling *p, void *ptr)
{
    if (ptr == NULL) {
        return;
    }
    if (p->free_fn != NULL) {
        p->free_fn(ptr);
    } else {
        free(ptr);
    }
}

/* Free whichever slot array has drained empty. */
static void releaseEmpty(KvsPendingSignaling *p)
{
    if (p->ice_count == 0 && p->ice != NULL) {
        pendingFree(p, p->ice);
        p->ice = NULL;
    }
    if (p->answer_count == 0 && p->answers != NULL) {
        pendingFree(p, p->answers);
        p->answers = NULL;
    }
}

static int findAnswer(const KvsPendingSignaling *p, const char *peerId)
{
    if (p->answers == NULL) {
        return -1;
    }
    for (uint32_t i = 0; i < p->answer_count; i++) {
        if (samePeer(p->answers[i].peer_client_id, peerId)) {
            return (int) i;
        }
    }
    return -1;
}

static int findOldestAnswer(const KvsPendingSignaling *p)
{
    int oldest = -1;
    for (uint32_t i = 0; i < p->answer_count; i++) {
        if (oldest < 0 || p->answers[i].queued_at_ms < p->answers[oldest].queued_at_ms) {
            oldest = (int) i;
        }
    }
    return oldest;
}

/* Free the answer at idx and close the gap (keeps the others in order). */
static void removeAnswerAt(KvsPendingSignaling *p, uint32_t idx)
{
    pendingFree(p, p->answers[idx].payload);
    for (uint32_t i = idx + 1; i < p->answer_count; i++) {
        p->answers[i - 1] = p->answers[i];
    }
    p->answer_count--;
}

static bool ensureAnswerSlots(KvsPendingSignaling *p)
{
    if (p->answers == NULL) {
        p->answers = (KvsPendingAnswer *) pendingCalloc(p, KVS_PENDING_ANSWER_MAX, sizeof(KvsPendingAnswer));
    }
    return p->answers != NULL;
}

/*
 * Place a filled answer slot (its payload already owned) into p: over the
 * same peer's answer if one waits, else in a free slot, else over the oldest
 * answer if that is older than this one. Returns false (payload not taken) if
 * there is no room for it.
 */
static bool insertAnswer(KvsPendingSignaling *p, const KvsPendingAnswer *a)
{
    int idx = findAnswer(p, a->peer_client_id);
    if (idx >= 0) {
        pendingFree(p, p->answers[idx].payload);
        p->answers[idx] = *a;
        return true;
    }
    if (p->answer_count >= KVS_PENDING_ANSWER_MAX) {
        int oldest = findOldestAnswer(p);
        if (oldest < 0 || p->answers[oldest].queued_at_ms > a->queued_at_ms) {
            return false;
        }
        /* its candidates are useless without it, and would otherwise go out alone */
        if (p->ice != NULL) {
            kvsPendingIcePurgePeer(p->ice, &p->ice_count, p->answers[oldest].peer_client_id);
        }
        removeAnswerAt(p, (uint32_t) oldest);
    }
    p->answers[p->answer_count++] = *a;
    return true;
}

void kvsPendingSignalingInit(KvsPendingSignaling *p, kvs_pending_calloc_fn calloc_fn,
                             kvs_pending_free_fn free_fn)
{
    if (p == NULL) {
        return;
    }
    memset(p, 0, sizeof(*p));
    p->calloc_fn = calloc_fn;
    p->free_fn = free_fn;
}

bool kvsPendingSignalingPushIce(KvsPendingSignaling *p, const char *peerId,
                                const char *payload, uint32_t payloadLen, uint32_t version)
{
    if (p == NULL) {
        return false;
    }
    if (p->ice == NULL) {
        p->ice = (KvsPendingIceCandidate *) pendingCalloc(p, KVS_PENDING_ICE_MAX, sizeof(KvsPendingIceCandidate));
        if (p->ice == NULL) {
            return false;
        }
    }
    bool ok = kvsPendingIcePush(p->ice, &p->ice_count, peerId, payload, payloadLen, version);
    releaseEmpty(p);   /* rejected first candidate: don't keep the array */
    return ok;
}

bool kvsPendingSignalingPutAnswer(KvsPendingSignaling *p, const char *peerId,
                                  const char *correlationId, const char *payload,
                                  uint32_t payloadLen, uint32_t version, uint64_t now_ms)
{
    if (p == NULL || payload == NULL || payloadLen == 0) {
        return false;
    }
    if (!ensureAnswerSlots(p)) {
        return false;
    }

    KvsPendingAnswer a;
    memset(&a, 0, sizeof(a));
    a.payload = (char *) pendingCalloc(p, 1, (size_t) payloadLen + 1);
    if (a.payload == NULL) {
        releaseEmpty(p);
        return false;
    }
    memcpy(a.payload, payload, payloadLen);
    a.payload[payloadLen] = '\0';
    a.payload_len = payloadLen;
    a.version = version;
    a.queued_at_ms = now_ms;
    copyPeerId(a.peer_client_id, peerId);
    if (correlationId != NULL) {
        strncpy(a.correlation_id, correlationId, KVS_PENDING_CORRELATION_ID_LEN - 1);
        a.correlation_id[KVS_PENDING_CORRELATION_ID_LEN - 1] = '\0';
    }

    if (!insertAnswer(p, &a)) {   /* not reached for a new answer (now_ms is newest) */
        pendingFree(p, a.payload);
        releaseEmpty(p);
        return false;
    }
    releaseEmpty(p);   /* an eviction may have emptied the ICE slots */
    return true;
}

bool kvsPendingSignalingHasAnswer(const KvsPendingSignaling *p, const char *peerId)
{
    return p != NULL && findAnswer(p, peerId) >= 0;
}

bool kvsPendingSignalingDropAnswer(KvsPendingSignaling *p, const char *peerId)
{
    if (p == NULL) {
        return false;
    }
    int idx = findAnswer(p, peerId);
    if (idx < 0) {
        return false;
    }
    removeAnswerAt(p, (uint32_t) idx);
    releaseEmpty(p);
    return true;
}

bool kvsPendingSignalingIsPurged(const KvsPendingSignaling *p, const char *peerId)
{
    if (p == NULL || p->purged == NULL) {
        return false;
    }
    for (uint32_t i = 0; i < p->purged_count; i++) {
        if (samePeer(p->purged[i], peerId)) {
            return true;
        }
    }
    return false;
}

/* Remember a peer purged while the batch is out (best effort: bounded). */
static void recordPurged(KvsPendingSignaling *p, const char *peerId)
{
    if (kvsPendingSignalingIsPurged(p, peerId)) {
        return;
    }
    if (p->purged == NULL) {
        p->purged = (char (*)[KVS_PENDING_ICE_PEERID_LEN]) pendingCalloc(p, KVS_PENDING_PURGED_MAX,
                                                                          KVS_PENDING_ICE_PEERID_LEN);
        if (p->purged == NULL) {
            return;
        }
    }
    if (p->purged_count < KVS_PENDING_PURGED_MAX) {
        copyPeerId(p->purged[p->purged_count++], peerId);
    }
}

static void clearPurged(KvsPendingSignaling *p)
{
    pendingFree(p, p->purged);
    p->purged = NULL;
    p->purged_count = 0;
}

uint32_t kvsPendingSignalingPurgePeer(KvsPendingSignaling *p, const char *peerId)
{
    if (p == NULL || peerId == NULL) {
        return 0;
    }
    uint32_t removed = 0;
    if (kvsPendingSignalingDropAnswer(p, peerId)) {
        removed++;
    }
    if (p->ice != NULL) {
        removed += kvsPendingIcePurgePeer(p->ice, &p->ice_count, peerId);
    }
    releaseEmpty(p);
    if (p->batch_out) {
        recordPurged(p, peerId);
    }
    return removed;
}

uint32_t kvsPendingSignalingCount(const KvsPendingSignaling *p)
{
    return (p != NULL) ? p->ice_count + p->answer_count : 0;
}

void kvsPendingSignalingTake(KvsPendingSignaling *p, KvsPendingSignaling *out)
{
    if (p == NULL || out == NULL) {
        return;
    }
    *out = *p;
    out->batch_out = false;
    out->purged = NULL;
    out->purged_count = 0;
    p->ice = NULL;
    p->ice_count = 0;
    p->answers = NULL;
    p->answer_count = 0;
    /* An empty batch is never merged back, so it must not open a batch window:
     * every later purge would be recorded and Merge() would drop that peer's
     * next batch, a returning viewer's answer included. */
    p->batch_out = (out->ice_count + out->answer_count) > 0;
}

uint32_t kvsPendingSignalingMerge(KvsPendingSignaling *p, KvsPendingSignaling *batch)
{
    if (p == NULL || batch == NULL) {
        return 0;
    }
    uint32_t dropped = 0;

    /* Peers purged while the batch was out have left: nothing of theirs goes back. */
    for (uint32_t i = 0; i < p->purged_count; i++) {
        int idx = findAnswer(batch, p->purged[i]);
        if (idx >= 0) {
            removeAnswerAt(batch, (uint32_t) idx);
            dropped++;
        }
        if (batch->ice != NULL) {
            dropped += kvsPendingIcePurgePeer(batch->ice, &batch->ice_count, p->purged[i]);
        }
    }

    for (uint32_t i = 0; i < batch->answer_count; i++) {
        KvsPendingAnswer *a = &batch->answers[i];
        if (findAnswer(p, a->peer_client_id) >= 0) {
            dropped++;           /* p has a newer answer for this peer */
        } else if (ensureAnswerSlots(p) && insertAnswer(p, a)) {
            a->payload = NULL;   /* now owned by p */
        } else {
            /* no room: the peer's candidates are useless without the answer */
            dropped++;
            if (batch->ice != NULL) {
                dropped += kvsPendingIcePurgePeer(batch->ice, &batch->ice_count, a->peer_client_id);
            }
        }
    }

    /* A later insert above can evict an answer this loop merged earlier.
     * insertAnswer() purges that peer's candidates from p, but the batch still
     * holds its own, which would then go out with no answer ahead of them. */
    for (uint32_t i = 0; i < batch->answer_count; i++) {
        const char *peer = batch->answers[i].peer_client_id;
        if (batch->answers[i].payload == NULL && findAnswer(p, peer) < 0) {
            dropped++;
            if (batch->ice != NULL) {
                dropped += kvsPendingIcePurgePeer(batch->ice, &batch->ice_count, peer);
            }
        }
    }

    if (batch->ice_count > 0) {
        if (p->ice == NULL) {
            /* nothing new arrived: adopt the array (keeps per-entry attempts) */
            p->ice = batch->ice;
            p->ice_count = batch->ice_count;
            batch->ice = NULL;
            batch->ice_count = 0;
        } else {
            for (uint32_t i = 0; i < batch->ice_count; i++) {
                const KvsPendingIceCandidate *c = &batch->ice[i];
                uint32_t at = p->ice_count;
                if (kvsPendingIcePush(p->ice, &p->ice_count, c->peer_client_id,
                                      c->payload, c->payload_len, c->version)) {
                    p->ice[at].attempts = c->attempts;
                } else {
                    dropped++;
                }
            }
        }
    }

    kvsPendingSignalingFree(batch);
    releaseEmpty(p);
    clearPurged(p);
    p->batch_out = false;
    return dropped;
}

typedef struct {
    kvs_pending_send_fn fn;
    void *ctx;
} pending_send_adapter_t;

static bool pendingIceSendAdapter(void *ctx, const KvsPendingIceCandidate *slot)
{
    pending_send_adapter_t *a = (pending_send_adapter_t *) ctx;
    return a->fn(a->ctx, KVS_PENDING_MSG_ICE_CANDIDATE, slot->peer_client_id, "",
                 slot->payload, slot->payload_len, slot->version);
}

static bool holdBehindAnswer(void *ctx, const char *peerId)
{
    return findAnswer((const KvsPendingSignaling *) ctx, peerId) >= 0;
}

uint32_t kvsPendingSignalingDrain(KvsPendingSignaling *p, kvs_pending_send_fn send_fn,
                                  void *ctx, uint64_t now_ms, KvsPendingDrainStats *stats)
{
    KvsPendingDrainStats local;
    if (stats == NULL) {
        stats = &local;
    }
    memset(stats, 0, sizeof(*stats));

    if (p == NULL || send_fn == NULL) {
        return kvsPendingSignalingCount(p);
    }

    /* Answers first: a candidate that reaches the viewer before its answer is
     * rejected (no remote description yet) and lost. */
    uint32_t w = 0;
    for (uint32_t r = 0; r < p->answer_count; r++) {
        KvsPendingAnswer *a = &p->answers[r];
        uint64_t age = (now_ms > a->queued_at_ms) ? now_ms - a->queued_at_ms : 0;

        if (age > KVS_PENDING_ANSWER_MAX_AGE_MS) {
            stats->answers_expired++;
            if (p->ice != NULL) {
                stats->ice_dropped += kvsPendingIcePurgePeer(p->ice, &p->ice_count, a->peer_client_id);
            }
            pendingFree(p, a->payload);
            continue;
        }
        if (send_fn(ctx, KVS_PENDING_MSG_ANSWER, a->peer_client_id, a->correlation_id,
                    a->payload, a->payload_len, a->version)) {
            stats->answers_sent++;
            pendingFree(p, a->payload);
            continue;
        }
        /* link dropped again: keep it (and its candidates) for the next reconnect */
        if (w != r) {
            p->answers[w] = *a;
        }
        w++;
    }
    if (p->answers != NULL) {
        p->answer_count = w;
    }

    if (p->ice != NULL) {
        pending_send_adapter_t adapter = { .fn = send_fn, .ctx = ctx };
        iceDrain(p->ice, &p->ice_count, pendingIceSendAdapter, &adapter,
                 holdBehindAnswer, p, &stats->ice_sent, &stats->ice_dropped);
    }

    releaseEmpty(p);
    return kvsPendingSignalingCount(p);
}

void kvsPendingSignalingFree(KvsPendingSignaling *p)
{
    if (p == NULL) {
        return;
    }
    if (p->answers != NULL) {
        for (uint32_t i = 0; i < p->answer_count; i++) {
            pendingFree(p, p->answers[i].payload);
        }
    }
    p->answer_count = 0;
    p->ice_count = 0;
    releaseEmpty(p);
    clearPurged(p);
    p->batch_out = false;
}
