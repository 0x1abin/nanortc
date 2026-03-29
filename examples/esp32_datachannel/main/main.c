/*
 * nanortc ESP32 DataChannel example
 *
 * Connects to WiFi, discovers the signaling server via UDP broadcast,
 * then acts as a WebRTC answerer. Echoes all DataChannel messages
 * back to the remote peer (browser or Linux offerer).
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
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include <lwip/sockets.h>

#include "nanortc.h"
#include "nanortc_crypto.h"
#include "run_loop.h"
#include "http_signaling.h"
#include "udp_discovery.h"

static const char *TAG = "nanortc_dc";

/* WiFi connection event group */
static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

/* nanortc state — static because nanortc_t is large */
static nanortc_t s_rtc;
static nano_run_loop_t s_loop;

/* ----------------------------------------------------------------
 * WiFi event handler
 * ---------------------------------------------------------------- */
static int s_retry_count;
#define WIFI_MAX_RETRY 10

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT &&
               event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_count < WIFI_MAX_RETRY) {
            esp_wifi_connect();
            s_retry_count++;
            ESP_LOGI(TAG, "Retry WiFi connection (%d/%d)", s_retry_count,
                     WIFI_MAX_RETRY);
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_count = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static esp_netif_t *wifi_init_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();

    esp_netif_t *netif = esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t inst_any_id;
    esp_event_handler_instance_t inst_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL,
        &inst_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL,
        &inst_got_ip));

    wifi_config_t wifi_config = {
        .sta =
            {
                .ssid = CONFIG_EXAMPLE_WIFI_SSID,
                .password = CONFIG_EXAMPLE_WIFI_PASSWORD,
            },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Connecting to %s ...", CONFIG_EXAMPLE_WIFI_SSID);

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE, pdFALSE, portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "WiFi connected");
    } else {
        ESP_LOGE(TAG, "WiFi connection failed");
    }

    return netif;
}

/* ----------------------------------------------------------------
 * Get station IP as string
 * ---------------------------------------------------------------- */
static int get_sta_ip(esp_netif_t *netif, char *ip_out, size_t ip_out_len)
{
    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(netif, &ip_info) != ESP_OK) {
        return -1;
    }
    /* esp_ip4addr_ntoa is provided by ESP-IDF (lwIP wrapper) */
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
 * Signaling: answerer mode
 * ---------------------------------------------------------------- */
static int do_answer_signaling(http_sig_t *sig, nanortc_t *rtc)
{
    char type[32];
    char payload[HTTP_SIG_BUF_SIZE];

    ESP_LOGI(TAG, "Waiting for SDP offer...");
    for (;;) {
        int rc = http_sig_recv(sig, type, sizeof(type),
                               payload, sizeof(payload), 2000);
        if (rc == -2) continue; /* timeout, retry */
        if (rc < 0) {
            ESP_LOGE(TAG, "Signaling error waiting for offer");
            return -1;
        }
        if (memcmp(type, "offer", 6) == 0) break;
        ESP_LOGW(TAG, "Ignoring '%s' (waiting for offer)", type);
    }

    size_t offer_len = strlen(payload); /* NANORTC_SAFE: API boundary */
    ESP_LOGI(TAG, "Got SDP offer (%u bytes)", (unsigned)offer_len);

    char answer[HTTP_SIG_BUF_SIZE];
    size_t answer_len = 0;
    int rc = nanortc_accept_offer(rtc, payload, answer, sizeof(answer),
                                  &answer_len);
    if (rc != NANORTC_OK) {
        ESP_LOGE(TAG, "nanortc_accept_offer failed: %d", rc);
        return rc;
    }
    ESP_LOGI(TAG, "Generated SDP answer (%u bytes)", (unsigned)answer_len);

    rc = http_sig_send(sig, "answer", answer, "sdp");
    if (rc < 0) {
        ESP_LOGE(TAG, "Failed to send SDP answer");
        return -1;
    }
    ESP_LOGI(TAG, "Sent SDP answer");
    return 0;
}

/* ----------------------------------------------------------------
 * Trickle ICE: poll signaling for remote candidates
 * ---------------------------------------------------------------- */
static uint32_t s_last_poll_ms;

static void poll_trickle_ice(http_sig_t *sig, nanortc_t *rtc)
{
    uint32_t now = nano_get_millis();
    if (now - s_last_poll_ms < 500) return;
    s_last_poll_ms = now;

    char type[32];
    char payload[HTTP_SIG_BUF_SIZE];

    int rc = http_sig_recv(sig, type, sizeof(type),
                           payload, sizeof(payload), 0);
    if (rc == 0) {
        if (memcmp(type, "candidate", 10) == 0 && payload[0] != '\0') {
            ESP_LOGI(TAG, "Trickle ICE candidate received");
            nanortc_add_remote_candidate(rtc, payload);
        } else if (memcmp(type, "candidate", 10) == 0) {
            ESP_LOGI(TAG, "End-of-candidates");
        }
    }
}

/* ----------------------------------------------------------------
 * Discover signaling server or fall back to Kconfig defaults
 * ---------------------------------------------------------------- */
static void resolve_signaling(char *host, size_t host_len, uint16_t *port)
{
    if (udp_discover_signaling(CONFIG_EXAMPLE_DISCOVERY_PORT,
                               host, host_len, port, 3) == 0) {
        return;
    }
    ESP_LOGW(TAG, "Discovery failed, using fallback %s:%d",
             CONFIG_EXAMPLE_SIGNALING_HOST, CONFIG_EXAMPLE_SIGNALING_PORT);
    size_t hlen = strlen(CONFIG_EXAMPLE_SIGNALING_HOST); /* NANORTC_SAFE: API boundary */
    if (hlen >= host_len) hlen = host_len - 1;
    memcpy(host, CONFIG_EXAMPLE_SIGNALING_HOST, hlen);
    host[hlen] = '\0';
    *port = CONFIG_EXAMPLE_SIGNALING_PORT;
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

    /* 3. WiFi connect */
    esp_netif_t *netif = wifi_init_sta();

    char sta_ip[16];
    if (get_sta_ip(netif, sta_ip, sizeof(sta_ip)) < 0) {
        ESP_LOGE(TAG, "Failed to get station IP");
        return;
    }
    ESP_LOGI(TAG, "Station IP: %s", sta_ip);

    /* 4. Discover + join signaling server (retry loop) */
    char sig_host[64];
    uint16_t sig_port = CONFIG_EXAMPLE_SIGNALING_PORT;
    http_sig_t sig;
    int rc;

    for (int attempt = 0; attempt < 10; attempt++) {
        if (attempt > 0) {
            ESP_LOGI(TAG, "Retrying signaling (%d/10)...", attempt + 1);
            vTaskDelay(pdMS_TO_TICKS(3000));
        }
        resolve_signaling(sig_host, sizeof(sig_host), &sig_port);
        rc = http_sig_join(&sig, sig_host, sig_port);
        if (rc == 0) break;
        ESP_LOGW(TAG, "Failed to join %s:%u", sig_host, sig_port);
    }
    if (rc < 0) {
        ESP_LOGE(TAG, "Could not join signaling server after retries");
        return;
    }

    /* 6. Init nanortc (answerer / CONTROLLED) */
    nanortc_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.crypto = nanortc_crypto_mbedtls();
    cfg.role = NANORTC_ROLE_CONTROLLED;

    rc = nanortc_init(&s_rtc, &cfg);
    if (rc != NANORTC_OK) {
        ESP_LOGE(TAG, "nanortc_init failed: %d", rc);
        http_sig_leave(&sig);
        return;
    }

    /* 7. Bind UDP socket, register station IP as candidate */
    rc = nano_run_loop_init(&s_loop, &s_rtc, NULL, CONFIG_EXAMPLE_UDP_PORT);
    if (rc < 0) {
        ESP_LOGE(TAG, "Failed to bind UDP port %d", CONFIG_EXAMPLE_UDP_PORT);
        http_sig_leave(&sig);
        nanortc_destroy(&s_rtc);
        return;
    }
    nanortc_add_local_candidate(&s_rtc, sta_ip, CONFIG_EXAMPLE_UDP_PORT);
    nano_run_loop_set_event_cb(&s_loop, on_event, NULL);

    ESP_LOGI(TAG, "nanortc ESP32 DC (answerer, udp=%s:%d, sig=%s:%u)",
             sta_ip, CONFIG_EXAMPLE_UDP_PORT, sig_host, sig_port);

    /* 8. SDP exchange (answerer: wait for offer → send answer) */
    rc = do_answer_signaling(&sig, &s_rtc);
    if (rc != 0) {
        goto cleanup;
    }

    /* 9. Event loop with trickle ICE polling */
    ESP_LOGI(TAG, "Entering event loop...");
    s_loop.running = 1;
    while (s_loop.running) {
        nano_run_loop_step(&s_loop);
        poll_trickle_ice(&sig, &s_rtc);
    }

cleanup:
    http_sig_leave(&sig);
    nano_run_loop_destroy(&s_loop);
    nanortc_destroy(&s_rtc);
    ESP_LOGI(TAG, "Done.");
}
