/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "sdkconfig.h"

#if CONFIG_IDF_TARGET_ESP32P4

/* Inside the target guard: linux/videodev2.h comes from esp_video, which only exists
 * for the P4, and this file is expected to compile to an empty object elsewhere. */
#include "H264FrameGrabber.h"
#include "linux/videodev2.h"

#if CONFIG_USE_ESP_VIDEO_IF
#define USE_ESP_VIDEO_IF 1
#endif

#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "esp_log.h"

#include "esp_dma_utils.h"
#include "video_rate_ctrl.h"
#include "esp_video_if_cam_sel.h"   /* MEDIA_STREAM_ENABLE_*_CAM_SENSOR selection */

#include "bsp/esp-bsp.h"
#if !USE_ESP_VIDEO_IF
/* Old camera API path - only available with local component override */
/* Define camera clock rate constants if not using esp_video_if */
#ifndef OV5647_MIPI_IDI_CLOCK_RATE_720P_50FPS
#define OV5647_MIPI_IDI_CLOCK_RATE_720P_50FPS   (74000000ULL)
#define OV5647_MIPI_CSI_LINE_RATE_720P_50FPS   (OV5647_MIPI_IDI_CLOCK_RATE_720P_50FPS * 4)
#define OV5647_MIPI_IDI_CLOCK_RATE_1080P_22FPS  (98437500ULL)
#define OV5647_MIPI_CSI_LINE_RATE_1080P_22FPS  (OV5647_MIPI_IDI_CLOCK_RATE_1080P_22FPS * 4)
#endif
/* Note: bsp/camera.h is not available in component manager version */
/* This path requires local component override */
#include "bsp/camera.h"
#endif
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"

#include "webrtc_mem_utils.h"
#include "video_capture.h"
#include "esp_h264_alloc.h"
#if USE_ESP_VIDEO_IF
#include "esp_video_if.h"
#endif

static const char *TAG = "esp32p4_frame_grabber";
#define H264_ENCODE     1
// #define SDCARD_SAVE     1

/* Annex-B helpers for camera-supplied H.264 (UVC passthrough).
 *
 * The camera emits SPS/PPS only every few seconds, so a viewer that joins in
 * between has nothing to initialise its decoder with and shows a black frame even
 * though RTP is flowing. Cache the parameter sets and prepend them to every IDR. */
#define H264_PS_MAX 128

static uint8_t s_sps[H264_PS_MAX], s_pps[H264_PS_MAX];
static uint32_t s_sps_len, s_pps_len;

/* Parameter sets describe one specific format/resolution, so they must not outlive it:
 * a stale SPS prepended after a resolution change tells the viewer the wrong geometry. */
/* True while the encoder task sits blocked on its run semaphore holding no capture
 * buffer. esp32p4_frame_grabber_stop() waits for this before returning so that whoever
 * tears the capture buffers down next cannot unmap memory the task is still reading. */
static volatile bool s_task_parked = true;


#if MEDIA_STREAM_ENABLE_USB_UVC_CAM_SENSOR && !CONFIG_MEDIA_STREAM_UVC_PASSTHROUGH_H264
/* YUY2 -> O_UYY_E_VYY, the only raw layout the P4 hardware encoder accepts.
 *
 * Both formats carry one chroma pair per two pixels, so this is a repack rather than a
 * resample: YUY2 stores Y0 U Y1 V, and the encoder wants U,Y0,Y1 on odd lines and
 * V,Y0,Y1 on even lines. Dropping the unused chroma of each line is what turns 4:2:2
 * into the encoder's 4:2:0-equivalent input. */
static void yuy2_to_o_uyy_e_vyy(const uint8_t *src, uint8_t *dst, uint32_t width, uint32_t height)
{
    for (uint32_t y = 0; y < height; y++) {
        const uint8_t *s = src + (size_t) y * width * 2;
        uint8_t *d = dst + (size_t) y * width * 3 / 2;
        const bool odd_line = ((y & 1u) == 0u);   /* line 0 is the first "odd" line */

        for (uint32_t x = 0; x < width; x += 2) {
            const uint8_t y0 = s[0];
            const uint8_t u  = s[1];
            const uint8_t y1 = s[2];
            const uint8_t v  = s[3];
            s += 4;

            *d++ = odd_line ? u : v;
            *d++ = y0;
            *d++ = y1;
        }
    }
}
#endif

#if MEDIA_STREAM_ENABLE_USB_UVC_CAM_SENSOR && !CONFIG_MEDIA_STREAM_UVC_PASSTHROUGH_H264
static uint8_t *s_conv_buf;
static size_t s_conv_buf_len;
#endif

/* Camera bring-up can miss a transient window (USB enumeration in particular). */
#define CAMERA_INIT_MAX_ATTEMPTS     3
#define CAMERA_INIT_RETRY_DELAY_MS   500

/* Set while the pipeline waits for a key frame to resync on (see the encoder task). */
static volatile bool s_wait_for_idr = true;
static uint32_t s_resync_dropped;

static void h264_forget_parameter_sets(void)
{
    s_sps_len = 0;
    s_pps_len = 0;
}

typedef struct {
    bool has_idr;
    bool has_sps;
    bool has_pps;
} h264_au_info_t;

static void h264_scan_au(const uint8_t *buf, uint32_t len, h264_au_info_t *info)
{
    memset(info, 0, sizeof(*info));

    for (uint32_t i = 0; i + 4 < len; i++) {
        if (buf[i] != 0x00 || buf[i + 1] != 0x00) {
            continue;
        }
        uint32_t sc_len;
        if (buf[i + 2] == 0x01) {
            sc_len = 3;
        } else if (buf[i + 2] == 0x00 && buf[i + 3] == 0x01) {
            sc_len = 4;
        } else {
            continue;
        }
        const uint32_t nal_off = i + sc_len;
        if (nal_off >= len) {
            break;
        }
        const uint8_t nal_type = buf[nal_off] & 0x1f;

        /* Find the next start code so parameter sets can be cached whole. */
        uint32_t next = len;
        for (uint32_t j = nal_off + 1; j + 3 < len; j++) {
            if (buf[j] == 0x00 && buf[j + 1] == 0x00 &&
                    (buf[j + 2] == 0x01 || (buf[j + 2] == 0x00 && buf[j + 3] == 0x01))) {
                next = j;
                break;
            }
        }

        switch (nal_type) {
        case 5:
            info->has_idr = true;
            break;
        case 7: {
            info->has_sps = true;
            const uint32_t nal_len = next - i;
            if (nal_len > H264_PS_MAX) {
                ESP_LOGW(TAG, "SPS of %" PRIu32 " bytes exceeds the %d-byte cache; joining viewers"
                              " will wait for the camera to resend it", nal_len, H264_PS_MAX);
            } else {
                memcpy(s_sps, buf + i, nal_len);
                s_sps_len = nal_len;
                /* profile_idc / level_idc sit right after the NAL header. */
                if (nal_off + 3 < len) {
                    static uint8_t logged_profile, logged_level;
                    if (logged_profile != buf[nal_off + 1] || logged_level != buf[nal_off + 3]) {
                        logged_profile = buf[nal_off + 1];
                        logged_level = buf[nal_off + 3];
                        ESP_LOGW(TAG, "camera H.264 SPS: profile_idc=0x%02x level_idc=0x%02x (profile-level-id %02x%02x%02x)",
                                 logged_profile, logged_level, logged_profile, buf[nal_off + 2], logged_level);
                    }
                }
            }
            break;
        }
        case 8: {
            info->has_pps = true;
            const uint32_t nal_len = next - i;
            if (nal_len > H264_PS_MAX) {
                ESP_LOGW(TAG, "PPS of %" PRIu32 " bytes exceeds the %d-byte cache", nal_len, H264_PS_MAX);
            } else {
                memcpy(s_pps, buf + i, nal_len);
                s_pps_len = nal_len;
            }
            break;
        }
        default:
            break;
        }
        i = nal_off;
    }
}

#if H264_ENCODE
#include "esp_h264_hw_enc.h"
#include "esp_h264_alloc.h"
#include "esp_cache.h"
#endif

#if SDCARD_SAVE
static FILE* f264 = NULL;
#endif

static volatile int frames_received_cnt = 0;

#ifndef USE_ESP_VIDEO_IF
static esp_h264_out_buf_t h264_out_data;
static uint8_t *jpeg_last_buf;
static uint8_t *jpeg_next_buf;
static uint8_t *camera_next_buf;
static uint8_t *camera_last_buf;

static bool camera_trans_done(void)
{
    jpeg_next_buf = camera_last_buf;

    bsp_camera_set_frame_buffer(camera_next_buf);
    camera_last_buf = camera_next_buf;

    frames_received_cnt++; // Update the frames received count
    return true;
}

static esp_err_t camera_init(void)
{
    size_t camera_fb_size = 0;
    bsp_camera_config_t camera_cfg = {
        .hor_res = WIDTH,
        .ver_res = HEIGHT,
        .fb_size_ptr = &camera_fb_size,
        .num_fbs = 3,
        .input_data_color_type = ISP_COLOR_RAW8,
        .output_data_color_type = ISP_COLOR_YUV420,
#if WIDTH > 1280
        .clock_rate_hz = OV5647_MIPI_IDI_CLOCK_RATE_1080P_22FPS,
        .csi_lane_rate_mbps = OV5647_MIPI_CSI_LINE_RATE_1080P_22FPS / 1000 / 1000,
#else
        .clock_rate_hz = OV5647_MIPI_IDI_CLOCK_RATE_720P_50FPS,
        .csi_lane_rate_mbps = OV5647_MIPI_CSI_LINE_RATE_720P_50FPS / 1000 / 1000,
#endif
        .flags = {
            .use_external_fb = 0,
        },
    };
    const isp_config_t isp_cfg = ISP_CONFIG_DEFAULT(camera_cfg.hor_res, camera_cfg.ver_res,
                                                    ISP_COLOR_RAW8, ISP_COLOR_YUV420);
    if(bsp_camera_new(&camera_cfg, &isp_cfg, NULL) != ESP_OK) {
        ESP_LOGE(TAG, "camera initialization failed");
        webrtc_mem_utils_print_stats(TAG);
        return ESP_FAIL;
    }
    ESP_ERROR_CHECK(bsp_camera_register_trans_done_callback(camera_trans_done));
    ESP_ERROR_CHECK(bsp_camera_get_frame_buffer(3, (void **) &camera_last_buf,
                                                (void **) &camera_next_buf, (void **) &jpeg_last_buf));
    jpeg_next_buf = jpeg_last_buf;

    return ESP_OK;
}

#if H264_ENCODE

// Data read callback to read raw data



static void data_read_callback(void *ctx, esp_h264_buf_t *in_data)
{
    if (jpeg_next_buf != jpeg_last_buf) {
        /* New frame not available */
        camera_next_buf = jpeg_last_buf;
        jpeg_last_buf = jpeg_next_buf;
        in_data->buffer = NULL;
    } else {
        in_data->buffer = jpeg_next_buf;
    }
}

// Data write callback to output encoded frames
static void data_write_callback(void *ctx, esp_h264_out_buf_t *out_data)
{
    h264_out_data.buffer = out_data->buffer;
    h264_out_data.len = out_data->len;
    h264_out_data.type = out_data->type;

    frames_received_cnt++;

#if SDCARD_SAVE
    if (f264) {
        esp_cache_msync(out_data->buffer, (out_data->len + 63) & ~63U, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
        int wr_len = fwrite(out_data->buffer, 1, out_data->len, f264);
        if (wr_len != out_data->len) {
            ESP_LOGW(TAG, "expected wr: %" PRIu32 " actual: %d", out_data->len, wr_len);
        }
    }
#endif
}
#endif
#endif

static void init_chip()
{
    asm volatile("li t0, 0x2000\n"
                 "csrrs t0, mstatus, t0\n"); /* FPU_state = 1 (initial) */
    asm volatile("li t0, 0x1\n"
                 "csrrs t0, 0x7F1, t0\n"); /* HWLP_state = 1 (initial) */
    asm volatile("li t0, 0x1\n"
                 "csrrs t0, 0x7F2, t0\n"); /* AIA_state = 1 (initial) */
}

void init_clock(void)
{
#include "soc/hp_sys_clkrst_reg.h"

    uint32_t rd;

    REG_SET_FIELD(HP_SYS_CLKRST_PERI_CLK_CTRL02_REG, HP_SYS_CLKRST_REG_MIPI_DSI_DPHY_CLK_SRC_SEL, 1);
    REG_SET_FIELD(HP_SYS_CLKRST_PERI_CLK_CTRL03_REG, HP_SYS_CLKRST_REG_MIPI_CSI_DPHY_CLK_SRC_SEL, 1);
    REG_CLR_BIT(HP_SYS_CLKRST_PERI_CLK_CTRL03_REG, HP_SYS_CLKRST_REG_MIPI_DSI_DPHY_CFG_CLK_EN);
    REG_SET_BIT(HP_SYS_CLKRST_PERI_CLK_CTRL03_REG, HP_SYS_CLKRST_REG_MIPI_DSI_DPHY_CFG_CLK_EN);
    REG_CLR_BIT(HP_SYS_CLKRST_PERI_CLK_CTRL03_REG, HP_SYS_CLKRST_REG_MIPI_DSI_DPHY_PLL_REFCLK_EN);
    REG_SET_BIT(HP_SYS_CLKRST_PERI_CLK_CTRL03_REG, HP_SYS_CLKRST_REG_MIPI_DSI_DPHY_PLL_REFCLK_EN);
    REG_CLR_BIT(HP_SYS_CLKRST_PERI_CLK_CTRL03_REG, HP_SYS_CLKRST_REG_MIPI_CSI_DPHY_CFG_CLK_EN);
    REG_SET_BIT(HP_SYS_CLKRST_PERI_CLK_CTRL03_REG, HP_SYS_CLKRST_REG_MIPI_CSI_DPHY_CFG_CLK_EN);
    REG_CLR_BIT(HP_SYS_CLKRST_PERI_CLK_CTRL03_REG, HP_SYS_CLKRST_REG_MIPI_DSI_DPHY_PLL_REFCLK_EN);
    REG_SET_BIT(HP_SYS_CLKRST_PERI_CLK_CTRL03_REG, HP_SYS_CLKRST_REG_MIPI_DSI_DPHY_PLL_REFCLK_EN);

    REG_CLR_BIT(HP_SYS_CLKRST_SOC_CLK_CTRL1_REG, HP_SYS_CLKRST_REG_DSI_SYS_CLK_EN);
    REG_SET_BIT(HP_SYS_CLKRST_SOC_CLK_CTRL1_REG, HP_SYS_CLKRST_REG_DSI_SYS_CLK_EN);
    REG_SET_BIT(HP_SYS_CLKRST_HP_RST_EN0_REG, HP_SYS_CLKRST_REG_RST_EN_DSI_BRG);
    REG_CLR_BIT(HP_SYS_CLKRST_HP_RST_EN0_REG, HP_SYS_CLKRST_REG_RST_EN_DSI_BRG);

    REG_CLR_BIT(HP_SYS_CLKRST_SOC_CLK_CTRL1_REG, HP_SYS_CLKRST_REG_CSI_HOST_SYS_CLK_EN);
    REG_SET_BIT(HP_SYS_CLKRST_SOC_CLK_CTRL1_REG, HP_SYS_CLKRST_REG_CSI_HOST_SYS_CLK_EN);
    REG_CLR_BIT(HP_SYS_CLKRST_SOC_CLK_CTRL1_REG, HP_SYS_CLKRST_REG_CSI_BRG_SYS_CLK_EN);
    REG_SET_BIT(HP_SYS_CLKRST_SOC_CLK_CTRL1_REG, HP_SYS_CLKRST_REG_CSI_BRG_SYS_CLK_EN);
    REG_SET_BIT(HP_SYS_CLKRST_HP_RST_EN0_REG, HP_SYS_CLKRST_REG_RST_EN_CSI_HOST);
    REG_CLR_BIT(HP_SYS_CLKRST_HP_RST_EN0_REG, HP_SYS_CLKRST_REG_RST_EN_CSI_HOST);
    REG_SET_BIT(HP_SYS_CLKRST_HP_RST_EN0_REG, HP_SYS_CLKRST_REG_RST_EN_CSI_BRG);
    REG_CLR_BIT(HP_SYS_CLKRST_HP_RST_EN0_REG, HP_SYS_CLKRST_REG_RST_EN_CSI_BRG);

    // REG_SET_FIELD(HP_SYS_CLKRST_PERI_CLK_CTRL03_REG, HP_SYS_CLKRST_REG_MIPI_DSI_DPICLK_DIV_NUM, (480000000 / MIPI_DPI_CLOCK_RATE) - 1);
    REG_SET_FIELD(HP_SYS_CLKRST_PERI_CLK_CTRL03_REG, HP_SYS_CLKRST_REG_MIPI_DSI_DPICLK_SRC_SEL, 1);
    REG_SET_BIT(HP_SYS_CLKRST_PERI_CLK_CTRL03_REG, HP_SYS_CLKRST_REG_MIPI_DSI_DPICLK_EN);

    REG_CLR_BIT(HP_SYS_CLKRST_SOC_CLK_CTRL1_REG, HP_SYS_CLKRST_REG_GDMA_SYS_CLK_EN);
    REG_SET_BIT(HP_SYS_CLKRST_SOC_CLK_CTRL1_REG, HP_SYS_CLKRST_REG_GDMA_SYS_CLK_EN);
    REG_SET_BIT(HP_SYS_CLKRST_HP_RST_EN0_REG, HP_SYS_CLKRST_REG_RST_EN_GDMA);
    REG_CLR_BIT(HP_SYS_CLKRST_HP_RST_EN0_REG, HP_SYS_CLKRST_REG_RST_EN_GDMA);

    REG_SET_FIELD(HP_SYS_CLKRST_PERI_CLK_CTRL26_REG, HP_SYS_CLKRST_REG_ISP_CLK_DIV_NUM, 1 - 1);
    REG_SET_FIELD(HP_SYS_CLKRST_PERI_CLK_CTRL25_REG, HP_SYS_CLKRST_REG_ISP_CLK_SRC_SEL, 1);
    REG_CLR_BIT(HP_SYS_CLKRST_PERI_CLK_CTRL25_REG, HP_SYS_CLKRST_REG_ISP_CLK_EN);
    REG_SET_BIT(HP_SYS_CLKRST_PERI_CLK_CTRL25_REG, HP_SYS_CLKRST_REG_ISP_CLK_EN);
    REG_SET_BIT(HP_SYS_CLKRST_HP_RST_EN0_REG, HP_SYS_CLKRST_REG_RST_EN_ISP);
    REG_CLR_BIT(HP_SYS_CLKRST_HP_RST_EN0_REG, HP_SYS_CLKRST_REG_RST_EN_ISP);

    rd = REG_READ(HP_SYS_CLKRST_REF_CLK_CTRL1_REG);
    REG_WRITE(HP_SYS_CLKRST_REF_CLK_CTRL1_REG, rd | HP_SYS_CLKRST_REG_REF_240M_CLK_EN);

    rd = REG_READ(HP_SYS_CLKRST_REF_CLK_CTRL2_REG);
    REG_WRITE(HP_SYS_CLKRST_REF_CLK_CTRL2_REG, rd | HP_SYS_CLKRST_REG_REF_160M_CLK_EN);

    rd = REG_READ(HP_SYS_CLKRST_HP_RST_EN2_REG);
    REG_WRITE(HP_SYS_CLKRST_HP_RST_EN2_REG, rd & (~HP_SYS_CLKRST_REG_RST_EN_H264));

    rd = REG_READ(HP_SYS_CLKRST_SOC_CLK_CTRL1_REG);
    rd = rd | HP_SYS_CLKRST_REG_H264_SYS_CLK_EN | HP_SYS_CLKRST_REG_GPSPI3_SYS_CLK_EN | HP_SYS_CLKRST_REG_AXI_PDMA_SYS_CLK_EN | HP_SYS_CLKRST_REG_GDMA_SYS_CLK_EN;
    rd = rd | HP_SYS_CLKRST_REG_CSI_HOST_SYS_CLK_EN | HP_SYS_CLKRST_REG_CSI_BRG_SYS_CLK_EN;
    REG_WRITE(HP_SYS_CLKRST_SOC_CLK_CTRL1_REG, rd);
    // H264_DMA
    rd = REG_READ(HP_SYS_CLKRST_PERI_CLK_CTRL26_REG);
    rd |= HP_SYS_CLKRST_REG_H264_CLK_EN;
    rd |= HP_SYS_CLKRST_REG_H264_CLK_SRC_SEL;
    REG_WRITE(HP_SYS_CLKRST_PERI_CLK_CTRL26_REG, rd);
}

void esp32p4_frame_grabber_cleanup(void)
{
#if SDCARD_SAVE
    if (f264) {
        fflush(f264);
        fclose(f264);
    }
#endif
}

typedef struct {
    QueueHandle_t frame_queue;
    TaskHandle_t encoder_task_handle;
    StaticTask_t *task_buffer;
    void *task_stack;
    bool encoder_initialized;
    bool running;
    SemaphoreHandle_t run_semaphore;
} esp32p4_encoder_data_t;

static esp32p4_encoder_data_t s_p4_enc_data = {0};

/* -------------------------------------------------------------------------- */
/*  Snapshot interceptor state                                                */
/* -------------------------------------------------------------------------- */

typedef struct {
    volatile bool requested;            /* Set by snapshot API, cleared by encoder task */
    SemaphoreHandle_t done;             /* Signaled when raw frame is copied */
    uint8_t *buffer;                    /* Destination buffer (caller-allocated) */
    size_t buffer_size;                 /* Allocated buffer size */
    size_t frame_len;                   /* Actual copied frame length */
    uint16_t width;                     /* Frame width */
    uint16_t height;                    /* Frame height */
    video_frame_pixformat_t pixfmt;     /* Pixel format of captured frame */
} snapshot_intercept_t;

static snapshot_intercept_t s_snapshot = {0};

#define QUEUE_RECEIVE_WAIT_MS  CONFIG_VIDEO_QUEUE_RECEIVE_WAIT_MS
#define QUEUE_SEND_WAIT_MS     CONFIG_VIDEO_QUEUE_SEND_WAIT_MS

esp_h264_out_buf_t *esp32p4_grab_one_frame()
{
    esp_h264_out_buf_t *frame_data = heap_caps_calloc(1, sizeof(esp_h264_out_buf_t), MALLOC_CAP_SPIRAM);
    if (xQueueReceive(s_p4_enc_data.frame_queue, frame_data, pdMS_TO_TICKS(QUEUE_RECEIVE_WAIT_MS)) != pdTRUE) {
        heap_caps_free(frame_data);
        return NULL;
    }
    return frame_data;
}

extern void *get_buffer();
extern esp_err_t esp_h264_hw_enc_set_reset_request();
static void video_encoder_task(void *arg)
{
    ESP_LOGD(TAG, "Video encoder task started (singleton mode - runs continuously)");

    while (1) {
        // Check if encoding should be running
        if (!s_p4_enc_data.running) {
            ESP_LOGD(TAG, "Video encoder paused, waiting for start signal...");
            /* Published before blocking: it tells esp32p4_frame_grabber_stop() that no
             * capture buffer is held any more, so the caller may unmap them. */
            s_task_parked = true;
            // Wait for start signal (blocking)
            xSemaphoreTake(s_p4_enc_data.run_semaphore, portMAX_DELAY);
            s_task_parked = false;
            ESP_LOGD(TAG, "Video encoder resumed");
        }

        video_frame_preprocess_fn_t frame_preprocess_fn = (video_frame_preprocess_fn_t) arg;
#if USE_ESP_VIDEO_IF
        // Get raw frame
        video_fb_t *raw_frame = esp_video_if_get_frame();
        if (!raw_frame) {
            /* Log rate-limited so we can see if the encoder task is starved
             * waiting for camera frames (ISP recovery during fast motion). */
            static uint32_t null_frame_count;
            null_frame_count++;
            if (null_frame_count == 1 || (null_frame_count & 0x3F) == 0) {
                ESP_LOGW(TAG, "esp_video_if_get_frame() returned NULL (count=%" PRIu32 ")", null_frame_count);
            }
            vTaskDelay(pdMS_TO_TICKS(QUEUE_RECEIVE_WAIT_MS));
            continue;
        }

        /* What the camera actually hands us decides everything below: a UVC camera
         * delivering H.264 gives a compressed access unit, not a YUV420 surface. */
        uint32_t capture_pixfmt = 0;
        const bool is_passthrough = (esp_video_if_get_pixel_format(&capture_pixfmt) == ESP_OK &&
                                     capture_pixfmt == V4L2_PIX_FMT_H264);

        // Snapshot interceptor: if a snapshot is requested, copy the raw frame
        /* Not for passthrough: consumers expect a raw surface of w*h*3/2 and would
         * read a compressed buffer as YUV420. */
        if (!is_passthrough && s_snapshot.requested && s_snapshot.buffer) {
            video_resolution_t snap_res = {0};
            esp_video_if_get_resolution(&snap_res);

            size_t copy_len = (raw_frame->len <= s_snapshot.buffer_size)
                              ? raw_frame->len : s_snapshot.buffer_size;
            memcpy(s_snapshot.buffer, raw_frame->buf, copy_len);
            s_snapshot.frame_len = copy_len;
            s_snapshot.width = snap_res.width;
            s_snapshot.height = snap_res.height;
            s_snapshot.pixfmt = PIXFMT_YUV420;
            s_snapshot.requested = false;

            if (s_snapshot.done) {
                xSemaphoreGive(s_snapshot.done);
            }
        }

        // This assumes frame is YUV420, so it cannot run on a passthrough (H.264) buffer
        if (!is_passthrough && frame_preprocess_fn)
        {
            video_resolution_t resolution;
            esp_err_t err = esp_video_if_get_resolution(&resolution);
            if (err == ESP_OK) {
                video_frame_raw_t frame = {
                    .len = raw_frame->len,
                    .buffer = raw_frame->buf,
                    .height = resolution.height,
                    .width = resolution.width,
                    .pixfmt = PIXFMT_YUV420,
                };

                frame_preprocess_fn(frame);
            }
        }

        /* Both congestion-control stages assume we own the encoder, which is not the
         * case when the camera hands us H.264 directly: its bitrate is not ours to set,
         * and dropping individual frames from a compressed stream breaks the reference
         * chain, so every skip would cost a full GOP of resync. Leave the passthrough
         * stream alone and let the queue and IDR resync absorb congestion. */
#if MEDIA_STREAM_ENABLE_USB_UVC_CAM_SENSOR && CONFIG_MEDIA_STREAM_UVC_PASSTHROUGH_H264
        /* No encoder exists in this build; start() already refuses a raw format, this
         * only covers a format change behind its back. */
        if (!is_passthrough) {
            esp_video_if_release_frame(raw_frame);
            continue;
        }
#endif

        if (!is_passthrough) {
            /* Congestion control stage 2: apply any pending encoder bitrate
             * change requested by the rate controller. */
            uint32_t new_bitrate = video_rate_ctrl_pull_bitrate_bps();
            if (new_bitrate) {
                esp_h264_hw_enc_set_bitrate(new_bitrate);
            }

            /* Congestion control stage 1: skip this frame to lower effective
             * fps (snapshot + preprocess above still ran). Releasing without
             * encoding saves both encoder CPU and uplink bandwidth. */
            if (!video_rate_ctrl_should_encode()) {
                esp_video_if_release_frame(raw_frame);
                continue;
            }
        }

        // Encode the raw frame, or pass it through if the camera already encoded it
        esp_h264_out_buf_t *frame = NULL;
        {
            if (!is_passthrough) {
#if MEDIA_STREAM_ENABLE_USB_UVC_CAM_SENSOR && !CONFIG_MEDIA_STREAM_UVC_PASSTHROUGH_H264
                /* A UVC camera delivers YUY2; the hardware encoder only takes
                 * O_UYY_E_VYY, so repack before handing it over. */
                video_resolution_t enc_res = {0};
                if (esp_video_if_get_resolution(&enc_res) == ESP_OK && enc_res.width && enc_res.height) {
                    const size_t need = (size_t) enc_res.width * enc_res.height * 3 / 2;
                    if (s_conv_buf_len < need) {
                        if (s_conv_buf) {
                            heap_caps_free(s_conv_buf);
                        }
                        s_conv_buf = heap_caps_aligned_calloc(64, 1, need, MALLOC_CAP_SPIRAM);
                        s_conv_buf_len = s_conv_buf ? need : 0;
                    }
                    if (s_conv_buf) {
                        yuy2_to_o_uyy_e_vyy(raw_frame->buf, s_conv_buf, enc_res.width, enc_res.height);
                        frame = esp_h264_hw_enc_encode_frame(s_conv_buf, need);
                    }
                }
#else
                frame = esp_h264_hw_enc_encode_frame(raw_frame->buf, raw_frame->len);
#endif
            } else {
                /* Camera already delivers H.264: pass it through. */
                frame = heap_caps_aligned_calloc(64, 1, sizeof(esp_h264_out_buf_t), MALLOC_CAP_SPIRAM);
                if (frame != NULL) {
                    h264_au_info_t au;
                    h264_scan_au(raw_frame->buf, raw_frame->len, &au);

                    /* A keyframe without parameter sets is undecodable for a viewer
                     * that joined after the camera last sent them, so prepend the
                     * cached copies. */
                    const bool prepend_ps = (au.has_idr && (!au.has_sps || !au.has_pps) &&
                                             s_sps_len && s_pps_len);
                    const uint32_t ps_len = prepend_ps ? (s_sps_len + s_pps_len) : 0;

                    frame->buffer = heap_caps_aligned_calloc(64, 1, ps_len + raw_frame->len, MALLOC_CAP_SPIRAM);
                    if (frame->buffer == NULL) {
                        heap_caps_free(frame);
                        frame = NULL;
                    } else {
                        if (prepend_ps) {
                            memcpy(frame->buffer, s_sps, s_sps_len);
                            memcpy(frame->buffer + s_sps_len, s_pps, s_pps_len);
                        }
                        memcpy(frame->buffer + ps_len, raw_frame->buf, raw_frame->len);
                        frame->len = ps_len + raw_frame->len;
                        /* Classify from the bitstream: the RTP packetizer uses this for
                         * IDR handling, and marking every frame IDR leaves joining
                         * viewers without parameter sets. */
                        frame->type = au.has_idr ? ESP_H264_FRAME_TYPE_IDR : ESP_H264_FRAME_TYPE_P;
                    }
                }
            }
        }

        // Release the raw frame as we're done with it
        esp_video_if_release_frame(raw_frame);

        // If encoding failed, continue to next frame
        if (!frame) {
            static uint32_t enc_fail_count;
            enc_fail_count++;
            if (enc_fail_count == 1 || (enc_fail_count & 0x1F) == 0) {
                ESP_LOGW(TAG, "esp_h264_hw_enc_encode_frame() failed (count=%" PRIu32 ")", enc_fail_count);
            }
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
#else
        static int frames_cnt = 0;
        esp_err_t ret = esp_h264_hw_enc_process_one_frame();
        if (ret != ESP_OK) {
            if (ret != ESP_ERR_NOT_FOUND) {
                ESP_LOGE(TAG, "Encoding failed");
            }
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        frames_cnt++;
        if (frames_cnt % 100 == 0) {
            ESP_LOGI(TAG, "frames_received_cnt %d, curr frame len %" PRIu32, frames_received_cnt, h264_out_data.len);
            print_mem_stats(TAG);
            if (frames_cnt % 250 == 0) {
                // Keep setting the bitrate periodically
                esp_h264_hw_enc_set_reset_request();
            }
        }

        /* Skip empty encoder output. esp_h264_hw_enc_process_one_frame() can
         * legitimately return ESP_OK with len=0 (e.g. skipped P-frame slot).
         * Passing such a frame downstream produces calloc(_, 0) ->
         * pathological tiny ptr -> later free() corrupts TLSF (same crash
         * family as the Opus zero-frame bug seen in writeFrame heap fault).
         */
        if (h264_out_data.len == 0) {
            continue;
        }

        // frame copy
        esp_h264_out_buf_t *frame = calloc(1, sizeof(esp_h264_out_buf_t));
        if (!frame) {
            ESP_LOGE(TAG, "Failed to alloc frame");
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        frame->len = h264_out_data.len;
        frame->buffer = heap_caps_aligned_calloc(64, 1, frame->len, MALLOC_CAP_SPIRAM);
        if (!frame->buffer) {
            ESP_LOGE(TAG, "Failed to alloc buffer. size %d", (int) frame->len);
            free(frame);
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        esp_cache_msync(h264_out_data.buffer, (frame->len + 63) & ~63, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
        memcpy(frame->buffer, h264_out_data.buffer, frame->len);
        frame->type = h264_out_data.type;
#endif
        bool queue_full = false;

        /* A P-frame that references a frame the viewer never got decodes to garbage,
         * so once anything is dropped we resync on the next IDR. Re-armed per session by
         * esp32p4_frame_grabber_start(), so the first frame handed downstream is an IDR. */
        if (s_wait_for_idr) {
            if (frame->type != ESP_H264_FRAME_TYPE_IDR) {
                s_resync_dropped++;
                if ((s_resync_dropped % 30u) == 1u) {
                    ESP_LOGW(TAG, "waiting for IDR to resync, dropped %" PRIu32 " frame(s)", s_resync_dropped);
                }
                free(frame->buffer);
                free(frame);
                continue;
            }
            ESP_LOGI(TAG, "resynced on IDR after %" PRIu32 " dropped frame(s)", s_resync_dropped);
            s_resync_dropped = 0;
            s_wait_for_idr = false;
        }

        if (xQueueSend(s_p4_enc_data.frame_queue, frame, pdMS_TO_TICKS(QUEUE_SEND_WAIT_MS)) != pdTRUE) {
            free(frame->buffer);
            vTaskDelay(pdMS_TO_TICKS(10));
            queue_full = true;
            s_wait_for_idr = true;   /* resync: the stream now has a hole */
        }
        free(frame);

        /* 1 s sliding-window encoder-out fps. Compare against the KVS DIAG
         * `ok` counter and the receive-side `fps: rx=…` line to pinpoint
         * where the pipeline saturates. */
        static uint64_t enc_fps_window_start_us;
        static uint32_t enc_fps_window_out;
        static uint32_t enc_fps_window_full_drops;
        enc_fps_window_out++;
        if (queue_full) enc_fps_window_full_drops++;
        uint64_t enc_now_us = esp_timer_get_time();
        if (enc_fps_window_start_us == 0) {
            enc_fps_window_start_us = enc_now_us;
        } else if (enc_now_us - enc_fps_window_start_us >= 1000000ULL) {
            uint64_t enc_window_us = enc_now_us - enc_fps_window_start_us;
            uint32_t enc_fps = (uint32_t)((uint64_t)enc_fps_window_out * 1000000ULL / enc_window_us);
            ESP_LOGI(TAG, "enc_fps: out=%" PRIu32 " (q_full_drops=%" PRIu32 ")",
                     enc_fps, enc_fps_window_full_drops);
            enc_fps_window_start_us = enc_now_us;
            enc_fps_window_out = 0;
            enc_fps_window_full_drops = 0;
        }
    }

    ESP_LOGE(TAG, "Video encoder task unexpectedly exited!");
}

void esp32p4_frame_grabber_init(video_frame_preprocess_fn_t frame_preprocess_fn)
{
    // Singleton pattern: encoder task remains, but check if esp_video_if needs reinitialization
    if (s_p4_enc_data.encoder_initialized) {
        ESP_LOGD(TAG, "ESP32P4 frame grabber already initialized (singleton)");
#if USE_ESP_VIDEO_IF
        // Reinitialize esp_video_if if it was deinitialized (for power/security reasons)
        // esp_video_if_init() will check internally and return early if already initialized
        esp_err_t ret = esp_video_if_init();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to reinitialize video interface: %s", esp_err_to_name(ret));
            return;
        }
#endif
        return;
    }

    init_chip();
    init_clock();

    s_p4_enc_data.frame_queue = xQueueCreate(CONFIG_VIDEO_FRAME_QUEUE_SIZE, sizeof(esp_h264_out_buf_t));
    if (!s_p4_enc_data.frame_queue) {
        ESP_LOGE(TAG, "Failed to create frame queue");
        goto cleanup;
    }

    // Create semaphore for start/stop control
    s_p4_enc_data.run_semaphore = xSemaphoreCreateBinary();
    if (!s_p4_enc_data.run_semaphore) {
        ESP_LOGE(TAG, "Failed to create run semaphore");
        goto cleanup;
    }
#if USE_ESP_VIDEO_IF
    /* Retry: a USB camera may not have finished enumerating when the first session
     * starts (the UVC device waits ~10 s for it, then open() returns ENODEV). Failing
     * outright here kills the whole session for what is often a transient miss. */
    esp_err_t ret = ESP_FAIL;
    for (int attempt = 1; attempt <= CAMERA_INIT_MAX_ATTEMPTS; attempt++) {
        ret = esp_video_if_init();
        if (ret == ESP_OK) {
            if (attempt > 1) {
                ESP_LOGW(TAG, "video interface initialized on attempt %d", attempt);
            }
            break;
        }
        ESP_LOGW(TAG, "video interface init attempt %d/%d failed: %s",
                 attempt, CAMERA_INIT_MAX_ATTEMPTS, esp_err_to_name(ret));
        if (attempt < CAMERA_INIT_MAX_ATTEMPTS) {
            vTaskDelay(pdMS_TO_TICKS(CAMERA_INIT_RETRY_DELAY_MS));
        }
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize video interface after %d attempts",
                 CAMERA_INIT_MAX_ATTEMPTS);
        goto cleanup;
    }
    ESP_LOGD(TAG, "video interface initialized");
#else
    if (camera_init() != ESP_OK) {
        ESP_LOGE(TAG, "Camera initialization failed");
        goto cleanup;
    }
#endif
    vTaskDelay(pdMS_TO_TICKS(10));
#if SDCARD_SAVE
    sdmmc_card_t *sdcard = bsp_sdcard_mount();
    if (sdcard == NULL) {
        ESP_LOGE(TAG, "sdcard initialization failed!");
        goto cleanup;
    }

    printf("Filesystem mounted\n");
    // Card has been initialized, print its properties
    sdmmc_card_print_info(stdout, sdcard);

    char h264_name[100] = "/sdcard/encoded_file.h264"; // CONFIG_BSP_SD_MOUNT_POINT
    // sprintf(h264_name, "/eMMC/res_%d_%d.264", 1920, 1080);
    printf("h264_name %s \n", h264_name);

    f264 = fopen(h264_name, "wb+");
    if (f264 == NULL) {
        printf("H264 file create failed\n");
        esp32p4_frame_grabber_cleanup();
        goto cleanup;
    }
#endif

#if H264_ENCODE
    h264_enc_user_cfg_t cfg = {
        .enc_cfg = DEFAULT_ENCODER_CFG()
    };

    /* Get the ACTUAL camera resolution (which may differ from desired due to fallback) */
#if USE_ESP_VIDEO_IF
    video_resolution_t actual_resolution = {0};
    if (esp_video_if_get_resolution(&actual_resolution) == ESP_OK &&
        actual_resolution.width != 0 && actual_resolution.height != 0) {
        cfg.enc_cfg.res.width = actual_resolution.width;
        cfg.enc_cfg.res.height = actual_resolution.height;
        if (actual_resolution.fps != 0) {
            cfg.enc_cfg.fps = actual_resolution.fps;
        }
        ESP_LOGI(TAG, "Configuring encoder with actual camera resolution: %dx%d@%d",
                 cfg.enc_cfg.res.width, cfg.enc_cfg.res.height, cfg.enc_cfg.fps);
    }

    cfg.enc_cfg.fps = 27; /* used to distribute the bitrate */
    /* Encoder bitrate ceiling = rate-controller max, derived as
     *     max = LINK_BASE * (height / REF_HEIGHT)
     * LINK_BASE is the link's sustainable budget (~1 Mbps on a contended 2.4 GHz
     * C6 uplink; would be ~2 Mbps on 5 GHz once band detection lands). The
     * height ratio scales it by resolution: 720p -> 1.0x, 1080p -> 1.5x. This
     * reproduces the 1M/1.5M (2.4G) and 2M/3M (5G) tiers from one base + a
     * ratio, and degrades sanely for smaller resolutions. The controller sheds
     * down to a flat ~256 Kbps survival floor; QP bounds cap per-frame quality. */
    #define LINK_BASE_BITRATE_BPS   (1024 * 1024)   /* 2.4 GHz budget */
    #define REF_HEIGHT              720
    cfg.enc_cfg.rc.bitrate = (uint32_t)(((uint64_t)LINK_BASE_BITRATE_BPS *
                                         cfg.enc_cfg.res.height) / REF_HEIGHT);
    /* qp_max raised to 46 so high-motion frames compress harder and stay
     * within the 2.4 GHz C6 uplink budget. At the old ceilings (40 / 42)
     * full-frame motion produced 20-32 KB frames that took 200-305 ms to
     * send, collapsing send_v_fps to 3-6. Higher qp_max trades transient
     * motion blur for stable fps; static scenes are unaffected (qp_min
     * unchanged, so quality stays high when there's bitrate headroom). */
    if (cfg.enc_cfg.res.height <= 720) {
        cfg.enc_cfg.rc.qp_min = 32;
        cfg.enc_cfg.rc.qp_max = 51;
    } else {
        cfg.enc_cfg.rc.qp_min = 35;
        cfg.enc_cfg.rc.qp_max = 51;
    }
    /* GOP trades steady-state bitrate against packet-loss recovery latency:
     * a shorter interval means the decoder resyncs at the next periodic IDR
     * soon after any loss, but keyframes are the largest frames so more of
     * them costs bandwidth. On the flaky 2.4GHz uplink, bounded recovery wins
     * over a leaner steady state. 15 = IDR every ~0.6s at 25fps / ~1s at 15fps. */
    cfg.enc_cfg.gop = 15;
#else
    cfg.read_cb = &data_read_callback,
    cfg.write_cb = &data_write_callback,
#endif

#if MEDIA_STREAM_ENABLE_USB_UVC_CAM_SENSOR && CONFIG_MEDIA_STREAM_UVC_PASSTHROUGH_H264
    /* The camera encodes for us, so neither the hardware encoder nor the rate
     * controller has anything to do: setting the encoder up would open the H.264
     * hardware and reserve a w*h*1.5 PSRAM output buffer (~3 MB at 1080p) that is never
     * written, and the controller can only throttle by dropping frames, which breaks a
     * compressed stream's reference chain.
     *
     * Consequence worth knowing: the PLI -> force-IDR path lives in that encoder, so a
     * viewer's PLI cannot shorten the wait for a key frame here - recovery is bounded by
     * the camera's own GOP until the driver exposes UVC encoding-unit controls. */
    ESP_LOGI(TAG, "UVC passthrough: hardware encoder and rate control stay idle");
#else
    esp_h264_setup_encoder(&cfg);

    /* Congestion-adaptive rate control: fps-first degradation (floor 12),
     * bitrate as stage 2, with recovery. Driven by send-latency reports
     * from kvs_media. */
    video_rate_ctrl_init(cfg.enc_cfg.fps, cfg.enc_cfg.rc.bitrate,
                         cfg.enc_cfg.res.width, cfg.enc_cfg.res.height);
#endif

#define ENC_TASK_STACK_SIZE     (5 * 1024)
#define ENC_TASK_PRIO           CONFIG_VIDEO_ENCODER_TASK_PRIORITY
    /* TCB stays in INTERNAL — accessed from the scheduler tick ISR.
     * Stack moves to SPIRAM (saves 5 KB of internal RAM) — encoder task
     * doesn't run with cache disabled. Requires CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY=y. */
    s_p4_enc_data.task_buffer = heap_caps_calloc(1, sizeof(StaticTask_t), MALLOC_CAP_INTERNAL);
    s_p4_enc_data.task_stack = heap_caps_calloc(1, ENC_TASK_STACK_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_p4_enc_data.task_buffer || !s_p4_enc_data.task_stack) {
        ESP_LOGE(TAG, "Failed to allocate task buffers");
        goto cleanup;
    }

    s_p4_enc_data.running = false;  // Start in stopped state
    /* Video path pinned to core 1, leaving core 0 free for audio I/O. */
    s_p4_enc_data.encoder_task_handle = xTaskCreateStatic(video_encoder_task, "video_encoder", ENC_TASK_STACK_SIZE,
                                                          frame_preprocess_fn, ENC_TASK_PRIO, s_p4_enc_data.task_stack, s_p4_enc_data.task_buffer);

    if (s_p4_enc_data.encoder_task_handle == NULL) {
        ESP_LOGE(TAG, "failed to create encoder task!");
        goto cleanup;
    }
#endif

    s_p4_enc_data.encoder_initialized = true;

    ESP_LOGD(TAG, "ESP32P4 frame grabber initialized as singleton (stopped, use start() to begin)");
    return;

cleanup:
    // Conditional cleanup based on what was allocated
    if (s_p4_enc_data.task_buffer != NULL) {
        heap_caps_free(s_p4_enc_data.task_buffer);
        s_p4_enc_data.task_buffer = NULL;
    }
    if (s_p4_enc_data.task_stack != NULL) {
        heap_caps_free(s_p4_enc_data.task_stack);
        s_p4_enc_data.task_stack = NULL;
    }
    if (s_p4_enc_data.run_semaphore != NULL) {
        vSemaphoreDelete(s_p4_enc_data.run_semaphore);
        s_p4_enc_data.run_semaphore = NULL;
    }
    if (s_p4_enc_data.frame_queue != NULL) {
        vQueueDelete(s_p4_enc_data.frame_queue);
        s_p4_enc_data.frame_queue = NULL;
    }

    ESP_LOGE(TAG, "ESP32P4 frame grabber initialization failed");
}

esp_err_t esp32p4_frame_grabber_start(void)
{
    if (!s_p4_enc_data.encoder_initialized) {
        ESP_LOGE(TAG, "ESP32P4 frame grabber not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    /* Each session starts from a key frame, and the previous session's parameter sets
     * may describe a different format. */
    s_wait_for_idr = true;
    s_resync_dropped = 0;
    h264_forget_parameter_sets();

    if (s_p4_enc_data.running) {
        ESP_LOGD(TAG, "ESP32P4 frame grabber already running");
        return ESP_OK;
    }

#if USE_ESP_VIDEO_IF && MEDIA_STREAM_ENABLE_USB_UVC_CAM_SENSOR && CONFIG_MEDIA_STREAM_UVC_PASSTHROUGH_H264
    /* A passthrough build never sets up the encoder, but the format fallback list can
     * still land on a raw format. Refuse here rather than crash on the first frame. */
    uint32_t pixfmt = 0;
    if (esp_video_if_get_pixel_format(&pixfmt) != ESP_OK || pixfmt != V4L2_PIX_FMT_H264) {
        ESP_LOGE(TAG, "UVC passthrough needs H.264 but the camera negotiated 0x%08" PRIx32
                 "; disable MEDIA_STREAM_UVC_PASSTHROUGH_H264 to encode locally", pixfmt);
        return ESP_ERR_NOT_SUPPORTED;
    }
#endif

    s_p4_enc_data.running = true;
    xSemaphoreGive(s_p4_enc_data.run_semaphore);  // Signal encoder to start
    ESP_LOGD(TAG, "ESP32P4 frame grabber started");

    return ESP_OK;
}

esp_err_t esp32p4_frame_grabber_stop(void)
{
    if (!s_p4_enc_data.encoder_initialized) {
        ESP_LOGE(TAG, "ESP32P4 frame grabber not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (!s_p4_enc_data.running) {
        ESP_LOGD(TAG, "ESP32P4 frame grabber already stopped");
        return ESP_OK;
    }

    s_p4_enc_data.running = false;

    /* Wait for the task to park for longer that one DQBUF. Without this, stop() can return
     * while the task is still mid-DQBUF or holding a raw frame, and a caller that then releases
     * the capture buffers (the UVC re-init path does) unmaps memory still in use. */
#if USE_ESP_VIDEO_IF
    const int park_timeout_ms = CONFIG_ESP_VIDEO_IF_DQBUF_TIMEOUT_MS > 0
                                ? CONFIG_ESP_VIDEO_IF_DQBUF_TIMEOUT_MS + 500 : 1000;
#else
    const int park_timeout_ms = 1000;
#endif
    for (int waited = 0; !s_task_parked && waited < park_timeout_ms; waited += 10) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (!s_task_parked) {
        ESP_LOGW(TAG, "encoder task did not park within %d ms; capture buffers stay mapped",
                 park_timeout_ms);
    }

    ESP_LOGD(TAG, "ESP32P4 frame grabber stopped");

    return ESP_OK;
}

esp_err_t esp32p4_frame_grabber_deinit(void)
{
    if (!s_p4_enc_data.encoder_initialized) {
        ESP_LOGD(TAG, "ESP32P4 frame grabber not initialized, nothing to deinitialize");
        return ESP_OK;
    }

    // Stop encoding first
    esp32p4_frame_grabber_stop();

    // Drain the frame queue with limited iterations to prevent infinite loop
    if (s_p4_enc_data.frame_queue != NULL) {
        ESP_LOGD(TAG, "Draining video frame queue...");
        esp_h264_out_buf_t h264_frame;
        int drained_count = 0;
        const int max_drain_iterations = CONFIG_VIDEO_FRAME_QUEUE_SIZE;  // Prevent infinite loop

        while (xQueueReceive(s_p4_enc_data.frame_queue, &h264_frame, 0) == pdTRUE &&
               drained_count < max_drain_iterations) {
            if (h264_frame.buffer) {
                heap_caps_free(h264_frame.buffer);
            }
            drained_count++;
        }

        if (drained_count > 0) {
            ESP_LOGD(TAG, "Drained %d video frames from queue", drained_count);
        }
        if (drained_count >= max_drain_iterations) {
            ESP_LOGI(TAG, "Reached max drain limit, queue may still contain frames");
        }
    }

    // Singleton pattern: encoder task remains running but paused
    ESP_LOGD(TAG, "ESP32P4 frame grabber is singleton - task remains paused until start() is called");

#if MEDIA_STREAM_ENABLE_USB_UVC_CAM_SENSOR && !CONFIG_MEDIA_STREAM_UVC_PASSTHROUGH_H264
    /* Only once the task has parked; stop() may have timed out with it still converting. */
    if (s_conv_buf && s_task_parked) {
        heap_caps_free(s_conv_buf);
        s_conv_buf = NULL;
        s_conv_buf_len = 0;
    }
#endif

#if USE_ESP_VIDEO_IF
    esp_video_if_deinit();
#endif
    return ESP_OK;
}

/* -------------------------------------------------------------------------- */
/*  Snapshot interceptor API                                                  */
/* -------------------------------------------------------------------------- */

bool esp32p4_is_encoder_running(void)
{
    return s_p4_enc_data.running;
}

bool esp32p4_is_encoder_initialized(void)
{
    return s_p4_enc_data.encoder_initialized;
}

esp_err_t video_capture_request_keyframe(void)
{
    if (!s_p4_enc_data.encoder_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    return esp_h264_hw_enc_request_idr();
}

#if USE_ESP_VIDEO_IF
/**
 * Intercept the next raw frame from the encoder task.
 * The caller provides a pre-allocated buffer. The encoder task copies the next
 * raw frame into it and signals completion via a semaphore.
 *
 * @param buf[out]        Pointer to store the raw frame buffer (caller-allocated, SPIRAM)
 * @param len[out]        Pointer to store the actual frame length
 * @param width[out]      Pointer to store frame width
 * @param height[out]     Pointer to store frame height
 * @param pixfmt[out]     Pointer to store pixel format
 * @param timeout_ms      Maximum wait time in milliseconds
 * @return ESP_OK on success, ESP_ERR_TIMEOUT on timeout
 */
esp_err_t esp32p4_snapshot_intercept_frame(uint8_t *buf, size_t buf_size,
                                           size_t *len, uint16_t *width,
                                           uint16_t *height,
                                           video_frame_pixformat_t *pixfmt,
                                           uint32_t timeout_ms)
{
    if (!buf || !len || !width || !height || buf_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_p4_enc_data.running) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Create the done semaphore if not yet created */
    if (!s_snapshot.done) {
        s_snapshot.done = xSemaphoreCreateBinary();
        if (!s_snapshot.done) {
            return ESP_ERR_NO_MEM;
        }
    }

    /* Prepare the snapshot request */
    s_snapshot.buffer = buf;
    s_snapshot.buffer_size = buf_size;
    s_snapshot.frame_len = 0;
    s_snapshot.width = 0;
    s_snapshot.height = 0;
    s_snapshot.requested = true;  /* Trigger the interceptor in encoder task */

    /* Wait for the encoder task to copy a frame */
    if (xSemaphoreTake(s_snapshot.done, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        s_snapshot.requested = false;
        ESP_LOGE(TAG, "Snapshot intercept timed out");
        return ESP_ERR_TIMEOUT;
    }

    *len = s_snapshot.frame_len;
    *width = s_snapshot.width;
    *height = s_snapshot.height;
    if (pixfmt) {
        *pixfmt = s_snapshot.pixfmt;
    }

    return ESP_OK;
}

/**
 * Directly grab a raw frame from the camera when the encoder task is NOT running.
 * This is safe only when the encoder task is paused (not consuming frames).
 *
 * @param buf[out]        Pre-allocated buffer for the raw frame
 * @param buf_size        Size of the buffer
 * @param len[out]        Actual frame length
 * @param width[out]      Frame width
 * @param height[out]     Frame height
 * @param pixfmt[out]     Pixel format
 * @param timeout_ms      Maximum wait time
 * @return ESP_OK on success
 */
esp_err_t esp32p4_snapshot_direct_grab(uint8_t *buf, size_t buf_size,
                                       size_t *len, uint16_t *width,
                                       uint16_t *height,
                                       video_frame_pixformat_t *pixfmt,
                                       uint32_t timeout_ms)
{
    if (!buf || !len || !width || !height || buf_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    bool need_init = false;
    bool need_start = false;

    /* If esp_video_if is not initialized, temporarily init it */
    if (!s_p4_enc_data.encoder_initialized) {
        /* Full initialization needed (chip, clock, video interface) */
        init_chip();
        init_clock();
        esp_err_t ret = esp_video_if_init();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to init video interface for snapshot: %s", esp_err_to_name(ret));
            return ret;
        }
        need_init = true;
        need_start = true;
    } else {
        /* Camera is initialized but encoder might be paused.
         * esp_video_if might have been deinitialized (streaming stopped).
         * Reinit to restart streaming if needed. */
        esp_err_t ret = esp_video_if_init();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to reinit video interface for snapshot: %s", esp_err_to_name(ret));
            return ret;
        }
        need_start = true;
    }

    /* Discard initial frames after STREAMON to let sensor/ISP settle
     * (exposure, white balance). Without this the first frame is often
     * noisy and produces an abnormally large JPEG. */
    if (need_start) {
        const int warmup_frames = 5;
        for (int i = 0; i < warmup_frames; i++) {
            video_fb_t *discard = esp_video_if_get_frame();
            if (discard) {
                esp_video_if_release_frame(discard);
            }
        }
        ESP_LOGD(TAG, "Discarded %d warmup frames", warmup_frames);
    }

    /* Get one raw frame */
    video_fb_t *raw_frame = esp_video_if_get_frame();
    if (!raw_frame) {
        ESP_LOGE(TAG, "Failed to get raw frame for snapshot");
        if (need_init) {
            esp_video_if_deinit();
        }
        return ESP_ERR_NOT_FOUND;
    }

    /* Copy frame data */
    size_t copy_len = (raw_frame->len <= buf_size) ? raw_frame->len : buf_size;
    memcpy(buf, raw_frame->buf, copy_len);

    video_resolution_t resolution = {0};
    esp_video_if_get_resolution(&resolution);

    *len = copy_len;
    *width = resolution.width;
    *height = resolution.height;
    if (pixfmt) {
        *pixfmt = PIXFMT_YUV420;
    }

    /* Release the raw frame back to the driver */
    esp_video_if_release_frame(raw_frame);

    /* Stop streaming so the next snapshot can cleanly restart */
    esp_video_if_stop();

    if (need_init) {
        esp_video_if_deinit();
    }

    return ESP_OK;
}
#endif /* USE_ESP_VIDEO_IF */

#endif
