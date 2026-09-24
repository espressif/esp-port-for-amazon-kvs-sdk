/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @brief KVS WebRTC + on-device person detection + LCD preview, from one camera.
 *
 * Three consumers share the camera through media_stream's raw-frame bus:
 *
 *   camera -> pump -> raw bus -+- "h264-enc"  passthrough -> H.264 -> WebRTC
 *                              +- "detect"    converted   -> esp-dl person detection
 *                              +- "preview"   converted   -> LCD, with boxes over it
 *
 * The two new sinks are CONVERTED, so the PPA hands each a small private frame at its own size and
 * the camera buffer is returned immediately. Neither draws on the camera's buffer budget, and
 * neither can slow the encoder down however long it takes - which is the whole point, since
 * inference costs ~60 ms a frame.
 *
 * The LCD shows the LOCAL camera, not a peer's stream: media_stream's receive path is off.
 *
 * Derived from examples/webrtc_classic.
 */

#include <string.h>
#include <stdlib.h>
#include <inttypes.h>
#include "esp_system.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "esp_cli.h"
#include "wifi_cli.h"
#include "app_storage.h"
#include "app_wifi_prov.h"

#include "app_webrtc.h"
#include "kvs_signaling.h"
#include "kvs_peer_connection.h"
#include "esp_webrtc_time.h"
#include "esp_work_queue.h"
#include "media_stream.h"
#include "video_capture.h"
#include "video_raw_sink.h"
#include "video_sink.h"
#include "video_rate_ctrl.h"

#include "esp_console.h"

#include "app_ctrl.h"
#include "detect_sink.h"
#include "preview_display.h"

static const char *TAG = "person_detect_main";

#if CONFIG_ESP_HOSTED_ENABLE_BT_NIMBLE
#include "esp_check.h"
#include "esp_hosted_misc.h"

static esp_err_t hosted_bt_prov_start(void *ctx)
{
    ESP_LOGI(TAG, "Initializing BT controller on coprocessor for provisioning");
    ESP_RETURN_ON_ERROR(esp_hosted_bt_controller_init(), TAG, "BT controller init failed");
    ESP_RETURN_ON_ERROR(esp_hosted_bt_controller_enable(), TAG, "BT controller enable failed");
    return ESP_OK;
}

static void hosted_bt_prov_end(void *ctx)
{
    ESP_LOGI(TAG, "Deinitializing BT controller on coprocessor");
    esp_hosted_bt_controller_disable();
    esp_hosted_bt_controller_deinit(true);
}
#endif

#ifdef CONFIG_SLAVE_FLASHER_ENABLE
#include "slave_flasher.h"
#endif

static video_capture_handle_t s_video_handle;

/* -------------------------------------------------------------------------- */
/*  WebRTC events                                                             */
/* -------------------------------------------------------------------------- */

/* -------------------------------------------------------------------------- */
/*  Console                                                                   */
/* -------------------------------------------------------------------------- */

typedef struct {
    char peer_id[256];
} trigger_offer_work_params_t;

static void trigger_offer_work_fn(void *priv_data)
{
    trigger_offer_work_params_t *params = (trigger_offer_work_params_t *)priv_data;
    if (params == NULL) {
        return;
    }
    ESP_LOGI(TAG, "Triggering offer for peer: %s", params->peer_id);
    const int result = app_webrtc_trigger_offer(params->peer_id);
    if (result != 0) {
        ESP_LOGE(TAG, "Trigger offer command failed with code: %d", result);
    }
    free(params);
}

static int trigger_offer_cli_handler(int argc, char *argv[])
{
    if (argc != 2 || strlen(argv[1]) >= 256) {
        ESP_LOGE(TAG, "Usage: trigger-offer <peer_id>");
        return -1;
    }
    /* Queued rather than called here because app_webrtc_trigger_offer() re-enters the WebRTC
     * stack, and doing that from the console task deadlocks. */
    trigger_offer_work_params_t *params = malloc(sizeof(*params));
    if (params == NULL) {
        return -1;
    }
    strncpy(params->peer_id, argv[1], sizeof(params->peer_id) - 1);
    params->peer_id[sizeof(params->peer_id) - 1] = '\0';

    if (esp_work_queue_add_task(trigger_offer_work_fn, params) != ESP_OK) {
        free(params);
        return -1;
    }
    return 0;
}

/* Console handlers only enqueue: starting WebRTC does blocking HTTPS, and doing that on
 * the REPL task would stall the console and risk re-entering the WebRTC stack from it. */
static void start_webrtc_work_fn(void *priv)   { (void)priv; webrtc_side_start(); }
static void stop_webrtc_work_fn(void *priv)    { (void)priv; webrtc_side_stop(); }

static int enqueue_work(esp_work_fn_t fn, const char *what)
{
    if (esp_work_queue_add_task(fn, NULL) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enqueue %s", what);
        return -1;
    }
    return 0;
}

static int start_webrtc_cli(int argc, char *argv[])
{
    (void)argc; (void)argv;
    return enqueue_work(start_webrtc_work_fn, "start-webrtc");
}

static int stop_webrtc_cli(int argc, char *argv[])
{
    (void)argc; (void)argv;
    return enqueue_work(stop_webrtc_work_fn, "stop-webrtc");
}

static int stats_overlay_cli_handler(int argc, char *argv[])
{
    if (argc != 2) {
        ESP_LOGE(TAG, "Usage: stats-overlay <on|off>");
        return -1;
    }
    const bool on = (strcmp(argv[1], "on") == 0);
    if (!on && strcmp(argv[1], "off") != 0) {
        ESP_LOGE(TAG, "Usage: stats-overlay <on|off>");
        return -1;
    }
    const esp_err_t err = preview_display_show_stats(on);
    ESP_LOGI(TAG, "stats overlay %s%s", on ? "on" : "off",
             err == ESP_OK ? "" : " (failed)");
    return err == ESP_OK ? 0 : -1;
}

static int detect_cli_handler(int argc, char *argv[])
{
    if (argc != 2) {
        ESP_LOGI(TAG, "Usage: detect <on|off>  (currently %s)",
                 detect_sink_is_enabled() ? "on" : "off");
        return -1;
    }
    const bool on = (strcmp(argv[1], "on") == 0);
    if (!on && strcmp(argv[1], "off") != 0) {
        ESP_LOGE(TAG, "Usage: detect <on|off>");
        return -1;
    }
    /* No work-queue indirection needed: video_raw_sink_set_enabled() is documented safe from any
     * task at any time. */
    const esp_err_t err = detect_sink_set_enabled(on);
    ESP_LOGI(TAG, "inference %s%s", on ? "on" : "off",
             err == ESP_OK ? "" : " (failed)");
    return err == ESP_OK ? 0 : -1;
}

/* The raw side of the bus: who got frames, and for each sink that did not, why not. Each
 * drop counter names a different cause because each calls for a different fix - and with
 * rate control driving the encoder sink's fps limit, d:rate on THAT row is the ladder
 * asking for fewer frames, which is a healthy number and not a fault. */
static void print_raw_sinks(void)
{
    video_raw_sink_stats_t raw[CONFIG_VIDEO_RAW_SINK_MAX];
    size_t n = 0;
    if (video_raw_sink_get_stats(raw, CONFIG_VIDEO_RAW_SINK_MAX, &n) != ESP_OK) {
        printf("failed to read raw sink stats\n");
        return;
    }
    printf("%-10s %-3s %8s %8s %8s %8s %8s %8s %6s\n",
           "sink", "en", "deliv", "d:queue", "d:flight", "d:nobuf", "d:rate", "d:cvt", "hold");
    for (size_t i = 0; i < n; i++) {
        printf("%-10s %-3s %8" PRIu32 " %8" PRIu32 " %8" PRIu32 " %8" PRIu32
               " %8" PRIu32 " %8" PRIu32 " %5" PRIu32 "u\n",
               raw[i].name, raw[i].enabled ? "y" : "n", raw[i].delivered,
               raw[i].dropped_queue, raw[i].dropped_inflight, raw[i].dropped_nobuf,
               raw[i].dropped_rate, raw[i].dropped_convert, raw[i].avg_hold_us);
    }
}

static void print_last_inference(void)
{
    detect_results_t res;
    if (detect_sink_get_results(&res) == ESP_OK) {
        printf("last inference: %d box(es), %" PRIu32 " us, seq %" PRIu32 "\n",
               res.count, res.infer_us, res.seq);
    } else {
        printf("last inference: none yet\n");
    }
}

static int detect_stats_cli_handler(int argc, char *argv[])
{
    (void)argc; (void)argv;
    print_raw_sinks();
    print_last_inference();
    return 0;
}

/* Everything the rate controller is doing, in the layout the s31-rate-control-harness
 * README documents - same command name and same columns as kvs_combined, so what is known
 * about reading one transfers to the other.
 *
 * The three tables answer one question together: which frames each sink got (raw), what
 * the encoded stream cost to send (encoded), and who asked the shared encoder to slow down
 * (controllers). */
static int sink_stats_cli_handler(int argc, char *argv[])
{
    (void)argc; (void)argv;

    printf("webrtc: %s\n", webrtc_side_is_running() ? "running" : "stopped");

    print_raw_sinks();

    video_sink_stats_t enc_sinks[8];
    size_t n = 0;
    if (video_sink_get_stats(enc_sinks, sizeof(enc_sinks) / sizeof(enc_sinks[0]), &n) == ESP_OK
        && n > 0) {
        printf("\n%-14s %-8s %10s %10s %9s %9s %9s %8s\n",
               "sink", "state", "frames", "gate_held", "max_us", "avg_us", "bytes_kb", "kbps");
        for (size_t i = 0; i < n; i++) {
            printf("%-14s %-8s %10" PRIu32 " %10" PRIu32 " %9" PRIu32 " %9" PRIu32
                   " %9" PRIu32 " %8" PRIu32 "\n",
                   enc_sinks[i].name, enc_sinks[i].enabled ? "enabled" : "off",
                   enc_sinks[i].calls, enc_sinks[i].gate_held, enc_sinks[i].max_us,
                   enc_sinks[i].avg_us, enc_sinks[i].bytes_kb, enc_sinks[i].kbps);
        }
    }

    /* `bar` is the recovery limit: the best rung this controller may climb back into,
     * raised whenever a rung congests. rung == bar means it is being held there
     * deliberately - "will not yet", not "cannot". */
    video_rate_ctrl_stats_t rc[8];
    size_t rc_n = 0;
    uint32_t enc_fps = 0, enc_bps = 0;
    if (video_rate_ctrl_get_stats(rc, sizeof(rc) / sizeof(rc[0]), &rc_n,
                                  &enc_fps, &enc_bps) != ESP_OK) {
        printf("failed to read rate-control stats\n");
        return -1;
    }
    if (rc_n == 0) {
        printf("\nno rate controllers (nothing is streaming yet)\n");
        print_last_inference();
        return 0;
    }
    printf("\n%-14s %-8s %5s %4s %5s %10s %6s %10s %9s\n",
           "controller", "state", "rung", "bar", "fps", "bitrate", "load%", "netceil", "reports");
    for (size_t i = 0; i < rc_n; i++) {
        printf("%-14s %-8s %5" PRIu32 " %4" PRIu32 " %5" PRIu32 " %10" PRIu32 " %6" PRIu32
               " %10" PRIu32 " %9" PRIu32 "\n",
               rc[i].name,
               rc[i].stale ? "stale" : (rc[i].enabled ? "adapting" : "off"),
               rc[i].level, rc[i].probe_bar_level, rc[i].target_fps, rc[i].target_bitrate_bps,
               rc[i].load_pct, rc[i].net_ceiling_bps, rc[i].reports);
    }
    printf("encoder (strict min of adapting rows): %" PRIu32 " fps / %" PRIu32 " bps\n",
           enc_fps, enc_bps);

    video_rate_ctrl_encoder_stats_t enc = {0};
    if (video_rate_ctrl_get_encoder_stats(&enc) == ESP_OK) {
        printf("encoder actuation: fps=%s bitrate=%s | requested %" PRIu32
               " bps, committed %" PRIu32 " bps%s\n",
               enc.fps_actuable ? "actuable" : "FIXED",
               enc.bitrate_actuable ? "actuable" : "IGNORED",
               enc.requested_bps, enc.committed_bps,
               (!enc.fps_actuable && !enc.bitrate_actuable)
                   ? "  <- no lever: rate control is inert" : "");
        /* Printed unconditionally, not just under a pin: the floor is what stops the
         * ladder asking for a bitrate this resolution cannot be encoded at, and
         * "deepest" says how many rungs are actually distinguishable above it. */
        printf("rung floor: %" PRIu32 " bps, ladder stops at L%" PRIu32 "\n",
               enc.floor_bps, enc.deepest_level);
        /* The ACHIEVED rates, which is a different question from every target above and
         * the same set the LCD overlay shows - so a run with nobody watching the panel can
         * still report what the panel reported. */
        video_capture_live_stats_t live = {0};
        uint16_t det_dfps = 0, lcd_dfps = 0;
        (void)video_capture_get_live_stats(&live);
        (void)preview_display_get_rates(&det_dfps, &lcd_dfps);
        printf("achieved: cap %" PRIu32 " fps | enc %" PRIu32 " fps %" PRIu32 " kbps "
               "(max frame %" PRIu32 " KB) | detect %u.%u fps | lcd %u.%u fps\n",
               live.cap_fps, live.enc_fps, live.enc_kbps, live.enc_max_frame_bytes / 1024,
               det_dfps / 10, det_dfps % 10, lcd_dfps / 10, lcd_dfps % 10);
        if (enc.fps_pin_wanted) {
            printf("frame-rate pin: %s | floor %" PRIu32 " bps (pinned needs %" PRIu32
                   " bps), ladder stops at L%" PRIu32 "\n",
                   enc.fps_pinned ? "held" : "GIVEN UP (bitrate too low for a stable rate)",
                   enc.floor_bps, enc.pinned_floor_bps, enc.deepest_level);
        }
    }

    print_last_inference();
    return 0;
}

static int sink_stats_reset_cli_handler(int argc, char *argv[])
{
    (void)argc; (void)argv;
    video_raw_sink_reset_stats();
    video_sink_reset_stats();
    printf("sink stats reset\n");
    return 0;
}

/* Force congestion without a congested network.
 *
 * The controller's network ceiling is normally fed by TWCC feedback from the viewer, so
 * reproducing a descend/hold/recover cycle otherwise means finding a link that is
 * genuinely too slow and holding the light steady while it happens. Setting the ceiling
 * by hand snaps the ladder down immediately and caps recovery at the same number, which
 * makes the whole cycle a two-command test: `rate-ceiling 300000`, watch it descend and
 * hold, then `rate-ceiling off` and watch it climb back.
 *
 * It writes the same field TWCC does, so a live viewer's estimate overwrites it on the
 * next feedback round - which is the honest behaviour: this is a nudge, not a clamp. */
/* Turn adaptive rate control on for this session.
 *
 * kvs_media creates the WebRTC controller and leaves it DISABLED, on purpose:
 * media_stream/README.md's rule is that a controller with a signal you have not verified
 * is worse than no controller, because the reports are the only thing that lets it climb
 * back up - a signal that always reads healthy ratchets to the top rung and stays there.
 * The send-latency signal on this path had not been checked, so it was left off.
 *
 * Testing the ladder needs it on, and an explicit operator command is the honest way to
 * do that: it puts the decision in the run rather than in a default, and the run can say
 * it made it. Disabling withdraws the controller and leaves the encoder at native rate.
 */
/* Both rate commands name the controller, defaulting to "webrtc".
 *
 * They used to hardcode "webrtc" with no way to say otherwise, and naming it matters
 * because kvs_media DESTROYS its controller when the last session ends: the moment a
 * viewer drops, `rate-ceiling` reports "no 'webrtc' rate controller" and a run that was
 * mid-measurement silently stops steering anything. The argument is what lets a
 * longer-lived controller be steered instead of that one. */
static esp_err_t rate_ctrl_by_name(int argc, char *argv[], int name_argc,
                                   video_rate_ctrl_handle_t *out)
{
    const char *name = (argc > name_argc) ? argv[name_argc] : "webrtc";
    if (video_rate_ctrl_find(name, out) != ESP_OK) {
        ESP_LOGE(TAG, "no '%s' rate controller - 'webrtc' only exists while a session "
                      "is up", name);
        return ESP_ERR_NOT_FOUND;
    }
    return ESP_OK;
}

static int rate_adapt_cli_handler(int argc, char *argv[])
{
    if (argc < 2 || argc > 3) {
        ESP_LOGE(TAG, "Usage: rate-adapt <on|off> [controller]");
        return -1;
    }
    const bool on = (strcmp(argv[1], "on") == 0);
    if (!on && strcmp(argv[1], "off") != 0) {
        ESP_LOGE(TAG, "Usage: rate-adapt <on|off> [controller]");
        return -1;
    }
    video_rate_ctrl_handle_t h = NULL;
    if (rate_ctrl_by_name(argc, argv, 2, &h) != ESP_OK) {
        return -1;
    }
    video_rate_ctrl_enable(h, on);
    ESP_LOGI(TAG, "adaptive rate control %s for '%s'",
             on ? "ENABLED" : "disabled", (argc > 2) ? argv[2] : "webrtc");
    return 0;
}

static int rate_ceiling_cli_handler(int argc, char *argv[])
{
    if (argc < 2 || argc > 3) {
        ESP_LOGE(TAG, "Usage: rate-ceiling <bps|off> [controller]");
        return -1;
    }
    video_rate_ctrl_handle_t h = NULL;
    if (rate_ctrl_by_name(argc, argv, 2, &h) != ESP_OK) {
        return -1;
    }
    const uint32_t bps = (strcmp(argv[1], "off") == 0) ? 0 : (uint32_t)strtoul(argv[1], NULL, 10);
    video_rate_ctrl_set_network_ceiling_bps(h, bps);
    if (bps == 0) {
        ESP_LOGI(TAG, "network ceiling cleared; recovery is unrestricted again");
    } else {
        ESP_LOGI(TAG, "network ceiling forced to %" PRIu32 " bps", bps);
    }
    return 0;
}

static void register_cli(void)
{
    static const esp_console_cmd_t cmds[] = {
        {
            .command = "trigger-offer",
            .help    = "Trigger WebRTC offer to a peer. Usage: trigger-offer <peer_id>",
            .hint    = "<peer_id>",
            .func    = trigger_offer_cli_handler,
        },
        {
            .command = "start-webrtc",
            .help    = "Start WebRTC signaling and streaming",
            .hint    = NULL,
            .func    = start_webrtc_cli,
        },
        {
            .command = "stop-webrtc",
            .help    = "Stop WebRTC and tear down any sessions",
            .hint    = NULL,
            .func    = stop_webrtc_cli,
        },
        {
            .command = "detect",
            .help    = "Enable or disable person detection. Usage: detect <on|off>",
            .hint    = "<on|off>",
            .func    = detect_cli_handler,
        },
        {
            .command = "stats-overlay",
            .help    = "Show or hide the live rate overlay on the LCD (capture, encoder, "
                       "detector, display). Usage: stats-overlay <on|off>",
            .hint    = "<on|off>",
            .func    = stats_overlay_cli_handler,
        },
        {
            .command = "detect-stats",
            .help    = "Print per-sink delivery and drop counters",
            .hint    = NULL,
            .func    = detect_stats_cli_handler,
        },
        {
            .command = "sink-stats",
            .help    = "Print raw sinks, encoded sinks and rate-control state",
            .hint    = NULL,
            .func    = sink_stats_cli_handler,
        },
        {
            .command = "sink-stats-reset",
            .help    = "Zero the raw and encoded sink counters",
            .hint    = NULL,
            .func    = sink_stats_reset_cli_handler,
        },
        {
            .command = "rate-adapt",
            .help    = "Enable or disable adaptive rate control on one controller "
                       "(default webrtc). Usage: rate-adapt <on|off> [controller]",
            .hint    = "<on|off> [controller]",
            .func    = rate_adapt_cli_handler,
        },
        {
            .command = "rate-ceiling",
            .help    = "Force one controller's network ceiling, which is the field TWCC "
                       "feedback writes (default webrtc). "
                       "Usage: rate-ceiling <bps|off> [controller]",
            .hint    = "<bps|off> [controller]",
            .func    = rate_ceiling_cli_handler,
        },
    };
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        if (esp_console_cmd_register(&cmds[i]) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to register '%s'", cmds[i].command);
        }
    }
}

/* -------------------------------------------------------------------------- */
/*  Camera                                                                    */
/* -------------------------------------------------------------------------- */

/* Bring the camera up ourselves rather than waiting for a peer.
 *
 * kvs_media starts capture when a session begins, so without this the LCD would stay dark until
 * somebody connected. init/start are reference counted, so both this and kvs_media can hold the
 * camera and it comes down after the last of them - and because the FIRST caller's profile is
 * the one that takes effect, this is where the example's resolution is actually decided.
 * APP_VIDEO_* is that one definition; see app_ctrl.h. */
static esp_err_t camera_start(media_stream_video_capture_t *video_capture)
{
    if (video_capture == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    video_capture_config_t cfg = {
        .codec      = VIDEO_CODEC_H264,
        .resolution = { .width  = APP_VIDEO_WIDTH,
                        .height = APP_VIDEO_HEIGHT,
                        .fps    = APP_VIDEO_FPS },
        .quality    = 80,
        .bitrate    = APP_VIDEO_BITRATE,
    };
    esp_err_t err = video_capture->init(&cfg, &s_video_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "video_capture init failed: %s", esp_err_to_name(err));
        return err;
    }
    err = video_capture->start(s_video_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "video_capture start failed: %s", esp_err_to_name(err));
    }
    return err;
}

/* -------------------------------------------------------------------------- */

void app_main(void)
{
    esp_err_t ret;

    ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

#ifdef CONFIG_SLAVE_FLASHER_ENABLE
    /* Program the attached C6/C5 co-processor before bringing up esp_hosted / Wi-Fi, so the
     * network path is available once the slave is running. */
    ret = flash_slave();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to flash slave co-processor: %s", esp_err_to_name(ret));
        return;
    }
#endif

    ESP_LOGI(TAG, "KVS WebRTC + person detection + LCD preview");

    /* The work queue has to exist before the commands are registered, and it is now this
     * example's job to create it: app_webrtc_init() used to do it at boot, but WebRTC is
     * no longer started there, and start-webrtc itself runs ON this queue.
     *
     * Sized for its most demanding user, which is WebRTC. app_webrtc dispatches
     * peer-connection setup onto this same queue and kvs_initializePeerConnection() puts
     * an RtcConfiguration on the stack - several KB of ICE-server URIs. app_webrtc_init()
     * asks for 32 KB, but esp_work_queue_init_with_config() returns early once the queue
     * exists, so its request is ignored and only this value takes effect. Keep it >= 32 KB
     * or the first offer overflows the stack. */
    esp_work_queue_config_t work_queue_config = ESP_WORK_QUEUE_CONFIG_DEFAULT();
    work_queue_config.stack_size = 32 * 1024;
    work_queue_config.size = 128;
    if (esp_work_queue_init_with_config(&work_queue_config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize work queue");
        return;
    }
    if (esp_work_queue_start() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start work queue");
        return;
    }

    esp_cli_start();
    wifi_register_cli();
    register_cli();

    app_wifi_prov_config_t net_cfg = APP_NETWORK_CONFIG_DEFAULT();
#if CONFIG_ESP_HOSTED_ENABLE_BT_NIMBLE
    net_cfg.prov_start_cb = hosted_bt_prov_start;
    net_cfg.prov_end_cb = hosted_bt_prov_end;
#endif
    ret = app_wifi_prov_init(&net_cfg);
    if (ret == ESP_ERR_TIMEOUT) {
        /* Wi-Fi not connected within wifi_connect_timeout_ms — common on the P4 + C6 hosted path,
         * where co-processor association takes longer than the timeout. The connection continues
         * in the background; WebRTC must still initialize regardless (signaling connects once
         * Wi-Fi is up), so don't abort here. */
        ESP_LOGW(TAG, "Wi-Fi not connected yet; continuing (will connect in background)");
    } else if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WiFi provisioning init failed: %s", esp_err_to_name(ret));
        return;
    }

    app_storage_init();

    /* AWS SigV4 rejects a device whose clock is wrong, so this has to succeed before signaling. */
    esp_webrtc_time_sntp_time_sync_and_wait();

    media_stream_video_capture_t *video_capture = media_stream_get_video_capture_if();

    if (video_capture == NULL) {
        ESP_LOGE(TAG, "No video capture interface - this example needs a camera");
        return;
    }

    /* Sinks first, camera second: registration does not need the camera, and doing it in this
     * order means the preview and the detector see the very first frame. */
    if (preview_display_init() != ESP_OK) {
        ESP_LOGE(TAG, "LCD preview unavailable - continuing without it");
    }
    if (detect_sink_init() != ESP_OK) {
        ESP_LOGE(TAG, "Person detection unavailable - continuing without it");
    }
    camera_start(video_capture);

    /* Deliberately NOT starting WebRTC here.
     *
     * Everything above is local and needs no network: the camera runs, the LCD shows it and
     * the detector works, on a device that has never reached AWS. Streaming is a console
     * command, which is also what makes it measurable - a run can compare the pipeline with
     * a viewer against the same pipeline without one, and never reflash. */
    ESP_LOGI(TAG, "Ready. Local pipeline is live (preview + detection).");
    ESP_LOGI(TAG, "Console: start-webrtc | sink-stats | detect on|off");
}
