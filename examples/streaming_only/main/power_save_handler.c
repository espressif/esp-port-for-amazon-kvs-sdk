/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "power_save_handler.h"
#include "hosted_recovery.h"
#if __has_include("esp_private/sleep_uart.h")
#include "esp_private/sleep_uart.h"
#define HAVE_SLEEP_UART_HANDLING 1
#else
#define HAVE_SLEEP_UART_HANDLING 0
#endif
#include "soc/soc_caps.h"
#include "esp_log.h"
#include <inttypes.h>

#if CONFIG_IDF_TARGET_ESP32P4 && CONFIG_EXAMPLE_ENABLE_POWER_SAVE
#include "esp_hosted_power_save.h"
#include "freertos/portmacro.h"
#include "esp_netif.h"
#include "esp_event.h"
#endif

#include <stdio.h>
#include <esp_console.h>

static const char *TAG = "power_save_handler";

#if CONFIG_IDF_TARGET_ESP32P4 && CONFIG_EXAMPLE_ENABLE_POWER_SAVE
/* Idle deep-sleep timer timeout - device sleeps if no activity for this duration */
#define IDLE_DEEP_SLEEP_TIMEOUT_MS (20 * 1000)
/* Post-streaming deep-sleep timer timeout - shorter timeout after streaming stops */
#define POST_STREAMING_DEEP_SLEEP_TIMEOUT_MS (10 * 1000)
/* Track active peer connection count - keep device active while any peer is connected */
static uint32_t g_active_peer_connections = 0;
static portMUX_TYPE g_peer_connections_lock = portMUX_INITIALIZER_UNLOCKED;
#if CONFIG_EXAMPLE_POWER_SAVE_DEFER_UNTIL_LINK_UP
/* Set when the idle timer was asked for before the host held an IP. */
static bool g_idle_timer_deferred = false;

static bool host_has_ip(void)
{
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip = { 0 };

    return netif != NULL && esp_netif_get_ip_info(netif, &ip) == ESP_OK && ip.ip.addr != 0;
}
#endif

static void start_idle_timer(const char *why)
{
#if CONFIG_EXAMPLE_POWER_SAVE_DEFER_UNTIL_LINK_UP
    /* The co-processor also wakes us for serial, RPC, private and flow-control
     * traffic, and on its own reboot, so sleeping without a link is not by
     * itself a trap. Deployments that expect only inbound network traffic opt
     * into waiting for one. */
    if (!host_has_ip()) {
        g_idle_timer_deferred = true;
        ESP_LOGW(TAG, "%s: no IP yet, deferring deep sleep until the link is up", why);
        return;
    }
    g_idle_timer_deferred = false;
#endif
    hosted_recovery_pause();
    if (esp_hosted_power_save_timer_start(IDLE_DEEP_SLEEP_TIMEOUT_MS) != 0) {
        ESP_LOGE(TAG, "idle deep-sleep timer refused; the host will not sleep");
    } else {
        ESP_LOGI(TAG, "%s: started idle deep-sleep timer (%d ms)", why, IDLE_DEEP_SLEEP_TIMEOUT_MS);
    }
}

#if CONFIG_EXAMPLE_POWER_SAVE_DEFER_UNTIL_LINK_UP
static void got_ip_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    portENTER_CRITICAL(&g_peer_connections_lock);
    uint32_t peers = g_active_peer_connections;
    portEXIT_CRITICAL(&g_peer_connections_lock);

    /* A link that comes back during a stream must not start the countdown. The
     * last peer to leave starts it. */
    if (g_idle_timer_deferred && peers == 0) {
        start_idle_timer("link up");
    }
}

/* Losing the address mid-countdown would park the host with the wake source it
 * was waiting for gone. Stand the timer down and let got_ip_handler re-arm it. */
static void lost_ip_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    esp_hosted_power_save_timer_stop();
    hosted_recovery_resume();
    g_idle_timer_deferred = true;
    ESP_LOGW(TAG, "link down: deep sleep held off until the host has an IP again");
}
#endif

/* Helper function to safely increment peer connection count */
static uint32_t increment_peer_connection(void)
{
    uint32_t count = 0;
    portENTER_CRITICAL(&g_peer_connections_lock);
    g_active_peer_connections++;
    count = g_active_peer_connections;
    portEXIT_CRITICAL(&g_peer_connections_lock);
    return count;
}

/* Helper function to safely decrement peer connection count */
static uint32_t decrement_peer_connection(void)
{
    uint32_t count = 0;
    portENTER_CRITICAL(&g_peer_connections_lock);
    if (g_active_peer_connections > 0) {
        g_active_peer_connections--;
    }
    count = g_active_peer_connections;
    portEXIT_CRITICAL(&g_peer_connections_lock);
    return count;
}
#endif

#if CONFIG_IDF_TARGET_ESP32P4 && CONFIG_EXAMPLE_ENABLE_POWER_SAVE
/**
 * @brief Power save event handler for WebRTC events
 */
static void power_save_event_handler(app_webrtc_event_data_t *event_data, void *user_ctx)
{
    if (event_data == NULL) {
        return;
    }

    switch (event_data->event_id) {
        case APP_WEBRTC_EVENT_INITIALIZED:
            /* Start idle timer when WebRTC is initialized */
            portENTER_CRITICAL(&g_peer_connections_lock);
            g_active_peer_connections = 0;
            portEXIT_CRITICAL(&g_peer_connections_lock);
            start_idle_timer("WebRTC initialized");
            break;
        case APP_WEBRTC_EVENT_PEER_CONNECTION_REQUESTED:
            start_idle_timer("peer connection requested");
            break;

        case APP_WEBRTC_EVENT_PEER_CONNECTED:
            /* Peer connected - increment count and stop timer */
            {
                uint32_t count = increment_peer_connection();
                ESP_LOGI(TAG, "Peer connected: %s (active peers: %" PRIu32 "), stopping host power save timer",
                         event_data->peer_id ? event_data->peer_id : "unknown", count);
                esp_hosted_power_save_timer_stop();
                hosted_recovery_resume();
            }
            break;

        case APP_WEBRTC_EVENT_PEER_DISCONNECTED:
            /* Peer disconnected - decrement count and start timer if no peers left */
            {
                uint32_t count = decrement_peer_connection();
                ESP_LOGI(TAG, "Peer disconnected: %s (active peers: %" PRIu32 ")",
                         event_data->peer_id ? event_data->peer_id : "unknown", count);
                if (count == 0) {
                    ESP_LOGI(TAG, "All peer connections closed, starting host power save timer");
                    /* Give host few seconds, as it might still get some packets */
                    hosted_recovery_pause();
                    if (esp_hosted_power_save_timer_start(POST_STREAMING_DEEP_SLEEP_TIMEOUT_MS) != 0) {
                        ESP_LOGE(TAG, "post-stream deep-sleep timer refused; the host will not sleep");
                    }
                } else {
                    ESP_LOGI(TAG, "Other peers still connected (%" PRIu32 "), keeping timer stopped", count);
                }
            }
            break;

        case APP_WEBRTC_EVENT_RECEIVED_OFFER:
            ESP_LOGI(TAG, "Received offer, resetting host power save timer");
            hosted_recovery_resume();
            esp_hosted_power_save_timer_start(IDLE_DEEP_SLEEP_TIMEOUT_MS);
            break;

        case APP_WEBRTC_EVENT_SENT_ANSWER:
            /* Answer sent means connection and streaming will happen, stop timer */
            ESP_LOGI(TAG, "Sent answer, stopping host power save timer");
            esp_hosted_power_save_timer_stop();
            break;

        default:
            /* Other events don't affect power save */
            break;
    }
}
#endif

#if CONFIG_IDF_TARGET_ESP32P4 && CONFIG_EXAMPLE_ENABLE_POWER_SAVE
/* IDF suspends every UART it believes is enabled before it sleeps.
 * uart_ll_is_enabled() reads the reset bit and the APB and system clock
 * enables, but not the core clock. UART1 passes that test out of reset even
 * though no driver ever claimed it, so uart_ll_update() waits on a register
 * bit that a stopped core clock never clears, and the interrupt watchdog
 * fires. The console is the only port this application uses, so tell the
 * sleep path to leave the others alone. */
static void skip_unused_uarts_on_sleep(void)
{
#if HAVE_SLEEP_UART_HANDLING && SOC_UART_HP_NUM > 0
    for (int i = 0; i < SOC_UART_HP_NUM; i++) {
        if (i == CONFIG_ESP_CONSOLE_UART_NUM) {
            continue;
        }
        sleep_uart_set_handling_mode(i, ESP_SLEEP_NO_HANDLING);
    }
    ESP_LOGI(TAG, "sleep: UART handling left to the console only");
#else
    ESP_LOGW(TAG, "sleep: this IDF has no per-UART sleep handling; an unused UART1 can hang sleep entry");
#endif
}
#endif

int32_t power_save_enable(void)
{
#if CONFIG_IDF_TARGET_ESP32P4 && CONFIG_EXAMPLE_ENABLE_POWER_SAVE
    /* hosted's auto-init constructor is off here: slave_flasher must program the
     * co-processor before hosted enumerates it. That constructor is what would
     * normally initialise this feature, so do it explicitly. Without it every
     * timer start returns -EINVAL and the host never sleeps. */
    int32_t ret = esp_hosted_power_save_init();
    if (ret != 0) {
        ESP_LOGE(TAG, "hosted power save init failed: %" PRId32, ret);
        return ret;
    }

    skip_unused_uarts_on_sleep();

#if CONFIG_EXAMPLE_POWER_SAVE_DEFER_UNTIL_LINK_UP
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, got_ip_handler, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_LOST_IP, lost_ip_handler, NULL);
#endif

    ret = app_webrtc_register_event_callback(power_save_event_handler, NULL);
    if (ret != 0) {
        ESP_LOGE(TAG, "Failed to enable power save");
        return ret;
    }
    ESP_LOGI(TAG, "Power save enabled successfully");
    return 0;
#else
    /* Power save not enabled or not supported for this target */
    return 0;
#endif
}

#if CONFIG_IDF_TARGET_ESP32P4 && CONFIG_EXAMPLE_ENABLE_POWER_SAVE
/**
 * @brief CLI handler for manual deep sleep command
 */
static int deep_sleep_cli_handler(int argc, char *argv[])
{
    ESP_LOGI(TAG, "Putting ESP32-P4 into deep sleep...");
    esp_hosted_power_save_start(HOSTED_POWER_SAVE_TYPE_DEEP_SLEEP);
    return 0;
}

static esp_console_cmd_t sleep_cmd[] = {
    {
        .command = "deep-sleep",
        .help = "Put P4 into deep sleep",
        .func = deep_sleep_cli_handler,
    }
};
#endif

int power_save_cli_register(void)
{
#if CONFIG_IDF_TARGET_ESP32P4 && CONFIG_EXAMPLE_ENABLE_POWER_SAVE
    int cmds_num = sizeof(sleep_cmd) / sizeof(esp_console_cmd_t);
    int i;
    for (i = 0; i < cmds_num; i++) {
        ESP_LOGI(TAG, "Registering command: %s", sleep_cmd[i].command);
        esp_console_cmd_register(&sleep_cmd[i]);
    }
    return 0;
#else
    /* Power save CLI not enabled or not supported for this target */
    return 0;
#endif
}
