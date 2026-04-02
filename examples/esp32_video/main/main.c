/*
 * nanortc ESP32 Audio/Video example — embedded media sender
 *
 * The ESP32 hosts a web page at http://<ip>/. The browser connects,
 * sends an SDP offer via POST /offer, and receives an answer.
 * ESP32 reads pre-encoded H.264 video and Opus audio from flash
 * (embedded blobs) and streams them to the browser via WebRTC.
 *
 * Build: cd examples/esp32_video && idf.py build
 * Flash: idf.py flash monitor
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "esp_http_server.h"
#include "protocol_examples_common.h"
#include <lwip/sockets.h>

#include "nanortc.h"
#include "nanortc_crypto.h"
#include "run_loop.h"
#include "media_source.h"

static const char *TAG = "nanortc_av";

static void nanortc_log_cb(const nanortc_log_message_t *msg, void *ctx)
{
    (void)ctx;
    ESP_LOGI("nrtc", "[%s] %s", msg->subsystem ? msg->subsystem : "?",
             msg->message ? msg->message : "");
}

#define VIDEO_FPS        CONFIG_EXAMPLE_VIDEO_FPS
#define VIDEO_INTERVAL   (1000 / VIDEO_FPS) /* ms per frame */
#define AUDIO_INTERVAL   20                 /* 20 ms per Opus frame */

/* nanortc state */
static nanortc_t s_rtc;
static nano_run_loop_t s_loop;
static char s_local_ip[16];
static int s_connected;
static int s_video_mid;
static int s_audio_mid;

/* Video state */
static nano_media_source_t s_video_src;
static int s_video_ready;
static uint32_t s_video_epoch_ms;
static uint32_t s_video_frame_count;

/* Audio state */
static nano_media_source_t s_audio_src;
static int s_audio_ready;
static uint32_t s_audio_epoch_ms;
static uint32_t s_audio_frame_count;

/* Embedded files */
extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[] asm("_binary_index_html_end");
extern const uint8_t video_blob_start[] asm("_binary_video_blob_start");
extern const uint8_t video_blob_end[] asm("_binary_video_blob_end");
extern const uint8_t audio_blob_start[] asm("_binary_audio_blob_start");
extern const uint8_t audio_blob_end[] asm("_binary_audio_blob_end");

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

    uint32_t target_frames = (now - s_video_epoch_ms) / VIDEO_INTERVAL;

    /* Skip frames if we fell too far behind */
    if (target_frames - s_video_frame_count > 2) {
        s_video_frame_count = target_frames - 1;
    }

    while (s_video_frame_count < target_frames) {
        static uint8_t sd_buf[NANORTC_MEDIA_MAX_FRAME_SIZE];
        size_t frame_len = 0;
        uint32_t ts_ms = 0;
        if (nano_media_source_next_frame(&s_video_src, sd_buf, sizeof(sd_buf), &frame_len,
                                         &ts_ms) != 0)
            break;

        nanortc_send_video(&s_rtc, (uint8_t)s_video_mid, (uint32_t)(esp_timer_get_time() / 1000),
                           sd_buf, frame_len);
        s_video_frame_count++;
    }
}

/* ----------------------------------------------------------------
 * Audio send tick (epoch-based pacing, 20ms Opus frames)
 * ---------------------------------------------------------------- */
static void audio_send_tick(uint32_t now)
{
    if (s_audio_epoch_ms == 0)
        s_audio_epoch_ms = now;

    uint32_t target_frames = (now - s_audio_epoch_ms) / AUDIO_INTERVAL;

    if (target_frames - s_audio_frame_count > 2) {
        s_audio_frame_count = target_frames - 1;
    }

    while (s_audio_frame_count < target_frames) {
        static uint8_t audio_buf[960]; /* Opus frames are small (~160 bytes) */
        size_t frame_len = 0;
        uint32_t ts_ms = 0;
        if (nano_media_source_next_frame(&s_audio_src, audio_buf, sizeof(audio_buf), &frame_len,
                                         &ts_ms) != 0)
            break;

        nanortc_send_audio(&s_rtc, (uint8_t)s_audio_mid, (uint32_t)(esp_timer_get_time() / 1000),
                           audio_buf, frame_len);
        s_audio_frame_count++;
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
        ESP_LOGI(TAG, "Connected — starting media");
        s_connected = 1;
        s_video_epoch_ms = 0;
        s_video_frame_count = 0;
        s_audio_epoch_ms = 0;
        s_audio_frame_count = 0;
        break;

    case NANORTC_EV_KEYFRAME_REQUEST:
        ESP_LOGI(TAG, "Keyframe requested (mid=%d) — resetting to frame 0",
                 evt->keyframe_request.mid);
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

/* GET /myip — return the client's IPv4 address (for mDNS replacement).
 * lwIP uses IPv6 sockets, so IPv4 clients appear as ::FFFF:x.x.x.x. */
static esp_err_t http_get_myip(httpd_req_t *req)
{
    int sockfd = httpd_req_to_sockfd(req);
    struct sockaddr_in6 addr6;
    socklen_t addr_len = sizeof(addr6);
    if (getpeername(sockfd, (struct sockaddr *)&addr6, &addr_len) != 0) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "getpeername");
        return ESP_FAIL;
    }
    char ip_str[64];
    /* Check for IPv4-mapped IPv6 (::FFFF:x.x.x.x) and extract IPv4 */
    if (addr6.sin6_family == AF_INET6 && IN6_IS_ADDR_V4MAPPED(&addr6.sin6_addr)) {
        struct in_addr v4;
        memcpy(&v4, &addr6.sin6_addr.s6_addr[12], 4);
        inet_ntop(AF_INET, &v4, ip_str, sizeof(ip_str));
    } else if (addr6.sin6_family == AF_INET) {
        struct sockaddr_in *a4 = (struct sockaddr_in *)&addr6;
        inet_ntop(AF_INET, &a4->sin_addr, ip_str, sizeof(ip_str));
    } else {
        inet_ntop(AF_INET6, &addr6.sin6_addr, ip_str, sizeof(ip_str));
    }
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, ip_str, -1);
    return ESP_OK;
}

/* GET /debug — return internal state for diagnostics */
static volatile uint32_t s_step_count;
static volatile uint32_t s_task_alive;

/* GET /debug — return internal state for diagnostics */
static esp_err_t http_get_debug(httpd_req_t *req)
{
    char buf[512];
    int n = snprintf(buf, sizeof(buf),
                     "running=%d fd=%d connected=%d video_ready=%d video_mid=%d\n"
                     "ice.remote_candidates=%d ice.state=%d\n"
                     "state=%d steps=%lu alive=%lu\n",
                     s_loop.running, s_loop.fd, s_connected, s_video_ready, s_video_mid,
                     s_rtc.ice.remote_candidate_count, s_rtc.ice.state, s_rtc.state,
                     (unsigned long)s_step_count, (unsigned long)s_task_alive);
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, buf, n);
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
    cfg.log.callback = nanortc_log_cb;
    cfg.log.level = NANORTC_LOG_DEBUG;

    int rc = nanortc_init(&s_rtc, &cfg);
    if (rc != NANORTC_OK) {
        ESP_LOGE(TAG, "nanortc_init failed: %d", rc);
        free(offer);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Init fail");
        return ESP_FAIL;
    }

    /* Add tracks in same order as browser offer (audio first, video second)
     * so that MID values match during SDP negotiation. */
    s_audio_mid = nanortc_add_audio_track(&s_rtc, NANORTC_DIR_SENDONLY, NANORTC_CODEC_OPUS, 48000,
                                          2);
    if (s_audio_mid < 0) {
        ESP_LOGE(TAG, "nanortc_add_audio_track failed: %d", s_audio_mid);
        free(offer);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Media fail");
        return ESP_FAIL;
    }

    s_video_mid = nanortc_add_video_track(&s_rtc, NANORTC_DIR_SENDONLY, NANORTC_CODEC_H264);
    if (s_video_mid < 0) {
        ESP_LOGE(TAG, "nanortc_add_video_track failed: %d", s_video_mid);
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
    s_loop.max_poll_ms = 20;
    /* Don't set running=1 yet — avoid race with webrtc_task during accept_offer */

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

    /* Safe to start event loop now — nanortc fully initialized */
    s_loop.running = 1;

    ESP_LOGI(TAG, "SDP answer generated (%u bytes), remote_candidates=%d", (unsigned)answer_len,
             s_rtc.ice.remote_candidate_count);
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
    httpd_uri_t uri_myip = {
        .uri = "/myip",
        .method = HTTP_GET,
        .handler = http_get_myip,
    };
    httpd_uri_t uri_offer = {
        .uri = "/offer",
        .method = HTTP_POST,
        .handler = http_post_offer,
    };
    httpd_uri_t uri_debug = {
        .uri = "/debug",
        .method = HTTP_GET,
        .handler = http_get_debug,
    };
    httpd_register_uri_handler(server, &uri_root);
    httpd_register_uri_handler(server, &uri_myip);
    httpd_register_uri_handler(server, &uri_debug);
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
    ESP_LOGI(TAG, "WebRTC task started on core %d", xPortGetCoreID());

    for (;;) {
        s_task_alive++;
        if (s_loop.running) {
            nano_run_loop_step(&s_loop);
            s_step_count++;
            if (s_connected && (s_video_ready || s_audio_ready)) {
                uint32_t now = nano_get_millis();
                if (s_video_ready)
                    video_send_tick(now);
                if (s_audio_ready)
                    audio_send_tick(now);
            } else {
                vTaskDelay(pdMS_TO_TICKS(20));
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
}

/* ----------------------------------------------------------------
 * app_main
 * ---------------------------------------------------------------- */
void app_main(void)
{
    ESP_LOGI(TAG, "nanortc ESP32 A/V example — H.264 + Opus from flash, %d fps", VIDEO_FPS);

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

    /* 5. Start WebRTC task.
     * The task idles until http_post_offer sets s_loop.running = 1. */
    xTaskCreate(webrtc_task, "webrtc", 8 * 1024, NULL, 5, NULL);

    /* 6. Init media sources from embedded flash blobs */
    memset(&s_video_src, 0, sizeof(s_video_src));
    s_video_src.blob = video_blob_start;
    s_video_src.blob_len = (size_t)(video_blob_end - video_blob_start);
    if (nano_media_source_init(&s_video_src, NANORTC_MEDIA_H264, NULL) == 0) {
        s_video_ready = 1;
        ESP_LOGI(TAG, "Embedded video: %d frames (%.1f KB)", s_video_src.frame_count,
                 (float)s_video_src.blob_len / 1024.0f);
    } else {
        ESP_LOGE(TAG, "Failed to init embedded video source");
    }

    memset(&s_audio_src, 0, sizeof(s_audio_src));
    s_audio_src.blob = audio_blob_start;
    s_audio_src.blob_len = (size_t)(audio_blob_end - audio_blob_start);
    if (nano_media_source_init(&s_audio_src, NANORTC_MEDIA_OPUS, NULL) == 0) {
        s_audio_ready = 1;
        ESP_LOGI(TAG, "Embedded audio: %d frames (%.1f KB)", s_audio_src.frame_count,
                 (float)s_audio_src.blob_len / 1024.0f);
    } else {
        ESP_LOGE(TAG, "Failed to init embedded audio source");
    }

    ESP_LOGI(TAG, "Open http://%s/ in your browser", s_local_ip);
}
