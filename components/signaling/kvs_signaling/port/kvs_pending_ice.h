/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/*
 * Signaling messages whose send failed because the websocket was momentarily
 * down, held for re-send once the signaling client reconnects:
 *
 * - ICE candidates: AWS FINs the signaling socket shortly after the SDP answer,
 *   while relay candidates are still gathering. Re-sending them lets a viewer on
 *   a relay-only path (e.g. cellular) still learn the device's relay candidate
 *   instead of failing with NO_CONNECTED_CANDIDATE_PAIR.
 * - SDP answers: a viewer offer can wake a parked host and the link can drop
 *   (Wi-Fi re-init, AP drop, AWS FIN) before the answer goes out. Without the
 *   answer the viewer never connects.
 *
 * Kept free of KVS/lws types so it can be unit-tested on the host. The owner
 * (kvs_signaling) serialises access under its send mutex and drains outside
 * that mutex (kvsPendingSignalingTake / Drain / Merge).
 */
#define KVS_PENDING_ICE_MAX          16
#define KVS_PENDING_ICE_PAYLOAD_LEN  512
#define KVS_PENDING_ICE_PEERID_LEN   256   /* >= MAX_SIGNALING_CLIENT_ID_LEN */
#define KVS_PENDING_ICE_MAX_ATTEMPTS 5     /* drop a candidate after this many failed re-sends */

/* Peers that can have an answer waiting at once. One slot per peer: a newer
 * answer for the same peer replaces the older one. When all slots are taken the
 * oldest answer is evicted, as it is the closest to expiring anyway. */
#define KVS_PENDING_ANSWER_MAX            4
#define KVS_PENDING_CORRELATION_ID_LEN    257   /* MAX_CORRELATION_ID_LEN + 1 */

/* A buffered answer older than this is dropped instead of sent. The session it
 * answers has to reach CONNECTED within KVS_LIVENESS_CONNECT_DEADLINE (30 s,
 * kvs_webrtc.c) of the offer or the liveness watchdog tears it down, so an
 * answer delivered later only points the viewer at a dead session. */
#define KVS_PENDING_ANSWER_MAX_AGE_MS     30000

/* Peers that can be purged while a taken batch is out (see Take / Merge). A purge
 * beyond this is not remembered, and Merge can put that peer's messages back. */
#define KVS_PENDING_PURGED_MAX            8

typedef struct {
    char     peer_client_id[KVS_PENDING_ICE_PEERID_LEN];
    char     payload[KVS_PENDING_ICE_PAYLOAD_LEN];
    uint32_t payload_len;
    uint32_t version;
    uint32_t attempts;   /* failed re-send attempts so far */
} KvsPendingIceCandidate;

typedef struct {
    char     peer_client_id[KVS_PENDING_ICE_PEERID_LEN];
    char     correlation_id[KVS_PENDING_CORRELATION_ID_LEN];
    char    *payload;        /* heap copy, NUL-terminated (SDP answers run to several KB) */
    uint32_t payload_len;
    uint32_t version;
    uint64_t queued_at_ms;   /* when the failed send was buffered (monotonic ms) */
} KvsPendingAnswer;

typedef enum {
    KVS_PENDING_MSG_ANSWER,
    KVS_PENDING_MSG_ICE_CANDIDATE,
} kvs_pending_msg_type_t;

typedef void *(*kvs_pending_calloc_fn)(size_t num, size_t size);
typedef void (*kvs_pending_free_fn)(void *ptr);

/*
 * Everything waiting for a reconnect. Both slot arrays are allocated on the
 * first buffered message of their kind and freed as soon as they drain empty,
 * so nothing (~12 KB for the ICE slots) stays resident while the link is up.
 * Answer payloads are allocated per answer and freed when sent or dropped.
 */
typedef struct {
    KvsPendingIceCandidate *ice;        /* KVS_PENDING_ICE_MAX slots, or NULL */
    uint32_t                ice_count;
    KvsPendingAnswer       *answers;    /* KVS_PENDING_ANSWER_MAX slots, or NULL */
    uint32_t                answer_count;
    kvs_pending_calloc_fn   calloc_fn;  /* NULL: libc calloc */
    kvs_pending_free_fn     free_fn;    /* NULL: libc free */
    bool                    batch_out;  /* Take() handed a non-empty batch out; Merge() ends it */
    /* Peers purged while the batch was out, so Merge() doesn't put their messages
     * back: KVS_PENDING_PURGED_MAX ids, allocated on first use, or NULL. */
    char                  (*purged)[KVS_PENDING_ICE_PEERID_LEN];
    uint32_t                purged_count;
} KvsPendingSignaling;

/* What one kvsPendingSignalingDrain() pass did, for the owner's log. */
typedef struct {
    uint32_t answers_sent;
    uint32_t answers_expired;   /* older than KVS_PENDING_ANSWER_MAX_AGE_MS, dropped */
    uint32_t ice_sent;
    uint32_t ice_dropped;       /* hit KVS_PENDING_ICE_MAX_ATTEMPTS, or their answer expired */
} KvsPendingDrainStats;

#ifdef __cplusplus
extern "C" {
#endif

/* ---- ICE candidate slot array (no allocation) --------------------------- */

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

/* ---- Answers + candidates, with lazy allocation ------------------------- */

/* Start empty with the given allocator (either may be NULL for libc). */
void kvsPendingSignalingInit(KvsPendingSignaling *p, kvs_pending_calloc_fn calloc_fn,
                             kvs_pending_free_fn free_fn);

/* Buffer one ICE candidate (allocates the slot array on first use). Same
 * rules and return value as kvsPendingIcePush. */
bool kvsPendingSignalingPushIce(KvsPendingSignaling *p, const char *peerId,
                                const char *payload, uint32_t payloadLen, uint32_t version);

/*
 * Buffer the answer for peerId, stamped now_ms. Replaces an answer already
 * waiting for the same peer (only the newest answer is worth sending); with no
 * slot free, evicts the oldest answer together with its peer's candidates, which
 * are useless without it. Returns false on invalid args or when memory for the
 * slots or the payload copy can't be allocated.
 */
bool kvsPendingSignalingPutAnswer(KvsPendingSignaling *p, const char *peerId,
                                  const char *correlationId, const char *payload,
                                  uint32_t payloadLen, uint32_t version, uint64_t now_ms);

/* True if an answer for peerId is waiting. */
bool kvsPendingSignalingHasAnswer(const KvsPendingSignaling *p, const char *peerId);

/* Drop the answer waiting for peerId, if any (a newer one went out directly).
 * Returns true if one was dropped. */
bool kvsPendingSignalingDropAnswer(KvsPendingSignaling *p, const char *peerId);

/*
 * Drop everything waiting for a departed peer. While a taken batch is out, also
 * remember the peer (up to KVS_PENDING_PURGED_MAX) so Merge() drops the batch's
 * messages for it too. Returns the number removed from p.
 */
uint32_t kvsPendingSignalingPurgePeer(KvsPendingSignaling *p, const char *peerId);

/* True if peerId was purged while the current batch is out: the owner skips
 * re-sending that peer's messages from the batch. */
bool kvsPendingSignalingIsPurged(const KvsPendingSignaling *p, const char *peerId);

/* Number of messages waiting (answers + candidates). */
uint32_t kvsPendingSignalingCount(const KvsPendingSignaling *p);

/*
 * Move everything from p into out (out's previous content is overwritten, not
 * freed) and leave p empty with its allocator, so the owner can drain out
 * without holding its lock. Marks the batch as out until Merge(); take only one
 * batch at a time.
 */
void kvsPendingSignalingTake(KvsPendingSignaling *p, KvsPendingSignaling *out);

/*
 * Move the survivors of a drained batch back into p and leave batch empty and
 * freed, ending the batch. Dropped instead of merged:
 * - messages for peers purged while the batch was out;
 * - the batch's answer for a peer when p has a newer one;
 * - an answer with no slot left, and that peer's candidates with it;
 * - candidates that no longer fit.
 * Returns the number of batch messages dropped.
 */
uint32_t kvsPendingSignalingMerge(KvsPendingSignaling *p, KvsPendingSignaling *batch);

/*
 * Callback used by kvsPendingSignalingDrain to re-send one message.
 * correlationId is "" for candidates. Returns true if the send succeeded.
 */
typedef bool (*kvs_pending_send_fn)(void *ctx, kvs_pending_msg_type_t type,
                                    const char *peerId, const char *correlationId,
                                    const char *payload, uint32_t payloadLen,
                                    uint32_t version);

/*
 * Re-send what is waiting: every answer first, then the candidates in FIFO
 * order, so a viewer never gets candidates before the answer they belong to.
 *
 * - An answer older than KVS_PENDING_ANSWER_MAX_AGE_MS at now_ms is dropped
 *   unsent, together with that peer's candidates (useless without it).
 * - An answer whose send fails is kept for the next reconnect (until it
 *   expires), and that peer's candidates are held back untouched behind it.
 * - Other candidates follow kvsPendingIceDrain (attempt cap).
 *
 * Arrays that end up empty are freed. stats may be NULL. Returns the number of
 * messages still waiting.
 */
uint32_t kvsPendingSignalingDrain(KvsPendingSignaling *p, kvs_pending_send_fn send_fn,
                                  void *ctx, uint64_t now_ms, KvsPendingDrainStats *stats);

/* Free everything (slots and answer payloads). */
void kvsPendingSignalingFree(KvsPendingSignaling *p);

#ifdef __cplusplus
}
#endif
