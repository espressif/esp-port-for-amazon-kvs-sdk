/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Co-processor (C6) reboot/hang recovery for split mode. On p4x the reset line
 * is one-way (P4 resets C6, not vice-versa), so a C6 self-reboot (factory reset,
 * OTA, crash) leaves the P4's ESP-Hosted SDIO link orphaned and the P4 deaf to
 * the C6 forever. We detect that via ESP-Hosted events and, by default,
 * re-establish the transport instead of rebooting the P4.
 *
 * Modeled on the esp_hosted host_hosted_events example.
 */

#include <inttypes.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_event.h"
#include "esp_hosted.h"
#include "esp_hosted_misc.h"

#include "hosted_recovery.h"

#define TAG "hosted_recovery"

#define HEARTBEAT_INTERVAL_SEC   CONFIG_STREAMING_HEARTBEAT_INTERVAL_SEC
#if CONFIG_STREAMING_ENABLE_HEARTBEAT_MONITOR
/* A timeout at or below the interval expires before the next heartbeat can re-arm
 * it, so the monitor trips a full transport teardown + Wi-Fi re-join on a healthy
 * C6 and the device cycles recovery forever. Catch it here rather than in the field. */
#if CONFIG_STREAMING_HEARTBEAT_TIMEOUT_SEC <= CONFIG_STREAMING_HEARTBEAT_INTERVAL_SEC
#error "STREAMING_HEARTBEAT_TIMEOUT_SEC must be greater than STREAMING_HEARTBEAT_INTERVAL_SEC"
#endif
#define HEARTBEAT_TIMEOUT_USEC   ((int64_t) CONFIG_STREAMING_HEARTBEAT_TIMEOUT_SEC * 1000000)
#endif

#define RESET_BIT  BIT0
#define ARRAY_SIZE_OF(a)  (sizeof(a) / sizeof((a)[0]))

static EventGroupHandle_t s_events;
static esp_timer_handle_t s_hb_timer;
static volatile bool s_recovering;
static bool s_first_init_event;
static bool s_first_heartbeat;
static uint32_t s_prev_heartbeat;

static hosted_recovery_teardown_cb_t s_teardown;
static hosted_recovery_restore_cb_t  s_restore;

static void request_reset(void)
{
    if (!s_recovering) {
        xEventGroupSetBits(s_events, RESET_BIT);
    }
}

#if CONFIG_STREAMING_ENABLE_HEARTBEAT_MONITOR
static void hb_timeout_cb(void *arg)
{
    ESP_LOGW(TAG, "co-processor heartbeat timeout - assuming C6 hung");
    request_reset();
}

/* Created once in hosted_recovery_start(); armed from both the recovery task and
 * the event-loop task, so it must not be created lazily from either. */
static esp_err_t hb_timer_create(void)
{
    const esp_timer_create_args_t args = {
        .callback = hb_timeout_cb, .dispatch_method = ESP_TIMER_TASK,
        .name = "cp_hb", .skip_unhandled_events = true,
    };
    esp_err_t err = esp_timer_create(&args, &s_hb_timer);
    if (err != ESP_OK) {
        s_hb_timer = NULL;
    }
    return err;
}

static void hb_timer_arm(void)
{
    if (!s_hb_timer) {
        return;
    }
    if (esp_timer_is_active(s_hb_timer)) {
        esp_timer_restart(s_hb_timer, HEARTBEAT_TIMEOUT_USEC);
    } else {
        esp_timer_start_once(s_hb_timer, HEARTBEAT_TIMEOUT_USEC);
    }
}
#endif

static void hosted_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base != ESP_HOSTED_EVENT) {
        return;
    }
    switch (id) {
    case ESP_HOSTED_EVENT_CP_INIT:
        /* First INIT is the expected boot; a later one means the C6 rebooted. */
        if (!s_first_init_event) {
            s_first_init_event = true;
        } else {
            ESP_LOGW(TAG, "unexpected C6 INIT event - C6 rebooted");
            request_reset();
        }
        break;
    case ESP_HOSTED_EVENT_TRANSPORT_FAILURE:
        ESP_LOGW(TAG, "ESP-Hosted transport failure");
        request_reset();
        break;
    case ESP_HOSTED_EVENT_CP_HEARTBEAT: {
        uint32_t hb = ((esp_hosted_event_heartbeat_t *) data)->heartbeat;
#if CONFIG_STREAMING_ENABLE_HEARTBEAT_MONITOR
        hb_timer_arm();
#endif
        if (s_first_heartbeat && hb != s_prev_heartbeat + 1) {
            ESP_LOGW(TAG, "heartbeat gap: expected %" PRIu32 " got %" PRIu32, s_prev_heartbeat + 1, hb);
        }
        s_first_heartbeat = true;
        s_prev_heartbeat = hb;
        break;
    }
    default:
        break;
    }
}

static esp_err_t enable_heartbeat(void)
{
    esp_err_t err = esp_hosted_configure_heartbeat(true, HEARTBEAT_INTERVAL_SEC);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to enable co-processor heartbeat: %s", esp_err_to_name(err));
    }
#if CONFIG_STREAMING_ENABLE_HEARTBEAT_MONITOR
    hb_timer_arm();
#endif
    return err;
}

#if CONFIG_STREAMING_DO_HOSTED_RECOVERY
#define CHK_STEP(call)                                                        \
    do {                                                                      \
        esp_err_t _e = (call);                                                 \
        if (_e != ESP_OK) {                                                    \
            ESP_LOGE(TAG, "%s failed: %s", #call, esp_err_to_name(_e));         \
            return _e;                                                         \
        }                                                                      \
    } while (0)

static esp_err_t recover_transport(void)
{
    ESP_LOGW(TAG, "re-establishing ESP-Hosted transport");
    if (s_teardown) {
        CHK_STEP(s_teardown());
    }
    /* deinit is not tied to a return code upstream, but init/connect are: a dead
     * C6 fails here, and that has to reach the caller so the cycle is retried. */
    esp_hosted_deinit();
    CHK_STEP(esp_hosted_init());
    CHK_STEP(esp_hosted_connect_to_slave());

    /* Heartbeat last: arming the timeout monitor before a restore() that can wait
     * ~21s for an IP would fire it mid-recovery, where the trigger is suppressed
     * and lost — which is how a single failed cycle used to kill the link. */
    if (s_restore) {
        CHK_STEP(s_restore());
    }
    /* Detector, not part of the restore: a slave without heartbeat support must not
     * make an otherwise-successful cycle fail and re-tear a healthy transport. The
     * CP_INIT and TRANSPORT_FAILURE triggers still work without it. */
    (void) enable_heartbeat();

    ESP_LOGI(TAG, "ESP-Hosted transport re-established");
    return ESP_OK;
}
#endif /* CONFIG_STREAMING_DO_HOSTED_RECOVERY */

/* Retry backoff for a failed cycle, in seconds. Capped, not unbounded: a C6 that
 * needs a long time (its own OTA, factory reset) should still be picked up. */
static const uint32_t RETRY_BACKOFF_SEC[] = { 2, 5, 10, 20, 30 };

static void recovery_task(void *arg)
{
    uint32_t failures = 0;

    enable_heartbeat();
    while (true) {
        xEventGroupWaitBits(s_events, RESET_BIT, pdTRUE, pdTRUE, portMAX_DELAY);
        s_recovering = true;

        /* Re-arm detection *before* the attempt: our own esp_hosted_init() raises a
         * CP_INIT, which has to consume the "first INIT" slot. Clearing this after
         * the attempt instead left the flag false, so the next genuine C6 reboot
         * looked like a first boot and went unrecovered.
         *
         * Only for the recovery choice, which is the only one that calls
         * esp_hosted_init(). Under DO_NOTHING no CP_INIT follows to consume the slot,
         * so clearing it here would swallow every other genuine C6 reboot -- exactly
         * the reporting that mode exists for. DO_HOST_RESET never returns from
         * esp_restart(), so it does not care either way. */
#if CONFIG_STREAMING_DO_HOSTED_RECOVERY
        s_first_init_event = false;
        s_first_heartbeat = false;
        s_prev_heartbeat = 0;
#endif

        esp_err_t err = ESP_OK;
#if CONFIG_STREAMING_DO_HOSTED_RECOVERY
        err = recover_transport();
#elif CONFIG_STREAMING_DO_HOST_RESET
        ESP_LOGW(TAG, "restarting host to recover from C6 error");
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_restart();
#else /* CONFIG_STREAMING_DO_NOTHING */
        ESP_LOGW(TAG, "C6 error detected; recovery disabled (doing nothing)");
#endif

        /* Clear before releasing the suppression, so a trigger raised from here on
         * survives into the next wait instead of being wiped at the end of the cycle. */
        xEventGroupClearBits(s_events, RESET_BIT);
        s_recovering = false;

        if (err == ESP_OK) {
            failures = 0;
            continue;
        }

        /* The cycle failed, so nothing re-armed detection — without an explicit
         * retry the link stays down for good with recovery nominally enabled. */
        uint32_t idx = failures < ARRAY_SIZE_OF(RETRY_BACKOFF_SEC) ? failures
                                                                  : ARRAY_SIZE_OF(RETRY_BACKOFF_SEC) - 1;
        uint32_t delay_sec = RETRY_BACKOFF_SEC[idx];
        failures++;
        ESP_LOGE(TAG, "recovery attempt %" PRIu32 " failed (%s) - retrying in %" PRIu32 "s",
                 failures, esp_err_to_name(err), delay_sec);
        vTaskDelay(pdMS_TO_TICKS(delay_sec * 1000));
        xEventGroupSetBits(s_events, RESET_BIT);
    }
}

esp_err_t hosted_recovery_start(hosted_recovery_teardown_cb_t teardown,
                                hosted_recovery_restore_cb_t restore)
{
    s_teardown = teardown;
    s_restore = restore;
    s_first_init_event = true; /* the boot INIT already happened before we start */

    s_events = xEventGroupCreate();
    if (!s_events) {
        return ESP_ERR_NO_MEM;
    }
#if CONFIG_STREAMING_ENABLE_HEARTBEAT_MONITOR
    esp_err_t timer_err = hb_timer_create();
    if (timer_err != ESP_OK) {
        ESP_LOGE(TAG, "failed to create heartbeat timer: %s", esp_err_to_name(timer_err));
        return timer_err;
    }
#endif
    esp_err_t err = esp_event_handler_instance_register(ESP_HOSTED_EVENT, ESP_EVENT_ANY_ID,
                                                        hosted_event_handler, NULL, NULL);
    if (err != ESP_OK) {
        return err;
    }
    if (xTaskCreate(recovery_task, "cp_recovery", 4096, NULL, 5, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "co-processor recovery started (heartbeat %ds)", HEARTBEAT_INTERVAL_SEC);
    return ESP_OK;
}
