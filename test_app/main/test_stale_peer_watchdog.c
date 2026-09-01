/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Tests for the kvs_webrtc per-session liveness watchdog.
 *
 * Guards the fix for the "device not responding / blank video, reboot-only
 * recovery" field bug: a dead relay peer whose ICE never gets declared
 * disconnected holds its viewer slot forever, so the device stops answering
 * offers (iceAgentSendPacket "Invalid state" spam). The watchdog must declare
 * such a session dead so kvs_webrtc can drive the normal DISCONNECTED teardown
 * and free the slot.
 */

#include "unity.h"
#include "kvs_liveness.h"

/* Arbitrary consistent time unit for the tests (firmware uses SDK 100ns ticks). */
#define TU 1000ULL
static const kvs_liveness_cfg_t CFG = { .connect_deadline = 20 * TU, .stall_deadline = 10 * TU };

TEST_CASE("liveness: session that never connects is evicted past the connect deadline", "[watchdog]")
{
    kvs_liveness_health_t h = { .terminate_flag = false, .ever_connected = false,
                                .offer_time = 5 * TU };
    TEST_ASSERT_FALSE(kvs_liveness_should_evict(&h, &CFG, 5 * TU + 19 * TU)); /* within deadline */
    TEST_ASSERT_TRUE(kvs_liveness_should_evict(&h, &CFG, 5 * TU + 21 * TU));  /* past deadline  */
}

TEST_CASE("liveness: a connected session whose traffic goes flat is evicted", "[watchdog]")
{
    kvs_liveness_health_t h = { .terminate_flag = false, .ever_connected = true,
                                .ever_sent = true, .offer_time = 0, .last_send_progress = 100 * TU };
    TEST_ASSERT_FALSE(kvs_liveness_should_evict(&h, &CFG, 100 * TU + 5 * TU));  /* bytes advanced recently */
    TEST_ASSERT_TRUE(kvs_liveness_should_evict(&h, &CFG, 100 * TU + 11 * TU));  /* flat > 10 */
}

TEST_CASE("liveness: a healthy connected session is never evicted", "[watchdog]")
{
    kvs_liveness_health_t h = { .terminate_flag = false, .ever_connected = true,
                                .ever_sent = true, .offer_time = 0, .last_send_progress = 500 * TU };
    /* traffic keeps advancing near now -> alive */
    TEST_ASSERT_FALSE(kvs_liveness_should_evict(&h, &CFG, 501 * TU));
}

TEST_CASE("liveness: a connected session that never moved traffic is left alone", "[watchdog]")
{
    /* Camera init failed and the app continued without video, or a receive-only /
     * data-channel-only viewer: CONNECTED, nothing ever sent or received on the
     * pair. Evicting these turned a healthy session into a ~40 s reconnect loop. */
    kvs_liveness_health_t h = { .terminate_flag = false, .ever_connected = true,
                                .offer_time = 0 };
    TEST_ASSERT_FALSE(kvs_liveness_should_evict(&h, &CFG, 11 * TU));
    TEST_ASSERT_FALSE(kvs_liveness_should_evict(&h, &CFG, 9999 * TU));
}

TEST_CASE("liveness: ever_connected is what gates the connect deadline", "[watchdog]")
{
    /* ever_connected is latched by the caller, so a session that reached CONNECTED
     * and has since gone quiet must not fall back to the connect-deadline branch
     * (which would evict it the moment it is older than connect_deadline). */
    kvs_liveness_health_t h = { .terminate_flag = false, .ever_connected = true,
                                .offer_time = 0 };
    TEST_ASSERT_FALSE(kvs_liveness_should_evict(&h, &CFG, 21 * TU));
}

TEST_CASE("liveness: an already-terminating session is not re-evicted", "[watchdog]")
{
    kvs_liveness_health_t h = { .terminate_flag = true, .ever_connected = false,
                                .offer_time = 0 };
    TEST_ASSERT_FALSE(kvs_liveness_should_evict(&h, &CFG, 9999 * TU));
}

TEST_CASE("liveness: NULL args are handled", "[watchdog]")
{
    kvs_liveness_health_t h = { 0 };
    TEST_ASSERT_FALSE(kvs_liveness_should_evict(NULL, &CFG, 1));
    TEST_ASSERT_FALSE(kvs_liveness_should_evict(&h, NULL, 1));
}

TEST_CASE("liveness: inbound traffic must not mask a dead send path", "[watchdog]")
{
    /* The reported field failure: our sends are rejected (the pair never reaches
     * SUCCEEDED so bytesSent stops advancing) while the viewer's RTCP keeps
     * bytesReceived climbing every tick. Judging on the two counters combined
     * would call this session healthy forever, which is the bug the watchdog
     * exists to catch. */
    kvs_liveness_health_t h = { .terminate_flag = false, .ever_connected = true,
                                .ever_sent = true,  .last_send_progress = 100 * TU,
                                .ever_recvd = true, .last_recv_progress = 200 * TU };
    TEST_ASSERT_TRUE(kvs_liveness_should_evict(&h, &CFG, 205 * TU));
}

TEST_CASE("liveness: a receive-only session is judged on its receive counter", "[watchdog]")
{
    /* Never sent (receive-only or data-channel-only), so receive is the only
     * liveness signal there is: fresh means alive, flat means dead. */
    kvs_liveness_health_t h = { .terminate_flag = false, .ever_connected = true,
                                .ever_sent = false, .ever_recvd = true,
                                .last_recv_progress = 100 * TU };
    TEST_ASSERT_FALSE(kvs_liveness_should_evict(&h, &CFG, 100 * TU + 5 * TU));
    TEST_ASSERT_TRUE(kvs_liveness_should_evict(&h, &CFG, 100 * TU + 11 * TU));
}

TEST_CASE("liveness: a backward clock step does not evict", "[watchdog]")
{
    /* GETTIME() is CLOCK_REALTIME. SNTP uses smooth sync, but a hard step would
     * make the unsigned delta underflow and evict every session at once. */
    kvs_liveness_health_t sending = { .terminate_flag = false, .ever_connected = true,
                                      .ever_sent = true, .last_send_progress = 500 * TU };
    TEST_ASSERT_FALSE(kvs_liveness_should_evict(&sending, &CFG, 100 * TU));

    kvs_liveness_health_t connecting = { .terminate_flag = false, .ever_connected = false,
                                         .offer_time = 500 * TU };
    TEST_ASSERT_FALSE(kvs_liveness_should_evict(&connecting, &CFG, 100 * TU));
}
