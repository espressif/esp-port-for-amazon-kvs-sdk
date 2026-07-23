/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file kvs_media.c
 * @brief KVS Media handling implementation
 *
 * This file implements all media-related functionality for KVS WebRTC,
 * separated from the peer connection logic in kvs_webrtc.c for better
 * code organization and maintainability.
 *
 * Features implemented:
 * - Media capture and transmission (camera/microphone)
 * - Media reception and playback (video/audio players)
 * - Sample file fallback for development/testing
 * - Frame processing and handling
 */

#include <inttypes.h>
#include "esp_timer.h"
#include "esp_log.h"
#include "kvs_media.h"
#include "kvs_webrtc_internal.h"  // For session structure access
#include "media_stream.h"
#include "video_rate_ctrl.h"
#include "fileio.h"
#include <com/amazonaws/kinesis/video/webrtcclient/Include.h>

static const char *TAG = "kvs_media";

/* Global media state following KVS official pattern */
static struct {
    void* client_data;                    // KVS client for session iteration
    kvs_media_config_t config;           // Global media configuration
    TID video_sender_tid;                // Global video thread
    TID audio_sender_tid;                // Global audio thread
    BOOL global_media_started;           // Global media threads running
    volatile ATOMIC_BOOL terminated;     // Global termination flag
    MUTEX global_media_mutex;            // Protects global state

    // Media capture handles (shared)
    void* video_handle;
    void* audio_handle;

    // Frame buffers (shared across all sessions)
    PBYTE video_frame_buffer;
    PBYTE audio_frame_buffer;
    UINT32 video_buffer_size;
    UINT32 audio_buffer_size;
} g_global_media = {0};

// Sample file fallback settings
#define KVS_SAMPLE_VIDEO_FRAME_DURATION (HUNDREDS_OF_NANOS_IN_A_SECOND / 30)  // 30 FPS
#define KVS_SAMPLE_AUDIO_FRAME_DURATION (20 * HUNDREDS_OF_NANOS_IN_A_MILLISECOND)  // 20ms audio frames
#define KVS_NUMBER_OF_OPUS_FRAME_FILES 20  // Number of OPUS sample files to cycle through
#define KVS_MAX_PATH_LEN 256

/* Forward declarations for internal functions */
// Functions exported in kvs_media.h

/* Global media thread functions - following KVS official pattern */
static PVOID kvs_global_video_sender_thread(PVOID args);
static PVOID kvs_global_audio_sender_thread(PVOID args);
static STATUS kvs_iterate_sessions_send_frame(Frame* frame, BOOL is_video);

/* Sample file fallback functions (used by global threads) */
// Note: Old per-session functions removed - replaced by global media threads

// Note: Old per-session media functions removed - functionality moved to global threads

/* Global media implementation following KVS official pattern */

/**
 * @brief Initialize global media state (call once at startup)
 */
STATUS kvs_media_init_shared_state(void)
{
    STATUS retStatus = STATUS_SUCCESS;

    CHK(!IS_VALID_MUTEX_VALUE(g_global_media.global_media_mutex), STATUS_INVALID_OPERATION);

    // Clear the state first
    MEMSET(&g_global_media, 0x00, SIZEOF(g_global_media));

    // Then initialize properly
    g_global_media.global_media_mutex = MUTEX_CREATE(FALSE);
    CHK_ERR(IS_VALID_MUTEX_VALUE(g_global_media.global_media_mutex), STATUS_NOT_ENOUGH_MEMORY, "Failed to create global media mutex");

    g_global_media.video_sender_tid = INVALID_TID_VALUE;
    g_global_media.audio_sender_tid = INVALID_TID_VALUE;
    g_global_media.global_media_started = FALSE;
    ATOMIC_STORE_BOOL(&g_global_media.terminated, FALSE);

CleanUp:
    return retStatus;
}

/**
 * @brief Cleanup global media state (call once at shutdown)
 */
void kvs_media_cleanup_shared_state(void)
{
    if (IS_VALID_MUTEX_VALUE(g_global_media.global_media_mutex)) {
        MUTEX_FREE(g_global_media.global_media_mutex);
        g_global_media.global_media_mutex = INVALID_MUTEX_VALUE;
    }

    // Free global frame buffers
    SAFE_MEMFREE(g_global_media.video_frame_buffer);
    SAFE_MEMFREE(g_global_media.audio_frame_buffer);

    MEMSET(&g_global_media, 0x00, SIZEOF(g_global_media));
}

/**
 * @brief Get the global video capture handle
 *
 * Provides controlled access to the global video handle for external modules
 * that need to perform video capture operations (e.g., bitrate control).
 *
 * @return void* Global video capture handle, or NULL if not initialized
 */
void* kvs_media_get_global_video_handle(void)
{
    return g_global_media.video_handle;
}

/**
 * @brief Adjust video bitrate dynamically based on network conditions
 *
 * Public API for coordinated bitrate management across the system.
 * Used by both adaptive bitrate logic and bandwidth estimation handlers.
 *
 * @param video_capture Video capture interface
 * @param handle Video capture handle
 * @param adjustment_kbps Bitrate adjustment in kbps (positive to increase, negative to decrease)
 * @param min_bitrate_kbps Minimum allowed bitrate
 * @param max_bitrate_kbps Maximum allowed bitrate
 * @param reason Reason for adjustment (for logging)
 * @param current_bitrate Pointer to current bitrate variable (will be updated)
 * @return true if bitrate was adjusted, false otherwise
 */
bool kvs_media_adjust_video_bitrate(media_stream_video_capture_t *video_capture,
                                    video_capture_handle_t handle,
                                    int32_t adjustment_kbps,
                                    uint32_t min_bitrate_kbps,
                                    uint32_t max_bitrate_kbps,
                                    const char *reason,
                                    uint32_t *current_bitrate)
{
    if (video_capture == NULL || handle == NULL || current_bitrate == NULL) {
        return false;
    }

    /* Check if bitrate control APIs are available */
    if (video_capture->get_bitrate == NULL || video_capture->set_bitrate == NULL) {
        return false;
    }

    /* Get current bitrate */
    uint32_t old_bitrate = 0;
    if (video_capture->get_bitrate(handle, &old_bitrate) != ESP_OK) {
        return false;
    }

    /* Calculate new bitrate */
    int32_t new_bitrate_signed = (int32_t)old_bitrate + adjustment_kbps;
    uint32_t new_bitrate = (uint32_t)(new_bitrate_signed < 0 ? 0 : new_bitrate_signed);

    /* Apply bounds */
    if (new_bitrate < min_bitrate_kbps) {
        new_bitrate = min_bitrate_kbps;
    }

    if (new_bitrate > max_bitrate_kbps) {
        new_bitrate = max_bitrate_kbps;
    }

    /* Set new bitrate if changed */
    if (new_bitrate != old_bitrate) {
        if (video_capture->set_bitrate(handle, new_bitrate) == ESP_OK) {
            ESP_LOGI(TAG, "Adjusted bitrate: %lu -> %lu kbps (%s)",
                    (unsigned long)old_bitrate, (unsigned long)new_bitrate, reason);
            *current_bitrate = new_bitrate;
            return true;
        }
    }

    return false;
}

/**
 * @brief Global video sender thread - equivalent to sendVideoPackets in kvsWebRTCClientMaster.c
 * Captures video frames and sends them to ALL active sessions
 */
static PVOID kvs_global_video_sender_thread(PVOID args)
{
    STATUS retStatus = STATUS_SUCCESS;
    Frame frame;
    UINT64 startTime;
    media_stream_video_capture_t *video_capture = NULL;
    video_frame_t *video_frame = NULL;
    UINT64 frame_duration_100ns = KVS_SAMPLE_VIDEO_FRAME_DURATION; // Default 30 fps
    UINT64 last_send_time_us = 0; // Track last frame send time for rate control
    UINT32 target_fps = 30; // Default FPS
    UINT64 frame_interval_us = 0; // Frame interval in microseconds

#if KVS_MEDIA_ENABLE_ADAPTIVE_BITRATE
    /* Adaptive bitrate control variables */
    UINT32 current_bitrate_kbps = 0;
    UINT32 smooth_frame_count = 0;
#endif

    ESP_LOGD(TAG, "Global video sender thread started");

    // Camera hardware and streaming are already initialized synchronously in kvs_media_start_global_transmission()
    // Thread just needs to check if camera is available and get frames
    if (g_global_media.config.video_capture != NULL && g_global_media.video_handle != NULL) {
        video_capture = (media_stream_video_capture_t*)g_global_media.config.video_capture;

        // Calculate frame duration from configured FPS
        target_fps = g_global_media.config.video_fps > 0 ? g_global_media.config.video_fps : 30;
        frame_duration_100ns = HUNDREDS_OF_NANOS_IN_A_SECOND / target_fps;
        frame_interval_us = frame_duration_100ns / 10; // Convert 100ns to microseconds
        ESP_LOGI(TAG, "Video frame rate control: %" PRIu32 " fps (frame interval: %" PRIu64 " us)",
                 target_fps, frame_interval_us);

#if KVS_MEDIA_ENABLE_ADAPTIVE_BITRATE
        /* Get initial bitrate if get_bitrate is available */
        if (video_capture->get_bitrate != NULL) {
            if (video_capture->get_bitrate(g_global_media.video_handle, &current_bitrate_kbps) == ESP_OK) {
                ESP_LOGI(TAG, "Initial video bitrate: %lu kbps", (unsigned long)current_bitrate_kbps);
            } else {
                current_bitrate_kbps = 500;  /* Default bitrate */
            }
        } else {
            current_bitrate_kbps = 500;  /* Default bitrate */
        }
#endif
    } else {
        goto CleanupVideo;
    }

    /* NOTE: Camera hardware initialization happens synchronously in kvs_media_start_global_transmission()
     * BEFORE this thread starts. This thread only starts camera streaming (hardware already initialized). */

    frame.version = FRAME_CURRENT_VERSION;
    frame.trackId = DEFAULT_VIDEO_TRACK_ID;
    frame.duration = 0;
    frame.index = 0;
    frame.presentationTs = 0;

    /* esp_timer_get_time() returns microseconds since boot. Not affected by time sync */
    startTime = esp_timer_get_time() * HUNDREDS_OF_NANOS_IN_A_MICROSECOND;
    UINT64 video_frame_index = 0;  /* Local frame counter for video */
    last_send_time_us = 0; // Initialize frame timing

    while (!ATOMIC_LOAD_BOOL(&g_global_media.terminated)) {
        BOOL frame_available = FALSE;

        if (video_capture != NULL && g_global_media.video_handle != NULL) {
            // Get frame from camera/file (wait up to frame_duration_ms for a frame)
            UINT32 timeout_ms = (UINT32)(frame_duration_100ns / HUNDREDS_OF_NANOS_IN_A_MILLISECOND);
            esp_err_t get_ret = video_capture->get_frame(g_global_media.video_handle, &video_frame, timeout_ms);
            if (get_ret == ESP_OK && video_frame != NULL) {
                frame.frameData = video_frame->buffer;
                frame.size = video_frame->len;
                frame.flags = (video_frame->type == VIDEO_FRAME_TYPE_I) ? FRAME_FLAG_KEY_FRAME : FRAME_FLAG_NONE;
                frame_available = TRUE;
            }
        }

        if (frame_available) {
            video_frame_index++;
            frame.index = (UINT32)video_frame_index;

            /* Use real-time timestamp - matches actual production rate. Not affected by time sync */
            frame.presentationTs = (esp_timer_get_time() * HUNDREDS_OF_NANOS_IN_A_MICROSECOND) - startTime;
            frame.decodingTs = frame.presentationTs;

            /* TX H.264 NAL accounting on key frames. Inline Annex-B scan
             * rather than cross-component dep on media_stream's h264_nalu_utils.
             *
             * What we want to know:
             *  1. The encoder profile we're putting on the wire (one-shot,
             *     logged on first/changed).
             *  2. Whether SPS *and* PPS are being repeated on EVERY IDR.
             *     If not, an Android HW decoder will rebind state at the
             *     first IDR and then drop every subsequent P-frame because
             *     it can't trust the ref chain — phone shows ~GOP-rate fps
             *     (1 fps observed at GOP=20 / 22 fps source).
             *
             * Counters dumped every 50 IDRs alongside total/has-SPS/has-PPS. */
            static UINT8  last_profile_idc, last_constraint, last_level;
            static BOOL   seen_profile;
            static UINT32 idr_total, idr_with_sps, idr_with_pps;
            if ((frame.flags & FRAME_FLAG_KEY_FRAME) != FRAME_FLAG_NONE && frame.frameData && frame.size > 4) {
                idr_total++;
                BOOL has_sps_this_frame = FALSE;
                BOOL has_pps_this_frame = FALSE;
                const UINT8 *d = frame.frameData;
                UINT32 n = frame.size;
                /* Walk all Annex-B NALs in this access unit. */
                for (UINT32 i = 0; i + 4 < n; ++i) {
                    if (d[i] != 0 || d[i + 1] != 0) continue;
                    UINT32 hdr_off;
                    if (d[i + 2] == 1)                       hdr_off = i + 3;
                    else if (d[i + 2] == 0 && d[i + 3] == 1) hdr_off = i + 4;
                    else continue;
                    if (hdr_off >= n) break;
                    UINT8 nal_type = d[hdr_off] & 0x1F;
                    if (nal_type == 7 /* SPS */) {
                        has_sps_this_frame = TRUE;
                        if (hdr_off + 3 < n &&
                            (!seen_profile ||
                             d[hdr_off + 1] != last_profile_idc ||
                             d[hdr_off + 2] != last_constraint ||
                             d[hdr_off + 3] != last_level)) {
                            UINT8 prof = d[hdr_off + 1];
                            UINT8 cons = d[hdr_off + 2];
                            UINT8 lev  = d[hdr_off + 3];
                            ESP_LOGI(TAG, "sending SPS: profile_idc=0x%02x constraint=0x%02x level_idc=%u (%s)",
                                     prof, cons, (unsigned)lev,
                                     prof == 66  ? "Baseline" :
                                     prof == 77  ? "Main"     :
                                     prof == 88  ? "Extended" :
                                     prof == 100 ? "High"     :
                                     prof == 110 ? "High10"   :
                                     prof == 122 ? "High422"  : "Unknown");
                            last_profile_idc = prof;
                            last_constraint  = cons;
                            last_level       = lev;
                            seen_profile     = TRUE;
                        }
                    } else if (nal_type == 8 /* PPS */) {
                        has_pps_this_frame = TRUE;
                    }
                }
                if (has_sps_this_frame) idr_with_sps++;
                if (has_pps_this_frame) idr_with_pps++;
                if (idr_total % 50 == 0) {
                    ESP_LOGI(TAG, "TX H.264 IDR accounting: total=%" PRIu32 " w/SPS=%" PRIu32 " w/PPS=%" PRIu32 " (%.0f%% / %.0f%%)",
                             idr_total, idr_with_sps, idr_with_pps,
                             100.0 * idr_with_sps / idr_total,
                             100.0 * idr_with_pps / idr_total);
                }
            }

            /* Log every 100 frames to monitor FPS and frame drops */
            if (video_frame_index % 100 == 0) {
                UINT64 pts_ms = frame.presentationTs / HUNDREDS_OF_NANOS_IN_A_MILLISECOND;
                DOUBLE actual_fps = video_frame_index * 1000.0 / (DOUBLE)pts_ms;
                ESP_LOGD(TAG, "Video: frame %" PRIu64 ", pts=%" PRIu64 "ms, fps=%.2f", video_frame_index, pts_ms, actual_fps);
            }
        }

        if (frame_available) {
            // Check termination before send to avoid blocking on shutdown
            if (ATOMIC_LOAD_BOOL(&g_global_media.terminated)) {
                ESP_LOGD(TAG, "Video thread: termination observed before send - breaking loop");
                break;
            }

            // Send frame to ALL sessions - measure time to detect bottleneck
            UINT64 sendStart = GETTIME();
            STATUS send_status = kvs_iterate_sessions_send_frame(&frame, TRUE);
            UINT64 sendDuration = GETTIME() - sendStart;

            if (STATUS_FAILED(send_status)) {
                ESP_LOGW(TAG, "Failed to send frame to sessions: 0x%08" PRIx32, (UINT32)send_status);
            }

            // Get end time after send completes
            UINT64 send_end_time_us = esp_timer_get_time();

            /* Feed the adaptive rate controller: this video frame's send
             * duration is the congestion signal. The controller adjusts
             * target fps (grabber skips frames) and, at the fps floor,
             * encoder bitrate. Supersedes the old fixed-fps send pacing. */
            video_rate_ctrl_report_send_ms((uint32_t)(sendDuration / HUNDREDS_OF_NANOS_IN_A_MILLISECOND));

#if KVS_MEDIA_ENABLE_ADAPTIVE_BITRATE
            /* Adaptive bitrate control based on send performance */
            if (sendDuration > 50 * HUNDREDS_OF_NANOS_IN_A_MILLISECOND) {  // >50ms is concerning
                ESP_LOGW(TAG, "Slow video frame send: %" PRIu64 "ms (frame %" PRIu64 "), size: %" PRIu32,
                         sendDuration / HUNDREDS_OF_NANOS_IN_A_MILLISECOND, video_frame_index, frame.size);

                /* Reduce bitrate using configured step */
                if (kvs_media_adjust_video_bitrate(video_capture, g_global_media.video_handle,
                                            -(int32_t)KVS_MEDIA_BITRATE_STEP_KBPS,
                                            KVS_MEDIA_MIN_BITRATE_KBPS, KVS_MEDIA_MAX_BITRATE_KBPS,
                                            "slow send detected",
                                            &current_bitrate_kbps)) {
                    smooth_frame_count = 0; /* Reset smooth counter after bitrate reduction */
                }
            } else {
                /* Frame sent smoothly - increment counter */
                smooth_frame_count++;

                /* Increase bitrate if we've had smooth sends for a while */
                if (smooth_frame_count >= KVS_MEDIA_SMOOTH_FRAMES_THRESHOLD) {
                    /* Increase bitrate using configured step */
                    if (kvs_media_adjust_video_bitrate(video_capture, g_global_media.video_handle,
                                                (int32_t)KVS_MEDIA_BITRATE_STEP_KBPS,
                                                KVS_MEDIA_MIN_BITRATE_KBPS, KVS_MEDIA_MAX_BITRATE_KBPS,
                                                "smooth streaming",
                                                &current_bitrate_kbps)) {
                        /* Bitrate was successfully increased */
                    }
                    /* Reset smooth counter regardless of adjustment success */
                    smooth_frame_count = 0;
                }
            }
#else
            /* Adaptive bitrate disabled - just log slow sends without adjustment */
            if (sendDuration > 50 * HUNDREDS_OF_NANOS_IN_A_MILLISECOND) {
                ESP_LOGW(TAG, "Slow video frame send: %" PRIu64 "ms (frame %" PRIu64 "), size: %" PRIu32,
                         sendDuration / HUNDREDS_OF_NANOS_IN_A_MILLISECOND, video_frame_index, frame.size);
            }
#endif

            /* Frame-rate limiting is now owned by video_rate_ctrl at the
             * grabber (skip-before-encode), which also saves encoder CPU.
             * The old fixed-fps send-side sleep here would double-limit and
             * delay already-encoded frames, so it's retired. last_send_time
             * kept only for diagnostics if needed. */
            (void)last_send_time_us;
            (void)frame_interval_us;
            last_send_time_us = send_end_time_us;

            // Release camera frame if used
            if (video_capture != NULL && video_frame != NULL) {
                video_capture->release_frame(g_global_media.video_handle, video_frame);
                video_frame = NULL;
            }
        } else {
            // No frame available - sleep a bit to avoid busy waiting
            THREAD_SLEEP(frame_duration_100ns / 4); // Sleep quarter frame duration
        }
    }

CleanupVideo:
    // Cleanup - release any pending frame
    // Note: Video capture stop and deinit are handled in kvs_media_stop_global_transmission after thread joins
    if (video_capture != NULL && g_global_media.video_handle != NULL) {
        if (video_frame != NULL) {
            // If the frame was not used/released, release it now to avoid memory leak
            video_capture->release_frame(g_global_media.video_handle, video_frame);
        }
        // Don't stop or deinit here - both happen in kvs_media_stop_global_transmission
    }

    ESP_LOGD(TAG, "Global video sender thread finished");
    return (PVOID) (uintptr_t) retStatus;
}

/**
 * @brief Global audio sender thread - equivalent to sendAudioPackets in kvsWebRTCClientMaster.c
 * Captures audio frames and sends them to ALL active sessions
 */
static PVOID kvs_global_audio_sender_thread(PVOID args)
{
    STATUS retStatus = STATUS_SUCCESS;
    Frame frame;
    UINT64 startTime;
    media_stream_audio_capture_t *audio_capture = NULL;
    audio_frame_t *audio_frame = NULL;
    UINT64 frame_duration_100ns = KVS_SAMPLE_AUDIO_FRAME_DURATION; // Default 20ms
    UINT32 frame_duration_ms = 20; // Default audio frame duration

    ESP_LOGD(TAG, "Global audio sender thread started");

    // Audio hardware and streaming are already initialized synchronously in kvs_media_start_global_transmission()
    // Thread just needs to check if audio is available and get frames
    if (g_global_media.config.audio_capture != NULL && g_global_media.audio_handle != NULL) {
        audio_capture = (media_stream_audio_capture_t*)g_global_media.config.audio_capture;

        frame_duration_ms = 20; // Default audio frame duration
        frame_duration_100ns = frame_duration_ms * HUNDREDS_OF_NANOS_IN_A_MILLISECOND;
        ESP_LOGI(TAG, "Audio sender: %" PRIu32 " ms per frame, wall-clock timestamps",
                 frame_duration_ms);
    } else {
        goto CleanupAudio;
    }

    /* NOTE: Audio hardware initialization happens synchronously in kvs_media_start_global_transmission()
     * BEFORE this thread starts. This thread only starts audio streaming (hardware already initialized). */

    frame.version = FRAME_CURRENT_VERSION;
    frame.trackId = DEFAULT_AUDIO_TRACK_ID;
    frame.duration = 0;
    frame.index = 0;
    frame.flags = FRAME_FLAG_NONE;
    frame.presentationTs = 0;

    /* esp_timer_get_time() returns microseconds since boot. Not affected by time sync */
    startTime = esp_timer_get_time() * HUNDREDS_OF_NANOS_IN_A_MICROSECOND;
    UINT64 audio_frame_index = 0;  /* Local frame counter for audio */

    while (!ATOMIC_LOAD_BOOL(&g_global_media.terminated)) {
        BOOL frame_available = FALSE;

        if (audio_capture != NULL) {
            // Get frame from microphone/file (wait up to frame_duration_ms)
            if (audio_capture->get_frame(g_global_media.audio_handle, &audio_frame, frame_duration_ms) == ESP_OK && audio_frame != NULL) {
                frame.frameData = audio_frame->buffer;
                frame.size = audio_frame->len;
                frame_available = TRUE;
            }
        }

        if (frame_available) {
            audio_frame_index++;
            frame.index = (UINT32)audio_frame_index;

            /* Use wall-clock timestamps so audio and video share the same time
             * reference — counter-based audio PTS (uniform 20 ms increments)
             * drifts away from video's wall-clock PTS whenever the audio
             * encoder doesn't run *exactly* every 20 ms (which it cannot
             * guarantee under bidirectional load). The receiver schedules
             * presentation off the SR's NTP/RTP pair derived from these PTS;
             * once the two tracks' clocks diverge, the phone discards video
             * frames as "late/early" relative to audio and never recovers.
             * The previous concern (jitter on the receive side) is mitigated
             * by the fact that get_frame() is paced by I2S DMA, so encode
             * time follows capture time within a small bounded delta. */
            frame.presentationTs = (esp_timer_get_time() * HUNDREDS_OF_NANOS_IN_A_MICROSECOND) - startTime;
            frame.decodingTs = frame.presentationTs;

            /* PTS divergence diagnostic: every 100 frames print audio_pts,
             * approx-counter-based-pts, and the delta — confirms wall-clock
             * stays bounded vs the alternative counter timeline. */
#if CONFIG_KVS_WEBRTC_MEDIA_DIAG_LOG
            if (audio_frame_index % 100 == 0) {
                UINT64 audio_pts_us = frame.presentationTs / HUNDREDS_OF_NANOS_IN_A_MICROSECOND;
                UINT64 counter_pts_us = (audio_frame_index * frame_duration_100ns) / HUNDREDS_OF_NANOS_IN_A_MICROSECOND;
                INT64 delta_us = (INT64)audio_pts_us - (INT64)counter_pts_us;
                ESP_LOGI(TAG, "PTS audio: idx=%" PRIu64 " wall=%" PRIu64 "us counter=%" PRIu64 "us delta=%+lldus",
                         audio_frame_index, audio_pts_us, counter_pts_us, (long long)delta_us);
            }
#endif

            // Check termination before send to avoid blocking on shutdown
            if (ATOMIC_LOAD_BOOL(&g_global_media.terminated)) {
                ESP_LOGD(TAG, "Audio thread: termination observed before send - breaking loop");
                break;
            }

            // Send frame to ALL sessions - measure time to detect bottleneck
            UINT64 sendStart = GETTIME();
            STATUS send_status = kvs_iterate_sessions_send_frame(&frame, FALSE);
            UINT64 sendDuration = GETTIME() - sendStart;

            if (STATUS_FAILED(send_status)) {
                ESP_LOGW(TAG, "Failed to send frame to sessions: 0x%08" PRIx32, (UINT32)send_status);
            }

            /* Log if frame send is slow (bottleneck detection) */
            if (sendDuration > 20 * HUNDREDS_OF_NANOS_IN_A_MILLISECOND) {
                ESP_LOGW(TAG, "Slow audio frame send: %" PRIu64 "ms (frame %" PRIu64 "), size: %" PRIu32,
                         sendDuration / HUNDREDS_OF_NANOS_IN_A_MILLISECOND, audio_frame_index, frame.size);
            }
            /* No rate-control sleep here: get_frame() already blocks on xQueueReceive
             * until the I2S DMA produces the next 20ms frame — natural pacing. Adding a
             * sleep on top would push dequeue later, increasing timestamp jitter. */

            // Release microphone frame if used
            if (audio_capture != NULL && audio_frame != NULL) {
                audio_capture->release_frame(g_global_media.audio_handle, audio_frame);
                audio_frame = NULL;
            }
        } else {
            // No frame available - sleep a bit to avoid busy waiting
            THREAD_SLEEP(frame_duration_100ns / 4); // Sleep quarter frame duration
        }
    }

CleanupAudio:
    // Cleanup - release any pending frame
    // Note: Audio capture stop and deinit are handled in kvs_media_stop_global_transmission after thread joins
    if (audio_capture != NULL && g_global_media.audio_handle != NULL) {
        if (audio_frame != NULL) {
            // If the frame was not used/released, release it now to avoid memory leak
            audio_capture->release_frame(g_global_media.audio_handle, audio_frame);
        }
        // Don't stop or deinit here - both happen in kvs_media_stop_global_transmission
    }

    ESP_LOGD(TAG, "Global audio sender thread finished");
    return (PVOID) (uintptr_t) retStatus;
}

/**
 * @brief Callback function for session iteration - called for each active session
 * This implements the writeFrame call from the official KVS pattern
 */
static STATUS kvs_session_frame_callback(UINT64 callerData, PHashEntry pHashEntry)
{
    static UINT32 diag_drop_reason_count[8] = {0};  /* Diagnostic counters */
    static UINT64 diag_last_log_time = 0;
    Frame* frame = (Frame*)HANDLE_TO_POINTER(callerData);
    kvs_pc_session_t* session = NULL;
    STATUS writeStatus;

    /* CRITICAL: Skip invalid entries gracefully to avoid aborting iteration */
    if (frame == NULL || pHashEntry == NULL) {
        diag_drop_reason_count[0]++;
        goto DiagLog;
    }

    session = (kvs_pc_session_t*)HANDLE_TO_POINTER(pHashEntry->value);

    /* CRITICAL: Skip invalid/terminated sessions gracefully */
    if (session == NULL || session->terminated) {
        diag_drop_reason_count[1]++;
        goto DiagLog;
    }

    /* CRITICAL: Skip sessions with invalid peer connection or client */
    if (session->peer_connection == NULL || session->client == NULL) {
        diag_drop_reason_count[2]++;
        goto DiagLog;
    }

    /* CRITICAL: Only send frames if peer connection is established (CONNECTED state) */
    /* media_started is set to TRUE when peer connection reaches CONNECTED state */
    if (!ATOMIC_LOAD_BOOL(&session->media_started)) {
        diag_drop_reason_count[3]++;
        goto DiagLog;
    }

    // Determine which transceiver to use based on frame track ID
    PRtcRtpTransceiver transceiver = NULL;
    if (frame->trackId == DEFAULT_VIDEO_TRACK_ID) {
        transceiver = session->video_transceiver;
    } else if (frame->trackId == DEFAULT_AUDIO_TRACK_ID) {
        transceiver = session->audio_transceiver;
    }

    /* Per-track success counters so we can read video send fps directly
     * (the original `ok` mixes audio + video, masking video drops). */
    static UINT32 ok_video_count = 0;
    static UINT32 ok_audio_count = 0;

    /* CRITICAL: Only call writeFrame if transceiver is valid and session is still active */
    if (transceiver != NULL && !session->terminated && session->peer_connection != NULL) {
        writeStatus = writeFrame(transceiver, frame);
        if (writeStatus == STATUS_SUCCESS) {
            diag_drop_reason_count[6]++;  /* Success counter (combined) */
            if (frame->trackId == DEFAULT_VIDEO_TRACK_ID) {
                ok_video_count++;
            } else if (frame->trackId == DEFAULT_AUDIO_TRACK_ID) {
                ok_audio_count++;
            }
        } else if (writeStatus == STATUS_SRTP_NOT_READY_YET) {
            diag_drop_reason_count[5]++;  /* SRTP not ready counter */
        } else {
            diag_drop_reason_count[7]++;  /* Other failure counter */
            if (session != NULL && !session->terminated) {
                ESP_LOGW(TAG, "writeFrame failed for session %s: 0x%08" PRIx32, session->peer_id, (UINT32)writeStatus);
            }
        }
    } else {
        diag_drop_reason_count[4]++;  /* NULL transceiver */
    }

DiagLog:
    /* Periodic diagnostic log - fires on ALL paths including early exits.
     * Bumped to INFO so the send-side fps is visible at the default log
     * level (the `ok` counter delta divided by the window is sender fps;
     * compare against the per-second `fps: rx=…` line in
     * video_player_adapter.c on the receive side). */
    {
        UINT64 now = GETTIME();
        if (now - diag_last_log_time > 2 * HUNDREDS_OF_NANOS_IN_A_SECOND) {
            /* Compute per-track fps over this 2 s window. Reset window
             * counters after logging so each line is the latest interval. */
#if CONFIG_KVS_WEBRTC_MEDIA_DIAG_LOG
            static UINT32 prev_ok_video = 0;
            static UINT32 prev_ok_audio = 0;
            UINT64 dt_ns = now - diag_last_log_time;
            UINT32 v_delta = ok_video_count - prev_ok_video;
            UINT32 a_delta = ok_audio_count - prev_ok_audio;
            UINT32 v_fps = (UINT32)((UINT64)v_delta * HUNDREDS_OF_NANOS_IN_A_SECOND / dt_ns);
            UINT32 a_fps = (UINT32)((UINT64)a_delta * HUNDREDS_OF_NANOS_IN_A_SECOND / dt_ns);
            ESP_LOGI(TAG, "DIAG frame_cb: send_v_fps=%" PRIu32 " send_a_fps=%" PRIu32 " | null_entry=%" PRIu32 " terminated=%" PRIu32 " no_pc=%" PRIu32 " not_connected=%" PRIu32 " no_xcvr=%" PRIu32 " srtp_notready=%" PRIu32 " ok=%" PRIu32 " fail=%" PRIu32 " | last_kvs_state=%d media_started=%d session=%p",
                     v_fps, a_fps,
                     diag_drop_reason_count[0], diag_drop_reason_count[1], diag_drop_reason_count[2],
                     diag_drop_reason_count[3], diag_drop_reason_count[4], diag_drop_reason_count[5],
                     diag_drop_reason_count[6], diag_drop_reason_count[7],
                     session ? (int)session->last_kvs_state : -1,
                     session ? (int)ATOMIC_LOAD_BOOL(&session->media_started) : -1,
                     session);
            prev_ok_video = ok_video_count;
            prev_ok_audio = ok_audio_count;
#endif
            diag_last_log_time = now;
        }
    }

    /* Always return SUCCESS to continue iteration, even if we skipped this session */
    return STATUS_SUCCESS;
}

/**
 * @brief Iterate through all active sessions and send frame - equivalent to kvsWebRTCClientMaster.c loop
 * This is the KEY function that implements the official KVS pattern
 */
static STATUS kvs_iterate_sessions_send_frame(Frame* frame, BOOL is_video)
{
    static UINT32 diag_dispatch_drop[5] = {0};  /* 0=no_sessions, 1=statslock_invalid, 2=statslock_busy, 3=null_table, 4=empty_table */
    static UINT64 diag_dispatch_last_log = 0;
    STATUS retStatus = STATUS_SUCCESS;

    CHK(frame != NULL, STATUS_NULL_ARG);
    CHK(g_global_media.client_data != NULL, STATUS_INVALID_OPERATION);

    // Get client data to access active sessions
    kvs_pc_client_t* client = (kvs_pc_client_t*)g_global_media.client_data;
    CHK(client != NULL && client->activeSessions != NULL, STATUS_NULL_ARG);

    // Quick check: skip if no active sessions (avoids unnecessary locking)
    if (IS_VALID_MUTEX_VALUE(client->session_count_mutex)) {
        if (MUTEX_TRYLOCK(client->session_count_mutex)) {
            if (client->session_count == 0) {
                MUTEX_UNLOCK(client->session_count_mutex);
                diag_dispatch_drop[0]++;
                retStatus = STATUS_SUCCESS;
                goto CleanUp;
            }
            MUTEX_UNLOCK(client->session_count_mutex);
        }
    }

    // Prefer statsLock to synchronize with metrics/cleanup without blocking destruction
    if (!IS_VALID_MUTEX_VALUE(client->statsLock)) {
        diag_dispatch_drop[1]++;
        retStatus = STATUS_INVALID_OPERATION;
        goto CleanUp;
    }

    if (!MUTEX_TRYLOCK(client->statsLock)) {
        diag_dispatch_drop[2]++;
        ESP_LOGD(TAG, "Sender: statsLock busy, skipping frame dispatch to avoid deadlock");
        retStatus = STATUS_SUCCESS;
        goto CleanUp;
    }

    // Quick check for empty table
    if (client->activeSessions == NULL) {
        MUTEX_UNLOCK(client->statsLock);
        diag_dispatch_drop[3]++;
        retStatus = STATUS_SUCCESS;
        goto CleanUp;
    }

    // Check if hash table is empty before iterating (prevents STATUS_INVALID_OPERATION when all sessions removed)
    UINT32 itemCount = 0;
    STATUS countStatus = hashTableGetCount(client->activeSessions, &itemCount);
    if (STATUS_FAILED(countStatus) || itemCount == 0) {
        MUTEX_UNLOCK(client->statsLock);
        diag_dispatch_drop[4]++;
        retStatus = STATUS_SUCCESS;
        goto CleanUp;
    }

    // Use the official KVS pattern: iterate through sessions and call writeFrame for each
    retStatus = hashTableIterateEntries(client->activeSessions, POINTER_TO_HANDLE(frame), kvs_session_frame_callback);
    if (STATUS_FAILED(retStatus)) {
        /* Log at WARN level to ensure visibility */
        ESP_LOGW(TAG, "Sender: hashTableIterateEntries FAILED: 0x%08" PRIx32, (UINT32) retStatus);
        /* Treat as success to avoid propagating errors during cleanup */
        retStatus = STATUS_SUCCESS;
    }

    MUTEX_UNLOCK(client->statsLock);

CleanUp:
    /* Periodic diagnostic log for dispatch drops (every 2 seconds, fires on ALL paths including early exits) */
    {
        UINT64 now = GETTIME();
        if (now - diag_dispatch_last_log > 2 * HUNDREDS_OF_NANOS_IN_A_SECOND) {
            diag_dispatch_last_log = now;
            ESP_LOGD(TAG, "DIAG dispatch: no_sessions=%" PRIu32 " statslock_invalid=%" PRIu32 " statslock_busy=%" PRIu32 " null_table=%" PRIu32 " empty_table=%" PRIu32,
                     diag_dispatch_drop[0], diag_dispatch_drop[1], diag_dispatch_drop[2],
                     diag_dispatch_drop[3], diag_dispatch_drop[4]);
        }
    }
    return retStatus;
}

/* Public API implementations following KVS official pattern */

/**
 * @brief Start global media transmission threads (called once at client level)
 */
STATUS kvs_media_start_global_transmission(void* client_data, kvs_media_config_t* config)
{
    STATUS retStatus = STATUS_SUCCESS;
    BOOL mutext_locked = FALSE;

    CHK(client_data != NULL && config != NULL, STATUS_NULL_ARG);
    CHK(IS_VALID_MUTEX_VALUE(g_global_media.global_media_mutex), STATUS_INVALID_OPERATION);

    MUTEX_LOCK(g_global_media.global_media_mutex);
    mutext_locked = TRUE;

    if (g_global_media.global_media_started) {
        ESP_LOGD(TAG, "Global media threads already started");
        goto CleanUp;
    }

    // Store client data and configuration
    g_global_media.client_data = client_data;
    MEMCPY(&g_global_media.config, config, SIZEOF(kvs_media_config_t));
    ATOMIC_STORE_BOOL(&g_global_media.terminated, FALSE);

    ESP_LOGD(TAG, "Starting global media threads");

    /* Initialize video capture synchronously before starting thread
     * For ESP32-P4: Camera hardware initialization must happen synchronously before display init.
     * For other targets: Video capture interface initialization is still needed.
     */
    if (config->video_capture != NULL) {
        media_stream_video_capture_t *video_capture = (media_stream_video_capture_t*)config->video_capture;

        video_capture_config_t video_config = {
            .codec = VIDEO_CODEC_H264,
            .resolution = {
                .width = config->video_width ? config->video_width : 1280,
                .height = config->video_height ? config->video_height : 720,
                .fps = config->video_fps ? config->video_fps : 30
            },
            .quality = 80,
            .bitrate = 500,
            .codec_specific = NULL
        };

        esp_err_t init_ret = video_capture->init(&video_config, &g_global_media.video_handle);
        if (init_ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to initialize video capture: 0x%x (non-fatal, continuing without video)", init_ret);
            g_global_media.video_handle = NULL;
            /* Continue to allow audio initialization even if video fails */
        } else {
            esp_err_t start_ret = video_capture->start(g_global_media.video_handle);
            if (start_ret != ESP_OK) {
                ESP_LOGW(TAG, "Failed to start video capture streaming: 0x%x (non-fatal, continuing without video)", start_ret);
                video_capture->deinit(g_global_media.video_handle);
                g_global_media.video_handle = NULL;
                /* Continue to allow audio initialization even if video fails */
            }
        }
    }

    // Start global video thread only if video capture is provided and initialized successfully
    if (config->video_capture != NULL && g_global_media.video_handle != NULL) {
        /* Explicit prio 5 (pthread default) so the relationship with kvsGlobalAudio is
         * obvious: audio is one slot above so its frames reach the wire first when both
         * are runnable. Keep below connListener (6) and i2s_read (9). */
        CHK_STATUS(THREAD_CREATE_EX_PRI(&g_global_media.video_sender_tid, "kvsGlobalVideo", 6 * 1024, TRUE,
                                        kvs_global_video_sender_thread, 5, NULL));
        ESP_LOGI(TAG, "Global video sender thread started");
    } else {
        if (config->video_capture != NULL) {
            ESP_LOGW(TAG, "Video capture provided but initialization failed - skipping video transmission");
        } else {
            ESP_LOGD(TAG, "Video capture not provided - skipping video transmission");
        }
    }

    // Initialize audio capture synchronously before starting thread (symmetric with video)
    if (config->audio_capture != NULL) {
        media_stream_audio_capture_t *audio_capture = (media_stream_audio_capture_t*)config->audio_capture;

        audio_capture_config_t audio_config = {
            .codec = AUDIO_CODEC_OPUS,
            .format = {
                .sample_rate = 16000,
                .channels = 1,
                .bits_per_sample = 16
            },
            .bitrate = 16000,
            .frame_duration_ms = 20,
            .codec_specific = NULL
        };

        esp_err_t init_ret = audio_capture->init(&audio_config, &g_global_media.audio_handle);
        if (init_ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to initialize audio capture hardware: 0x%x (non-fatal, continuing without audio)", init_ret);
            g_global_media.audio_handle = NULL;
            /* Continue to allow video even if audio fails */
        } else {
            esp_err_t start_ret = audio_capture->start(g_global_media.audio_handle);
            if (start_ret != ESP_OK) {
                ESP_LOGW(TAG, "Failed to start audio capture streaming: 0x%x (non-fatal, continuing without audio)", start_ret);
                audio_capture->deinit(g_global_media.audio_handle);
                g_global_media.audio_handle = NULL;
                /* Continue to allow video even if audio fails */
            }
        }
    }

    // Start global audio thread only if audio capture is provided and initialized successfully
    if (config->audio_capture != NULL && g_global_media.audio_handle != NULL) {
        /* prio 6 — one above kvsGlobalVideo, one above pthread default. Audio frame
         * loss is audible; video can adapt. Stays at or below connListener (6) so
         * inbound RTP isn't held off, and well below i2s_read (9). */
        CHK_STATUS(THREAD_CREATE_EX_PRI(&g_global_media.audio_sender_tid, "kvsGlobalAudio", 6 * 1024, TRUE,
                                    kvs_global_audio_sender_thread, 6, NULL));
        ESP_LOGI(TAG, "Global audio sender thread started");
    } else if (config->audio_capture != NULL && g_global_media.audio_handle == NULL) {
        ESP_LOGW(TAG, "Audio capture provided but initialization failed - skipping audio transmission");
    } else {
        ESP_LOGD(TAG, "Audio capture not provided - skipping audio transmission");
    }

    g_global_media.global_media_started = TRUE;
    ESP_LOGD(TAG, "Global media transmission started successfully");

CleanUp:
    if (mutext_locked) {
        MUTEX_UNLOCK(g_global_media.global_media_mutex);
    }
    return retStatus;
}

/**
 * @brief Stop global media transmission threads
 */
STATUS kvs_media_stop_global_transmission(void* client_data)
{
    STATUS retStatus = STATUS_SUCCESS;
    BOOL mutext_locked = FALSE;

    CHK(client_data != NULL, STATUS_NULL_ARG);
    if (!IS_VALID_MUTEX_VALUE(g_global_media.global_media_mutex)) {
        ESP_LOGW(TAG, "Global media mutex not valid - skipping stop");
        retStatus = STATUS_INVALID_OPERATION;
        goto CleanUp;
    }

    MUTEX_LOCK(g_global_media.global_media_mutex);
    mutext_locked = TRUE;

    if (!g_global_media.global_media_started) {
        ESP_LOGI(TAG, "Global media threads not running");
        goto CleanUp;
    }

    ESP_LOGD(TAG, "Stopping global media threads");

    // Signal termination
    ATOMIC_STORE_BOOL(&g_global_media.terminated, TRUE);

    // Wait for threads to complete
    if (IS_VALID_TID_VALUE(g_global_media.video_sender_tid)) {
        THREAD_JOIN(g_global_media.video_sender_tid, NULL);
        g_global_media.video_sender_tid = INVALID_TID_VALUE;
        ESP_LOGI(TAG, "Global video thread stopped");
    }

    if (IS_VALID_TID_VALUE(g_global_media.audio_sender_tid)) {
        THREAD_JOIN(g_global_media.audio_sender_tid, NULL);
        g_global_media.audio_sender_tid = INVALID_TID_VALUE;
        ESP_LOGI(TAG, "Global audio thread stopped");
    }

    // Deinitialize camera hardware after threads are stopped (power and security)
    if (g_global_media.config.video_capture != NULL && g_global_media.video_handle != NULL) {
        media_stream_video_capture_t *video_capture = (media_stream_video_capture_t*)g_global_media.config.video_capture;
        ESP_LOGI(TAG, "Deinitializing camera hardware (no active sessions)");
        video_capture->stop(g_global_media.video_handle);
        video_capture->deinit(g_global_media.video_handle);
        g_global_media.video_handle = NULL;
        ESP_LOGI(TAG, "Camera hardware deinitialized successfully");
    }

    // Deinitialize audio capture hardware after threads are stopped
    if (g_global_media.config.audio_capture != NULL && g_global_media.audio_handle != NULL) {
        media_stream_audio_capture_t *audio_capture = (media_stream_audio_capture_t*)g_global_media.config.audio_capture;
        ESP_LOGI(TAG, "Deinitializing audio capture hardware (no active sessions)");
        audio_capture->stop(g_global_media.audio_handle);
        audio_capture->deinit(g_global_media.audio_handle);
        g_global_media.audio_handle = NULL;
        ESP_LOGI(TAG, "Audio capture hardware deinitialized successfully");
    }

    g_global_media.global_media_started = FALSE;
    ESP_LOGD(TAG, "Global media transmission stopped successfully");

CleanUp:
    if (mutext_locked) {
        MUTEX_UNLOCK(g_global_media.global_media_mutex);
    }
    return retStatus;
}

// Reception routine removed — media reception is entirely callback-driven.
// No dedicated thread needed; frame callbacks are set up in kvs_media_setup_players.

// Note: kvs_media_start_transmission() removed - replaced with global transmission

STATUS kvs_media_start_reception(kvs_pc_session_t* session, kvs_media_config_t* config)
{
    STATUS retStatus = STATUS_SUCCESS;

    CHK(session != NULL && config != NULL, STATUS_NULL_ARG);

    // Copy media config to session for access by media threads
    session->client->config.video_player = config->video_player;
    session->client->config.audio_player = config->audio_player;
    session->client->config.receive_media = config->receive_media;

    // Setup media players
    CHK_STATUS(kvs_media_setup_players(session, config));

    // Media reception is callback-driven — no dedicated thread needed

CleanUp:
    return retStatus;
}

STATUS kvs_media_stop_session(kvs_pc_session_t* session)
{
    STATUS retStatus = STATUS_SUCCESS;

    CHK(session != NULL, STATUS_NULL_ARG);

    // Mark session as terminated (global media threads handle transmission termination separately)
    session->terminated = TRUE;

    // Cleanup media players (session-specific)
    if (session->client->config.video_player != NULL && session->video_player_handle != NULL) {
        media_stream_video_player_t *video_player = (media_stream_video_player_t*)session->client->config.video_player;
        video_player->stop(session->video_player_handle);
        video_player->deinit(session->video_player_handle);
        session->video_player_handle = NULL;
    }

    if (session->client->config.audio_player != NULL && session->audio_player_handle != NULL) {
        media_stream_audio_player_t *audio_player = (media_stream_audio_player_t*)session->client->config.audio_player;
        audio_player->stop(session->audio_player_handle);
        audio_player->deinit(session->audio_player_handle);
        session->audio_player_handle = NULL;
    }

    // Session-specific frame buffers are now handled by global threads
    // No per-session cleanup needed

CleanUp:
    return retStatus;
}

STATUS kvs_media_setup_players(kvs_pc_session_t* session, kvs_media_config_t* config)
{
    STATUS retStatus = STATUS_SUCCESS;

    CHK(session != NULL && config != NULL, STATUS_NULL_ARG);

    // Initialize media players if not already done
    if (!session->media_players_initialized) {
        // Initialize video player if available
        if (config->video_player != NULL && session->video_player_handle == NULL) {
            media_stream_video_player_t *video_player = (media_stream_video_player_t*)config->video_player;

            // Initialize with default configuration
            video_player_config_t video_config = {
                .codec = VIDEO_PLAYER_CODEC_H264,
                .format = {
                    .width = 640,
                    .height = 480,
                    .framerate = 30
                },
                .buffer_frames = 5,
                .codec_specific = NULL,
                .display_handle = NULL
            };

            esp_err_t init_ret = video_player->init(&video_config, &session->video_player_handle);
            if (init_ret != ESP_OK) {
                ESP_LOGE(TAG, "Failed to initialize video player: %s", esp_err_to_name(init_ret));
                session->video_player_handle = NULL;
            } else {
                esp_err_t start_ret = video_player->start(session->video_player_handle);
                if (start_ret != ESP_OK) {
                    ESP_LOGE(TAG, "Failed to start video player: %s", esp_err_to_name(start_ret));
                    video_player->deinit(session->video_player_handle);
                    session->video_player_handle = NULL;
                }
            }
        }

        // Initialize audio player if available (only if video player succeeded or not needed)
        if (config->audio_player != NULL && session->audio_player_handle == NULL) {
            media_stream_audio_player_t *audio_player = (media_stream_audio_player_t*)config->audio_player;

            // Initialize with default configuration
            audio_player_config_t audio_config = {
                .codec = AUDIO_PLAYER_CODEC_OPUS,
                .format = {
                    .sample_rate = 48000,
                    .channels = 1,
                    .bits_per_sample = 16
                },
                .buffer_ms = 500,
                .codec_specific = NULL
            };

            esp_err_t audio_init_ret = audio_player->init(&audio_config, &session->audio_player_handle);
            if (audio_init_ret != ESP_OK) {
                ESP_LOGW(TAG, "Failed to initialize audio player: %s (non-fatal, continuing)", esp_err_to_name(audio_init_ret));
                session->audio_player_handle = NULL;
            } else {
                esp_err_t audio_start_ret = audio_player->start(session->audio_player_handle);
                if (audio_start_ret != ESP_OK) {
                    ESP_LOGW(TAG, "Failed to start audio player: %s (non-fatal, continuing)", esp_err_to_name(audio_start_ret));
                    audio_player->deinit(session->audio_player_handle);
                    session->audio_player_handle = NULL;
                }
            }
        }

        session->media_players_initialized = true;
    }

CleanUp:
    return retStatus;
}

STATUS kvs_media_setup_frame_callbacks(kvs_pc_session_t* session)
{
    STATUS retStatus = STATUS_SUCCESS;

    CHK(session != NULL, STATUS_NULL_ARG);

    if (!session->client->config.receive_media) {
        ESP_LOGD(TAG, "Skipping frame callback setup: receive_media disabled");
        goto CleanUp;
    }

    // Set up video frame callback. Both video and audio transceivers can
    // legitimately be NULL on the initial offer (audio-only call, no camera
    // enabled yet on the remote). Skip the one that isn't present so the
    // other still gets wired. The caller now re-invokes this on every offer,
    // so a transceiver that appears later (e.g. via renegotiation when the
    // remote enables its camera) gets picked up then.
    if (session->video_transceiver == NULL) {
        ESP_LOGD(TAG, "video_transceiver is NULL for peer: %s - skipping video callback (will retry on next offer)", session->peer_id);
    } else {
        CHK_STATUS(transceiverOnFrame(session->video_transceiver,
                                      POINTER_TO_HANDLE(session),
                                      kvs_media_video_frame_handler));
    }

    // Set up audio frame callback
    if (session->audio_transceiver == NULL) {
        ESP_LOGD(TAG, "audio_transceiver is NULL for peer: %s - skipping audio callback (will retry on next offer)", session->peer_id);
    } else {
        CHK_STATUS(transceiverOnFrame(session->audio_transceiver,
                                      POINTER_TO_HANDLE(session),
                                      kvs_media_audio_frame_handler));
    }

CleanUp:
    return retStatus;
}

VOID kvs_media_video_frame_handler(UINT64 customData, PFrame pFrame)
{
    static uint32_t rx_frame_count = 0;
    kvs_pc_session_t *session = (kvs_pc_session_t *)HANDLE_TO_POINTER(customData);

    if (pFrame == NULL || pFrame->frameData == NULL || session == NULL) {
        ESP_LOGW(TAG, "Invalid video frame or session data");
        return;
    }

    // Get the video player interface
    if (session->client->config.video_player == NULL || session->video_player_handle == NULL) {
        if (rx_frame_count == 0) {
            ESP_LOGW(TAG, "Video player not available for peer: %s (video frames will be dropped)", session->peer_id);
        }
        rx_frame_count++;
        return;
    }

    rx_frame_count++;

    media_stream_video_player_t *video_player = (media_stream_video_player_t*)session->client->config.video_player;

    // Create video frame structure
    video_frame_t video_frame = {
        .buffer = pFrame->frameData,
        .len = pFrame->size,
        .timestamp = pFrame->presentationTs,
        .type = (pFrame->flags & FRAME_FLAG_KEY_FRAME) ? VIDEO_FRAME_TYPE_I : VIDEO_FRAME_TYPE_P
    };

    // Send frame to player
    esp_err_t err = video_player->play_frame(session->video_player_handle,
                                             video_frame.buffer, video_frame.len,
                                             video_frame.type == VIDEO_FRAME_TYPE_I);

    if (err != ESP_OK && rx_frame_count <= 10) {
        ESP_LOGW(TAG, "Failed to play video frame: %s", esp_err_to_name(err));
    }
}

VOID kvs_media_audio_frame_handler(UINT64 customData, PFrame pFrame)
{
    static uint32_t rx_audio_frame_count = 0;
    kvs_pc_session_t *session = (kvs_pc_session_t *)HANDLE_TO_POINTER(customData);

    if (pFrame == NULL || pFrame->frameData == NULL || session == NULL) {
        ESP_LOGW(TAG, "Invalid audio frame or session data");
        return;
    }

    // Get the audio player interface
    if (session->client->config.audio_player == NULL || session->audio_player_handle == NULL) {
        if (rx_audio_frame_count == 0) {
            ESP_LOGW(TAG, "Audio player not available for peer: %s (audio frames will be dropped)", session->peer_id);
        }
        rx_audio_frame_count++;
        return;
    }

    rx_audio_frame_count++;

    media_stream_audio_player_t *audio_player = (media_stream_audio_player_t*)session->client->config.audio_player;

    // Create audio frame structure
    audio_frame_t audio_frame = {
        .buffer = pFrame->frameData,
        .len = pFrame->size,
        .timestamp = pFrame->presentationTs
    };

    // Send frame to player
    esp_err_t err = audio_player->play_frame(session->audio_player_handle,
                                             audio_frame.buffer, audio_frame.len);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to write audio frame to player: %s", esp_err_to_name(err));
    }
}

/**
 * @brief Print frame transmission statistics for debugging
 */
void kvs_media_print_stats(kvs_pc_session_t* session)
{
    if (session == NULL) {
        return;
    }

    UINT64 currentTime = GETTIME();
    UINT64 sessionDuration = (currentTime - session->start_time) / HUNDREDS_OF_NANOS_IN_A_SECOND;

    if (sessionDuration == 0) sessionDuration = 1; // Avoid division by zero

    UINT64 totalVideoFrames = session->video_frames_sent + session->video_frames_dropped + session->video_frames_failed;
    UINT64 totalAudioFrames = session->audio_frames_sent + session->audio_frames_dropped + session->audio_frames_failed;

    ESP_LOGI(TAG, "Frame Statistics for peer: %s", session->peer_id);
    ESP_LOGI(TAG, "  Video: Sent=%" PRIu64 ", Dropped=%" PRIu64 ", Failed=%" PRIu64 ", Total=%" PRIu64,
             session->video_frames_sent, session->video_frames_dropped,
             session->video_frames_failed, totalVideoFrames);
    ESP_LOGI(TAG, "  Audio: Sent=%" PRIu64 ", Dropped=%" PRIu64 ", Failed=%" PRIu64 ", Total=%" PRIu64,
             session->audio_frames_sent, session->audio_frames_dropped,
             session->audio_frames_failed, totalAudioFrames);

    if (totalVideoFrames > 0) {
        DOUBLE videoSuccessRate = (DOUBLE)session->video_frames_sent / totalVideoFrames * 100.0;
        DOUBLE videoFPS = (DOUBLE)session->video_frames_sent / sessionDuration;
        ESP_LOGI(TAG, "  Video Success Rate: %.2f%%, FPS: %.2f", videoSuccessRate, videoFPS);
    }

    if (totalAudioFrames > 0) {
        DOUBLE audioSuccessRate = (DOUBLE)session->audio_frames_sent / totalAudioFrames * 100.0;
        DOUBLE audioFPS = (DOUBLE)session->audio_frames_sent / sessionDuration;
        ESP_LOGI(TAG, "  Audio Success Rate: %.2f%%, FPS: %.2f", audioSuccessRate, audioFPS);
    }

    if (session->first_video_frame_sent > 0) {
        UINT64 timeToFirstVideo = (session->first_video_frame_sent - session->start_time) / HUNDREDS_OF_NANOS_IN_A_MILLISECOND;
        ESP_LOGI(TAG, "  Time to first video frame: %" PRIu64 " ms", timeToFirstVideo);
    }

    if (session->first_audio_frame_sent > 0) {
        UINT64 timeToFirstAudio = (session->first_audio_frame_sent - session->start_time) / HUNDREDS_OF_NANOS_IN_A_MILLISECOND;
        ESP_LOGI(TAG, "  Time to first audio frame: %" PRIu64 " ms", timeToFirstAudio);
    }

    ESP_LOGI(TAG, "  Session Duration: %" PRIu64 " seconds", sessionDuration);
}
