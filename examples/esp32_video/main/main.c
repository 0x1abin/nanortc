/*
 * nanortc ESP32 Video example — H.264 SD card video sender
 *
 * The ESP32 hosts a web page at http://<ip>/. The browser connects,
 * sends an SDP offer via POST /offer, and receives an answer.
 * ESP32 reads pre-encoded H.264 frames from the SD card and streams
 * them to the browser via WebRTC video.
 *
 * Build: cd examples/esp32_video && idf.py build
 * Flash: idf.py flash monitor
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "esp_http_server.h"
#include "protocol_examples_common.h"
#include <lwip/sockets.h>

/* SD card (SDMMC 1-line) */
#include "driver/sdmmc_host.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

#include "nanortc.h"
#include "nanortc_crypto.h"
#include "run_loop.h"
#include "media_source.h"
#include "h264_utils.h"

static const char *TAG = "nanortc_video";

#define SD_MOUNT_POINT "/sd"
#define VIDEO_FPS      CONFIG_EXAMPLE_VIDEO_FPS
#define FRAME_INTERVAL (1000 / VIDEO_FPS) /* ms per frame */

/* nanortc state */
static nanortc_t s_rtc;
static nano_run_loop_t s_loop;
static char s_local_ip[16];
static int s_connected;
static nanortc_writer_t s_video_writer;
static int s_video_mid; /* MID returned by nanortc_add_track() */

/* Video state */
static nano_media_source_t s_video_src;
static int s_video_ready;
static int s_psram_mode;
static uint32_t s_video_epoch_ms;
static uint32_t s_video_frame_count;

/* PSRAM preloaded frame index (used when PSRAM available) */
typedef struct {
    uint32_t offset;
    uint32_t len;
} frame_entry_t;

#define MAX_VIDEO_FRAMES 1500
static uint8_t *s_frame_data;
static frame_entry_t s_frames[MAX_VIDEO_FRAMES];
static int s_frame_total;

/* Embedded HTML file */
extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[] asm("_binary_index_html_end");

/* ----------------------------------------------------------------
 * SD card mount (SDMMC 1-line)
 * ---------------------------------------------------------------- */
static int sd_card_mount(void)
{
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;
    host.flags = SDMMC_HOST_FLAG_1BIT;

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 1;
    slot.cmd = CONFIG_EXAMPLE_SD_CMD_PIN;
    slot.clk = CONFIG_EXAMPLE_SD_CLK_PIN;
    slot.d0 = CONFIG_EXAMPLE_SD_D0_PIN;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
    };

    sdmmc_card_t *card = NULL;
    esp_err_t ret = esp_vfs_fat_sdmmc_mount(SD_MOUNT_POINT, &host, &slot, &mount_cfg, &card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SD card mount failed: %s", esp_err_to_name(ret));
        return -1;
    }

    ESP_LOGI(TAG, "SD card mounted: %s (%lluMB)", card->cid.name,
             (unsigned long long)(((uint64_t)card->csd.capacity) * card->csd.sector_size /
                                  (1024 * 1024)));

    return 0;
}

/* ----------------------------------------------------------------
 * Try to preload all H.264 frames into PSRAM (returns -1 if no PSRAM)
 * ---------------------------------------------------------------- */
static int preload_frames(const char *dir)
{
    char path[300];
    uint32_t total_size = 0;
    int count = 0;

    for (int i = 1; i <= MAX_VIDEO_FRAMES; i++) {
        snprintf(path, sizeof(path), "%s/frame-%04d.h264", dir, i);
        FILE *f = fopen(path, "rb");
        if (!f)
            break;
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fclose(f);
        if (sz <= 0)
            break;
        total_size += (uint32_t)sz;
        count++;
    }
    if (count == 0)
        return -1;

    ESP_LOGI(TAG, "Trying PSRAM preload: %d frames, %lu KB...", count,
             (unsigned long)(total_size / 1024));

    s_frame_data = heap_caps_malloc(total_size, MALLOC_CAP_SPIRAM);
    if (!s_frame_data) {
        ESP_LOGW(TAG, "PSRAM not available (%lu bytes)", (unsigned long)total_size);
        return -1;
    }

    uint32_t offset = 0;
    for (int i = 0; i < count; i++) {
        snprintf(path, sizeof(path), "%s/frame-%04d.h264", dir, i + 1);
        FILE *f = fopen(path, "rb");
        if (!f)
            break;
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        size_t nread = fread(s_frame_data + offset, 1, (size_t)sz, f);
        fclose(f);
        s_frames[i].offset = offset;
        s_frames[i].len = (uint32_t)nread;
        offset += (uint32_t)nread;
    }
    s_frame_total = count;
    ESP_LOGI(TAG, "PSRAM preload OK: %d frames, %lu KB", count, (unsigned long)(offset / 1024));
    return 0;
}

/* ----------------------------------------------------------------
 * Get station IP
 * ---------------------------------------------------------------- */
static int get_sta_ip(char *ip_out, size_t ip_out_len)
{
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!netif) {
        return -1;
    }
    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(netif, &ip_info) != ESP_OK) {
        return -1;
    }
    esp_ip4addr_ntoa(&ip_info.ip, ip_out, (int)ip_out_len);
    return 0;
}

/* ----------------------------------------------------------------
 * Video send tick (epoch-based pacing, from linux_media_send)
 * ---------------------------------------------------------------- */
static void video_send_tick(uint32_t now)
{
    if (s_video_epoch_ms == 0)
        s_video_epoch_ms = now;

    uint32_t target_frames = (now - s_video_epoch_ms) / FRAME_INTERVAL;

    /* Skip frames if we fell too far behind */
    if (target_frames - s_video_frame_count > 2) {
        s_video_frame_count = target_frames - 1;
    }

    while (s_video_frame_count < target_frames) {
        const uint8_t *frame_buf;
        size_t frame_len;
        static uint8_t sd_buf[NANORTC_MEDIA_MAX_FRAME_SIZE];

        if (s_psram_mode) {
            int idx = (int)(s_video_frame_count % (uint32_t)s_frame_total);
            frame_buf = s_frame_data + s_frames[idx].offset;
            frame_len = s_frames[idx].len;
        } else {
            uint32_t ts_ms = 0;
            frame_len = 0;
            if (nano_media_source_next_frame(&s_video_src, sd_buf, sizeof(sd_buf), &frame_len,
                                             &ts_ms) != 0)
                break;
            frame_buf = sd_buf;
        }

        /* RTP timestamp: 90kHz clock */
        uint32_t video_ts_rtp = s_video_frame_count * (90000 / VIDEO_FPS);

        /* Split Annex-B into individual NALUs and send each */
        size_t offset = 0;
        size_t nal_len = 0;
        const uint8_t *nal;
        while ((nal = annex_b_find_nal(frame_buf, frame_len, &offset, &nal_len)) != NULL) {
            int flags = 0;
            if ((nal[0] & 0x1F) == 5)
                flags |= NANORTC_VIDEO_FLAG_KEYFRAME;
            /* Peek ahead: if no more NALs, this is the last one (marker) */
            size_t peek_off = offset;
            size_t peek_len = 0;
            if (annex_b_find_nal(frame_buf, frame_len, &peek_off, &peek_len) == NULL) {
                flags |= NANORTC_VIDEO_FLAG_MARKER;
            }
            nanortc_writer_write(&s_video_writer, video_ts_rtp, nal, nal_len, flags);
        }
        s_video_frame_count++;
    }
}

/* ----------------------------------------------------------------
 * nanortc event callback
 * ---------------------------------------------------------------- */
static void on_event(nanortc_t *rtc, const nanortc_event_t *evt, void *userdata)
{
    (void)rtc;
    (void)userdata;

    switch (evt->type) {
    case NANORTC_EV_ICE_STATE_CHANGE:
        if (evt->ice_state == NANORTC_ICE_STATE_CONNECTED) {
            ESP_LOGI(TAG, "ICE connected");
        }
        break;

    case NANORTC_EV_CONNECTED:
        ESP_LOGI(TAG, "Connected — starting video");
        s_connected = 1;
        s_video_epoch_ms = 0;
        s_video_frame_count = 0;
        /* Obtain writer handle for video track */
        if (nanortc_writer(rtc, (uint8_t)s_video_mid, &s_video_writer) != NANORTC_OK) {
            ESP_LOGE(TAG, "Failed to obtain video writer");
            s_connected = 0;
        }
        break;

    case NANORTC_EV_KEYFRAME_REQUEST:
        ESP_LOGI(TAG, "Keyframe requested (mid=%d) — resetting to frame 0",
                 evt->keyframe_request.mid);
        if (!s_psram_mode)
            nano_media_source_reset(&s_video_src);
        s_video_epoch_ms = 0;
        s_video_frame_count = 0;
        break;

    case NANORTC_EV_DISCONNECTED:
        ESP_LOGI(TAG, "Disconnected");
        s_connected = 0;
        nano_run_loop_stop(&s_loop);
        break;

    default:
        break;
    }
}

/* ----------------------------------------------------------------
 * HTTP handlers
 * ---------------------------------------------------------------- */

/* GET / — serve index.html */
static esp_err_t http_get_root(httpd_req_t *req)
{
    size_t html_len = (size_t)(index_html_end - index_html_start);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, (const char *)index_html_start, (ssize_t)html_len);
    return ESP_OK;
}

/* POST /offer — receive SDP offer, return SDP answer */
static esp_err_t http_post_offer(httpd_req_t *req)
{
    int content_len = req->content_len;
    if (content_len <= 0 || content_len > 8192) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad offer size");
        return ESP_FAIL;
    }

    char *offer = malloc((size_t)content_len + 1);
    if (!offer) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }

    int received = httpd_req_recv(req, offer, (size_t)content_len);
    if (received <= 0) {
        free(offer);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Read failed");
        return ESP_FAIL;
    }
    offer[received] = '\0';

    ESP_LOGI(TAG, "Got SDP offer (%d bytes)", received);

    /* Re-initialize nanortc for new session */
    s_connected = 0;
    nano_run_loop_destroy(&s_loop);
    nanortc_destroy(&s_rtc);

    nanortc_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.crypto = nanortc_crypto_mbedtls();
    cfg.role = NANORTC_ROLE_CONTROLLED;

    int rc = nanortc_init(&s_rtc, &cfg);
    if (rc != NANORTC_OK) {
        ESP_LOGE(TAG, "nanortc_init failed: %d", rc);
        free(offer);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Init fail");
        return ESP_FAIL;
    }

    /* Add video track via Writer handle pattern */
    s_video_mid = nanortc_add_track(&s_rtc, NANORTC_TRACK_VIDEO, NANORTC_DIR_SENDONLY,
                                    NANORTC_CODEC_H264, 90000, 0);
    if (s_video_mid < 0) {
        ESP_LOGE(TAG, "nanortc_add_track failed: %d", s_video_mid);
        free(offer);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Media fail");
        return ESP_FAIL;
    }

    rc = nano_run_loop_init(&s_loop, &s_rtc, NULL, CONFIG_EXAMPLE_UDP_PORT);
    if (rc < 0) {
        ESP_LOGE(TAG, "Failed to bind UDP port");
        free(offer);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Bind fail");
        return ESP_FAIL;
    }
    nanortc_add_local_candidate(&s_rtc, s_local_ip, CONFIG_EXAMPLE_UDP_PORT);
    nano_run_loop_set_event_cb(&s_loop, on_event, NULL);
    s_loop.max_poll_ms = 5;
    s_loop.running = 1;

    char *answer = malloc(8192);
    if (!answer) {
        free(offer);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }
    size_t answer_len = 0;
    rc = nanortc_accept_offer(&s_rtc, offer, answer, 8192, &answer_len);
    free(offer);

    if (rc != NANORTC_OK) {
        ESP_LOGE(TAG, "nanortc_accept_offer failed: %d (%s)", rc, nanortc_err_name(rc));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, nanortc_err_name(rc));
        free(answer);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "SDP answer generated (%u bytes)", (unsigned)answer_len);
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, answer, (ssize_t)answer_len);
    free(answer);
    return ESP_OK;
}

/* ----------------------------------------------------------------
 * HTTP server setup
 * ---------------------------------------------------------------- */
static httpd_handle_t start_http_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 4;
    config.max_open_sockets = 2;
    config.stack_size = 8192;

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return NULL;
    }

    httpd_uri_t uri_root = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = http_get_root,
    };
    httpd_uri_t uri_offer = {
        .uri = "/offer",
        .method = HTTP_POST,
        .handler = http_post_offer,
    };
    httpd_register_uri_handler(server, &uri_root);
    httpd_register_uri_handler(server, &uri_offer);

    ESP_LOGI(TAG, "HTTP server started on port %d", config.server_port);
    return server;
}

/* ----------------------------------------------------------------
 * WebRTC event loop task
 * ---------------------------------------------------------------- */
static void webrtc_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "WebRTC task started");

    for (;;) {
        if (s_loop.running) {
            nano_run_loop_step(&s_loop);
            if (s_connected && s_video_ready) {
                uint32_t now = nano_get_millis();
                video_send_tick(now);
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
}

/* ----------------------------------------------------------------
 * app_main
 * ---------------------------------------------------------------- */
void app_main(void)
{
    ESP_LOGI(TAG, "nanortc ESP32 Video example — H.264 from SD, %d fps", VIDEO_FPS);

    /* 1. NVS init (required for WiFi) */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* 2. Network init + WiFi */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(example_connect());

    if (get_sta_ip(s_local_ip, sizeof(s_local_ip)) < 0) {
        ESP_LOGE(TAG, "Failed to get station IP");
        return;
    }
    ESP_LOGI(TAG, "Station IP: %s", s_local_ip);

    /* 3. Init run loop state (not started until POST /offer) */
    memset(&s_loop, 0, sizeof(s_loop));
    s_loop.fd = -1;

    /* 4. Start HTTP server */
    if (!start_http_server()) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return;
    }

    /* 5. Start WebRTC task early (before SD mount which may block for seconds).
     * The task idles until http_post_offer sets s_loop.running = 1. */
    xTaskCreatePinnedToCore(webrtc_task, "webrtc", 32768, NULL, 5, NULL, 0);

    /* 6. Mount SD card, try PSRAM preload, fallback to per-frame I/O */
    if (sd_card_mount() != 0) {
        ESP_LOGE(TAG, "SD card mount failed — video will not work");
    } else {
        char h264_path[128];
        snprintf(h264_path, sizeof(h264_path), "%s/%s", SD_MOUNT_POINT, CONFIG_EXAMPLE_H264_DIR);

        if (preload_frames(h264_path) == 0) {
            s_psram_mode = 1;
            s_video_ready = 1;
        } else if (nano_media_source_init(&s_video_src, NANORTC_MEDIA_H264, h264_path) == 0) {
            s_video_ready = 1;
            ESP_LOGI(TAG, "SD I/O mode: %s (%d frames)", h264_path, s_video_src.frame_count);
        } else {
            ESP_LOGE(TAG, "Cannot open H.264 frames in %s", h264_path);
        }
    }

    ESP_LOGI(TAG, "Open http://%s/ in your browser", s_local_ip);
}
