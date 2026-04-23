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
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "protocol_examples_common.h"

#include "nanortc.h"
#include "nanortc_crypto.h"
#include "run_loop.h"
#include "webserver.h"

static const char *TAG = "nanortc_dc";

/* nanortc state — static because nanortc_t is large */
static nanortc_t s_rtc;
static nano_run_loop_t s_loop;
static char s_local_ip[16];
static TaskHandle_t s_webrtc_handle;

/* Embedded HTML file (linked by EMBED_TXTFILES in CMakeLists.txt) */
extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[] asm("_binary_index_html_end");

/* ----------------------------------------------------------------
 * nanortc event callback
 * ---------------------------------------------------------------- */
static void on_event(nanortc_t *rtc, const nanortc_event_t *evt, void *userdata)
{
    (void)userdata;

    switch (evt->type) {
    case NANORTC_EV_ICE_STATE_CHANGE:
        if (evt->ice_state == NANORTC_ICE_STATE_CONNECTED) {
            ESP_LOGI(TAG, "ICE connected");
        }
        break;

    case NANORTC_EV_CONNECTED:
        ESP_LOGI(TAG, "Connected");
        break;

    case NANORTC_EV_DATACHANNEL_OPEN:
        ESP_LOGI(TAG, "DataChannel open (id=%d)", evt->datachannel_open.id);
        break;

    case NANORTC_EV_DATACHANNEL_DATA:
        if (evt->datachannel_data.binary) {
            ESP_LOGI(TAG, "DC data (%u bytes), echoing back", (unsigned)evt->datachannel_data.len);
            nanortc_datachannel_send(rtc, evt->datachannel_data.id, evt->datachannel_data.data,
                                     evt->datachannel_data.len);
        } else {
            ESP_LOGI(TAG, "DC string: %.*s", (int)evt->datachannel_data.len,
                     (char *)evt->datachannel_data.data);
            nanortc_datachannel_send_string(rtc, evt->datachannel_data.id,
                                            (const char *)evt->datachannel_data.data);
        }
        break;

    case NANORTC_EV_DATACHANNEL_CLOSE:
        ESP_LOGI(TAG, "DataChannel closed");
        break;

    case NANORTC_EV_DISCONNECTED:
        ESP_LOGI(TAG, "Disconnected");
        nano_run_loop_stop(&s_loop);
        break;

    default:
        break;
    }
}

/* ----------------------------------------------------------------
 * POST /offer handler — full nanortc session lifecycle
 * ---------------------------------------------------------------- */
static int handle_offer(const char *offer, char *answer, size_t answer_size, size_t *answer_len,
                        void *userdata)
{
    (void)userdata;

    nanortc_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.crypto = nanortc_crypto_mbedtls();
    cfg.role = NANORTC_ROLE_CONTROLLED;

    /* No tracks needed for DataChannel */
    nano_accept_offer_params_t params = {
        .rtc_cfg = &cfg,
        .local_ip = s_local_ip,
        .udp_port = CONFIG_EXAMPLE_UDP_PORT,
        .max_poll_ms = 5,
        .event_cb = on_event,
    };

    int rc = nano_session_accept_offer(&s_rtc, &s_loop, &params, offer, answer, answer_size,
                                       answer_len);
    if (rc != NANORTC_OK) {
        ESP_LOGE(TAG, "nano_session_accept_offer failed: %d (%s)", rc, nanortc_err_name(rc));
    }
    return rc;
}

/* ----------------------------------------------------------------
 * WebRTC event loop task
 * ---------------------------------------------------------------- */
static void webrtc_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "WebRTC task started");

    uint32_t last_hwm_log_ms = 0;
    for (;;) {
        if (s_loop.running) {
            nano_run_loop_step(&s_loop);
        } else {
            vTaskDelay(pdMS_TO_TICKS(50));
        }

        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
        if (now_ms - last_hwm_log_ms >= 5000) {
            last_hwm_log_ms = now_ms;
            ESP_LOGI(TAG, "[stack HWM] webrtc=%lu words free",
                     (unsigned long)uxTaskGetStackHighWaterMark(s_webrtc_handle));
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
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
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

    /* 5. Start HTTP server */
    nano_webserver_config_t wscfg;
    memset(&wscfg, 0, sizeof(wscfg));
    wscfg.html_start = index_html_start;
    wscfg.html_end = index_html_end;
    wscfg.offer_handler = handle_offer;
    wscfg.tag = TAG;

    if (!nano_webserver_start(&wscfg))
        return;

    /* 6. Start WebRTC event loop task. DC-only path has no SRTP/RTP/jitter; the
     * main stack consumer is mbedTLS DTLS handshake (~3-4 KB peak). 4 KB gives
     * ~1 KB headroom; the HWM log above verifies this on first handshake. */
    xTaskCreate(webrtc_task, "webrtc", 4096, NULL, 5, &s_webrtc_handle);

    ESP_LOGI(TAG, "Open http://%s/ in your browser", s_local_ip);
}
