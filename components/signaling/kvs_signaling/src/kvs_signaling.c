/*
 * SPDX-FileCopyrightText: 2025-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "kvs_signaling.h"
#include "../port/kvs_pending_ice.h"
#include "webrtc_mem_utils.h"
#include "app_webrtc.h"
#include "fileio.h"
#ifdef CONFIG_IOT_CORE_ENABLE_CREDENTIALS
#include "iot_credential_provider.h"
#endif

// For callback based credentials provider
#include "file_credential_provider.h"

#ifdef CONFIG_USE_ESP_WEBSOCKET_CLIENT
#include "Signaling_esp.h"
#endif

#include "esp_work_queue.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <com/amazonaws/kinesis/video/webrtcclient/Include.h>

// Include KVS signaling headers for refresh_ice_configuration and PSignalingClient
#include "Signaling.h"

static const char *TAG = "kvs_signaling";

/*
 * Progressive ICE Server Optimization
 * When enabled, returns STUN servers immediately and fetches TURN servers asynchronously
 * This can reduce connection startup time from 6+ seconds to near-instant for STUN-only connections
 */
#ifndef CONFIG_KVS_PROGRESSIVE_ICE_SERVERS
#define CONFIG_KVS_PROGRESSIVE_ICE_SERVERS 1    // Enable by default for better performance
#endif

// kvs fetch credentials context
typedef struct kvs_cred_fetch_ctx {
    kvs_fetch_credentials_cb_t cb;
    uint64_t ud;
} kvs_cred_fetch_ctx_t;

// Progressive ICE callback context
typedef struct {
    uint64_t customData;
    WEBRTC_STATUS (*on_ice_servers_updated)(uint64_t, uint32_t);
} kvs_ice_callback_ctx_t;

// Forward declarations for internal KVS functions (not exposed in public header)
STATUS createKvsSignalingClient(kvs_signaling_config_t *pConfig, PVOID *ppSignalingClient);
STATUS connectKvsSignalingClient(PVOID pSignalingClient);
STATUS disconnectKvsSignalingClient(PVOID pSignalingClient);
STATUS abortKvsSignalingClient(PVOID pSignalingClient);
STATUS sendKvsSignalingMessage(PVOID pSignalingClient, webrtc_message_t *pMessage);
STATUS kvsSignalingPurgePeerCandidates(PVOID pSignalingClient, const char *peerId);
STATUS freeKvsSignalingClient(PVOID pSignalingClient);
STATUS setKvsSignalingCallbacks(PVOID pSignalingClient,
                              PVOID customData,
                              STATUS (*on_msg_received)(UINT64, webrtc_message_t*),
                              STATUS (*on_state_changed)(UINT64, SIGNALING_CLIENT_STATE),
                              STATUS (*on_error)(UINT64, STATUS, PCHAR, UINT32));
STATUS setKvsSignalingRoleType(PVOID pSignalingClient, SIGNALING_CHANNEL_ROLE_TYPE role_type);
STATUS getKvsSignalingIceServers(PVOID pSignalingClient, PUINT32 pIceConfigCount, RtcIceServer iceServers[]);
STATUS kvsSignalingQueryServerGetByIdx(PVOID pSignalingClient, int index, bool useTurn, uint8_t **data, int *len, bool *have_more);
static WEBRTC_STATUS kvsIsIceRefreshNeededWrapper(void *pSignalingClient, bool *refreshNeeded);
static WEBRTC_STATUS kvsHasValidIceConfigWrapper(void *pSignalingClient, bool *pHasValidConfig);


#ifndef CONFIG_USE_ESP_WEBSOCKET_CLIENT
/**
 * @brief Custom realloc wrapper for libwebsockets to use SPIRAM-capable memory
 */
static void *realloc_wrapper(void *ptr, size_t size, const char *reason)
{
    (void) reason;
    return heap_caps_realloc_prefer(ptr, size, 2, MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM, MALLOC_CAP_DEFAULT);
}
extern void lws_set_allocator(void *(*realloc)(void *ptr, size_t size, const char *reason));
#endif

/**
 * @brief Adapter data structure for callback conversion
 */
 typedef struct {
    uint64_t originalCustomData;
    WEBRTC_STATUS (*originalOnSignalingStateChanged)(uint64_t, webrtc_signaling_state_t);
    WEBRTC_STATUS (*originalOnError)(uint64_t, WEBRTC_STATUS, char*, uint32_t);
} CallbackAdapterData;

/**
 * @brief Structure for KVS signaling client data
 */
typedef struct {
    SIGNALING_CLIENT_HANDLE signalingClientHandle;
    ChannelInfo channelInfo;
    SignalingClientInfo clientInfo;
    SignalingClientCallbacks signalingClientCallbacks;
    PAwsCredentialProvider pCredentialProvider;
    BOOL initialized;
    BOOL connected;
    // Set by a terminate on another task; connect gives up at its next step
    volatile ATOMIC_BOOL aborting;
    MUTEX signalingSendMessageLock;

    // Callback data
    PVOID customData;
    STATUS (*on_msg_received)(UINT64, webrtc_message_t*);
    STATUS (*on_state_changed)(UINT64, SIGNALING_CLIENT_STATE);
    STATUS (*on_error)(UINT64, STATUS, PCHAR, UINT32);

    // Adapter data for callback conversion (if needed)
    CallbackAdapterData *pCallbackAdapterData;

    // Configuration
    kvs_signaling_config_t config;

    // Metrics
    SignalingClientMetrics metrics;

    // Optional credential-fetch callback adapter context
    void *pCredFetchAdapterCtx;

    // Progressive ICE server callback context
    kvs_ice_callback_ctx_t ice_callback_ctx;

    // Last notified state to suppress duplicate state change callbacks
    SIGNALING_CLIENT_STATE lastNotifiedState;

    // SDP answers and ICE candidates whose send failed on connection loss,
    // re-sent after the client reconnects (protected by signalingSendMessageLock).
    // The slot arrays are allocated on the first buffered message and freed once
    // they drain empty, so the ~12 KB of ICE slots aren't resident for the
    // client's whole lifetime.
    KvsPendingSignaling pending;
    // Flushes currently sending a batch taken out of `pending` (under
    // signalingSendMessageLock). While one runs, new ICE candidates queue behind
    // it so they can't reach a viewer ahead of the answer being re-sent.
    UINT32 pendingFlushes;
    // Set when a flush found another one running and returned: the running flush
    // takes one more pass, so a CONNECTED edge during a failing flush isn't lost.
    // Every flush runs on the single work-queue worker today, so this is a guard
    // for a second caller, not a path that runs.
    BOOL flushAgain;
    // Peers whose answer is in the batch the running flush holds, copied when it
    // is taken: only their candidates and newer answers must queue behind it.
    char flushAnswerPeers[KVS_PENDING_ANSWER_MAX][KVS_PENDING_ICE_PEERID_LEN];
    UINT32 flushAnswerPeerCount;

    // ICE refresh task only: fetches that failed in a row, and when the timer may try again
    UINT32 iceFetchFailures;
    UINT64 iceFetchRetryTime;
    // A session asked for a refresh. Kept until the client has a valid config or a fetch ran.
    volatile BOOL iceRefreshRequested;
    // A fetch for this client is running on the ICE refresh task
    volatile BOOL iceFetchActive;
} KvsSignalingClientData;

/* Live-client guard: work-queue tasks capture a raw KvsSignalingClientData*.
 * freeKvsSignalingClient() unregisters before teardown, so a task starting after
 * that bails instead of touching freed memory (in-flight tasks are drained via
 * esp_work_queue_sync). The registry mutex is static so it outlives any client. */
#define KVS_MAX_LIVE_CLIENTS 4
static void *s_liveClients[KVS_MAX_LIVE_CLIENTS];
static MUTEX s_liveClientsLock = INVALID_MUTEX_VALUE;

static void kvsSignalingClientRegister(void *pClient)
{
    if (!IS_VALID_MUTEX_VALUE(s_liveClientsLock)) {
        s_liveClientsLock = MUTEX_CREATE(FALSE);
        if (!IS_VALID_MUTEX_VALUE(s_liveClientsLock)) {
            return;
        }
    }
    MUTEX_LOCK(s_liveClientsLock);
    for (UINT32 i = 0; i < KVS_MAX_LIVE_CLIENTS; i++) {
        if (s_liveClients[i] == NULL) {
            s_liveClients[i] = pClient;
            break;
        }
    }
    MUTEX_UNLOCK(s_liveClientsLock);
}

static void kvsSignalingClientUnregister(void *pClient)
{
    if (!IS_VALID_MUTEX_VALUE(s_liveClientsLock)) {
        return;
    }
    MUTEX_LOCK(s_liveClientsLock);
    for (UINT32 i = 0; i < KVS_MAX_LIVE_CLIENTS; i++) {
        if (s_liveClients[i] == pClient) {
            s_liveClients[i] = NULL;
            break;
        }
    }
    MUTEX_UNLOCK(s_liveClientsLock);
}

/* TRUE while the client is still registered (not being torn down). */
static bool kvsSignalingClientIsLive(void *pClient)
{
    bool live = false;
    if (!IS_VALID_MUTEX_VALUE(s_liveClientsLock)) {
        return false;
    }
    MUTEX_LOCK(s_liveClientsLock);
    for (UINT32 i = 0; i < KVS_MAX_LIVE_CLIENTS; i++) {
        if (s_liveClients[i] == pClient) {
            live = true;
            break;
        }
    }
    MUTEX_UNLOCK(s_liveClientsLock);
    return live;
}

/**
 * @brief Work queue task for applying cached ICE servers asynchronously.
 *
 * When ICE config is "up to date" (not expired), we still need to notify
 * the callback so cached TURN servers are applied to new peer connections.
 * This must be async because the callback chain (on_ice_servers_updated →
 * app_webrtc_update_ice_servers → get_ice_servers) can synchronously drive
 * the signaling state machine, causing a 22s+ block on HTTP calls.
 */
static void kvs_apply_cached_ice_task(void *arg)
{
    if (arg == NULL || !kvsSignalingClientIsLive(arg)) {
        return;
    }

    KvsSignalingClientData *pClientData = (KvsSignalingClientData *)arg;

    if (pClientData->ice_callback_ctx.on_ice_servers_updated != NULL) {
        UINT32 iceConfigCount = 0;
#ifdef CONFIG_USE_ESP_WEBSOCKET_CLIENT
        STATUS iceCountStatus = signaling_copy_ice_config((PSignalingClient) pClientData->signalingClientHandle, 0,
                                                          NULL, &iceConfigCount);
#else
        STATUS iceCountStatus = signalingClientGetIceConfigInfoCount(
            pClientData->signalingClientHandle, &iceConfigCount);
#endif
        if (STATUS_SUCCEEDED(iceCountStatus) && iceConfigCount > 0) {
            DLOGI("Applying %" PRIu32 " cached ICE servers to peer connection (async)", iceConfigCount);
            pClientData->ice_callback_ctx.on_ice_servers_updated(
                pClientData->ice_callback_ctx.customData, iceConfigCount);
        }
    }
}

/* Queue passing the cached servers to the peer connection. Session setup reads the same
 * list on the work queue. */
static void kvs_queue_ice_apply(KvsSignalingClientData *pClientData)
{
    esp_err_t result = esp_work_queue_add_task(&kvs_apply_cached_ice_task, (void *) pClientData);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Failed to queue cached ICE apply task: %d", (int) result);
    }
}

#ifdef CONFIG_USE_ESP_WEBSOCKET_CLIENT
/*
 * ICE config refresh task.
 *
 * A refresh is one GetIceServerConfig call over HTTPS, a few seconds. It runs on its own
 * task because inbound signaling messages (offers, ICE candidates) go through the work
 * queue, and would wait behind it. The task makes the call directly, without the signaling
 * state machine. It holds the client's stateLock for the call, so a connect or reconnect
 * cannot change the endpoint or the credentials under it.
 *
 * The task runs below the SDK threads, so the TLS handshake does not delay the ICE checks
 * and DTLS of a session that is starting.
 *
 * A session asks for a refresh through iceRefreshRequested. With
 * CONFIG_KVS_ICE_CONFIG_PREFETCH the task also refreshes on its own once the cached config
 * is due, so a new session normally finds a valid config. Between refreshes the task
 * sleeps until the next one is due.
 */
#define KVS_ICE_REFRESH_TASK_STACK CONFIG_KVS_ICE_REFRESH_TASK_STACK_SIZE
#define KVS_ICE_REFRESH_TASK_PRIO  (CONFIG_PTHREAD_TASK_PRIO_DEFAULT - 1)
// After a failed fetch the timer waits 30 s, doubling per failure up to 10 min.
// A session's request waits only for 30 s after the last call.
#define KVS_ICE_FETCH_RETRY_MIN    (30 * HUNDREDS_OF_NANOS_IN_A_SECOND)
#define KVS_ICE_FETCH_RETRY_MAX    (10 * 60 * HUNDREDS_OF_NANOS_IN_A_SECOND)
// The state lock is busy, such as during a connect or its backoff: try again after this.
// A connect's CONNECTED edge also wakes the task.
#define KVS_ICE_REFRESH_BUSY_RETRY (10 * HUNDREDS_OF_NANOS_IN_A_SECOND)
// The longest the task sleeps, so a step of the wall clock cannot stall it for long
#define KVS_ICE_REFRESH_MAX_SLEEP  (10 * 60 * HUNDREDS_OF_NANOS_IN_A_SECOND)
// A fetch is one HTTPS call, plus one for the credentials when they expired. Each call has
// 15 s connect and read timeouts. The free waits this long for a running fetch.
#define KVS_ICE_REFRESH_DRAIN_MS   (65 * 1000)

static TaskHandle_t s_iceRefreshTask = NULL;
// Held while the task works on a client. Teardown takes it to wait for a running fetch.
static SemaphoreHandle_t s_iceRefreshRunLock = NULL;

/* Fetch the ICE config of one client. The caller holds the client's stateLock. */
static void kvs_refresh_ice_config(KvsSignalingClientData *pClientData)
{
    PSignalingClient pClient = (PSignalingClient) pClientData->signalingClientHandle;
    UINT64 retryDelay = KVS_ICE_FETCH_RETRY_MIN;

    // Active before the request clears, so a session querying in between sees a fetch in progress
    pClientData->iceFetchActive = TRUE;
    pClientData->iceRefreshRequested = FALSE;
    STATUS status = signaling_fetch_ice_config(pClient);
    BOOL valid = signaling_has_valid_ice_config(pClient);
    pClientData->iceFetchActive = FALSE;

    // A failed HTTP call returns success and leaves no valid config. Back off either way.
    if (STATUS_FAILED(status) || !valid) {
        pClientData->iceFetchFailures++;
        for (UINT32 i = 1; i < pClientData->iceFetchFailures && retryDelay < KVS_ICE_FETCH_RETRY_MAX; i++) {
            retryDelay *= 2;
        }
        retryDelay = MIN(retryDelay, KVS_ICE_FETCH_RETRY_MAX);
        pClientData->iceFetchRetryTime = GETTIME() + retryDelay;
        ESP_LOGW(TAG, "ICE config fetch failed (0x%08" PRIx32 ", %" PRIu32 " in a row); next timed refresh in %" PRIu64 " s",
                 status, pClientData->iceFetchFailures, retryDelay / HUNDREDS_OF_NANOS_IN_A_SECOND);
        return;
    }
    pClientData->iceFetchFailures = 0;
    pClientData->iceFetchRetryTime = 0;
    ESP_LOGI(TAG, "ICE config refreshed");
    kvs_queue_ice_apply(pClientData);
}

/* When the client is next due a fetch, or MAX_UINT64 when nothing is due. A request is due
 * 30 s after the last call. With prefetch, a valid config is due at its expiry and a missing
 * one after the backoff. */
static UINT64 kvs_ice_next_due(KvsSignalingClientData *pClientData, PSignalingClient pClient)
{
    if (pClientData->iceRefreshRequested) {
        return pClient->getIceConfigTime == 0 ? 0 : pClient->getIceConfigTime + KVS_ICE_FETCH_RETRY_MIN;
    }
#ifdef CONFIG_KVS_ICE_CONFIG_PREFETCH
    return signaling_has_valid_ice_config(pClient) ? pClient->iceConfigExpiration : pClientData->iceFetchRetryTime;
#else
    return MAX_UINT64;
#endif
}

/* Look at one client, and fetch its ICE config when it is due. Returns when it is next due. */
static UINT64 kvs_ice_refresh_client(KvsSignalingClientData *pClientData)
{
    SIGNALING_CLIENT_STATE state = SIGNALING_CLIENT_STATE_UNKNOWN;
    PSignalingClient pClient = NULL;
    UINT64 due = MAX_UINT64;

    xSemaphoreTake(s_iceRefreshRunLock, portMAX_DELAY);
    // Check that the client is live under the run lock. Teardown unregisters first, then takes the lock.
    if (!kvsSignalingClientIsLive(pClientData) ||
        pClientData->signalingClientHandle == INVALID_SIGNALING_CLIENT_HANDLE_VALUE) {
        goto Done;
    }
    pClient = (PSignalingClient) pClientData->signalingClientHandle;

    if (pClientData->iceRefreshRequested && signaling_has_valid_ice_config(pClient)) {
        // Fetched by another path, such as a reconnect. The session that asked still needs the servers.
        pClientData->iceRefreshRequested = FALSE;
        kvs_queue_ice_apply(pClientData);
    }
    due = kvs_ice_next_due(pClientData, pClient);
    if (due > GETTIME()) {
        goto Done;
    }
    if (!MUTEX_TRYLOCK(pClient->stateLock)) {
        due = GETTIME() + KVS_ICE_REFRESH_BUSY_RETRY;
        goto Done;
    }
    // Fetch in the steady states only. In the others a connect is in progress, and its
    // CONNECTED edge wakes the task.
    if (STATUS_SUCCEEDED(signalingClientGetCurrentState(pClientData->signalingClientHandle, &state)) &&
        (state == SIGNALING_CLIENT_STATE_CONNECTED || state == SIGNALING_CLIENT_STATE_JOIN_SESSION_CONNECTED)) {
        kvs_refresh_ice_config(pClientData);
        due = kvs_ice_next_due(pClientData, pClient);
    } else {
        due = MAX_UINT64;
    }
    MUTEX_UNLOCK(pClient->stateLock);

Done:
    xSemaphoreGive(s_iceRefreshRunLock);
    return due;
}

static void kvs_ice_refresh_task_fn(void *arg)
{
    void *clients[KVS_MAX_LIVE_CLIENTS];
    TickType_t wait = portMAX_DELAY;

    UNUSED_PARAM(arg);

    for (;;) {
        UINT64 next = MAX_UINT64;

        // Wake on a request from a session, on a CONNECTED edge, or when a client is due
        (void) ulTaskNotifyTake(pdTRUE, wait);

        MUTEX_LOCK(s_liveClientsLock);
        MEMCPY(clients, s_liveClients, SIZEOF(clients));
        MUTEX_UNLOCK(s_liveClientsLock);

        for (UINT32 i = 0; i < KVS_MAX_LIVE_CLIENTS; i++) {
            if (clients[i] != NULL) {
                next = MIN(next, kvs_ice_refresh_client((KvsSignalingClientData *) clients[i]));
            }
        }

        if (next == MAX_UINT64) {
            wait = portMAX_DELAY;
        } else {
            UINT64 now = GETTIME();
            UINT64 sleep = next > now ? MIN(next - now, KVS_ICE_REFRESH_MAX_SLEEP) : 0;
            // At least one tick, so a client that is still due cannot spin the task
            wait = MAX((TickType_t) 1, (TickType_t) pdMS_TO_TICKS(sleep / HUNDREDS_OF_NANOS_IN_A_MILLISECOND));
        }
    }
}

/* Wake the refresh task, if it runs */
static void kvs_ice_refresh_wake(void)
{
    if (s_iceRefreshTask != NULL) {
        xTaskNotifyGive(s_iceRefreshTask);
    }
}

/* Start the refresh task once. It serves every client and never exits. */
static STATUS kvs_ice_refresh_task_start(void)
{
    STATUS retStatus = STATUS_SUCCESS;
    StaticTask_t *pTaskBuffer = NULL;
    StackType_t *pTaskStack = NULL;

    CHK(s_iceRefreshTask == NULL, retStatus);

    if (s_iceRefreshRunLock == NULL) {
        s_iceRefreshRunLock = xSemaphoreCreateMutex();
        CHK(s_iceRefreshRunLock != NULL, STATUS_NOT_ENOUGH_MEMORY);
    }

    // The task never exits, so its buffers are never freed
    pTaskBuffer = heap_caps_calloc(1, sizeof(StaticTask_t), MALLOC_CAP_INTERNAL);
    pTaskStack = heap_caps_malloc_prefer(KVS_ICE_REFRESH_TASK_STACK, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                                         MALLOC_CAP_INTERNAL);
    if (pTaskBuffer != NULL && pTaskStack != NULL) {
        s_iceRefreshTask = xTaskCreateStatic(kvs_ice_refresh_task_fn, "kvs_ice_refresh", KVS_ICE_REFRESH_TASK_STACK,
                                             NULL, KVS_ICE_REFRESH_TASK_PRIO, pTaskStack, pTaskBuffer);
    }
    if (s_iceRefreshTask == NULL) {
        heap_caps_free(pTaskBuffer);
        heap_caps_free(pTaskStack);
        CHK(FALSE, STATUS_NOT_ENOUGH_MEMORY);
    }

CleanUp:
    return retStatus;
}
#endif

/**
 * @brief Convert SIGNALING_CLIENT_STATE to webrtc_signaling_state_t
 *
 * This conversion matches the logic from app_webrtc.c to properly handle
 * all intermediate KVS signaling states.
 */
static webrtc_signaling_state_t convertKvsToWebrtcSignalingState(SIGNALING_CLIENT_STATE kvsState)
{
    switch (kvsState) {
        case SIGNALING_CLIENT_STATE_NEW:
            return WEBRTC_SIGNALING_STATE_NEW;
        case SIGNALING_CLIENT_STATE_CONNECTING:
        case SIGNALING_CLIENT_STATE_GET_CREDENTIALS:
        case SIGNALING_CLIENT_STATE_DESCRIBE:
        case SIGNALING_CLIENT_STATE_CREATE:
        case SIGNALING_CLIENT_STATE_GET_ENDPOINT:
        case SIGNALING_CLIENT_STATE_GET_ICE_CONFIG:
        case SIGNALING_CLIENT_STATE_READY:
            return WEBRTC_SIGNALING_STATE_CONNECTING;
        case SIGNALING_CLIENT_STATE_CONNECTED:
        case SIGNALING_CLIENT_STATE_JOIN_SESSION_CONNECTED:
            return WEBRTC_SIGNALING_STATE_CONNECTED;
        case SIGNALING_CLIENT_STATE_DISCONNECTED:
        case SIGNALING_CLIENT_STATE_DELETE:
        case SIGNALING_CLIENT_STATE_DELETED:
            return WEBRTC_SIGNALING_STATE_DISCONNECTED;
        default:
            return WEBRTC_SIGNALING_STATE_FAILED;
    }
}

/**
 * @brief Convert STATUS to WEBRTC_STATUS
 */
static WEBRTC_STATUS convertStatusToWebrtcStatus(STATUS retStatus)
{
    return (retStatus == STATUS_SUCCESS) ? WEBRTC_STATUS_SUCCESS : WEBRTC_STATUS_INTERNAL_ERROR;
}

/**
 * @brief Adapter function for state change callback
 */
static STATUS adapterStateChangedCallback(UINT64 customData, SIGNALING_CLIENT_STATE state)
{
    CallbackAdapterData *pAdapterData = (CallbackAdapterData *) customData;

    if (pAdapterData == NULL || pAdapterData->originalOnSignalingStateChanged == NULL) {
        return STATUS_SUCCESS;
    }

    webrtc_signaling_state_t webrtcSignalingState = convertKvsToWebrtcSignalingState(state);
    WEBRTC_STATUS result = pAdapterData->originalOnSignalingStateChanged(pAdapterData->originalCustomData, webrtcSignalingState);

    return (result == WEBRTC_STATUS_SUCCESS) ? STATUS_SUCCESS : STATUS_INTERNAL_ERROR;
}

/**
 * @brief Map KVS signaling errors to generic WebRTC status codes
 */
static WEBRTC_STATUS mapKvsErrorToWebrtcStatus(STATUS kvsStatus)
{
    /* Map specific KVS signaling errors to generic WebRTC signaling errors.
     * These error codes are defined in the KVS SDK and should trigger reconnection.
     */
    switch (kvsStatus) {
        /* KVS-specific error codes that should trigger signaling reconnection */
        case STATUS_SIGNALING_ICE_CONFIG_REFRESH_FAILED:
            return WEBRTC_STATUS_SIGNALING_ICE_REFRESH_FAILED;
        case STATUS_SIGNALING_RECONNECT_FAILED:
            return WEBRTC_STATUS_SIGNALING_RECONNECT_FAILED;

        /* Connection and network related errors */
        case STATUS_SIGNALING_CONNECT_CALL_FAILED:
        case STATUS_SIGNALING_LWS_CLIENT_CONNECT_FAILED:
        case STATUS_SIGNALING_LWS_CALL_FAILED:
        case STATUS_SIGNALING_CONNECTED_CALLBACK_FAILED:
        case STATUS_SIGNALING_MESSAGE_DELIVERY_FAILED:
        case STATUS_SIGNALING_DISCONNECTED_CALLBACK_FAILED:
            return WEBRTC_STATUS_SIGNALING_CONNECTION_LOST;

        /* Authentication and credentials related errors */
        case STATUS_SIGNALING_GET_TOKEN_CALL_FAILED:
            return WEBRTC_STATUS_SIGNALING_AUTH_FAILED;

        /* API call timeouts and failures */
        case STATUS_SIGNALING_DESCRIBE_CALL_FAILED:
        case STATUS_SIGNALING_CREATE_CALL_FAILED:
        case STATUS_SIGNALING_GET_ENDPOINT_CALL_FAILED:
        case STATUS_SIGNALING_GET_ICE_CONFIG_CALL_FAILED:
        case STATUS_SIGNALING_DELETE_CALL_FAILED:
            return WEBRTC_STATUS_SIGNALING_TIMEOUT;

        default:
            /* For other errors, return the original KVS status code.
             * This preserves fine-grained error information while allowing
             * app_webrtc to handle known reconnection cases.
             */
            return (WEBRTC_STATUS)kvsStatus;
    }
}

/**
 * @brief Adapter function for error callback
 */
static STATUS adapterErrorCallback(UINT64 customData, STATUS errorStatus, PCHAR errorMessage, UINT32 subErrorCode)
{
    CallbackAdapterData *pAdapterData = (CallbackAdapterData *) customData;

    if (pAdapterData == NULL || pAdapterData->originalOnError == NULL) {
        return STATUS_SUCCESS;
    }

    // Map KVS errors to generic WebRTC status codes for proper reconnection handling
    WEBRTC_STATUS webrtcStatus = mapKvsErrorToWebrtcStatus(errorStatus);

    ESP_LOGI(TAG, "Mapped KVS error 0x%08" PRIx32 " to WebRTC status 0x%08" PRIx32, (uint32_t) errorStatus, (uint32_t) webrtcStatus);

    WEBRTC_STATUS result = pAdapterData->originalOnError(pAdapterData->originalCustomData, webrtcStatus, errorMessage, subErrorCode);

    return (result == WEBRTC_STATUS_SUCCESS) ? STATUS_SUCCESS : STATUS_INTERNAL_ERROR;
}

// Forward declarations
STATUS createCredentialProvider(KvsSignalingClientData *pClientData);
static STATUS fetchCredentialsAdapter(UINT64 customData,
                                      PCHAR* pAccessKey, PUINT32 pAccessKeyLen,
                                      PCHAR* pSecretKey, PUINT32 pSecretKeyLen,
                                      PCHAR* pSessionToken, PUINT32 pSessionTokenLen,
                                      PUINT64 pExpiration);

/* Allocator for the re-send buffer: the KVS allocators (PSRAM when present). */
/* Buffered answers are aged on a monotonic clock. GETTIME() is wall-clock time,
 * which an SNTP sync steps forward: an answer buffered just before a sync would
 * look older than the age bound and be dropped unsent. */
static UINT64 kvsPendingNowMs(void)
{
    return (UINT64) (esp_timer_get_time() / 1000);
}

static void *kvsPendingCalloc(size_t num, size_t size)
{
    return MEMCALLOC(num, size);
}

static void kvsPendingFree(void *ptr)
{
    MEMFREE(ptr);
}

/* Rounds of one flush: messages queued behind a flush while it sends go out in
 * the next round. Bounded so a steady trickle can't hold the work queue; what is
 * left goes out from a flush queued again behind the other work. */
#define KVS_PENDING_FLUSH_MAX_ROUNDS 4

/**
 * @brief Send callback used by kvsPendingSignalingDrain to re-send one
 *        buffered answer or ICE candidate.
 */
static bool kvsResendPendingMessage(void *ctx, kvs_pending_msg_type_t type, const char *peerId,
                                    const char *correlationId, const char *payload,
                                    uint32_t payloadLen, uint32_t version)
{
    KvsSignalingClientData *pClientData = (KvsSignalingClientData *) ctx;

    /* Teardown started: fail the rest unsent, so the flush gets back to the work
     * queue after at most the send already in progress. */
    if (!kvsSignalingClientIsLive(pClientData)) {
        return false;
    }

    /* The batch is drained outside the send lock, so the buffer can move meanwhile.
     * A peer purged since the batch was taken has left: drop its messages. An
     * answer queued since then for the same peer is newer: drop the batch's answer,
     * and move the batch's candidates behind the newer one. */
    MUTEX_LOCK(pClientData->signalingSendMessageLock);
    BOOL purged = kvsPendingSignalingIsPurged(&pClientData->pending, peerId);
    BOOL newerAnswer = !purged && kvsPendingSignalingHasAnswer(&pClientData->pending, peerId);
    BOOL superseded = newerAnswer && (type == KVS_PENDING_MSG_ANSWER);
    BOOL requeued = FALSE;
    if (newerAnswer && type == KVS_PENDING_MSG_ICE_CANDIDATE) {
        /* Its answer is queued behind this flush: sent now, the candidate would reach
         * the viewer first. Move it behind that answer instead. */
        requeued = kvsPendingSignalingPushIce(&pClientData->pending, peerId, payload, payloadLen, version);
    }
    MUTEX_UNLOCK(pClientData->signalingSendMessageLock);
    if (newerAnswer && type == KVS_PENDING_MSG_ICE_CANDIDATE) {
        if (!requeued) {
            ESP_LOGE(TAG, "Buffered ICE candidate for peer %s dropped: the re-send buffer is full", peerId);
        }
        return true;   /* moved behind its answer (or dropped): done with it here */
    }
    if (purged || superseded) {
        ESP_LOGI(TAG, "Not re-sending buffered %s to peer %s: %s",
                 (type == KVS_PENDING_MSG_ANSWER) ? "answer" : "ICE candidate", peerId,
                 purged ? "the peer has left" : "a newer answer is queued");
        return true;   /* done with it: drop, don't retry */
    }

    SignalingMessage m;
    MEMSET(&m, 0, SIZEOF(m));
    m.version = version;
    m.messageType = (type == KVS_PENDING_MSG_ANSWER) ? SIGNALING_MESSAGE_TYPE_ANSWER : SIGNALING_MESSAGE_TYPE_ICE_CANDIDATE;
    strlcpy(m.peerClientId, peerId, MAX_SIGNALING_CLIENT_ID_LEN);
    strlcpy(m.correlationId, correlationId, MAX_CORRELATION_ID_LEN);
#ifdef DYNAMIC_SIGNALING_PAYLOAD
    m.payload = (PCHAR) payload;
#else
    MEMCPY(m.payload, payload, payloadLen);
#endif
    m.payloadLen = payloadLen;

    STATUS s = signalingClientSendMessageSync(pClientData->signalingClientHandle, &m);
    if (STATUS_FAILED(s)) {
        ESP_LOGW(TAG, "Re-send of buffered %s failed: 0x%08" PRIx32,
                 (type == KVS_PENDING_MSG_ANSWER) ? "answer" : "ICE candidate", (uint32_t) s);
    } else if (type == KVS_PENDING_MSG_ANSWER) {
        ESP_LOGI(TAG, "Re-sent buffered answer to peer %s after signaling reconnect", peerId);
    }
    return STATUS_SUCCEEDED(s);
}

static void kvs_flush_pending_task(void *arg);

/* Whether the running flush's batch holds an answer for peerId (call under
 * signalingSendMessageLock). */
static BOOL kvsFlushHoldsAnswerFor(const KvsSignalingClientData *pClientData, const char *peerId)
{
    for (UINT32 i = 0; i < pClientData->flushAnswerPeerCount; i++) {
        if (STRNCMP(pClientData->flushAnswerPeers[i], peerId, KVS_PENDING_ICE_PEERID_LEN) == 0) {
            return TRUE;
        }
    }
    return FALSE;
}

/**
 * @brief Re-send the answers and ICE candidates that were buffered while the
 *        signaling websocket was down. Called once the client reconnects.
 */
static void kvsFlushPendingSignaling(KvsSignalingClientData *pClientData)
{
    if (pClientData == NULL || pClientData->signalingClientHandle == INVALID_SIGNALING_CLIENT_HANDLE_VALUE) {
        return;
    }

    for (UINT32 round = 0; round < KVS_PENDING_FLUSH_MAX_ROUNDS; round++) {
        /* Teardown started: stop between rounds so its work-queue sync returns. */
        if (!kvsSignalingClientIsLive(pClientData)) {
            return;
        }
        /* Take ownership of the buffer under the lock, then re-send OUTSIDE it:
         * each send can block on the websocket, and holding
         * signalingSendMessageLock across up to KVS_PENDING_ICE_MAX +
         * KVS_PENDING_ANSWER_MAX blocking sends would stall new SDP answers on a
         * degraded link. */
        KvsPendingSignaling batch;
        MUTEX_LOCK(pClientData->signalingSendMessageLock);
        if (pClientData->pendingFlushes > 0) {
            /* Another flush holds a batch (not reachable while every flush runs
             * on the one work-queue worker; kept for a second caller). Taking a
             * second batch would let this flush send a peer's candidates while
             * the other still holds its answer. Ask the running flush for another
             * pass instead. */
            pClientData->flushAgain = TRUE;
            MUTEX_UNLOCK(pClientData->signalingSendMessageLock);
            return;
        }
        kvsPendingSignalingTake(&pClientData->pending, &batch);
        if (kvsPendingSignalingCount(&batch) == 0) {
            MUTEX_UNLOCK(pClientData->signalingSendMessageLock);
            kvsPendingSignalingFree(&batch);
            return;
        }
        pClientData->pendingFlushes++;
        pClientData->flushAgain = FALSE;
        pClientData->flushAnswerPeerCount = 0;
        for (UINT32 i = 0; i < batch.answer_count && i < KVS_PENDING_ANSWER_MAX; i++) {
            STRNCPY(pClientData->flushAnswerPeers[i], batch.answers[i].peer_client_id, KVS_PENDING_ICE_PEERID_LEN - 1);
            pClientData->flushAnswerPeers[i][KVS_PENDING_ICE_PEERID_LEN - 1] = '\0';
            pClientData->flushAnswerPeerCount++;
        }
        MUTEX_UNLOCK(pClientData->signalingSendMessageLock);

        ESP_LOGI(TAG, "Re-sending %" PRIu32 " buffered answer(s) and %" PRIu32 " ICE candidate(s) after signaling reconnect",
                 batch.answer_count, batch.ice_count);
        KvsPendingDrainStats stats;
        UINT64 nowMs = kvsPendingNowMs();
        UINT32 remaining = kvsPendingSignalingDrain(&batch, kvsResendPendingMessage, pClientData, nowMs, &stats);
        if (stats.answers_expired > 0) {
            ESP_LOGE(TAG, "Dropped %" PRIu32 " buffered answer(s) older than %d ms: the viewer's session has timed out",
                     stats.answers_expired, KVS_PENDING_ANSWER_MAX_AGE_MS);
        }
        if (stats.ice_dropped > 0) {
            ESP_LOGW(TAG, "Dropped %" PRIu32 " buffered ICE candidate(s) (retry cap, or their answer expired)",
                     stats.ice_dropped);
        }

        /* Some re-sends failed (link dropped again): keep the survivors for the
         * next reconnect instead of losing them, merged with whatever was
         * buffered meanwhile. */
        MUTEX_LOCK(pClientData->signalingSendMessageLock);
        UINT32 mergeDropped = kvsPendingSignalingMerge(&pClientData->pending, &batch);
        pClientData->pendingFlushes--;
        pClientData->flushAnswerPeerCount = 0;
        BOOL again = pClientData->flushAgain;
        pClientData->flushAgain = FALSE;
        BOOL more = (remaining == 0 || again) && (kvsPendingSignalingCount(&pClientData->pending) > 0);
        MUTEX_UNLOCK(pClientData->signalingSendMessageLock);
        if (mergeDropped > 0) {
            ESP_LOGW(TAG, "Dropped %" PRIu32 " buffered message(s) superseded or out of room after a failed re-send",
                     mergeDropped);
        }

        /* Everything went out and more queued behind this flush, or another flush
         * asked for a pass while this one ran (the link came back meanwhile):
         * send that too. Otherwise the link is down again, wait for the next
         * reconnect. */
        if (!more) {
            return;
        }
    }

    /* Out of rounds with the link up and messages still queued. A peer's answer
     * and candidates can all be among them, and then no direct send for that peer
     * comes along to drain them, so don't wait for one: go again from the work
     * queue. */
    esp_err_t queueStatus = esp_work_queue_add_task(&kvs_flush_pending_task, (void *) pClientData);
    if (queueStatus != ESP_OK) {
        ESP_LOGW(TAG, "Failed to re-queue the signaling re-send flush: %d (will retry on next send)", (int) queueStatus);
    }
}

/**
 * @brief Work-queue task that runs the re-send flush off the signaling state
 *        callback.
 *
 * kvsFlushPendingSignaling() does up to KVS_PENDING_ICE_MAX +
 * KVS_PENDING_ANSWER_MAX blocking websocket sends. The CONNECTED edge is
 * delivered from kvsStateChangedCallback, which runs inside
 * signalingStateMachineIterator with the signaling stateLock held, so flushing
 * there would stall getIceConfigInfo / disconnect / other state-machine
 * operations for the whole re-send burst on a degraded link. Deferring to the
 * work queue lets the state-machine iterator release stateLock first; the sends
 * then run in the worker task without it held.
 */
static void kvs_flush_pending_task(void *arg)
{
    if (arg == NULL || !kvsSignalingClientIsLive(arg)) {
        return;
    }
    kvsFlushPendingSignaling((KvsSignalingClientData *) arg);
}

/* TRUE in the states where the signaling websocket is up: CONNECTED, and the
 * storage-session states that run on top of it. */
static BOOL kvsStateHasWebSocket(SIGNALING_CLIENT_STATE state)
{
    switch (state) {
        case SIGNALING_CLIENT_STATE_CONNECTED:
        case SIGNALING_CLIENT_STATE_JOIN_SESSION:
        case SIGNALING_CLIENT_STATE_JOIN_SESSION_WAITING:
        case SIGNALING_CLIENT_STATE_JOIN_SESSION_CONNECTED:
            return TRUE;
        default:
            return FALSE;
    }
}

/**
 * @brief KVS signaling state change callback
 */
static STATUS kvsStateChangedCallback(UINT64 customData, SIGNALING_CLIENT_STATE state)
{
    KvsSignalingClientData *pClientData = (KvsSignalingClientData *) customData;

    if (pClientData == NULL) {
        return STATUS_NULL_ARG;
    }

    // Suppress duplicate state change notifications caused by state machine
    // self-transitions (e.g., CONNECTED → CONNECTED during ICE refresh cycles)
    if (state == pClientData->lastNotifiedState) {
        ESP_LOGD(TAG, "Suppressing duplicate state change notification for state %" PRIu32, (uint32_t) state);
        return STATUS_SUCCESS;
    }
    pClientData->lastNotifiedState = state;

    ESP_LOGI(TAG, "KVS signaling state changed to %" PRIu32, (uint32_t) state);

    // Update internal state. Every exit from CONNECTED clears `connected`, not
    // only DISCONNECTED: a websocket error reconnects through DESCRIBE /
    // GET_ENDPOINT / GET_ICE_CONFIG / READY / CONNECTING and never passes
    // DISCONNECTED, so the flag stayed TRUE for the whole reconnect and a send
    // failing in it was taken for a permanent error instead of being buffered.
    BOOL wasConnected = pClientData->connected;
    pClientData->connected = kvsStateHasWebSocket(state);
    if (pClientData->connected && !wasConnected) {
        // Websocket back up: flush buffered answers and ICE candidates. Deferred
        // to the work queue since this runs under the state-machine stateLock and
        // the flush does blocking sends. If queueing fails, the next successful
        // send queues the flush.
        esp_err_t queueStatus = esp_work_queue_add_task(&kvs_flush_pending_task, (void *) pClientData);
        if (queueStatus != ESP_OK) {
            ESP_LOGW(TAG, "Failed to queue the signaling re-send flush: %d (will retry on next send)", (int) queueStatus);
        }
#ifdef CONFIG_USE_ESP_WEBSOCKET_CLIENT
        // An ICE refresh that waited for the connect can run now
        kvs_ice_refresh_wake();
#endif
    }

    // Call user callback if set. Read it once: a free that leaks the client clears it.
    STATUS (*onStateChanged)(UINT64, SIGNALING_CLIENT_STATE) = pClientData->on_state_changed;
    if (onStateChanged != NULL) {
        ESP_LOGD(TAG, "Calling user on_state_changed callback");
        return onStateChanged((UINT64)pClientData->customData, state);
    } else {
        ESP_LOGW(TAG, "No on_state_changed callback set - events will not be propagated!");
    }

    return STATUS_SUCCESS;
}

/**
 * @brief KVS signaling error callback
 */
static STATUS kvsErrorCallback(UINT64 customData, STATUS status, PCHAR errorMsg, UINT32 errorMsgLen)
{
    KvsSignalingClientData *pClientData = (KvsSignalingClientData *) customData;

    if (pClientData == NULL) {
        return STATUS_NULL_ARG;
    }

    ESP_LOGW(TAG, "KVS signaling error: 0x%08" PRIx32 " - %.*s", (uint32_t) status, (int) errorMsgLen, errorMsg);

    // Call user callback if set. Read it once: a free that leaks the client clears it.
    STATUS (*onError)(UINT64, STATUS, PCHAR, UINT32) = pClientData->on_error;
    if (onError != NULL) {
        /* Convert KVS status to WEBRTC status before forwarding to user callback */
        WEBRTC_STATUS webrtcStatus = mapKvsErrorToWebrtcStatus(status);
        return onError((UINT64)pClientData->customData, webrtcStatus, errorMsg, errorMsgLen);
    }

    return STATUS_SUCCESS;
}

/**
 * @brief Convert KVS SDK message format to standardized WebRTC signaling message format
 */
static STATUS convertKvsToWebRtcMessage(PReceivedSignalingMessage pKvsMessage, webrtc_message_t* pWebRtcMessage)
{
    if (pKvsMessage == NULL || pWebRtcMessage == NULL) {
        return STATUS_NULL_ARG;
    }

    // Clear the output structure
    memset(pWebRtcMessage, 0, sizeof(webrtc_message_t));

    // Convert message type
    switch (pKvsMessage->signalingMessage.messageType) {
        case SIGNALING_MESSAGE_TYPE_OFFER:
            pWebRtcMessage->message_type = WEBRTC_MESSAGE_TYPE_OFFER;
            break;
        case SIGNALING_MESSAGE_TYPE_ANSWER:
            pWebRtcMessage->message_type = WEBRTC_MESSAGE_TYPE_ANSWER;
            break;
        case SIGNALING_MESSAGE_TYPE_ICE_CANDIDATE:
            pWebRtcMessage->message_type = WEBRTC_MESSAGE_TYPE_ICE_CANDIDATE;
            break;
        default:
            ESP_LOGW(TAG, "Unknown KVS message type: %" PRIu32, (uint32_t) pKvsMessage->signalingMessage.messageType);
            return STATUS_INVALID_ARG;
    }

    // Copy basic fields
    pWebRtcMessage->version = pKvsMessage->signalingMessage.version;

    // Copy correlation ID and peer client ID (with bounds checking)
    strlcpy(pWebRtcMessage->correlation_id, pKvsMessage->signalingMessage.correlationId, MAX_CORRELATION_ID_LEN);

    strlcpy(pWebRtcMessage->peer_client_id, pKvsMessage->signalingMessage.peerClientId, MAX_SIGNALING_CLIENT_ID_LEN);

    // Copy payload (note: this is a shallow copy - the payload memory is shared)
    pWebRtcMessage->payload = pKvsMessage->signalingMessage.payload;
    pWebRtcMessage->payload_len = pKvsMessage->signalingMessage.payloadLen;

    return STATUS_SUCCESS;
}

/**
 * @brief Internal callback that converts KVS message format to standardized format
 */
static STATUS kvsMessageReceivedCallback(UINT64 customData, PReceivedSignalingMessage pReceivedSignalingMessage)
{
    STATUS retStatus = STATUS_SUCCESS;
    KvsSignalingClientData *pClientData = (KvsSignalingClientData *)(UINT64)customData;
    webrtc_message_t webRtcMessage = {0};

    if (pClientData == NULL || pReceivedSignalingMessage == NULL) {
        retStatus = STATUS_NULL_ARG;
        CHK(FALSE, retStatus);
    }

    // Call user callback if set. Read it once: a free that leaks the client clears it.
    STATUS (*onMsgReceived)(UINT64, webrtc_message_t*) = pClientData->on_msg_received;
    if (onMsgReceived != NULL) {
        // Convert KVS message format to standardized WebRTC message format
        retStatus = convertKvsToWebRtcMessage(pReceivedSignalingMessage, &webRtcMessage);
        CHK_STATUS(retStatus);

        CallbackAdapterData *pAdapterData = (CallbackAdapterData *) pClientData->customData;
        // Call the user callback with the standardized message format
        retStatus = onMsgReceived((UINT64) pAdapterData->originalCustomData, &webRtcMessage);
    }

CleanUp:
    return retStatus;
}

/**
 * @brief Initialize KVS signaling client
 */
STATUS createKvsSignalingClient(kvs_signaling_config_t *pConfig, PVOID *ppSignalingClient)
{
    STATUS retStatus = STATUS_SUCCESS;
    KvsSignalingClientData *pClientData = NULL;

    CHK(pConfig != NULL && ppSignalingClient != NULL, STATUS_NULL_ARG);

#ifndef CONFIG_USE_ESP_WEBSOCKET_CLIENT
    // Set custom allocator for libwebsockets to use SPIRAM-capable memory
    lws_set_allocator(realloc_wrapper);
#endif

    // Allocate client data
    pClientData = (KvsSignalingClientData *)MEMCALLOC(1, SIZEOF(KvsSignalingClientData));
    CHK(pClientData != NULL, STATUS_NOT_ENOUGH_MEMORY);
    kvsPendingSignalingInit(&pClientData->pending, kvsPendingCalloc, kvsPendingFree);

    // Track this client so deferred work-queue tasks can detect teardown.
    kvsSignalingClientRegister(pClientData);

#ifdef CONFIG_USE_ESP_WEBSOCKET_CLIENT
    CHK_STATUS(kvs_ice_refresh_task_start());
#endif

    // Copy configuration
    MEMCPY(&pClientData->config, pConfig, SIZEOF(kvs_signaling_config_t));

    // Initialize mutex
    pClientData->signalingSendMessageLock = MUTEX_CREATE(FALSE);
    CHK(IS_VALID_MUTEX_VALUE(pClientData->signalingSendMessageLock), STATUS_INVALID_OPERATION);

    // Set up channel info - role type will be set by WebRTC app
    pClientData->channelInfo.version = CHANNEL_INFO_CURRENT_VERSION;
    pClientData->channelInfo.pChannelName = pConfig->pChannelName;
    pClientData->channelInfo.pKmsKeyId = NULL;
    pClientData->channelInfo.tagCount = 0;
    pClientData->channelInfo.pTags = NULL;
    pClientData->channelInfo.channelType = SIGNALING_CHANNEL_TYPE_SINGLE_MASTER;
    pClientData->channelInfo.channelRoleType = SIGNALING_CHANNEL_ROLE_TYPE_MASTER; // Default, will be updated
#if CONFIG_USE_ESP_WEBSOCKET_CLIENT || CONFIG_SPIRAM_BOOT_INIT
    pClientData->channelInfo.cachingPolicy = SIGNALING_API_CALL_CACHE_TYPE_FILE;  // Uses NVS on ESP platform
#else // stack usage is quite high in default websocket client mode
    pClientData->channelInfo.cachingPolicy = SIGNALING_API_CALL_CACHE_TYPE_NONE;
#endif
    pClientData->channelInfo.cachingPeriod = SIGNALING_API_CALL_CACHE_TTL_SENTINEL_VALUE;
    pClientData->channelInfo.asyncIceServerConfig = TRUE;
    pClientData->channelInfo.retry = TRUE;
    pClientData->channelInfo.reconnect = TRUE;
    pClientData->channelInfo.pCertPath = pConfig->caCertPath;
    pClientData->channelInfo.messageTtl = 0; // Default is 60 seconds
    pClientData->channelInfo.pRegion = pConfig->awsRegion;

    // Set up client info. The log level is filled in when the client is created.
    pClientData->clientInfo.version = SIGNALING_CLIENT_INFO_CURRENT_VERSION;
    pClientData->clientInfo.cacheFilePath = NULL; // Use the default path
    pClientData->clientInfo.signalingClientCreationMaxRetryAttempts = CREATE_SIGNALING_CLIENT_RETRY_ATTEMPTS_SENTINEL_VALUE;

    // Set default client ID (will be updated based on role type later)
    STRCPY(pClientData->clientInfo.clientId, DEFAULT_MASTER_CLIENT_ID);

    // Set up callbacks
    pClientData->signalingClientCallbacks.version = SIGNALING_CLIENT_CALLBACKS_CURRENT_VERSION;
    pClientData->signalingClientCallbacks.customData = (UINT64) pClientData;
    pClientData->signalingClientCallbacks.errorReportFn = kvsErrorCallback;
    pClientData->signalingClientCallbacks.stateChangeFn = kvsStateChangedCallback;
    pClientData->signalingClientCallbacks.messageReceivedFn = kvsMessageReceivedCallback;

    // Initialize metrics
    pClientData->metrics.version = SIGNALING_CLIENT_METRICS_CURRENT_VERSION;

    // Create credential provider
    CHK_STATUS(createCredentialProvider(pClientData));

    *ppSignalingClient = pClientData;

CleanUp:
    if (STATUS_FAILED(retStatus) && pClientData != NULL) {
        freeKvsSignalingClient(pClientData);
        pClientData = NULL;
    }

    return retStatus;
}

/**
 * @brief Create credential provider for KVS signaling
 */
STATUS createCredentialProvider(KvsSignalingClientData *pClientData)
{
    STATUS retStatus = STATUS_SUCCESS;
    PCHAR pIotCoreCredentialEndPoint = NULL;
    PCHAR pIotCoreCert = NULL;
    PCHAR pIotCorePrivateKey = NULL;
    PCHAR pIotCoreRoleAlias = NULL;
    PCHAR pIotCoreThingName = NULL;
    PCHAR pAccessKey = NULL;
    PCHAR pSecretKey = NULL;
    PCHAR pSessionToken = NULL;

    CHK(pClientData != NULL, STATUS_NULL_ARG);

    ESP_LOGI(TAG, "Creating credential provider with region: %s", pClientData->config.awsRegion);
    ESP_LOGI(TAG, "Credential type: %s", pClientData->config.useIotCredentials ? "IoT Core" : "Static");

    // Create credential provider based on type
    if (pClientData->config.fetch_credentials_cb != NULL) {
        // Highest precedence: callback-based credentials supplier
        kvs_cred_fetch_ctx_t *ctx = (kvs_cred_fetch_ctx_t*) MEMCALLOC(1, SIZEOF(kvs_cred_fetch_ctx_t));
        CHK(ctx != NULL, STATUS_NOT_ENOUGH_MEMORY);
        ctx->cb = (kvs_fetch_credentials_cb_t) pClientData->config.fetch_credentials_cb;
        ctx->ud = pClientData->config.fetch_credentials_user_data;
        pClientData->pCredFetchAdapterCtx = ctx;

        CHK_STATUS(createCallbackCredentialProvider(fetchCredentialsAdapter,
                                                    (UINT64)(uintptr_t) ctx,
                                                    &pClientData->pCredentialProvider));
        ESP_LOGI(TAG, "Using callback-based credential provider");
    } else if (pClientData->config.useIotCredentials) {
        // Use IoT Core credentials from the options
        pIotCoreCredentialEndPoint = pClientData->config.iotCoreCredentialEndpoint;
        pIotCoreCert = pClientData->config.iotCoreCert;
        pIotCorePrivateKey = pClientData->config.iotCorePrivateKey;
        pIotCoreRoleAlias = pClientData->config.iotCoreRoleAlias;
        pIotCoreThingName = pClientData->config.iotCoreThingName;

        // Validate required fields
        CHK_ERR(pIotCoreCredentialEndPoint != NULL && pIotCoreCredentialEndPoint[0] != '\0', STATUS_INVALID_OPERATION,
                "IoT Core credential endpoint must be set");
        CHK_ERR(pIotCoreCert != NULL && pIotCoreCert[0] != '\0', STATUS_INVALID_OPERATION,
                "IoT Core certificate must be set");
        CHK_ERR(pIotCorePrivateKey != NULL && pIotCorePrivateKey[0] != '\0', STATUS_INVALID_OPERATION,
                "IoT Core private key must be set");
        CHK_ERR(pIotCoreRoleAlias != NULL && pIotCoreRoleAlias[0] != '\0', STATUS_INVALID_OPERATION,
                "IoT Core role alias must be set");
        CHK_ERR(pIotCoreThingName != NULL && pIotCoreThingName[0] != '\0', STATUS_INVALID_OPERATION,
                "IoT Core thing name must be set");

        ESP_LOGI(TAG, "Creating IoT credential provider with endpoint: %s", pIotCoreCredentialEndPoint);
        ESP_LOGI(TAG, "IoT Core thing name: %s, role alias: %s", pIotCoreThingName, pIotCoreRoleAlias);
        ESP_LOGI(TAG, "Certificate path: %s", pIotCoreCert);
        ESP_LOGI(TAG, "Private key path: %s", pIotCorePrivateKey);
        ESP_LOGI(TAG, "CA cert path: %s", pClientData->channelInfo.pCertPath);

        // Try to read the certificate file to verify it exists and is accessible
        BOOL cert_exists = FALSE;
        if (fileExists(pIotCoreCert, &cert_exists) != STATUS_SUCCESS || !cert_exists) {
            ESP_LOGE(TAG, "Failed to open certificate file: %s", pIotCoreCert);
            CHK(FALSE, STATUS_INVALID_OPERATION);
        } else {
            ESP_LOGI(TAG, "Successfully verified certificate file exists and is readable");
        }

        // Try to read the private key file to verify it exists and is accessible
        BOOL key_exists = FALSE;
        if (fileExists(pIotCorePrivateKey, &key_exists) != STATUS_SUCCESS || !key_exists) {
            ESP_LOGE(TAG, "Failed to open private key file: %s", pIotCorePrivateKey);
            CHK(FALSE, STATUS_INVALID_OPERATION);
        } else {
            ESP_LOGI(TAG, "Successfully verified private key file exists and is readable");
        }

        // Try to read the CA cert file to verify it exists and is accessible
        if (pClientData->channelInfo.pCertPath != NULL) {
            BOOL ca_exists = FALSE;
            if (fileExists(pClientData->channelInfo.pCertPath, &ca_exists) != STATUS_SUCCESS || !ca_exists) {
                ESP_LOGE(TAG, "Failed to open CA cert file: %s", pClientData->channelInfo.pCertPath);
                CHK(FALSE, STATUS_INVALID_OPERATION);
            } else {
                ESP_LOGI(TAG, "Successfully verified CA cert file exists and is readable");
            }
        }

#ifdef CONFIG_IOT_CORE_ENABLE_CREDENTIALS
        retStatus = createIotCredentialProvider(
            pIotCoreCredentialEndPoint,
            pClientData->config.awsRegion,
            pIotCoreCert,
            pIotCorePrivateKey,
            pClientData->channelInfo.pCertPath,
            pIotCoreRoleAlias,
            pIotCoreThingName,
            &pClientData->pCredentialProvider);
#else
        // IoT Core credentials not supported in this build
        ESP_LOGE(TAG, "IoT Core credentials not supported in this build");
        CHK(FALSE, STATUS_NOT_IMPLEMENTED);
#endif

#ifdef CONFIG_IOT_CORE_ENABLE_CREDENTIALS
        if (STATUS_FAILED(retStatus)) {
            ESP_LOGE(TAG, "Failed to create credential provider: 0x%08" PRIx32, retStatus);
            if ((retStatus >> 24) == 0x52) {
                ESP_LOGE(TAG, "This appears to be a networking error. Check network connectivity.");
            } else if ((retStatus >> 24) == 0x50) {
                ESP_LOGE(TAG, "This appears to be a platform error. Check certificate and key files.");
            } else if ((retStatus >> 24) == 0x58) {
                ESP_LOGE(TAG, "This appears to be a client error. Check IoT Core configuration.");
            }
            CHK(FALSE, retStatus);
        }

        ESP_LOGI(TAG, "IoT credential provider created successfully");
#endif
    } else {
        // Use direct AWS credentials from the options
        pAccessKey = pClientData->config.awsAccessKey;
        pSecretKey = pClientData->config.awsSecretKey;
        pSessionToken = pClientData->config.awsSessionToken;

        // Validate required fields
        CHK_ERR(pAccessKey != NULL && pAccessKey[0] != '\0', STATUS_INVALID_OPERATION,
                "AWS access key must be set");
        CHK_ERR(pSecretKey != NULL && pSecretKey[0] != '\0', STATUS_INVALID_OPERATION,
                "AWS secret key must be set");

        ESP_LOGI(TAG, "Creating static credential provider with access key ID: %.*s...", 4, pAccessKey);
        retStatus = createStaticCredentialProvider(
            pAccessKey,
            0,
            pSecretKey,
            0,
            pSessionToken,
            0,
            MAX_UINT64,
            &pClientData->pCredentialProvider);

        if (STATUS_FAILED(retStatus)) {
            ESP_LOGE(TAG, "Failed to create static credential provider: 0x%08" PRIx32, retStatus);
            CHK(FALSE, retStatus);
        }

        ESP_LOGI(TAG, "Static credential provider created successfully");
    }

CleanUp:
    return retStatus;
}

static STATUS fetchCredentialsAdapter(UINT64 customData,
                                      PCHAR* pAccessKey, PUINT32 pAccessKeyLen,
                                      PCHAR* pSecretKey, PUINT32 pSecretKeyLen,
                                      PCHAR* pSessionToken, PUINT32 pSessionTokenLen,
                                      PUINT64 pExpiration)
{
    kvs_cred_fetch_ctx_t *ctx = (kvs_cred_fetch_ctx_t*) (uintptr_t) customData;
    if (ctx == NULL || ctx->cb == NULL) {
        return STATUS_NULL_ARG;
    }
    const char *ak = NULL, *sk = NULL, *tk = NULL; uint32_t akl=0, skl=0, tkl=0; uint32_t exp=0;
    int rc = ctx->cb(ctx->ud, &ak, &akl, &sk, &skl, &tk, &tkl, &exp);
    if (rc != 0) {
        return STATUS_INTERNAL_ERROR;
    }
    *pAccessKey = (PCHAR) ak; *pAccessKeyLen = akl;
    *pSecretKey = (PCHAR) sk; *pSecretKeyLen = skl;
    *pSessionToken = (PCHAR) tk; *pSessionTokenLen = tkl;

    /* Convert relative expiration (seconds from now) to absolute nanoseconds */
    UINT64 currentTimeNs = GETTIME();
    UINT64 expirationNs = currentTimeNs + (exp * HUNDREDS_OF_NANOS_IN_A_SECOND);
    *pExpiration = expirationNs;
    return STATUS_SUCCESS;
}

/**
 * @brief Connect KVS signaling client
 */
STATUS connectKvsSignalingClient(PVOID pSignalingClient)
{
    STATUS retStatus = STATUS_SUCCESS;
    KvsSignalingClientData *pClientData = (KvsSignalingClientData *)pSignalingClient;

    CHK(pClientData != NULL, STATUS_NULL_ARG);
    CHK(!ATOMIC_LOAD_BOOL(&pClientData->aborting), STATUS_SIGNALING_FREE_IN_PROGRESS);

    // Create KVS signaling client if not already created
    if (pClientData->signalingClientHandle == INVALID_SIGNALING_CLIENT_HANDLE_VALUE) {
        // Upstream Signaling.c sets the global log level from clientInfo. Pass the current
        // level so that it keeps what app_webrtc_set_log_level() set.
        pClientData->clientInfo.loggingLevel = GET_LOGGER_LOG_LEVEL();
        CHK_STATUS(createSignalingClientSync(
            &pClientData->clientInfo,
            &pClientData->channelInfo,
            &pClientData->signalingClientCallbacks,
            pClientData->pCredentialProvider,
            &pClientData->signalingClientHandle));
    }

    // Fetch the signaling client
    CHK(!ATOMIC_LOAD_BOOL(&pClientData->aborting), STATUS_SIGNALING_FREE_IN_PROGRESS);
    CHK_STATUS(signalingClientFetchSync(pClientData->signalingClientHandle));

    // Connect the signaling client
    CHK(!ATOMIC_LOAD_BOOL(&pClientData->aborting), STATUS_SIGNALING_FREE_IN_PROGRESS);
    CHK_STATUS(signalingClientConnectSync(pClientData->signalingClientHandle));

    // Get metrics
    CHK_STATUS(signalingClientGetMetrics(pClientData->signalingClientHandle, &pClientData->metrics));

    // Log metrics
    ESP_LOGI(TAG, "[Signaling Get token] %" PRIu64 " ms", pClientData->metrics.signalingClientStats.getTokenCallTime);
    ESP_LOGI(TAG, "[Signaling Describe] %" PRIu64 " ms", pClientData->metrics.signalingClientStats.describeCallTime);
    ESP_LOGI(TAG, "[Signaling Get endpoint] %" PRIu64 " ms", pClientData->metrics.signalingClientStats.getEndpointCallTime);
    ESP_LOGI(TAG, "[Signaling Get ICE config] %" PRIu64 " ms", pClientData->metrics.signalingClientStats.getIceConfigCallTime);
    ESP_LOGI(TAG, "[Signaling Connect] %" PRIu64 " ms", pClientData->metrics.signalingClientStats.connectCallTime);

    pClientData->initialized = TRUE;

CleanUp:
    return retStatus;
}

/**
 * @brief Stop a connect or reconnect running on another task, without waiting
 *
 * For a terminate during an outage: the task that owns the teardown can sit in
 * connect() for minutes (fetch retries, state backoffs, the network wait). This
 * sets the SDK client's shutdown flag early, as freeSignaling() would, so those
 * waits end and connect() returns STATUS_SIGNALING_FREE_IN_PROGRESS. The caller
 * must keep the client alive until it returns; it takes no lock the connecting
 * task may hold. The client is only good for freeing afterwards.
 */
STATUS abortKvsSignalingClient(PVOID pSignalingClient)
{
    STATUS retStatus = STATUS_SUCCESS;
    KvsSignalingClientData *pClientData = (KvsSignalingClientData *)pSignalingClient;
    PSignalingClient pClient;

    CHK(pClientData != NULL, STATUS_NULL_ARG);
    ATOMIC_STORE_BOOL(&pClientData->aborting, TRUE);
    pClient = (PSignalingClient) pClientData->signalingClientHandle;
    if (pClient != NULL) {
        ATOMIC_STORE_BOOL(&pClient->shutdown, TRUE);
        CVAR_BROADCAST(pClient->connectedCvar);
    }
#ifdef CONFIG_IOT_CORE_ENABLE_CREDENTIALS
    // A GET_TOKEN refresh retries its fetch. Stop it at the next attempt.
    // Same provider-type order as the free in freeKvsSignalingClient().
    if (pClientData->pCredentialProvider != NULL && pClientData->config.fetch_credentials_cb == NULL &&
        pClientData->config.useIotCredentials) {
        iotCredentialProviderAbort(pClientData->pCredentialProvider);
    }
#endif
#ifdef CONFIG_USE_ESP_WEBSOCKET_CLIENT
    // After the store: a woken wait re-checks shutdown
    wakeEspSignalingWaits();
#endif

CleanUp:
    return retStatus;
}

/**
 * @brief Disconnect KVS signaling client
 */
STATUS disconnectKvsSignalingClient(PVOID pSignalingClient)
{
    STATUS retStatus = STATUS_SUCCESS;
    KvsSignalingClientData *pClientData = (KvsSignalingClientData *)pSignalingClient;

    CHK(pClientData != NULL, STATUS_NULL_ARG);
    CHK(pClientData->signalingClientHandle != INVALID_SIGNALING_CLIENT_HANDLE_VALUE, STATUS_INVALID_OPERATION);

    // Disconnect the signaling client
    CHK_STATUS(signalingClientDisconnectSync(pClientData->signalingClientHandle));

CleanUp:
    return retStatus;
}

/**
 * @brief TRUE when a failed send is down to the link: the client has left
 *        CONNECTED (it is reconnecting), or the transport has already seen the
 *        websocket close while the state machine still says CONNECTED.
 */
static BOOL kvsSignalingLinkDown(KvsSignalingClientData *pClientData)
{
    PSignalingClient pSignalingClient = (PSignalingClient) pClientData->signalingClientHandle;
    return !pClientData->connected || pSignalingClient == NULL || !ATOMIC_LOAD_BOOL(&pSignalingClient->connected);
}

/**
 * @brief Buffer a message whose send failed on connection loss, for re-send
 *        once the client reconnects. Called with signalingSendMessageLock held.
 *
 * Covers the SDP answer (a viewer offer wakes a parked host and the link drops
 * before the answer goes out) and trickle ICE candidates (AWS often FINs the
 * websocket right after the answer, while relay candidates are still
 * gathering). Returns TRUE if buffered.
 */
static BOOL kvsBufferFailedSend(KvsSignalingClientData *pClientData, webrtc_message_t *pMessage, STATUS sendStatus)
{
    KvsPendingSignaling *pPending = &pClientData->pending;

    if (pMessage->message_type == WEBRTC_MESSAGE_TYPE_ANSWER) {
        UINT64 nowMs = kvsPendingNowMs();
        if (kvsPendingSignalingPutAnswer(pPending, pMessage->peer_client_id, pMessage->correlation_id,
                                         pMessage->payload, pMessage->payload_len, pMessage->version, nowMs)) {
            ESP_LOGW(TAG, "Signaling send failed (0x%08" PRIx32 ") for answer to peer %s; buffered for re-send on reconnect (pending answers=%" PRIu32 ")",
                     (uint32_t) sendStatus, pMessage->peer_client_id, pPending->answer_count);
            return TRUE;
        }
        ESP_LOGE(TAG, "Answer re-send buffer unavailable; answer to peer %s dropped (len=%" PRIu32 ")",
                 pMessage->peer_client_id, pMessage->payload_len);
    } else if (pMessage->message_type == WEBRTC_MESSAGE_TYPE_ICE_CANDIDATE) {
        if (kvsPendingSignalingPushIce(pPending, pMessage->peer_client_id, pMessage->payload,
                                       pMessage->payload_len, pMessage->version)) {
            ESP_LOGW(TAG, "Signaling send failed (0x%08" PRIx32 ") for ICE candidate; buffered for re-send on reconnect (pending=%" PRIu32 ")",
                     (uint32_t) sendStatus, pPending->ice_count);
            return TRUE;
        }
        ESP_LOGE(TAG, "ICE candidate re-trickle buffer unavailable/full; candidate dropped (pending=%" PRIu32 ", len=%" PRIu32 ")",
                 pPending->ice_count, pMessage->payload_len);
    }
    return FALSE;
}

/**
 * @brief Send message through KVS signaling client
 */
STATUS sendKvsSignalingMessage(PVOID pSignalingClient, webrtc_message_t *pMessage)
{
    STATUS retStatus = STATUS_SUCCESS;
    KvsSignalingClientData *pClientData = (KvsSignalingClientData *)pSignalingClient;
    SignalingMessage signalingMessage;
    BOOL locked = FALSE;
    BOOL buffered = FALSE;
    BOOL flushBacklog = FALSE;

    CHK(pClientData != NULL && pMessage != NULL, STATUS_NULL_ARG);
    CHK(pClientData->signalingClientHandle != INVALID_SIGNALING_CLIENT_HANDLE_VALUE, STATUS_INVALID_OPERATION);

    // Convert the message to KVS format
    signalingMessage.version = pMessage->version;
    signalingMessage.messageType = (SIGNALING_MESSAGE_TYPE)pMessage->message_type;
    strlcpy(signalingMessage.peerClientId, pMessage->peer_client_id, MAX_SIGNALING_CLIENT_ID_LEN);
    strlcpy(signalingMessage.correlationId, pMessage->correlation_id, MAX_CORRELATION_ID_LEN);
#ifdef DYNAMIC_SIGNALING_PAYLOAD
    // Allocate a buffer to store the decoded data
    signalingMessage.payload = pMessage->payload;
#else
    MEMCPY(signalingMessage.payload, pMessage->payload, pMessage->payload_len);
#endif
    signalingMessage.payloadLen = pMessage->payload_len;

    // Send the message with thread safety
    MUTEX_LOCK(pClientData->signalingSendMessageLock);
    locked = TRUE;

    // A candidate whose peer's answer is still buffered, or is in the batch a flush
    // is re-sending, queues behind that answer: a candidate that reaches the viewer
    // before its answer is rejected (no remote description yet). So does a newer
    // answer for a peer whose older answer is in that batch. Other peers' messages
    // go out directly.
    BOOL inFlush = pClientData->pendingFlushes > 0 && kvsFlushHoldsAnswerFor(pClientData, pMessage->peer_client_id);
    if (pMessage->message_type == WEBRTC_MESSAGE_TYPE_ICE_CANDIDATE &&
        (inFlush || kvsPendingSignalingHasAnswer(&pClientData->pending, pMessage->peer_client_id))) {
        if (kvsPendingSignalingPushIce(&pClientData->pending, pMessage->peer_client_id, pMessage->payload,
                                       pMessage->payload_len, pMessage->version)) {
            ESP_LOGD(TAG, "ICE candidate for peer %s queued behind its buffered answer", pMessage->peer_client_id);
            buffered = TRUE;
        } else {
            // Sent now it would arrive ahead of its answer and be rejected anyway.
            ESP_LOGE(TAG, "ICE candidate for peer %s dropped: the re-send buffer is full (pending=%" PRIu32 ")",
                     pMessage->peer_client_id, pClientData->pending.ice_count);
            retStatus = STATUS_NOT_ENOUGH_MEMORY;
        }
    } else if (pMessage->message_type == WEBRTC_MESSAGE_TYPE_ANSWER && inFlush &&
               kvsPendingSignalingPutAnswer(&pClientData->pending, pMessage->peer_client_id,
                                            pMessage->correlation_id, pMessage->payload, pMessage->payload_len,
                                            pMessage->version, kvsPendingNowMs())) {
        // The flush sends it in its next round, after the batch, and skips the
        // batch's own answer for this peer since this one is newer.
        ESP_LOGI(TAG, "Answer to peer %s queued behind the running re-send flush", pMessage->peer_client_id);
        buffered = TRUE;
    } else {
        retStatus = signalingClientSendMessageSync(pClientData->signalingClientHandle, &signalingMessage);

        // An answer or ICE candidate that couldn't be sent because the websocket is
        // down is buffered for re-send once the client reconnects. Only buffer on
        // connection loss: a send that fails while still connected is a permanent
        // error (bad version, too large, OOM) and must propagate to the caller, not
        // be silently swallowed and retried forever.
        if (STATUS_FAILED(retStatus) && kvsSignalingLinkDown(pClientData)) {
            buffered = kvsBufferFailedSend(pClientData, pMessage, retStatus);
            if (buffered) {
                retStatus = STATUS_SUCCESS;  // handled: will retry after reconnect
            }
        } else if (STATUS_SUCCEEDED(retStatus) && pMessage->message_type == WEBRTC_MESSAGE_TYPE_ANSWER &&
                   kvsPendingSignalingDropAnswer(&pClientData->pending, pMessage->peer_client_id)) {
            ESP_LOGI(TAG, "Dropped the buffered answer to peer %s: a newer one was sent", pMessage->peer_client_id);
        }
    }

    // Opportunistically drain the buffer once a send goes through: that proves the
    // link is up, and a transient failure may have buffered messages without any
    // following CONNECTED edge to flush them. Not while a flush already runs.
    flushBacklog = !buffered && STATUS_SUCCEEDED(retStatus) && pClientData->pendingFlushes == 0 &&
        kvsPendingSignalingCount(&pClientData->pending) > 0;
    MUTEX_UNLOCK(pClientData->signalingSendMessageLock);
    locked = FALSE;

    // Update metrics only after an answer actually went out. A real send failure
    // is left in retStatus and returned via the CleanUp block below.
    if (STATUS_SUCCEEDED(retStatus) && !buffered && pMessage->message_type == WEBRTC_MESSAGE_TYPE_ANSWER) {
        CHK_STATUS(signalingClientGetMetrics(pClientData->signalingClientHandle, &pClientData->metrics));
    }

    // On the work queue, not here: the backlog can be a dozen blocking sends, and
    // this caller (an answer or its ICE trickle) shouldn't wait for them.
    if (flushBacklog) {
        esp_err_t queueStatus = esp_work_queue_add_task(&kvs_flush_pending_task, (void *) pClientData);
        if (queueStatus != ESP_OK) {
            ESP_LOGW(TAG, "Failed to queue the signaling re-send flush: %d (will retry on next send)", (int) queueStatus);
        }
    }

CleanUp:
    if (locked) {
        MUTEX_UNLOCK(pClientData->signalingSendMessageLock);
    }

    return retStatus;
}

/**
 * @brief Drop the answer and ICE candidates buffered for a departed peer.
 *
 * Called from the peer/session teardown path so a viewer that leaves releases
 * its slots immediately, instead of pinning them for the whole disconnect
 * window (until KVS_PENDING_ICE_MAX_ATTEMPTS or KVS_PENDING_ANSWER_MAX_AGE_MS
 * drains them). Takes the send lock, matching the buffer/flush locking
 * discipline; the buffer frees its slot arrays once they are empty.
 */
STATUS kvsSignalingPurgePeerCandidates(PVOID pSignalingClient, const char *peerId)
{
    STATUS retStatus = STATUS_SUCCESS;
    KvsSignalingClientData *pClientData = (KvsSignalingClientData *)pSignalingClient;

    CHK(pClientData != NULL && peerId != NULL, STATUS_NULL_ARG);

    MUTEX_LOCK(pClientData->signalingSendMessageLock);
    UINT32 removed = kvsPendingSignalingPurgePeer(&pClientData->pending, peerId);
    if (removed > 0) {
        ESP_LOGI(TAG, "Purged %" PRIu32 " buffered signaling message(s) for departed peer %s (pending=%" PRIu32 ")",
                 removed, peerId, kvsPendingSignalingCount(&pClientData->pending));
    }
    MUTEX_UNLOCK(pClientData->signalingSendMessageLock);

CleanUp:
    return retStatus;
}

/**
 * @brief Set callbacks for KVS signaling client
 */
STATUS setKvsSignalingCallbacks(PVOID pSignalingClient,
                              PVOID customData,
                              STATUS (*on_msg_received)(UINT64, webrtc_message_t*),
                              STATUS (*on_state_changed)(UINT64, SIGNALING_CLIENT_STATE),
                              STATUS (*on_error)(UINT64, STATUS, PCHAR, UINT32))
{
    STATUS retStatus = STATUS_SUCCESS;
    KvsSignalingClientData *pClientData = (KvsSignalingClientData *)pSignalingClient;

    CHK(pClientData != NULL, STATUS_NULL_ARG);

    // Store the callbacks
    pClientData->customData = customData;
    pClientData->on_msg_received = on_msg_received;
    pClientData->on_state_changed = on_state_changed;
    pClientData->on_error = on_error;

CleanUp:
    return retStatus;
}

/**
 * @brief Free KVS signaling client
 */
STATUS freeKvsSignalingClient(PVOID pSignalingClient)
{
    STATUS retStatus = STATUS_SUCCESS;
    KvsSignalingClientData *pClientData = (KvsSignalingClientData *)pSignalingClient;
    BOOL unregistered = FALSE;

    CHK(pClientData != NULL, STATUS_NULL_ARG);
#ifdef CONFIG_USE_ESP_WEBSOCKET_CLIENT
    // A free from the ICE refresh task would wait on the run lock that task holds
    CHK(s_iceRefreshTask == NULL || xTaskGetCurrentTaskHandle() != s_iceRefreshTask, STATUS_INVALID_OPERATION);
#endif

    // Stop new deferred tasks from starting on this client...
    kvsSignalingClientUnregister(pClientData);
    unregistered = TRUE;

#ifdef CONFIG_USE_ESP_WEBSOCKET_CLIENT
    // ...wait for an ICE fetch that may be running on it. Set shutdown first, as freeSignaling()
    // does, so the refresh task does not start another one.
    if (pClientData->signalingClientHandle != INVALID_SIGNALING_CLIENT_HANDLE_VALUE) {
        ATOMIC_STORE_BOOL(&((PSignalingClient) pClientData->signalingClientHandle)->shutdown, TRUE);
    }
    if (s_iceRefreshRunLock != NULL) {
        if (xSemaphoreTake(s_iceRefreshRunLock, pdMS_TO_TICKS(KVS_ICE_REFRESH_DRAIN_MS)) != pdTRUE) {
            ESP_LOGE(TAG, "ICE refresh drain timed out; leaking signaling client to avoid use-after-free");
            CHK(FALSE, STATUS_OPERATION_TIMED_OUT);
        }
        xSemaphoreGive(s_iceRefreshRunLock);
    }
#endif

    // ...then drain any in-flight one BEFORE freeing anything it may still touch.
    // A flush task that already passed its live-guard can be mid-send, holding
    // signalingSendMessageLock and dereferencing the signaling object / pClientData,
    // so the barrier must precede every free below (freeSignalingClient included).
    if (esp_work_queue_sync(2000) != ESP_OK) {
        // The task is still blocked in a send (on the ESP backend the send can
        // block until the bounded send-timeout lands) and may hold the send lock
        // or reference the client. Freeing now would be a use-after-free, so leak
        // this client rather than crash.
        ESP_LOGE(TAG, "Work queue drain timed out; leaking signaling client to avoid use-after-free");
        CHK(FALSE, STATUS_OPERATION_TIMED_OUT);
    }

    // Safe past this point: no deferred task is running or can start.

    // Free the signaling client if initialized
    if (pClientData->signalingClientHandle != INVALID_SIGNALING_CLIENT_HANDLE_VALUE) {
        STATUS freeStatus = freeSignalingClient(&pClientData->signalingClientHandle);
        if (STATUS_FAILED(freeStatus)) {
            // Refused (called from one of the client's own threads) or not drained in time:
            // the client still runs and uses everything freed below, so leak it all instead
            ESP_LOGE(TAG, "freeSignalingClient failed (0x%08" PRIx32 "); leaking the signaling client", freeStatus);
            CHK(FALSE, freeStatus);
        }
    }

    // Free the credential provider
    if (pClientData->pCredentialProvider != NULL) {
        if (pClientData->config.fetch_credentials_cb != NULL) {
            // Callback-based credential provider
            freeCallbackCredentialProvider(&pClientData->pCredentialProvider);
        } else if (pClientData->config.useIotCredentials) {
            freeIotCredentialProvider(&pClientData->pCredentialProvider);
        } else {
            freeStaticCredentialProvider(&pClientData->pCredentialProvider);
        }
    }

    // Free the callback credential-fetch adapter context (allocated in
    // createCredentialProvider); freed after the provider that referenced it.
    if (pClientData->pCredFetchAdapterCtx != NULL) {
        MEMFREE(pClientData->pCredFetchAdapterCtx);
        pClientData->pCredFetchAdapterCtx = NULL;
    }

    // Free the mutex
    if (IS_VALID_MUTEX_VALUE(pClientData->signalingSendMessageLock)) {
        MUTEX_FREE(pClientData->signalingSendMessageLock);
    }

    // Free the callback adapter data if it was allocated
    if (pClientData->pCallbackAdapterData != NULL) {
        MEMFREE(pClientData->pCallbackAdapterData);
    }

    // Free the lazily-allocated re-send buffer if still resident
    kvsPendingSignalingFree(&pClientData->pending);

    // Free the client data
    MEMFREE(pClientData);

CleanUp:
    if (STATUS_FAILED(retStatus) && unregistered) {
        // Past the unregister, a failure leaks the client, and it may still run. Its owner
        // frees the context the callbacks were given, so the client must not call them.
        pClientData->on_msg_received = NULL;
        pClientData->on_state_changed = NULL;
        pClientData->on_error = NULL;
    }
    return retStatus;
}

/**
 * @brief Get ICE server configuration from KVS signaling client
 */
STATUS getKvsSignalingIceServers(PVOID pSignalingClient, PUINT32 pIceConfigCount, RtcIceServer iceServers[])
{
    STATUS retStatus = STATUS_SUCCESS;
    KvsSignalingClientData *pClientData = (KvsSignalingClientData *)pSignalingClient;
    UINT32 i, j, iceConfigCount, uriCount = 0;
    PIceConfigInfo pIceConfigInfo;
    PCHAR pKinesisVideoStunUrlPostFix;
#ifdef CONFIG_USE_ESP_WEBSOCKET_CLIENT
    // A copy: the refresh task can rewrite the cached config while this runs
    PIceConfigInfo pIceConfigCopy = NULL;
#endif

    CHK(pClientData != NULL && pIceConfigCount != NULL && iceServers != NULL, STATUS_NULL_ARG);

    // Check if signaling client handle is valid - if not, fall back to default STUN servers
    if (pClientData->signalingClientHandle == INVALID_SIGNALING_CLIENT_HANDLE_VALUE) {
        ESP_LOGW(TAG, "Signaling client not yet connected, using fallback STUN servers");

        // Use fallback STUN server
        SNPRINTF(iceServers[0].urls, MAX_ICE_CONFIG_URI_LEN,
                 APP_WEBRTC_DEFAULT_STUN_SERVER);

        // Make sure credentials are empty for STUN
        iceServers[0].username[0] = '\0';
        iceServers[0].credential[0] = '\0';

        *pIceConfigCount = 1;
        ESP_LOGI(TAG, "Using fallback STUN server: %s", iceServers[0].urls);
        CHK(FALSE, STATUS_SUCCESS);
    }

    // Set the STUN server
    pKinesisVideoStunUrlPostFix = KINESIS_VIDEO_STUN_URL_POSTFIX;
    // If region is in CN, add CN region uri postfix
    if (STRSTR(pClientData->channelInfo.pRegion, "cn-")) {
        pKinesisVideoStunUrlPostFix = KINESIS_VIDEO_STUN_URL_POSTFIX_CN;
    }

    // Make sure the username and credential are empty for STUN
    iceServers[0].username[0] = '\0';
    iceServers[0].credential[0] = '\0';

    // Use the KVS STUN server URL
    SNPRINTF(iceServers[0].urls, MAX_ICE_CONFIG_URI_LEN, KINESIS_VIDEO_STUN_URL,
             pClientData->channelInfo.pRegion, pKinesisVideoStunUrlPostFix);

    // Get the TURN servers from the configuration
#ifdef CONFIG_USE_ESP_WEBSOCKET_CLIENT
    // From the cache only: a missing or expired config means STUN only, never a fetch here
    retStatus = signaling_copy_ice_config((PSignalingClient) pClientData->signalingClientHandle, 0, NULL, &iceConfigCount);
#else
    retStatus = signalingClientGetIceConfigInfoCount(pClientData->signalingClientHandle, &iceConfigCount);
#endif
    if (STATUS_FAILED(retStatus)) {
        ESP_LOGW(TAG, "Failed to get ice config count, proceeding anyway...");
        retStatus = STATUS_SUCCESS;
        iceConfigCount = 0;
    }

    /* signalingClientGetIceConfigInfoCount can return more than one turn server. Use only one to optimize
     * candidate gathering latency. But user can also choose to use more than 1 turn server. */
    UINT32 maxTurnServer = 1;  // This can be made configurable if needed
    for (uriCount = 0, i = 0; i < maxTurnServer && i < iceConfigCount; i++) {
#ifdef CONFIG_USE_ESP_WEBSOCKET_CLIENT
        if (pIceConfigCopy == NULL) {
            pIceConfigCopy = (PIceConfigInfo) MEMALLOC(SIZEOF(IceConfigInfo));
        }
        retStatus = pIceConfigCopy == NULL ? STATUS_NOT_ENOUGH_MEMORY :
                    signaling_copy_ice_config((PSignalingClient) pClientData->signalingClientHandle, i, pIceConfigCopy, NULL);
        pIceConfigInfo = pIceConfigCopy;
#else
        retStatus = signalingClientGetIceConfigInfo(pClientData->signalingClientHandle, i, &pIceConfigInfo);
#endif
        if (STATUS_FAILED(retStatus)) {
            ESP_LOGW(TAG, "Failed to get ice config, proceeding anyway...");
            retStatus = STATUS_SUCCESS;
            break;
        }
        for (j = 0; j < pIceConfigInfo->uriCount && (uriCount + 1) < MAX_ICE_SERVERS_COUNT; j++) {
            /*
             * if iceServers[uriCount + 1].urls is "turn:ip:port?transport=udp" then ICE will try TURN over UDP
             * if iceServers[uriCount + 1].urls is "turn:ip:port?transport=tcp" then ICE will try TURN over TCP/TLS
             * if iceServers[uriCount + 1].urls is "turns:ip:port?transport=udp", it's currently ignored because sdk dont do TURN
             * over DTLS yet. if iceServers[uriCount + 1].urls is "turns:ip:port?transport=tcp" then ICE will try TURN over TCP/TLS
             * if iceServers[uriCount + 1].urls is "turn:ip:port" then ICE will try both TURN over UDP and TCP/TLS
             *
             * It's recommended to not pass too many TURN iceServers because it will slow down ice gathering in non-trickle mode.
             */

            /* Skip turns:...transport=udp: the SDK has no TURN-over-DTLS, so this URI yields no
             * usable relay candidate and its stalled allocation only slows ICE gathering. */
            if (STRNCMP(pIceConfigInfo->uris[j], "turns:", 6) == 0 && STRSTR(pIceConfigInfo->uris[j], "transport=udp") != NULL) {
                DLOGI("Skipping unusable TURNS-over-UDP URI: %s", pIceConfigInfo->uris[j]);
                continue;
            }

            strlcpy(iceServers[uriCount + 1].urls, pIceConfigInfo->uris[j], MAX_ICE_CONFIG_URI_LEN);
            strlcpy(iceServers[uriCount + 1].credential, pIceConfigInfo->password, MAX_ICE_CONFIG_CREDENTIAL_LEN);
            strlcpy(iceServers[uriCount + 1].username, pIceConfigInfo->userName, MAX_ICE_CONFIG_USER_NAME_LEN);

            uriCount++;
        }
    }

    // Add 1 for the STUN server
    *pIceConfigCount = uriCount + 1;
    DLOGI("Total ICE servers configured: %" PRIu32, *pIceConfigCount);

CleanUp:
#ifdef CONFIG_USE_ESP_WEBSOCKET_CLIENT
    SAFE_MEMFREE(pIceConfigCopy);
#endif
    return retStatus;
}

/**
 * @brief Wrapper for get_ice_server_by_idx to convert return types
 */
static WEBRTC_STATUS kvsQueryServerGetByIdxWrapper(void *pSignalingClient, int index, bool useTurn, uint8_t **data, int *len, bool *have_more)
{
    STATUS retStatus = kvsSignalingQueryServerGetByIdx(pSignalingClient, index, useTurn, data, len, have_more);
    return (retStatus == STATUS_SUCCESS) ? WEBRTC_STATUS_SUCCESS : WEBRTC_STATUS_INTERNAL_ERROR;
}

/**
 * @brief Query ICE server by index (following reference RPC pattern)
 *
 * This function implements the same pattern as app_signaling_queryServer_get_by_idx:
 * - Index 0: Returns STUN server immediately, triggers background TURN refresh if needed
 * - Index 1+: Returns TURN servers from cached ICE configuration
 */
STATUS kvsSignalingQueryServerGetByIdx(PVOID pSignalingClient, int index, bool useTurn, uint8_t **data, int *len, bool *have_more)
{
    STATUS retStatus = STATUS_SUCCESS;
    KvsSignalingClientData *pClientData = (KvsSignalingClientData *)pSignalingClient;
    app_webrtc_ice_server_t* pIceServer = NULL;
    PIceConfigInfo pTurnConfig = NULL;
    UINT32 turnIndex;
#ifdef CONFIG_USE_ESP_WEBSOCKET_CLIENT
    BOOL fetchPending = FALSE;
#endif

    CHK(pClientData != NULL && data != NULL && len != NULL && have_more != NULL, STATUS_NULL_ARG);
    *len = 0;
    *have_more = false;

    if (index == 0) {
        // Allocate ICE server structure
        pIceServer = (app_webrtc_ice_server_t*) MEMCALLOC(1, sizeof(app_webrtc_ice_server_t));
        CHK(pIceServer != NULL, STATUS_NOT_ENOUGH_MEMORY);
        *len = sizeof(app_webrtc_ice_server_t);

        // Set the STUN server (region-aware KVS STUN server)
        PCHAR pKinesisVideoStunUrlPostFix = KINESIS_VIDEO_STUN_URL_POSTFIX;
        // If region is in CN, add CN region uri postfix
        if (pClientData->channelInfo.pRegion != NULL && STRSTR(pClientData->channelInfo.pRegion, "cn-")) {
            pKinesisVideoStunUrlPostFix = KINESIS_VIDEO_STUN_URL_POSTFIX_CN;
        }

        SNPRINTF(pIceServer->urls, APP_WEBRTC_MAX_ICE_CONFIG_URI_LEN, KINESIS_VIDEO_STUN_URL,
                 pClientData->channelInfo.pRegion, pKinesisVideoStunUrlPostFix);
        pIceServer->username[0] = '\0';    // STUN has no username
        pIceServer->credential[0] = '\0';  // STUN has no credential

        if (useTurn && pClientData->signalingClientHandle != INVALID_SIGNALING_CLIENT_HANDLE_VALUE) {
#ifdef CONFIG_USE_ESP_WEBSOCKET_CLIENT
            if (signaling_has_valid_ice_config((PSignalingClient) pClientData->signalingClientHandle)) {
                // Valid config: pass it to the peer connection on the work queue, off the offer path
                DLOGI("ICE configuration is up to date, scheduling an async apply");
                kvs_queue_ice_apply(pClientData);
            } else if (s_iceRefreshTask != NULL) {
                // The refresh task fetches it and applies it. Until then the TURN indexes answer
                // "in progress".
                DLOGI("ICE refresh needed, asking the refresh task");
                pClientData->iceRefreshRequested = TRUE;
                kvs_ice_refresh_wake();
            } else {
                ESP_LOGE(TAG, "ICE refresh task not running; using STUN only");
            }
#else
            // The TURN indexes fetch the config when there is none
            bool refreshNeeded = false;
            if (kvsIsIceRefreshNeededWrapper(pClientData, &refreshNeeded) == WEBRTC_STATUS_SUCCESS && !refreshNeeded) {
                kvs_queue_ice_apply(pClientData);
            }
#endif
        }

        DLOGI("Sending immediate STUN server: %s", pIceServer->urls);
        *have_more = true; // Always ask for more to get TURN servers
        goto CleanUp;
    }

    // Index 1 and up: the TURN servers of the first ICE config
    if (index < 0 || !useTurn || pClientData->signalingClientHandle == INVALID_SIGNALING_CLIENT_HANDLE_VALUE) {
        goto NoMore;
    }

#ifdef CONFIG_USE_ESP_WEBSOCKET_CLIENT
    /* Each query copies the config under the ICE config lock and frees the copy. Nothing is kept
     * between queries, so sessions that query at the same time do not share state. */
    /* Read before the copy, not after it: a fetch that completes in between writes the config,
     * then clears both flags, and a later read would answer "STUN only" with a valid config in
     * the cache. */
    fetchPending = pClientData->iceRefreshRequested || pClientData->iceFetchActive;
    pTurnConfig = (PIceConfigInfo) MEMALLOC(SIZEOF(IceConfigInfo));
    CHK(pTurnConfig != NULL, STATUS_NOT_ENOUGH_MEMORY);
    if (STATUS_FAILED(signaling_copy_ice_config((PSignalingClient) pClientData->signalingClientHandle, 0, pTurnConfig,
                                                NULL))) {
        if (fetchPending) {
            // An empty answer with have_more asks the caller to ask for this index again
            DLOGI("ICE refresh still in progress, asking the caller to wait");
            *have_more = true;
            goto CleanUp;
        }
        ESP_LOGW(TAG, "No valid ICE configuration; using STUN only");
        goto NoMore;
    }
#else
    {
        UINT32 iceConfigCount = 0;
        STATUS status = signalingClientGetIceConfigInfoCount(pClientData->signalingClientHandle, &iceConfigCount);
        if (STATUS_SUCCEEDED(status) && iceConfigCount > 0) {
            status = signalingClientGetIceConfigInfo(pClientData->signalingClientHandle, 0, &pTurnConfig);
        }
        if (STATUS_FAILED(status) || iceConfigCount == 0 || pTurnConfig == NULL) {
            ESP_LOGE(TAG, "Failed to get the ICE config: 0x%08" PRIx32, status);
            pTurnConfig = NULL;
            goto NoMore;
        }
    }
#endif

    turnIndex = (UINT32) index - 1;
    if (turnIndex < pTurnConfig->uriCount) {
        pIceServer = (app_webrtc_ice_server_t*) MEMCALLOC(1, sizeof(app_webrtc_ice_server_t));
        CHK(pIceServer != NULL, STATUS_NOT_ENOUGH_MEMORY);
        *len = sizeof(app_webrtc_ice_server_t);

        STRNCPY(pIceServer->urls, pTurnConfig->uris[turnIndex], APP_WEBRTC_MAX_ICE_CONFIG_URI_LEN);
        pIceServer->urls[APP_WEBRTC_MAX_ICE_CONFIG_URI_LEN - 1] = '\0';
        STRNCPY(pIceServer->username, pTurnConfig->userName, APP_WEBRTC_MAX_ICE_CONFIG_USER_NAME_LEN);
        pIceServer->username[APP_WEBRTC_MAX_ICE_CONFIG_USER_NAME_LEN - 1] = '\0';
        STRNCPY(pIceServer->credential, pTurnConfig->password, APP_WEBRTC_MAX_ICE_CONFIG_CREDENTIAL_LEN);
        pIceServer->credential[APP_WEBRTC_MAX_ICE_CONFIG_CREDENTIAL_LEN - 1] = '\0';

        *have_more = (turnIndex + 1 < pTurnConfig->uriCount);

        DLOGI("Sending TURN server %" PRIu32 ": %s [have_more: %s]", turnIndex, pIceServer->urls,
              *have_more ? "true" : "false");
        goto CleanUp;
    }

NoMore:
    DLOGI("No more ICE servers available for index %d", index);
    *have_more = false;

CleanUp:
#ifdef CONFIG_USE_ESP_WEBSOCKET_CLIENT
    SAFE_MEMFREE(pTurnConfig);
#endif
    if (data != NULL) {
        *data = (uint8_t*) pIceServer;
    }
    return retStatus;
}

/**
 * @brief Set the role type for the signaling client
 */
STATUS setKvsSignalingRoleType(PVOID pSignalingClient, SIGNALING_CHANNEL_ROLE_TYPE role_type)
{
    STATUS retStatus = STATUS_SUCCESS;
    KvsSignalingClientData *pClientData = (KvsSignalingClientData *)pSignalingClient;

    CHK(pClientData != NULL, STATUS_NULL_ARG);

    // Update the channel role type
    pClientData->channelInfo.channelRoleType = role_type;

    // Update the client ID based on role type
    if (role_type == SIGNALING_CHANNEL_ROLE_TYPE_MASTER) {
        STRCPY(pClientData->clientInfo.clientId, DEFAULT_MASTER_CLIENT_ID);
    } else {
        STRCPY(pClientData->clientInfo.clientId, DEFAULT_VIEWER_CLIENT_ID);
    }

CleanUp:
    return retStatus;
}

/**
 * @brief Wrapper function to match the portable interface signature
 */
static WEBRTC_STATUS kvsInitWrapper(void *signaling_cfg, void **ppSignalingClient)
{
    STATUS retStatus = createKvsSignalingClient((kvs_signaling_config_t *) signaling_cfg, ppSignalingClient);
    // Convert KVS SDK STATUS to portable WEBRTC_STATUS
    return (retStatus == STATUS_SUCCESS) ? WEBRTC_STATUS_SUCCESS : WEBRTC_STATUS_INTERNAL_ERROR;
}

/**
 * @brief Wrapper for connect to convert return types
 */
static WEBRTC_STATUS kvsConnectWrapper(void *pSignalingClient)
{
    STATUS retStatus = connectKvsSignalingClient(pSignalingClient);
    return (retStatus == STATUS_SUCCESS) ? WEBRTC_STATUS_SUCCESS : WEBRTC_STATUS_INTERNAL_ERROR;
}

/**
 * @brief Wrapper for disconnect to convert return types
 */
static WEBRTC_STATUS kvsDisconnectWrapper(void *pSignalingClient)
{
    STATUS retStatus = disconnectKvsSignalingClient(pSignalingClient);
    return (retStatus == STATUS_SUCCESS) ? WEBRTC_STATUS_SUCCESS : WEBRTC_STATUS_INTERNAL_ERROR;
}

/**
 * @brief Wrapper for send_message to convert return types
 */
static WEBRTC_STATUS kvsSendMessageWrapper(void *pSignalingClient, webrtc_message_t *pMessage)
{
    STATUS retStatus = sendKvsSignalingMessage(pSignalingClient, pMessage);
    return (retStatus == STATUS_SUCCESS) ? WEBRTC_STATUS_SUCCESS : WEBRTC_STATUS_INTERNAL_ERROR;
}

/**
 * @brief Wrapper for free to convert return types
 */
static WEBRTC_STATUS kvsFreeWrapper(void *pSignalingClient)
{
    STATUS retStatus = freeKvsSignalingClient(pSignalingClient);
    return (retStatus == STATUS_SUCCESS) ? WEBRTC_STATUS_SUCCESS : WEBRTC_STATUS_INTERNAL_ERROR;
}

static WEBRTC_STATUS kvsAbortWrapper(void *pSignalingClient)
{
    STATUS retStatus = abortKvsSignalingClient(pSignalingClient);
    return (retStatus == STATUS_SUCCESS) ? WEBRTC_STATUS_SUCCESS : WEBRTC_STATUS_INTERNAL_ERROR;
}

/**
 * @brief Wrapper for purge_peer_candidates to convert return types
 */
static WEBRTC_STATUS kvsPurgePeerCandidatesWrapper(void *pSignalingClient, const char *peerId)
{
    STATUS retStatus = kvsSignalingPurgePeerCandidates(pSignalingClient, peerId);
    return (retStatus == STATUS_SUCCESS) ? WEBRTC_STATUS_SUCCESS : WEBRTC_STATUS_INTERNAL_ERROR;
}

/**
 * @brief Wrapper for set_callbacks to convert types
 */
static WEBRTC_STATUS kvsSetCallbacksWrapper(void *pSignalingClient,
                                            uint64_t customData,
                                            WEBRTC_STATUS (*on_msg_received)(uint64_t, webrtc_message_t*),
                                            WEBRTC_STATUS (*on_signaling_state_changed)(uint64_t, webrtc_signaling_state_t),
                                            WEBRTC_STATUS (*on_error)(uint64_t, WEBRTC_STATUS, char*, uint32_t))
{
    STATUS retStatus = STATUS_SUCCESS;
    KvsSignalingClientData *pClientData = (KvsSignalingClientData *)pSignalingClient;
    CallbackAdapterData *pAdapterData = NULL;

    CHK(pClientData != NULL, STATUS_NULL_ARG);

    // Allocate adapter data if we have callbacks that need conversion
    if (on_signaling_state_changed != NULL || on_error != NULL) {
        pAdapterData = (CallbackAdapterData *)MEMCALLOC(1, SIZEOF(CallbackAdapterData));
        CHK(pAdapterData != NULL, STATUS_NOT_ENOUGH_MEMORY);

        pAdapterData->originalOnSignalingStateChanged = on_signaling_state_changed;
        pAdapterData->originalOnError = on_error;
        pAdapterData->originalCustomData = (uint64_t)customData;

        // Store adapter data in client data for cleanup
        pClientData->pCallbackAdapterData = pAdapterData;
    }

    retStatus = setKvsSignalingCallbacks(pSignalingClient,
                                       (PVOID)pAdapterData,
                                       (STATUS (*)(UINT64, webrtc_message_t*))(void*)on_msg_received,
                                       on_signaling_state_changed ? adapterStateChangedCallback : NULL,
                                       on_error ? adapterErrorCallback : NULL);

CleanUp:
    if (STATUS_FAILED(retStatus) && pAdapterData != NULL) {
        MEMFREE(pAdapterData);
        if (pClientData != NULL) {
            pClientData->pCallbackAdapterData = NULL;
        }
    }

    return (retStatus == STATUS_SUCCESS) ? WEBRTC_STATUS_SUCCESS : WEBRTC_STATUS_INTERNAL_ERROR;
}

/**
 * @brief Wrapper for set_role_type to convert types
 */
static WEBRTC_STATUS kvsSetRoleTypeWrapper(void *pSignalingClient, webrtc_channel_role_type_t role_type)
{
    // Convert portable role type to KVS role type
    SIGNALING_CHANNEL_ROLE_TYPE kvsRoleType;
    switch (role_type) {
        case WEBRTC_CHANNEL_ROLE_TYPE_MASTER:
            kvsRoleType = SIGNALING_CHANNEL_ROLE_TYPE_MASTER;
            break;
        case WEBRTC_CHANNEL_ROLE_TYPE_VIEWER:
            kvsRoleType = SIGNALING_CHANNEL_ROLE_TYPE_VIEWER;
            break;
        default:
            ESP_LOGW(TAG, "Unknown role type: %d, defaulting to MASTER", role_type);
            kvsRoleType = SIGNALING_CHANNEL_ROLE_TYPE_MASTER;
            break;
    }

    STATUS retStatus = setKvsSignalingRoleType(pSignalingClient, kvsRoleType);
    return (retStatus == STATUS_SUCCESS) ? WEBRTC_STATUS_SUCCESS : WEBRTC_STATUS_INTERNAL_ERROR;
}

/**
 * @brief Wrapper for get_ice_servers to convert types
 *
 * @param pSignalingClient - KVS signaling client instance
 * @param pIceConfigCount - IN/OUT - Number of ICE servers
 * @param pIceServersArray - OUT - Array of ICE servers (RtcIceServer format)
 */
static WEBRTC_STATUS kvsGetIceServersWrapper(void *pSignalingClient, uint32_t *pIceConfigCount, void *pIceServersArray)
{
    STATUS retStatus = STATUS_SUCCESS;

    RtcIceServer *iceServers = MEMCALLOC(MAX_ICE_SERVERS_COUNT, sizeof(RtcIceServer));
    CHK(iceServers != NULL, STATUS_NOT_ENOUGH_MEMORY);

    // Call the KVS function with the local ice servers array
    PUINT32 kvsIceConfigCount = (PUINT32)pIceConfigCount;
    retStatus = getKvsSignalingIceServers(pSignalingClient, kvsIceConfigCount, iceServers);

    uint32_t max_ice_config_count = APP_WEBRTC_MAX_ICE_SERVERS_COUNT;
    if (max_ice_config_count > MAX_ICE_CONFIG_COUNT) {
        max_ice_config_count = MAX_ICE_CONFIG_COUNT;
    }

    if (STATUS_SUCCEEDED(retStatus) && *pIceConfigCount > 0) {
        if (*pIceConfigCount > max_ice_config_count) {
            ESP_LOGW(TAG, "KVS returned %u ICE servers but only %u fit (APP_WEBRTC_MAX_ICE_SERVERS_COUNT); using the first %u",
                     (unsigned) *pIceConfigCount, (unsigned) max_ice_config_count, (unsigned) max_ice_config_count);
            *pIceConfigCount = max_ice_config_count; // Adjust the count to the max allowed
        }
        // Convert field-by-field: the source is RtcIceServer (KVS SDK,
        // urls[256]) but the destination buffer is app_webrtc_ice_server_t
        // (urls[128]) — a different layout. A bulk MEMCPY with RtcIceServer
        // stride misaligns every entry after [0], blanking the TURN URLs and
        // breaking relay (so streaming fails on cellular). Copy per field,
        // bounded by the destination size.
        app_webrtc_ice_server_t *out = (app_webrtc_ice_server_t *)pIceServersArray;
        for (uint32_t i = 0; i < *pIceConfigCount; i++) {
            STRNCPY(out[i].urls, iceServers[i].urls, SIZEOF(out[i].urls) - 1);
            out[i].urls[SIZEOF(out[i].urls) - 1] = '\0';
            STRNCPY(out[i].username, iceServers[i].username, SIZEOF(out[i].username) - 1);
            out[i].username[SIZEOF(out[i].username) - 1] = '\0';
            STRNCPY(out[i].credential, iceServers[i].credential, SIZEOF(out[i].credential) - 1);
            out[i].credential[SIZEOF(out[i].credential) - 1] = '\0';
        }
    }

CleanUp:
    SAFE_MEMFREE(iceServers);
    return (retStatus == STATUS_SUCCESS) ? WEBRTC_STATUS_SUCCESS : WEBRTC_STATUS_INTERNAL_ERROR;
}

/**
 * @brief Set ICE update callback for progressive ICE server delivery
 */
static WEBRTC_STATUS kvsSetIceUpdateCallbackWrapper(void *pSignalingClient,
                                                    uint64_t customData,
                                                    WEBRTC_STATUS (*on_ice_servers_updated)(uint64_t, uint32_t))
{
    STATUS retStatus = STATUS_SUCCESS;
    KvsSignalingClientData *pClientData = (KvsSignalingClientData *)pSignalingClient;

    CHK(pClientData != NULL, STATUS_NULL_ARG);

    // Store the callback context
    pClientData->ice_callback_ctx.customData = customData;
    pClientData->ice_callback_ctx.on_ice_servers_updated = on_ice_servers_updated;

    ESP_LOGD(TAG, "ICE update callback set successfully");

CleanUp:
    return convertStatusToWebrtcStatus(retStatus);
}

/**
 * @brief Report whether a cached ICE configuration can be read without going on the wire
 *
 * Used by callers that want STUN+TURN before they create a peer connection but must
 * not block. Answering TRUE means signalingClientGetIceConfigInfo() will be served
 * from the cache; see signaling_has_valid_ice_config() for why the negation of
 * is_ice_refresh_needed is not a substitute.
 */
static WEBRTC_STATUS kvsHasValidIceConfigWrapper(void *pSignalingClient, bool *pHasValidConfig)
{
    if (pHasValidConfig == NULL) {
        return WEBRTC_STATUS_NULL_ARG;
    }

    *pHasValidConfig = false;

    KvsSignalingClientData *pClientData = (KvsSignalingClientData *)pSignalingClient;
    if (pClientData == NULL || pClientData->signalingClientHandle == INVALID_SIGNALING_CLIENT_HANDLE_VALUE) {
        return WEBRTC_STATUS_SUCCESS;
    }

#ifdef CONFIG_USE_ESP_WEBSOCKET_CLIENT
    *pHasValidConfig =
        (signaling_has_valid_ice_config((PSignalingClient) pClientData->signalingClientHandle) == TRUE);
#else
    /* Without the ESP port layer there is no way to inspect the cached config's
     * expiry without a call that may refresh it, so report FALSE and let the
     * caller take its non-blocking path. */
    *pHasValidConfig = false;
#endif

    return WEBRTC_STATUS_SUCCESS;
}

/**
 * @brief Wrapper for is_ice_refresh_needed to convert return types
 */
static WEBRTC_STATUS kvsIsIceRefreshNeededWrapper(void *pSignalingClient, bool *refreshNeeded)
{
    if (refreshNeeded == NULL) {
        return WEBRTC_STATUS_NULL_ARG;
    }

    KvsSignalingClientData *pClientData = (KvsSignalingClientData *)pSignalingClient;
    if (pClientData == NULL || pClientData->signalingClientHandle == INVALID_SIGNALING_CLIENT_HANDLE_VALUE) {
        // If no signaling client, assume refresh is needed
        *refreshNeeded = true;
        return WEBRTC_STATUS_SUCCESS;
    }

#ifdef CONFIG_USE_ESP_WEBSOCKET_CLIENT
    // Use ESP signaling specific function if available
    BOOL refreshResult = signaling_is_ice_config_refresh_needed((PSignalingClient)pClientData->signalingClientHandle);
    *refreshNeeded = (refreshResult == TRUE);
#else
    // Default implementation: Use KVS signaling APIs to check ICE config availability
    UINT32 iceConfigCount = 0;
    STATUS status = signalingClientGetIceConfigInfoCount(pClientData->signalingClientHandle, &iceConfigCount);

    if (STATUS_FAILED(status) || iceConfigCount == 0) {
        // If we can't get ICE config count or there are no configs, refresh is needed
        *refreshNeeded = true;
    } else {
        // If we have ICE configurations, assume they're valid for now
        // (More sophisticated expiration checking would require access to internal state)
        *refreshNeeded = false;
    }
#endif

    return WEBRTC_STATUS_SUCCESS;
}

static WEBRTC_STATUS kvsRefreshIceConfigurationWrapper(void *pSignalingClient)
{
    KvsSignalingClientData *pClientData = (KvsSignalingClientData *)pSignalingClient;
    if (pClientData == NULL || pClientData->signalingClientHandle == INVALID_SIGNALING_CLIENT_HANDLE_VALUE) {
        ESP_LOGE(TAG, "Invalid signaling client for ICE refresh");
        return WEBRTC_STATUS_INVALID_ARG;
    }

#ifdef CONFIG_USE_ESP_WEBSOCKET_CLIENT
    // Non-blocking, as app_webrtc expects: the refresh task does the fetch
    if (s_iceRefreshTask == NULL) {
        ESP_LOGE(TAG, "ICE refresh task not running");
        return WEBRTC_STATUS_INTERNAL_ERROR;
    }
    pClientData->iceRefreshRequested = TRUE;
    xTaskNotifyGive(s_iceRefreshTask);
    ESP_LOGI(TAG, "ICE configuration refresh requested");
#else
    ESP_LOGE(TAG, "ICE refresh is not supported in default websocket client mode, skipping...");
#endif

    return WEBRTC_STATUS_SUCCESS;
}

static WEBRTC_STATUS kvsGetStateWrapper(void *pSignalingClient, webrtc_signaling_state_t *pState)
{
    if (pSignalingClient == NULL || pState == NULL) {
        return WEBRTC_STATUS_INVALID_ARG;
    }

    KvsSignalingClientData *pClientData = (KvsSignalingClientData *)pSignalingClient;

    /* Query the actual KVS signaling client state using the public API */
    if (pClientData->signalingClientHandle != INVALID_SIGNALING_CLIENT_HANDLE_VALUE) {
        SIGNALING_CLIENT_STATE kvsState = SIGNALING_CLIENT_STATE_UNKNOWN;
        STATUS status = signalingClientGetCurrentState(pClientData->signalingClientHandle, &kvsState);

        if (STATUS_SUCCEEDED(status)) {
            *pState = convertKvsToWebrtcSignalingState(kvsState);
        } else {
            ESP_LOGW(TAG, "Failed to get signaling client state: 0x%08" PRIx32, (UINT32)status);
            /* Fallback: use cached connection flag */
            if (pClientData->connected) {
                *pState = WEBRTC_SIGNALING_STATE_CONNECTED;
            } else if (pClientData->initialized) {
                *pState = WEBRTC_SIGNALING_STATE_DISCONNECTED;
            } else {
                *pState = WEBRTC_SIGNALING_STATE_NEW;
            }
        }
    } else {
        /* Client not initialized yet */
        *pState = WEBRTC_SIGNALING_STATE_NEW;
    }

    return WEBRTC_STATUS_SUCCESS;
}

/**
 * @brief Get the KVS signaling client interface
 */
webrtc_signaling_client_if_t* kvs_signaling_client_if_get(void)
{
    static webrtc_signaling_client_if_t kvs_signaling_client_if = {
        .init = kvsInitWrapper,
        .connect = kvsConnectWrapper,
        .disconnect = kvsDisconnectWrapper,
        .send_message = kvsSendMessageWrapper,
        .free = kvsFreeWrapper,
        .set_callbacks = kvsSetCallbacksWrapper,
        .set_role_type = kvsSetRoleTypeWrapper,
        .get_ice_servers = kvsGetIceServersWrapper,
        .get_ice_server_by_idx = kvsQueryServerGetByIdxWrapper,
        .is_ice_refresh_needed = kvsIsIceRefreshNeededWrapper,
        .has_valid_ice_config = kvsHasValidIceConfigWrapper,
        .refresh_ice_configuration = kvsRefreshIceConfigurationWrapper,
        .set_ice_update_callback = kvsSetIceUpdateCallbackWrapper,
        .get_state = kvsGetStateWrapper,
        .purge_peer_candidates = kvsPurgePeerCandidatesWrapper,
        .abort = kvsAbortWrapper
    };

    return &kvs_signaling_client_if;
}
