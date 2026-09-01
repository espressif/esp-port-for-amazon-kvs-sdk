/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Tests for the P4's READY_QUERY answer in split mode.
 *
 * The C6 (signaling_bridge_adapter) parks incoming offers in a
 * WAITING_FOR_WAKEUP queue and flushes the whole queue the moment the P4
 * answers a READY_QUERY with a READY. That flush frees each message with no
 * retry, so the answer is only safe once the P4 can actually consume messages
 * (on_msg_received registered) — answering earlier loses every queued OFFER
 * silently. Staying quiet costs nothing: the C6 keeps the queue and
 * bridgeConnect() sends READY as soon as the client is up.
 */

#include <string.h>
#include <stdlib.h>
#include "unity.h"
#include "signaling_serializer.h"
#include "webrtc_bridge_signaling.h"

/* ---- capture of what bridge_signaling sends back over the transport ---- */
static int   g_sent_count;
static char  g_last_sent[512];
static size_t g_last_sent_len;

/* ---- stubs for bridge_signaling.c's external dependencies ----
 * bridge_signaling.c is compiled standalone for this host test, so every
 * symbol it references must resolve. Only webrtc_bridge_send_message captures;
 * the rest are inert. */
void webrtc_bridge_send_message(const char *data, int len)
{
    g_sent_count++;
    g_last_sent_len = (size_t)len;
    if (len > 0 && (size_t)len <= sizeof(g_last_sent)) {
        memcpy(g_last_sent, data, (size_t)len);
    }
    /* bridge_signaling transfers ownership of the serialized buffer to us */
    free((void *)data);
}

void  webrtc_bridge_register_handler(int msg_id, void (*cb)(const void *, int)) { (void)msg_id; (void)cb; }

void  app_webrtc_trigger_offer(const char *peer_id) { (void)peer_id; }

int   esp_work_queue_add_task(void *fn, void *arg) { (void)fn; (void)arg; return 0; }

/* ice_bridge_client_* — declared in bridge_signaling/src/ice_bridge_client.h */
int   ice_bridge_client_init(void *cfg) { (void)cfg; return 0; }
void  ice_bridge_client_clear_cache(void) {}
int   ice_bridge_client_get_cached_servers(void *out) { (void)out; return 0; }
int   ice_bridge_client_get_servers(void *out) { (void)out; return 0; }
int   ice_bridge_client_request_ice_servers_async(int idx) { (void)idx; return 0; }
void  ice_bridge_client_set_ice_server_response(void *r) { (void)r; }
void  ice_bridge_client_set_servers_updated_callback(void *cb, unsigned long long d) { (void)cb; (void)d; }

static void reset_capture(void)
{
    g_sent_count = 0;
    g_last_sent_len = 0;
    memset(g_last_sent, 0, sizeof(g_last_sent));
}

static WEBRTC_STATUS dummy_on_msg(uint64_t d, webrtc_message_t *m) { (void)d; (void)m; return WEBRTC_STATUS_SUCCESS; }

/* Serialize a READY_QUERY exactly as the C6 sends it and feed it to the P4. */
static void feed_ready_query(void)
{
    signaling_msg_t query = {0};
    query.version = 0;
    query.messageType = SIGNALING_MSG_TYPE_READY_QUERY;
    size_t query_len = 0;
    char *query_buf = serialize_signaling_message(&query, &query_len);
    TEST_ASSERT_NOT_NULL(query_buf);
    bridge_message_handler(query_buf, (int)query_len);
    free(query_buf);
}

/*
 * Not-ready window: no client, or a client with no on_msg_received yet. Answering
 * here would make the C6 flush its queue into a handler that drops and frees every
 * message, so the P4 must stay silent and let the C6 keep queuing.
 */
TEST_CASE("READY_QUERY is not answered while the P4 cannot consume messages", "[bridge_signaling]")
{
    signaling_serializer_init();
    reset_capture();

    /* Fresh module state => g_bridge_client is NULL. */
    feed_ready_query();
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, g_sent_count,
                                  "P4 answered READY with no client - C6 would flush offers into a drop");

    /* Client allocated but callbacks not registered yet (bridgeInit -> set_callbacks gap). */
    webrtc_signaling_client_if_t *iface = bridge_signaling_client_if_get();
    TEST_ASSERT_NOT_NULL(iface);
    bridge_signaling_config_t cfg = { .client_id = NULL, .log_level = 0 };
    void *client = NULL;
    TEST_ASSERT_EQUAL(WEBRTC_STATUS_SUCCESS, iface->init(&cfg, &client));
    TEST_ASSERT_NOT_NULL(client);

    reset_capture();
    feed_ready_query();
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, g_sent_count,
                                  "P4 answered READY before on_msg_received was registered");

    /* ...and once the callback is in place, the same query is answered. */
    TEST_ASSERT_EQUAL(WEBRTC_STATUS_SUCCESS,
                      iface->set_callbacks(client, 0, dummy_on_msg, NULL, NULL));
    reset_capture();
    feed_ready_query();
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, g_sent_count,
                                  "P4 stayed silent on READY_QUERY while ready (wedge regression)");

    signaling_msg_t reply = {0};
    TEST_ASSERT_EQUAL(ESP_OK, deserialize_signaling_message(g_last_sent, g_last_sent_len, &reply));
    TEST_ASSERT_EQUAL_INT_MESSAGE(SIGNALING_MSG_TYPE_READY, reply.messageType,
                                  "P4 reply to READY_QUERY was not a READY signal");

    iface->free(client);
}
