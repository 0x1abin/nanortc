/*
 * nanortc ESP32 Audio example — Algorithmic music sender
 *
 * The ESP32 hosts a web page at http://<ip>/. The browser connects,
 * sends an SDP offer via POST /offer, and receives an answer.
 * ESP32 generates a three-voice melody (Twinkle, Twinkle, Little Star),
 * encodes it (PCMU/PCMA/Opus), and streams it to the browser via
 * WebRTC audio.
 *
 * Build: cd examples/esp32_audio && idf.py build
 * Flash: idf.py flash monitor
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <string.h>

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

#include "nanortc.h"
#include "nanortc_crypto.h"
#include "run_loop.h"

#include "music_gen.h"

#include "esp_audio_enc.h"
#include "esp_audio_enc_default.h"
#if defined(CONFIG_EXAMPLE_AUDIO_CODEC_OPUS)
#include "esp_opus_enc.h"
#else
#include "esp_g711_enc.h"
#endif

static const char *TAG = "nanortc_audio";

/* ----------------------------------------------------------------
 * Codec configuration (resolved at compile time from Kconfig)
 * ---------------------------------------------------------------- */
#if defined(CONFIG_EXAMPLE_AUDIO_CODEC_OPUS)
#define AUDIO_CODEC      NANORTC_CODEC_OPUS
#define AUDIO_SAMPLE_RATE 48000
#define AUDIO_CHANNELS    1
#define SAMPLES_PER_FRAME 960  /* 48kHz * 20ms */
#define RTP_TS_INCREMENT  960
#define CODEC_NAME        "Opus 48kHz mono"
#elif defined(CONFIG_EXAMPLE_AUDIO_CODEC_PCMA)
#define AUDIO_CODEC      NANORTC_CODEC_PCMA
#define AUDIO_SAMPLE_RATE 8000
#define AUDIO_CHANNELS    1
#define SAMPLES_PER_FRAME 160  /* 8kHz * 20ms */
#define RTP_TS_INCREMENT  160
#define CODEC_NAME        "G.711 A-law 8kHz"
#else /* default: PCMU */
#define AUDIO_CODEC      NANORTC_CODEC_PCMU
#define AUDIO_SAMPLE_RATE 8000
#define AUDIO_CHANNELS    1
#define SAMPLES_PER_FRAME 160  /* 8kHz * 20ms */
#define RTP_TS_INCREMENT  160
#define CODEC_NAME        "G.711 mu-law 8kHz"
#endif

/* nanortc state — static because nanortc_t is large */
static nanortc_t s_rtc;
static nano_run_loop_t s_loop;
static char s_local_ip[16];
static int s_connected;

/* Audio state */
static uint32_t s_audio_epoch_ms;
static uint32_t s_audio_frame_count;
static uint32_t s_audio_rtp_ts;

/* Audio encoder (esp_audio_codec unified API) */
static esp_audio_enc_handle_t s_encoder;
static int s_enc_in_size;
static int s_enc_out_size;
static void encoder_init(void);

/* Embedded HTML file (linked by EMBED_TXTFILES in CMakeLists.txt) */
extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[] asm("_binary_index_html_end");

/* BPM for startup log */
#ifndef CONFIG_EXAMPLE_MUSIC_BPM
#define CONFIG_EXAMPLE_MUSIC_BPM 120
#endif
#define MUSIC_BPM CONFIG_EXAMPLE_MUSIC_BPM

/* ----------------------------------------------------------------
 * Audio encode + send
 * ---------------------------------------------------------------- */
static void audio_send_tick(uint32_t now)
{
    if (s_audio_epoch_ms == 0)
        s_audio_epoch_ms = now;

    uint32_t target_frames = (now - s_audio_epoch_ms) / 20;

    /* Skip frames if we fell too far behind */
    if (target_frames - s_audio_frame_count > 2) {
        s_audio_frame_count = target_frames - 1;
    }

    while (s_audio_frame_count < target_frames) {
        /* 1. Generate music PCM */
        static int16_t pcm_buf[SAMPLES_PER_FRAME * AUDIO_CHANNELS];
        music_generate(pcm_buf, SAMPLES_PER_FRAME, AUDIO_SAMPLE_RATE,
                       AUDIO_CHANNELS);

        /* 2. Encode (unified esp_audio_codec API) */
        static uint8_t encoded[1024];
        if (s_encoder) {
            esp_audio_enc_in_frame_t in_frame = {
                .buffer = (uint8_t *)pcm_buf,
                .len = (uint32_t)s_enc_in_size,
            };
            esp_audio_enc_out_frame_t out_frame = {
                .buffer = encoded,
                .len = (uint32_t)s_enc_out_size,
            };
            esp_audio_err_t ret =
                esp_audio_enc_process(s_encoder, &in_frame, &out_frame);
            if (ret == ESP_AUDIO_ERR_OK && out_frame.encoded_bytes > 0) {
                nanortc_send_audio(&s_rtc, s_audio_rtp_ts, encoded,
                                   out_frame.encoded_bytes);
            }
        }

        s_audio_rtp_ts += RTP_TS_INCREMENT;
        s_audio_frame_count++;
    }
}

/* ----------------------------------------------------------------
 * nanortc event callback
 * ---------------------------------------------------------------- */
static void on_event(nanortc_t *rtc, const nanortc_event_t *evt,
                     void *userdata)
{
    (void)rtc;
    (void)userdata;

    switch (evt->type) {
    case NANORTC_EVENT_ICE_CONNECTED:
        ESP_LOGI(TAG, "ICE connected");
        break;

    case NANORTC_EVENT_DTLS_CONNECTED:
        ESP_LOGI(TAG, "DTLS connected — starting audio");
        s_connected = 1;
        s_audio_epoch_ms = 0;
        s_audio_frame_count = 0;
        s_audio_rtp_ts = 0;
        music_reset();
        encoder_init();
        break;

    case NANORTC_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "Disconnected");
        s_connected = 0;
        if (s_encoder) {
            esp_audio_enc_close(s_encoder);
            s_encoder = NULL;
        }
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
    /* Read offer body */
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

    /* Re-initialize nanortc for new session (handles browser refresh) */
    s_connected = 0;
    if (s_encoder) {
        esp_audio_enc_close(s_encoder);
        s_encoder = NULL;
    }
    nano_run_loop_destroy(&s_loop);
    nanortc_destroy(&s_rtc);

    nanortc_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.crypto = nanortc_crypto_mbedtls();
    cfg.role = NANORTC_ROLE_CONTROLLED;
    cfg.audio_codec = AUDIO_CODEC;
    cfg.audio_sample_rate = AUDIO_SAMPLE_RATE;
    cfg.audio_channels = AUDIO_CHANNELS;
    cfg.audio_direction = NANORTC_DIR_SENDONLY;

    int rc = nanortc_init(&s_rtc, &cfg);
    if (rc != NANORTC_OK) {
        ESP_LOGE(TAG, "nanortc_init failed: %d", rc);
        free(offer);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Init fail");
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

    /* Generate SDP answer (heap-allocated to avoid httpd stack overflow) */
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
        ESP_LOGE(TAG, "nanortc_accept_offer failed: %d (%s)", rc,
                 nanortc_err_to_name(rc));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            nanortc_err_to_name(rc));
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
        .uri = "/", .method = HTTP_GET, .handler = http_get_root,
    };
    httpd_uri_t uri_offer = {
        .uri = "/offer", .method = HTTP_POST, .handler = http_post_offer,
    };
    httpd_register_uri_handler(server, &uri_root);
    httpd_register_uri_handler(server, &uri_offer);

    ESP_LOGI(TAG, "HTTP server started on port %d", config.server_port);
    return server;
}

/* ----------------------------------------------------------------
 * Audio encoder init (esp_audio_codec unified API)
 * ---------------------------------------------------------------- */
static void encoder_init(void)
{
    if (s_encoder) {
        esp_audio_enc_close(s_encoder);
        s_encoder = NULL;
    }

    esp_audio_enc_register_default();

    esp_audio_enc_config_t enc_cfg = {0};

#if defined(CONFIG_EXAMPLE_AUDIO_CODEC_PCMU)
    esp_g711_enc_config_t g711_cfg = ESP_G711_ENC_CONFIG_DEFAULT();
    g711_cfg.frame_duration = 20;
    enc_cfg.type = ESP_AUDIO_TYPE_G711U;
    enc_cfg.cfg = &g711_cfg;
    enc_cfg.cfg_sz = sizeof(g711_cfg);
#elif defined(CONFIG_EXAMPLE_AUDIO_CODEC_PCMA)
    esp_g711_enc_config_t g711_cfg = ESP_G711_ENC_CONFIG_DEFAULT();
    g711_cfg.frame_duration = 20;
    enc_cfg.type = ESP_AUDIO_TYPE_G711A;
    enc_cfg.cfg = &g711_cfg;
    enc_cfg.cfg_sz = sizeof(g711_cfg);
#elif defined(CONFIG_EXAMPLE_AUDIO_CODEC_OPUS)
    esp_opus_enc_config_t opus_cfg = ESP_OPUS_ENC_CONFIG_DEFAULT();
    opus_cfg.sample_rate = ESP_AUDIO_SAMPLE_RATE_48K;
    opus_cfg.channel = ESP_AUDIO_MONO;
    opus_cfg.bitrate = 64000;
    opus_cfg.application_mode = ESP_OPUS_ENC_APPLICATION_VOIP;
    opus_cfg.complexity = 1;
    enc_cfg.type = ESP_AUDIO_TYPE_OPUS;
    enc_cfg.cfg = &opus_cfg;
    enc_cfg.cfg_sz = sizeof(opus_cfg);
#endif

    esp_audio_err_t ret = esp_audio_enc_open(&enc_cfg, &s_encoder);
    if (ret != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "esp_audio_enc_open failed: %d", ret);
        s_encoder = NULL;
        return;
    }
    esp_audio_enc_get_frame_size(s_encoder, &s_enc_in_size, &s_enc_out_size);
    ESP_LOGI(TAG, "Encoder initialized: in=%d out=%d bytes", s_enc_in_size,
             s_enc_out_size);
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
            if (s_connected) {
                uint32_t now = nano_get_millis();
                audio_send_tick(now);
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
    ESP_LOGI(TAG, "nanortc ESP32 Audio example — %s, %d BPM music",
             CODEC_NAME, MUSIC_BPM);

    /* 1. NVS init (required for WiFi) */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* 2. Network init */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* 3. Connect (WiFi or Ethernet via menuconfig) */
    ESP_ERROR_CHECK(example_connect());

    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!netif) {
        ESP_LOGE(TAG, "Failed to get WiFi STA interface");
        return;
    }
    esp_netif_ip_info_t ip_info;
    ESP_ERROR_CHECK(esp_netif_get_ip_info(netif, &ip_info));
    esp_ip4addr_ntoa(&ip_info.ip, s_local_ip, sizeof(s_local_ip));
    ESP_LOGI(TAG, "Station IP: %s", s_local_ip);

    /* 4. Init run loop state (not started until POST /offer) */
    memset(&s_loop, 0, sizeof(s_loop));
    s_loop.fd = -1;

    /* 5. Start HTTP server */
    if (!start_http_server()) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return;
    }

    /* 6. Start WebRTC event loop task */
    xTaskCreatePinnedToCore(webrtc_task, "webrtc", 32768, NULL, 5, NULL, 0);

    ESP_LOGI(TAG, "Open http://%s/ in your browser", s_local_ip);
}
