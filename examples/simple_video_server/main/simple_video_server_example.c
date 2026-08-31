/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Simple video server - HTTP MJPEG streaming.
 * Backend: direct V4L2 or media_stream (select via Kconfig).
 */
#include <string.h>
#include "esp_event.h"
#include "esp_err.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_http_server.h"
#include "protocol_examples_common.h"
#include "mdns.h"
#include "lwip/apps/netbiosns.h"

/* lwip sockets.h (pulled in by netbiosns.h above) defines BSD-style _IO/_IOR/_IOW;
 * the V4L2 headers below define the Linux-style encodings this file actually uses.
 * Neither header guards them, so drop the lwip ones before including V4L2. */
#undef _IO
#undef _IOR
#undef _IOW
#undef _IOWR

#if CONFIG_CAPTURE_DIRECT_V4L2
#include "example_video_common.h"
#include "esp_video_device.h"
#include "video_capture_direct.h"
#else
#include "example_video_common.h"
#include "video_capture_media_stream.h"
#include "video_capture.h"
#endif

#define PART_BOUNDARY          "123456789000000000000987654321"
#define EXAMPLE_MDNS_INSTANCE  "simple video web"
#define EXAMPLE_MDNS_HOST_NAME "esp-web"

static const char *STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char *TAG = "example";

static esp_err_t root_handler(httpd_req_t *req)
{
#if CONFIG_CAPTURE_DIRECT_V4L2
    const char *backend = "direct V4L2";
    const char *extra_links = "";
#else
    const char *backend = "media_stream";
    const char *extra_links = "<p><a href=\"/snapshot\">Snapshot (direct)</a> <small>append ?quality=1..100</small></p>";
#endif
    char html[512];
    snprintf(html, sizeof(html),
        "<html><head><title>Simple Video Server</title></head>"
        "<body><h1>Simple Video Server (%s)</h1>"
        "<p><a href=\"/stream\">MJPEG Stream</a></p>"
        "<p><a href=\"/capture\">Single Capture</a></p>"
        "<p><a href=\"/record.bin\">Record (raw)</a></p>"
        "%s"
        "</body></html>", backend, extra_links);
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
}

#if CONFIG_CAPTURE_DIRECT_V4L2
static esp_err_t stream_handler(httpd_req_t *req)
{
    web_cam_direct_t *cam = (web_cam_direct_t *)req->user_ctx;
    httpd_resp_set_type(req, STREAM_CONTENT_TYPE);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return web_cam_direct_stream(cam, req);
}

static esp_err_t pic_handler(httpd_req_t *req)
{
    web_cam_direct_t *cam = (web_cam_direct_t *)req->user_ctx;
    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=capture.jpg");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return web_cam_direct_capture_to_http(cam, req, true);
}

static esp_err_t record_bin_handler(httpd_req_t *req)
{
    web_cam_direct_t *cam = (web_cam_direct_t *)req->user_ctx;
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=record.bin");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return web_cam_direct_capture_to_http(cam, req, false);
}
#else
static esp_err_t stream_handler(httpd_req_t *req)
{
    web_cam_media_stream_t *cam = (web_cam_media_stream_t *)req->user_ctx;
    httpd_resp_set_type(req, STREAM_CONTENT_TYPE);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return web_cam_media_stream_stream(cam, req);
}

static esp_err_t pic_handler(httpd_req_t *req)
{
    web_cam_media_stream_t *cam = (web_cam_media_stream_t *)req->user_ctx;
    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=capture.jpg");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return web_cam_media_stream_capture_to_http(cam, req, true);
}

static esp_err_t record_bin_handler(httpd_req_t *req)
{
    web_cam_media_stream_t *cam = (web_cam_media_stream_t *)req->user_ctx;
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=record.bin");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return web_cam_media_stream_capture_to_http(cam, req, false);
}

/* /snapshot — calls video_capture_get_snapshot() directly (bypasses MJPEG pipeline).
 * Optional query parameter: ?quality=N (1-100, default from Kconfig). */
static esp_err_t snapshot_handler(httpd_req_t *req)
{
    uint8_t quality = CONFIG_CAPTURE_MEDIA_STREAM_QUALITY;

    /* Parse optional ?quality=N */
    size_t qlen = httpd_req_get_url_query_len(req);
    if (qlen > 0 && qlen < 32) {
        char query[32];
        if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
            char val[8];
            if (httpd_query_key_value(query, "quality", val, sizeof(val)) == ESP_OK) {
                int q = atoi(val);
                if (q >= 1 && q <= 100) {
                    quality = (uint8_t)q;
                }
            }
        }
    }

    uint8_t *jpeg_buf = NULL;
    size_t jpeg_len = 0;
    esp_err_t ret = video_capture_get_snapshot(&jpeg_buf, &jpeg_len, quality, 5000);
    if (ret != ESP_OK || !jpeg_buf) {
        ESP_LOGE(TAG, "video_capture_get_snapshot failed: %s", esp_err_to_name(ret));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Snapshot failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Snapshot: %zu bytes, quality=%u", jpeg_len, quality);

    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=snapshot.jpg");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    esp_err_t send_ret = httpd_resp_send(req, (const char *)jpeg_buf, jpeg_len);

    video_capture_snapshot_free(jpeg_buf);
    return send_ret;
}
#endif

static void initialise_mdns(void)
{
    if (mdns_init() != ESP_OK) {
        ESP_LOGE(TAG, "mDNS init failed");
        return;
    }
    mdns_hostname_set(EXAMPLE_MDNS_HOST_NAME);
    mdns_instance_name_set(EXAMPLE_MDNS_INSTANCE);
    mdns_txt_item_t txt[] = {{"board", "esp32"}, {"path", "/"}};
    mdns_service_add("ESP32-WebServer", "_http", "_tcp", 80, txt, 2);
}

void app_main(void)
{
#if CONFIG_CAPTURE_DIRECT_V4L2
    ESP_LOGI(TAG, "Starting simple_video_server (direct V4L2)");
#else
    ESP_LOGI(TAG, "Starting simple_video_server (media_stream)");
#endif

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    initialise_mdns();
    netbiosns_init();
    netbiosns_set_name(EXAMPLE_MDNS_HOST_NAME);
    ESP_ERROR_CHECK(example_connect());

#if CONFIG_CAPTURE_DIRECT_V4L2
    ESP_ERROR_CHECK(example_video_init());
    web_cam_direct_t *cam = NULL;
    ret = web_cam_direct_init(ESP_VIDEO_MIPI_CSI_DEVICE_NAME, &cam);
    if (ret != ESP_OK || !cam) {
        ESP_LOGE(TAG, "web_cam_direct_init failed: %s", esp_err_to_name(ret));
        return;
    }
    void *user_ctx = cam;
#else
    /* Use example_video_init so camera setup matches direct path; esp_video_if reuses /dev/video0 */
    ESP_ERROR_CHECK(example_video_init());
    web_cam_media_stream_t *cam = NULL;
    ret = web_cam_media_stream_init(
        CONFIG_CAPTURE_MEDIA_STREAM_WIDTH,
        CONFIG_CAPTURE_MEDIA_STREAM_HEIGHT,
        CONFIG_CAPTURE_MEDIA_STREAM_QUALITY,
        &cam);
    if (ret != ESP_OK || !cam) {
        ESP_LOGE(TAG, "web_cam_media_stream_init failed: %s", esp_err_to_name(ret));
        return;
    }
    void *user_ctx = cam;
#endif

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.lru_purge_enable = true;
    cfg.max_open_sockets = 5;
    cfg.send_wait_timeout = 10;
    cfg.recv_wait_timeout = 10;

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed");
#if CONFIG_CAPTURE_DIRECT_V4L2
        web_cam_direct_deinit(cam);
#else
        web_cam_media_stream_deinit(cam);
#endif
        return;
    }

    httpd_uri_t root = { .uri = "/", .method = HTTP_GET, .handler = root_handler, .user_ctx = NULL };
    httpd_uri_t stream = { .uri = "/stream", .method = HTTP_GET, .handler = stream_handler, .user_ctx = user_ctx };
    httpd_uri_t pic = { .uri = "/capture", .method = HTTP_GET, .handler = pic_handler, .user_ctx = user_ctx };
    httpd_uri_t rec = { .uri = "/record.bin", .method = HTTP_GET, .handler = record_bin_handler, .user_ctx = user_ctx };

    httpd_register_uri_handler(server, &root);
    httpd_register_uri_handler(server, &stream);
    httpd_register_uri_handler(server, &pic);
    httpd_register_uri_handler(server, &rec);

#if !CONFIG_CAPTURE_DIRECT_V4L2
    httpd_uri_t snap = { .uri = "/snapshot", .method = HTTP_GET, .handler = snapshot_handler, .user_ctx = user_ctx };
    httpd_register_uri_handler(server, &snap);
#endif

    ESP_LOGI(TAG, "Camera web server: http://" EXAMPLE_MDNS_HOST_NAME ".local:%d/stream", cfg.server_port);
    ESP_LOGI(TAG, "If mDNS fails: http://<device-ip>:%d/stream", cfg.server_port);
}
