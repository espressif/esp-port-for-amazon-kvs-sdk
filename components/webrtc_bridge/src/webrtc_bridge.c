/*
 * SPDX-FileCopyrightText: 2024-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/semphr.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_work_queue.h"
#include "hosted_chunked_transport.h"
#include "webrtc_bridge.h"

#if CONFIG_ESP_WEBRTC_BRIDGE_HOSTED
/* Coprocessor (slave) uses peer_data API; host uses esp_hosted */
#if CONFIG_ESP_HOSTED_COPROCESSOR
#include "esp_hosted_peer_data.h"
#elif CONFIG_ESP_HOSTED_ENABLED
#include "esp_hosted.h"
#include "esp_hosted_misc.h"
#endif
#endif

#define RECEIVED_MSG_BUF_SIZE (10 * 1024)

#if CONFIG_ESP_WEBRTC_BRIDGE_HOSTED
static SemaphoreHandle_t mutex;

/* WebRTC message ID for custom data transfer */
#define WEBRTC_MSG_ID ((uint32_t)0x1000)

#else
#include "mqtt_client.h"
#define BROKER_URI "mqtt://mqtt.eclipseprojects.io"

#define SIGNALING_TOPIC "signal"
#define STREAMING_TOPIC "stream"

/*
 * MQTT needs separate topics per device role when two devices communicate via broker.
 * Signalling device: publishes to streaming topic, subscribes to signalling.
 * Streaming device: publishes to signalling topic, subscribes to streaming.
 * ENABLE_SIGNALLING_ONLY / ENABLE_STREAMING_ONLY are set by the project (e.g. example's option()).
 */
#if ENABLE_SIGNALLING_ONLY
#define TO_TOPIC STREAMING_TOPIC
#define FROM_TOPIC SIGNALING_TOPIC
#else
#define TO_TOPIC SIGNALING_TOPIC
#define FROM_TOPIC STREAMING_TOPIC
#endif

#define MAX_MQTT_MSG_SIZE (RECEIVED_MSG_BUF_SIZE)
// static char mqtt_message[MAX_MQTT_MSG_SIZE];

static esp_mqtt_client_handle_t g_mqtt_client;
#endif

static const char *TAG = "webrtc_bridge";

/* Callback for when we gather a received message from other side */
void on_webrtc_bridge_msg_received(const void *data, int len);

/* Function pointer for the message handler */
static webrtc_bridge_msg_cb_t message_handler = NULL;

/* Register message handler function */
void webrtc_bridge_register_handler(webrtc_bridge_msg_cb_t handler)
{
    message_handler = handler;
}

#if CONFIG_ESP_WEBRTC_BRIDGE_HOSTED
typedef struct {
    uint8_t *data;
    size_t len;
} webrtc_bridge_msg_ctx_t;

static void handle_on_message_received(void *priv_data)
{
    webrtc_bridge_msg_ctx_t *ctx = (webrtc_bridge_msg_ctx_t *)priv_data;

    ESP_LOGD(TAG, "handle_on_message_received: processing message (%zu bytes), handler=%p",
             ctx->len, message_handler);

    if (message_handler) {
        message_handler((const void *)ctx->data, (int)ctx->len);
    } else {
        on_webrtc_bridge_msg_received((void *)ctx->data, (int)ctx->len);
    }

    free(ctx->data);
    free(ctx);
}

static void webrtc_bridge_on_hosted_message(const uint8_t *data, size_t len)
{
    webrtc_bridge_msg_ctx_t *ctx = malloc(sizeof(webrtc_bridge_msg_ctx_t));
    if (!ctx) {
        ESP_LOGE(TAG, "Failed to allocate message context");
        free((void *)data);
        return;
    }
    ctx->data = (uint8_t *)data;
    ctx->len = len;

    ESP_LOGD(TAG, "Message complete (%zu bytes), queuing for processing", len);
    esp_err_t ret = esp_work_queue_add_task(&handle_on_message_received, ctx);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to queue message: %s", esp_err_to_name(ret));
        free((void *)data);
        free(ctx);
    }
}

static void webrtc_bridge_receive_callback(uint32_t msg_id, const uint8_t *data, size_t data_len, void *local_context)
{
    if (msg_id != WEBRTC_MSG_ID) {
        ESP_LOGW(TAG, "Unexpected msg_id: 0x%" PRIx32, msg_id);
        return;
    }
    hosted_chunked_process_chunk(WEBRTC_MSG_ID, data, data_len);
}
#endif /* CONFIG_ESP_WEBRTC_BRIDGE_HOSTED */

void webrtc_bridge_send_message(const char *data, int len)
{
#if CONFIG_ESP_WEBRTC_BRIDGE_HOSTED
    hosted_chunked_send(WEBRTC_MSG_ID, (const uint8_t *)data, (size_t)len,
                        mutex, HOSTED_CHUNK_WIRE_LEGACY);
    free((void *)data);
#else
    int msg_id = esp_mqtt_client_publish(g_mqtt_client, TO_TOPIC, data, len, 1, 0);
    ESP_LOGI(TAG, "sent publish successful, msg_id=%d", msg_id);
    free((void*)data);
#endif
}

#ifndef CONFIG_ESP_WEBRTC_BRIDGE_HOSTED
static void log_error_if_nonzero(const char *message, int error_code)
{
    if (error_code != 0) {
        ESP_LOGE(TAG, "Last error %s: 0x%x", message, error_code);
    }
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;
    esp_mqtt_client_handle_t client = event->client;
    int msg_id;
    // your_context_t *context = event->context;
    switch (event->event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
        msg_id = esp_mqtt_client_subscribe(client, FROM_TOPIC, 0);
        ESP_LOGI(TAG, "sent subscribe successful, msg_id=%d", msg_id);

        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
        break;

    case MQTT_EVENT_SUBSCRIBED:
        ESP_LOGI(TAG, "MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);

        break;
    case MQTT_EVENT_UNSUBSCRIBED:
        ESP_LOGI(TAG, "MQTT_EVENT_UNSUBSCRIBED, msg_id=%d", event->msg_id);
        break;
    case MQTT_EVENT_PUBLISHED:
        ESP_LOGI(TAG, "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
        break;
    case MQTT_EVENT_DATA:
        ESP_LOGI(TAG, "MQTT_EVENT_DATA");
        if (message_handler) {
            message_handler(event->data, event->data_len);
        } else {
            on_webrtc_bridge_msg_received((void *) event->data, event->data_len);
        }
        break;
    case MQTT_EVENT_ERROR:
        ESP_LOGI(TAG, "MQTT_EVENT_ERROR");
        if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
            log_error_if_nonzero("reported from esp-tls", event->error_handle->esp_tls_last_esp_err);
            log_error_if_nonzero("reported from tls stack", event->error_handle->esp_tls_stack_err);
            log_error_if_nonzero("captured as transport's socket errno",  event->error_handle->esp_transport_sock_errno);
            ESP_LOGI(TAG, "Last errno string (%s)", strerror(event->error_handle->esp_transport_sock_errno));
        }
        break;
    default:
        ESP_LOGI(TAG, "Other event id:%d", event->event_id);
        break;
    }
}
#endif

// Default implementation for when no handler is registered
void on_webrtc_bridge_msg_received(const void *data, int len)
{
    /* Forward the received WebRTC message to the registered handler */
    if (message_handler) {
        ESP_LOGD(TAG, "Forwarding WebRTC message to registered handler (len: %d)", len);
        message_handler((const char *) data, len);
    } else {
        ESP_LOGW(TAG, "Message received but no handler registered (len: %d)", len);
    }
}

void webrtc_bridge_start(void)
{
    static bool init_done = false;
    if (init_done) {
        ESP_LOGI(TAG, "webrtc_bridge already started");
        return;
    }
#if CONFIG_ESP_WEBRTC_BRIDGE_HOSTED
    ESP_LOGI(TAG, "Hosted mode enabled, initializing...");
    mutex = xSemaphoreCreateMutex();
    if (!mutex) {
        ESP_LOGE(TAG, "Failed to create mutex");
        return;
    }
    ESP_LOGI(TAG, "Mutex created successfully");

    hosted_chunked_register(WEBRTC_MSG_ID, webrtc_bridge_on_hosted_message);

    ESP_LOGI(TAG, "Registering WebRTC callback with msg_id: 0x%" PRIx32 " (%" PRIu32 ")", WEBRTC_MSG_ID, WEBRTC_MSG_ID);
    esp_err_t ret = esp_hosted_register_custom_callback(WEBRTC_MSG_ID, webrtc_bridge_receive_callback, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register WebRTC callback (msg_id: 0x%" PRIx32 "): %s", WEBRTC_MSG_ID, esp_err_to_name(ret));
        return;
    }
    ESP_LOGI(TAG, "WebRTC bridge callback registered successfully (msg_id: 0x%" PRIx32 ")", WEBRTC_MSG_ID);
#else
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = BROKER_URI,
        .buffer.size = MAX_MQTT_MSG_SIZE,
        .buffer.out_size = MAX_MQTT_MSG_SIZE,
        .outbox.limit = 2 * MAX_MQTT_MSG_SIZE,
        .task.stack_size = (10 * 1024), // 6K is insufficient
    };

    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&mqtt_cfg);
    g_mqtt_client = client;
    /* The last argument may be used to pass data to the event handler, in this example mqtt_event_handler */
    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(client);
#endif
    init_done = true;
}
