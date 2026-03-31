/*
 * nanortc ESP32 DataChannel example — Self-hosted Web UI
 *
 * The ESP32 hosts a web page at http://<ip>/. The browser connects,
 * sends an SDP offer via POST /offer, and receives an answer.
 * DataChannel messages are echoed back.
 *
 * Build: cd examples/esp32_datachannel && idf.py build
 * Flash: idf.py flash monitor
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "esp_http_server.h"
#include <lwip/sockets.h>
#include "protocol_examples_common.h"

#include "nanortc.h"
#include "nanortc_crypto.h"
#include "run_loop.h"

static const char *TAG = "nanortc_dc";

/* nanortc state — static because nanortc_t is large */
static nanortc_t s_rtc;
static nano_run_loop_t s_loop;
static char s_local_ip[16];

/* Embedded HTML file (linked by EMBED_TXTFILES in CMakeLists.txt) */
extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[] asm("_binary_index_html_end");

/* ----------------------------------------------------------------
 * Get station IP as string
 * ---------------------------------------------------------------- */
static int get_sta_ip(esp_netif_t *netif, char *ip_out, size_t ip_out_len)
{
    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(netif, &ip_info) != ESP_OK) {
        return -1;
    }
    esp_ip4addr_ntoa(&ip_info.ip, ip_out, (int)ip_out_len);
    return 0;
}

/* ----------------------------------------------------------------
 * nanortc event callback
 * ---------------------------------------------------------------- */
static void on_event(nanortc_t *rtc, const nanortc_event_t *evt,
                     void *userdata)
{
    (void)userdata;

    switch (evt->type) {
    case NANORTC_EVENT_ICE_CONNECTED:
        ESP_LOGI(TAG, "ICE connected");
        break;

    case NANORTC_EVENT_DTLS_CONNECTED:
        ESP_LOGI(TAG, "DTLS connected");
        break;

    case NANORTC_EVENT_SCTP_CONNECTED:
        ESP_LOGI(TAG, "SCTP connected");
        break;

    case NANORTC_EVENT_DATACHANNEL_OPEN:
        ESP_LOGI(TAG, "DataChannel open (stream=%d)", evt->stream_id);
        break;

    case NANORTC_EVENT_DATACHANNEL_DATA:
        ESP_LOGI(TAG, "DC data (%u bytes), echoing back", (unsigned)evt->len);
        nanortc_send_datachannel(rtc, evt->stream_id, evt->data, evt->len);
        break;

    case NANORTC_EVENT_DATACHANNEL_STRING:
        ESP_LOGI(TAG, "DC string: %.*s", (int)evt->len, (char *)evt->data);
        nanortc_send_datachannel_string(rtc, evt->stream_id,
                                        (const char *)evt->data);
        break;

    case NANORTC_EVENT_DATACHANNEL_CLOSE:
        ESP_LOGI(TAG, "DataChannel closed");
        break;

    case NANORTC_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "Disconnected");
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
 * WebRTC event loop task
 * ---------------------------------------------------------------- */
static void webrtc_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "WebRTC task started");

    for (;;) {
        if (s_loop.running) {
            nano_run_loop_step(&s_loop);
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

    /* 3. Connect (WiFi or Ethernet, configured via menuconfig) */
    ESP_ERROR_CHECK(example_connect());
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");

    if (get_sta_ip(netif, s_local_ip, sizeof(s_local_ip)) < 0) {
        ESP_LOGE(TAG, "Failed to get station IP");
        return;
    }
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
    xTaskCreatePinnedToCore(webrtc_task, "webrtc", 8192, NULL, 5, NULL, 0);

    ESP_LOGI(TAG, "Open http://%s/ in your browser", s_local_ip);
}
