/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stddef.h>
#include "kvs_liveness.h"

bool kvs_liveness_should_evict(const kvs_liveness_health_t *h,
                               const kvs_liveness_cfg_t *cfg,
                               uint64_t now)
{
    if (h == NULL || cfg == NULL) {
        return false;
    }
    /* Already being reclaimed by the normal teardown path — don't double-act. */
    if (h->terminate_flag) {
        return false;
    }

    if (!h->ever_connected) {
        /* Never reached CONNECTED. Give it until the connect deadline from when
         * the session started, then declare it dead. */
        if (now < h->offer_time) {
            return false;   /* clock stepped back; don't underflow into an eviction */
        }
        return (now - h->offer_time) > cfg->connect_deadline;
    }

    /* Has sent before: judge on the send counter alone. A dead send path freezes
     * it, while inbound traffic from a still-live viewer must not excuse that. */
    if (h->ever_sent) {
        if (now < h->last_send_progress) {
            return false;
        }
        return (now - h->last_send_progress) > cfg->stall_deadline;
    }

    /* Never sent, but has received: receive-only or data-channel-only, so the
     * receive counter is the only liveness signal available. */
    if (h->ever_recvd) {
        if (now < h->last_recv_progress) {
            return false;
        }
        return (now - h->last_recv_progress) > cfg->stall_deadline;
    }

    /* Connected but no traffic has ever been seen — a quiet session looks
     * identical to a dead one on this signal, so leave it alone. */
    return false;
}
