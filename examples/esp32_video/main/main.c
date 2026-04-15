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

#include "nanortc.h"
#include "nanortc_crypto.h"
#include "run_loop.h"
#include "webserver.h"
#include "media_source.h"
#include "media_pacer.h"

static const char *TAG = "nanortc_av";

static void nanortc_log_cb(const nanortc_log_message_t *msg, void *ctx)
{
    (void)ctx;
    ESP_LOGI("nrtc", "[%s] %s", msg->subsystem ? msg->subsystem : "?",
             msg->message ? msg->message : "");
}

#define VIDEO_FPS      CONFIG_EXAMPLE_VIDEO_FPS
#define VIDEO_INTERVAL (1000 / VIDEO_FPS) /* ms per frame */
#define AUDIO_INTERVAL 20                 /* 20 ms per Opus frame */

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
static nano_media_pacer_t s_video_pacer = {.interval_ms = VIDEO_INTERVAL};

/* Audio state */
static nano_media_source_t s_audio_src;
static int s_audio_ready;
static nano_media_pacer_t s_audio_pacer = {.interval_ms = AUDIO_INTERVAL};

/* Embedded files */
extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[] asm("_binary_index_html_end");
extern const uint8_t video_blob_start[] asm("_binary_video_blob_start");
extern const uint8_t video_blob_end[] asm("_binary_video_blob_end");
extern const uint8_t audio_blob_start[] asm("_binary_audio_blob_start");
extern const uint8_t audio_blob_end[] asm("_binary_audio_blob_end");

/* Debug counters */
static volatile uint32_t s_step_count;
static volatile uint32_t s_task_alive;

/* ----------------------------------------------------------------
 * Video send tick (epoch-based pacing)
 * ---------------------------------------------------------------- */
static void video_send_tick(nanortc_t *rtc, uint8_t mid, uint32_t now)
{
    uint32_t due = nano_media_pacer_due(&s_video_pacer, now);
    for (uint32_t i = 0; i < due; i++) {
        static uint8_t sd_buf[NANORTC_MEDIA_MAX_FRAME_SIZE];
        size_t frame_len = 0;
        uint32_t ts_ms = 0;
        if (nano_media_source_next_frame(&s_video_src, sd_buf, sizeof(sd_buf), &frame_len,
                                         &ts_ms) != 0)
            break;

        nanortc_send_video(rtc, mid, (uint32_t)(esp_timer_get_time() / 1000), sd_buf, frame_len);
        nano_media_pacer_advance(&s_video_pacer);
    }
}

/* ----------------------------------------------------------------
 * Audio send tick (epoch-based pacing, 20ms Opus frames)
 * ---------------------------------------------------------------- */
static void audio_send_tick(nanortc_t *rtc, uint8_t mid, uint32_t now)
{
    uint32_t due = nano_media_pacer_due(&s_audio_pacer, now);
    for (uint32_t i = 0; i < due; i++) {
        static uint8_t audio_buf[960]; /* Opus frames are small (~160 bytes) */
        size_t frame_len = 0;
        uint32_t ts_ms = 0;
        if (nano_media_source_next_frame(&s_audio_src, audio_buf, sizeof(audio_buf), &frame_len,
                                         &ts_ms) != 0)
            break;

        nanortc_send_audio(rtc, mid, (uint32_t)(esp_timer_get_time() / 1000), audio_buf, frame_len);
        nano_media_pacer_advance(&s_audio_pacer);
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
        nano_media_pacer_reset(&s_video_pacer);
        nano_media_pacer_reset(&s_audio_pacer);
        break;

    case NANORTC_EV_KEYFRAME_REQUEST:
        ESP_LOGI(TAG, "Keyframe requested (mid=%d) — resetting to frame 0",
                 evt->keyframe_request.mid);
        nano_media_source_reset(&s_video_src);
        nano_media_pacer_reset(&s_video_pacer);
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
 * Track setup callback — add audio first, video second.
 * Mid order must match the browser's m-line order.
 * ---------------------------------------------------------------- */
static int setup_av_tracks(nanortc_t *rtc, void *userdata)
{
    (void)userdata;

    s_audio_mid = nanortc_add_audio_track(rtc, NANORTC_DIR_SENDONLY, NANORTC_CODEC_OPUS, 48000, 2);
    if (s_audio_mid < 0) {
        ESP_LOGE(TAG, "nanortc_add_audio_track failed: %d", s_audio_mid);
        return s_audio_mid;
    }

    s_video_mid = nanortc_add_video_track(rtc, NANORTC_DIR_SENDONLY, NANORTC_CODEC_H264);
    if (s_video_mid < 0) {
        ESP_LOGE(TAG, "nanortc_add_video_track failed: %d", s_video_mid);
        return s_video_mid;
    }
    return 0;
}

/* ----------------------------------------------------------------
 * POST /offer handler — full nanortc session lifecycle
 * ---------------------------------------------------------------- */
static int handle_offer(const char *offer, char *answer, size_t answer_size, size_t *answer_len,
                        void *userdata)
{
    (void)userdata;

    s_connected = 0;

    nanortc_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.crypto = nanortc_crypto_mbedtls();
    cfg.role = NANORTC_ROLE_CONTROLLED;
    cfg.log.callback = nanortc_log_cb;
    cfg.log.level = NANORTC_LOG_DEBUG;

    nano_accept_offer_params_t params = {
        .rtc_cfg = &cfg,
        .track_setup = setup_av_tracks,
        .local_ip = s_local_ip,
        .udp_port = CONFIG_EXAMPLE_UDP_PORT,
        .max_poll_ms = 20,
        .event_cb = on_event,
    };

    int rc = nano_session_accept_offer(&s_rtc, &s_loop, &params, offer, answer, answer_size,
                                       answer_len);
    if (rc != NANORTC_OK) {
        ESP_LOGE(TAG, "nano_session_accept_offer failed: %d (%s)", rc, nanortc_err_name(rc));
        return rc;
    }

    ESP_LOGI(TAG, "remote_candidates=%d", s_rtc.ice.remote_candidate_count);
    return 0;
}

/* ----------------------------------------------------------------
 * Custom HTTP handler: GET /debug
 * ---------------------------------------------------------------- */
static esp_err_t http_get_debug(httpd_req_t *req)
{
    char buf[512];
    int n = snprintf(buf, sizeof(buf),
                     "running=%d fd=%d connected=%d video_ready=%d video_mid=%d\n"
                     "ice.remote_candidates=%d ice.state=%d\n"
                     "state=%d steps=%lu alive=%lu\n",
                     s_loop.running, s_loop.fds[0], s_connected, s_video_ready, s_video_mid,
                     s_rtc.ice.remote_candidate_count, s_rtc.ice.state, s_rtc.state,
                     (unsigned long)s_step_count, (unsigned long)s_task_alive);
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, buf, n);
    return ESP_OK;
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
                    video_send_tick(&s_rtc, (uint8_t)s_video_mid, now);
                if (s_audio_ready)
                    audio_send_tick(&s_rtc, (uint8_t)s_audio_mid, now);
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

    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!netif) {
        ESP_LOGE(TAG, "Failed to get station IP");
        return;
    }
    esp_netif_ip_info_t ip_info;
    ESP_ERROR_CHECK(esp_netif_get_ip_info(netif, &ip_info));
    esp_ip4addr_ntoa(&ip_info.ip, s_local_ip, sizeof(s_local_ip));
    ESP_LOGI(TAG, "Station IP: %s", s_local_ip);

    /* 3. Init run loop state (not started until POST /offer) */
    memset(&s_loop, 0, sizeof(s_loop));

    /* 4. Start HTTP server */
    nano_webserver_config_t wscfg;
    memset(&wscfg, 0, sizeof(wscfg));
    wscfg.html_start = index_html_start;
    wscfg.html_end = index_html_end;
    wscfg.offer_handler = handle_offer;
    wscfg.tag = TAG;

    httpd_handle_t server = nano_webserver_start(&wscfg);
    if (!server)
        return;

    /* Register custom /debug route */
    httpd_uri_t uri_debug = {
        .uri = "/debug",
        .method = HTTP_GET,
        .handler = http_get_debug,
    };
    httpd_register_uri_handler(server, &uri_debug);

    /* 5. Start WebRTC task */
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
