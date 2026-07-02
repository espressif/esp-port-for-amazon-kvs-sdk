/*
* SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
*
* SPDX-License-Identifier: Apache-2.0
*/

#include <stdio.h>
#include <esp_err.h>
#include <esp_console.h>
#include <esp_log.h>
#include <slave_light_sleep.h>
#include "app_webrtc.h"

#define SLAVE_HOOKS_FOR_HOST_DEEP_SLEEP (1)
#define SLAVE_LIGHT_SLEEP_ON_HOST_DEEP_SLEEP (1)

#define SUSPEND_NON_CRITICAL_TASKS_WHILE_LIGHT_SLEEPING (0)

#if SUSPEND_NON_CRITICAL_TASKS_WHILE_LIGHT_SLEEPING && !SLAVE_LIGHT_SLEEP_ON_HOST_DEEP_SLEEP
#error "Incompatible config"
#endif

#if SUSPEND_NON_CRITICAL_TASKS_WHILE_LIGHT_SLEEPING
#ifndef CONFIG_FREERTOS_USE_TRACE_FACILITY
    #error "CONFIG_FREERTOS_USE_TRACE_FACILITY is mandatory for SUSPEND_NON_CRITICAL_TASKS_WHILE_LIGHT_SLEEPING to work"
#endif
#endif

static const char *TAG = "power_save";

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#if SLAVE_LIGHT_SLEEP_ON_HOST_DEEP_SLEEP


/* Part 1: Wi-Fi Modem sleep enforcement */
/* Part 2: Mandate non critical tasks to suspend */

/* Part 1 : Wi-Fi Modem sleep */
#include "esp_wifi.h"
#include "esp_pm.h"

static esp_err_t configure_wifi_power_save(void)
{
    esp_err_t ret;

    /* Enable WiFi modem sleep */
    /* WIFI_PS_MIN_MODEM: Minimum modem power saving (better performance) */
    /* WIFI_PS_MAX_MODEM: Maximum modem power saving (better power savings) */
    ret = esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set WiFi power save mode: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "WiFi modem sleep enabled");

    /* Optional: Configure listen interval for better power savings */
    wifi_config_t wifi_config = {};
    ret = esp_wifi_get_config(WIFI_IF_STA, &wifi_config);
    if (ret == ESP_OK) {
        wifi_config.sta.listen_interval = 3; /* Listen every 3 beacons (default is 3) */
        ret = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "WiFi listen interval configured");
        }
    }

    return ret;
}


/* Part 2: Mandate non critical tasks to suspend */

#if SUSPEND_NON_CRITICAL_TASKS_WHILE_LIGHT_SLEEPING

#define MAX_SUSPENDED_TASKS 25

static TaskHandle_t suspended_tasks[MAX_SUSPENDED_TASKS];
static UBaseType_t suspended_task_count = 0;
static portMUX_TYPE suspend_mux = portMUX_INITIALIZER_UNLOCKED;

static bool is_critical_task(const char *task_name)
{
    /* List of tasks that must NEVER be suspended */
    const char *critical_tasks[] = {

        /* FreeRTOS core */
        "IDLE", "IDLE0", "IDLE1",
        "Tmr Svc", "esp_timer",
        "ipc0", "ipc1",

        /* Network */
        "tiT", "sys_evt", "eventTask",

        /* Wi-Fi */
        // "wifi",

        /* ESP-Hosted infrastructure - NEVER suspend these! */
        "sdio_rx_task",		 /* ESP-Hosted manages deinit */
        "sdio_tx_done_ta",	 /* ESP-Hosted manages deinit */
        "host_reset_task",	 /* Needed for wake */
        "ps_alert_task",    /* Gets killed automatically */

        /* Main task */
        "main",

        /* WebRTC */
        "webrtc_run",
        "esp_workq_task",
        // "flash_op_task",
        "console_repl",
        "websocket_task",
        // "pserial_task",
        // "recv_task"
    };

    /* Check if current task (can't suspend self) */
    TaskHandle_t current = xTaskGetCurrentTaskHandle();
    TaskHandle_t named = xTaskGetHandle(task_name);
    if (current == named) {
        return true;
    }

    /* Check against critical task list */
    for (int i = 0; i < sizeof(critical_tasks) / sizeof(critical_tasks[0]); i++) {
        if (strcmp(task_name, critical_tasks[i]) == 0) {
            return true;
        }
    }

    return false;
}

static void suspend_application_tasks(void)
{
    UBaseType_t task_count = uxTaskGetNumberOfTasks();
    ESP_EARLY_LOGI(TAG, "Total tasks in system: %d", task_count);

    /* Allocate array for task info */
    TaskStatus_t *task_array = pvPortMalloc(task_count * sizeof(TaskStatus_t));
    if (task_array == NULL) {
        ESP_EARLY_LOGE(TAG, "Failed to allocate task array");
        return;
    }

    /* Allocate temporary array to store tasks to suspend */
    TaskHandle_t *to_suspend = pvPortMalloc(MAX_SUSPENDED_TASKS * sizeof(TaskHandle_t));
    if (to_suspend == NULL) {
        ESP_EARLY_LOGE(TAG, "Failed to allocate suspension array");
        vPortFree(task_array);
        return;
    }

    /* Enter critical section to prevent task deletion during enumeration */
    taskENTER_CRITICAL(&suspend_mux);

    /* Get all task information
    * Pass NULL for runtime stats - works with just CONFIG_FREERTOS_USE_TRACE_FACILITY
    * No need for CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
    */
    UBaseType_t actual_count = uxTaskGetSystemState(task_array, task_count, NULL);

    /* Build list of tasks to suspend while in critical section */
    UBaseType_t to_suspend_count = 0;

    for (UBaseType_t i = 0; i < actual_count && to_suspend_count < MAX_SUSPENDED_TASKS; i++) {
        const char *task_name = task_array[i].pcTaskName;
        TaskHandle_t task_handle = task_array[i].xHandle;

        /* Skip deleted tasks */
        if (task_array[i].eCurrentState == eDeleted) {
            continue;
        }

        /* Skip critical tasks */
        if (is_critical_task(task_name)) {
            ESP_EARLY_LOGD(TAG, "Skipping critical task: %s", task_name);
            continue;
        }

        /* Skip if already suspended */
        if (task_array[i].eCurrentState == eSuspended) {
            ESP_EARLY_LOGD(TAG, "Task already suspended: %s", task_name);
            continue;
        }

        /* Add to suspension list */
        to_suspend[to_suspend_count++] = task_handle;
        ESP_EARLY_LOGD(TAG, "Queuing task for suspension: %s (priority: %d, state: %d)",
                    task_name,
                    task_array[i].uxCurrentPriority,
                    task_array[i].eCurrentState);
    }

    taskEXIT_CRITICAL(&suspend_mux);

    /* Now suspend tasks outside critical section */
    suspended_task_count = 0;

    for (UBaseType_t i = 0; i < to_suspend_count; i++) {
        /* Double-check task still exists before suspending */
        eTaskState state = eTaskGetState(to_suspend[i]);
        if (state != eDeleted && state != eInvalid) {
            const char *name = pcTaskGetName(to_suspend[i]);
            ESP_EARLY_LOGI(TAG, "Suspending task: %s", name ? name : "unknown");
            vTaskSuspend(to_suspend[i]);
            suspended_tasks[suspended_task_count++] = to_suspend[i];
        } else {
            ESP_EARLY_LOGW(TAG, "Task was deleted before suspension");
        }
    }

    /* Free allocated memory */
    vPortFree(to_suspend);
    vPortFree(task_array);

    ESP_EARLY_LOGI(TAG, "Suspended %d application tasks", suspended_task_count);
}

static void resume_application_tasks(void)
{
    ESP_EARLY_LOGI(TAG, "Resuming %d suspended tasks", suspended_task_count);

    for (UBaseType_t i = 0; i < suspended_task_count; i++) {
        if (suspended_tasks[i] != NULL) {
            /* Verify task wasn't deleted while suspended */
            eTaskState state = eTaskGetState(suspended_tasks[i]);
            if (state != eDeleted && state != eInvalid) {
                const char *name = pcTaskGetName(suspended_tasks[i]);
                ESP_EARLY_LOGI(TAG, "Resuming task: %s", name ? name : "unknown");
                vTaskResume(suspended_tasks[i]);
            } else {
                ESP_EARLY_LOGW(TAG, "Task was deleted while suspended, skipping resume");
            }
            /* Clear the handle */
            suspended_tasks[i] = NULL;
        }
    }

    suspended_task_count = 0;
    ESP_EARLY_LOGI(TAG, "All application tasks resumed");
}

#endif /* SUSPEND_NON_CRITICAL_TASKS_WHILE_LIGHT_SLEEPING */

#endif /* SLAVE_LIGHT_SLEEP_ON_HOST_DEEP_SLEEP */




#if !defined(CONFIG_ESP_HOSTED_LIGHT_SLEEP_ENABLE) && (ALLOW_SLAVE_LIGHT_SLEEP_ON_HOST_DEEP_SLEEP)
#error "Incompatible config, check slave config"
#endif

#if CONFIG_IDF_TARGET_ESP32C6 || CONFIG_IDF_TARGET_ESP32C5
    #define IS_VALID_SLAVE_CHIPSET_FOR_SLEEP_INTEGRATION 1
#else
    #define IS_VALID_SLAVE_CHIPSET_FOR_SLEEP_INTEGRATION 0
#endif

#if IS_VALID_SLAVE_CHIPSET_FOR_SLEEP_INTEGRATION
#include "host_power_save.h"
#include "slave_light_sleep.h"
#include "esp_hosted_cli.h"
#endif

#if SLAVE_LIGHT_SLEEP_ON_HOST_DEEP_SLEEP

/* Callback implementations for automatic light sleep on host power save events */

static void host_power_save_on_prepare_cb(void)
{
    ESP_EARLY_LOGI(TAG, "==> Host preparing to enter power save");
    /* User can add custom pre-sleep cleanup here:
    * - Save application state
    * - Flush buffers
    * - Stop non-essential tasks
    * - etc.
    */
    esp_err_t err = configure_wifi_power_save();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "configure_wifi_power_save failed: %s", esp_err_to_name(err));
    }

#if SUSPEND_NON_CRITICAL_TASKS_WHILE_LIGHT_SLEEPING
    // Get wifi task handle by name and suspend it
    TaskHandle_t wifi_task = xTaskGetHandle("wifi");
    if (wifi_task != NULL) {
        ESP_EARLY_LOGI(TAG, "Suspending wifi task");
        vTaskSuspend(wifi_task);
    }

    /* Suspend all non-critical tasks */
    suspend_application_tasks();

    /* Give tasks time to finish current operations */
    vTaskDelay(pdMS_TO_TICKS(200));
#endif /* SUSPEND_NON_CRITICAL_TASKS_WHILE_LIGHT_SLEEPING */
}

static void IRAM_ATTR host_power_save_on_ready_cb(void)
{
    ESP_EARLY_LOGI(TAG, "==> Host power save active - entering light sleep");

#ifdef CONFIG_ESP_HOSTED_LIGHT_SLEEP_ENABLE

    /* Handle CLI based on peripheral powerdown configuration */
#if defined(CONFIG_PM_POWER_DOWN_PERIPHERAL_IN_LIGHT_SLEEP) && defined(CONFIG_ESP_HOSTED_LIGHT_SLEEP_PERIPHERAL_POWERDOWN)
    /* Peripheral powerdown enabled - UART will die, stop CLI */
    ESP_EARLY_LOGI(TAG, "Stopping CLI (UART powering down)");
    esp_hosted_cli_stop();
#else
    /* Peripheral stays powered - CLI can remain active */
    ESP_EARLY_LOGI(TAG, "CLI remains active (UART stays powered)");
#endif

    esp_err_t ret = slave_light_sleep_start();
    if (ret == ESP_OK) {
        ESP_EARLY_LOGI(TAG, "Light sleep started successfully");
    } else {
        ESP_EARLY_LOGW(TAG, "Failed to start light sleep: %s", esp_err_to_name(ret));
    }
#else
    ESP_EARLY_LOGW(TAG, "Light sleep not enabled in menuconfig");
#endif
    // Resume wifi task
    // TaskHandle_t wifi_task = xTaskGetHandle("wifi");
    // if (wifi_task != NULL) {
    //     ESP_EARLY_LOGI(TAG, "Resuming wifi task");
    //     vTaskResume(wifi_task);
    // }
}

static void IRAM_ATTR host_power_save_off_prepare_cb(void)
{
#ifdef CONFIG_ESP_HOSTED_LIGHT_SLEEP_ENABLE
    esp_err_t ret = slave_light_sleep_stop();
    if (ret == ESP_OK) {
        /* ESP_EARLY_LOGI(TAG, "Light sleep stopped successfully"); */
    } else {
        /* ESP_EARLY_LOGW(TAG, "Failed to stop light sleep: %s", esp_err_to_name(ret)); */
    }

#endif
}

static void IRAM_ATTR host_power_save_off_ready_cb(void)
{
    ESP_EARLY_LOGI(TAG, "==> Host power save off - device fully ready");

#if SUSPEND_NON_CRITICAL_TASKS_WHILE_LIGHT_SLEEPING
    /* Resume all suspended tasks */
    resume_application_tasks();
#endif

    /* Restart CLI if it was stopped due to peripheral powerdown */
    /* This happens here (after ready) to ensure UART is fully powered up */
#if defined(CONFIG_PM_POWER_DOWN_PERIPHERAL_IN_LIGHT_SLEEP) && defined(CONFIG_ESP_HOSTED_LIGHT_SLEEP_PERIPHERAL_POWERDOWN)
    ESP_EARLY_LOGI(TAG, "Restarting CLI (UART now fully powered up)");
    esp_hosted_cli_start();
#endif

    /* User can add custom post-wake initialization here:
    * - Restore application state
    * - Resume tasks
    * - Re-initialize peripherals if needed
    * - etc.
    */
}
#endif /*ALLOW_SLAVE_LIGHT_SLEEP_ON_HOST_DEEP_SLEEP*/

#if IS_VALID_SLAVE_CHIPSET_FOR_SLEEP_INTEGRATION
static void app_webrtc_event_handler(app_webrtc_event_data_t *event_data, void *user_ctx)
{
    if (event_data == NULL) {
        return;
    }

    switch (event_data->event_id) {
        case APP_WEBRTC_EVENT_RECEIVED_OFFER:
            ESP_LOGI(TAG, "Received offer, waking up host");
            wakeup_host(portMAX_DELAY);
            break;
        case APP_WEBRTC_EVENT_STREAMING_STARTED:
            ESP_LOGI(TAG, "Streaming started, waking up host");
            wakeup_host(portMAX_DELAY);
            break;
        default:
            break;
    }
}
#endif /* IS_VALID_SLAVE_CHIPSET_FOR_SLEEP_INTEGRATION */

esp_err_t power_save_init(void)
{
    esp_err_t ret = ESP_OK;
#if SLAVE_HOOKS_FOR_HOST_DEEP_SLEEP && IS_VALID_SLAVE_CHIPSET_FOR_SLEEP_INTEGRATION
    ESP_LOGI(TAG, "Initializing power save API");

#if SLAVE_LIGHT_SLEEP_ON_HOST_DEEP_SLEEP
    /*
    * STEP 1: Initialize host power save infrastructure
    * This enables monitoring of host power save events.
    * Without this, callbacks won't be invoked.
    */
    host_power_save_config_t ps_config = HOST_POWER_SAVE_DEFAULT_CONFIG();
    host_power_save_callbacks_t callbacks = {
        .host_power_save_on_prepare_cb = host_power_save_on_prepare_cb,
        .host_power_save_on_ready_cb = host_power_save_on_ready_cb,
        .host_power_save_off_prepare_cb = host_power_save_off_prepare_cb,
        .host_power_save_off_ready_cb = host_power_save_off_ready_cb
    };
    ps_config.callbacks = callbacks;
    ret = host_power_save_init(&ps_config);
    if (ret) {
        ESP_LOGE(TAG, "Host power save init failed: %u", ret);
    }

#else /* SLAVE_LIGHT_SLEEP_ON_HOST_DEEP_SLEEP */
    ret = host_power_save_init(NULL);
    if (ret) {
        ESP_LOGE(TAG, "Host power save init failed: %u", ret);
    }
#endif /*SLAVE_LIGHT_SLEEP_ON_HOST_DEEP_SLEEP*/


#if SLAVE_LIGHT_SLEEP_ON_HOST_DEEP_SLEEP
    /*
    * STEP 2: Initialize slave light sleep (optional)
    * This component can be used independently of host power save.
    * Comment this out if you only want host event monitoring.
    */
#ifdef CONFIG_ESP_HOSTED_LIGHT_SLEEP_ENABLE
    ESP_LOGI(TAG, "Step 2: Initializing slave light sleep component");
    ret = slave_light_sleep_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "  ✗ Light sleep init failed: %s", esp_err_to_name(ret));
        ESP_LOGE(TAG, "  Check menuconfig: PM_ENABLE and FREERTOS_USE_TICKLESS_IDLE");
        ESP_LOGE(TAG, "  Continuing without light sleep...");
    } else {
        ESP_LOGI(TAG, "  ✓ Light sleep component ready");
    }
#else
    ESP_LOGI(TAG, "Step 2: Light sleep component not enabled");
    ESP_LOGI(TAG, "  ⓘ Enable in: Light Sleep Power Management menu");
    ESP_LOGI(TAG, "  ⓘ Host power save callbacks will still work");
#endif
#endif /* SLAVE_LIGHT_SLEEP_ON_HOST_DEEP_SLEEP */

#if IS_VALID_SLAVE_CHIPSET_FOR_SLEEP_INTEGRATION
    /* Register WebRTC event handler to wake host on streaming start */
    if (app_webrtc_register_event_callback(app_webrtc_event_handler, NULL) != 0) {
        ESP_LOGE(TAG, "Failed to register WebRTC event callback for power save");
    } else {
        ESP_LOGI(TAG, "Registered WebRTC event callback for host wakeup");
    }
#endif /* IS_VALID_SLAVE_CHIPSET_FOR_SLEEP_INTEGRATION */

#else /* SLAVE_HOOKS_FOR_HOST_DEEP_SLEEP && IS_VALID_SLAVE_CHIPSET_FOR_SLEEP_INTEGRATION */
    /* esp_hosted's host_power_save API is C5/C6-only (it's the slave-side
     * mechanism that lets the co-processor sleep until the host wakes it
     * over SDIO). On other targets there's no host to coordinate with —
     * skip silently. */
    ESP_LOGI(TAG, "Slave-side host power save hooks not applicable on this target");
#endif

    return ret;
}

/* CLI command handler for waking up host */
static int wakeup_cli_handler(int argc, char *argv[])
{
#if IS_VALID_SLAVE_CHIPSET_FOR_SLEEP_INTEGRATION
    ESP_LOGI(TAG, "Waking up host...");
    wakeup_host_mandate(portMAX_DELAY);
#else
    ESP_LOGI(TAG, "Wake-up only supported on ESP32-C6/C5");
#endif
    return 0;
}

static esp_console_cmd_t power_save_cmds[] = {
    {
        .command = "wake-up",
        .help = "Wake up host from deep sleep",
        .func = wakeup_cli_handler,
    }
};

int power_save_register_cli(void)
{
    int cmds_num = sizeof(power_save_cmds) / sizeof(esp_console_cmd_t);
    for (int i = 0; i < cmds_num; i++) {
        ESP_LOGI(TAG, "Registering command: %s", power_save_cmds[i].command);
        esp_console_cmd_register(&power_save_cmds[i]);
    }
    return 0;
}
