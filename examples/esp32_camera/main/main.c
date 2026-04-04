/*
 * nanortc ESP32-P4 Camera example — live H264 + Opus audio/video streaming
 *
 * The ESP32-P4 captures video from an OV5647 camera (MIPI CSI) and
 * audio from an ES8311 codec (I2S microphone), encodes H264 + Opus,
 * and streams both to a browser via NanoRTC WebRTC.
 *
 * Hardware initialization is handled by esp_board_manager using the
 * boards/esp32_p4_nano/ YAML configuration. Application code only
 * deals with V4L2 capture, H264 encoding, and esp_capture audio.
 *
 * Architecture:
 *   camera_task    (core 1, pri 6): camera grab -> H264 encode -> frame queue
 *   microphone_task(core 0, pri 7): poll esp_capture for Opus frames -> mic queue
 *   webrtc_task    (core 0, pri 5): nanortc run loop -> dequeue -> RTP send
 *
 * Build: cd examples/esp32_camera && idf.py set-target esp32p4 && idf.py build
 * Flash: idf.py flash monitor
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"
#include "esp_http_server.h"
#include "protocol_examples_common.h"

#include "esp_board_device.h"
#include "esp_board_periph.h"
#include "esp_board_manager_defs.h"

#include "nanortc.h"
#include "nanortc_crypto.h"
#include "run_loop.h"
#include "webserver.h"

#include "camera.h"
#include "encoder.h"
#include "microphone.h"

static const char *TAG = "nanortc_cam";

static void nanortc_log_cb(const nanortc_log_message_t *msg, void *ctx)
{
    (void)ctx;
    ESP_LOGI("nrtc", "[%s] %s", msg->subsystem ? msg->subsystem : "?",
             msg->message ? msg->message : "");
}

/* nanortc state */
static nanortc_t s_rtc;
static nano_run_loop_t s_loop;
static char s_local_ip[16];
static volatile int s_connected;
static int s_video_mid;
static int s_mic_mid;

/* Embedded HTML page */
extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[] asm("_binary_index_html_end");

/* Debug counters */
static volatile uint32_t s_step_count;
static volatile uint32_t s_task_alive;

/* Task handles for stack high-water-mark monitoring */
static TaskHandle_t s_webrtc_handle;
static TaskHandle_t s_mic_handle;
static TaskHandle_t s_camera_handle;

/* ----------------------------------------------------------------
 * Microphone queue: microphone_task -> webrtc_task
 * ---------------------------------------------------------------- */
typedef struct {
    uint8_t *data;
    size_t len;
    uint32_t pts_ms;
} mic_msg_t;

#define MIC_QUEUE_DEPTH 3
static QueueHandle_t s_mic_queue;

/* ----------------------------------------------------------------
 * Frame queue: camera_task -> webrtc_task
 * ---------------------------------------------------------------- */
typedef struct {
    uint8_t *data;
    size_t len;
    uint32_t pts_ms;
} frame_msg_t;

#define FRAME_QUEUE_DEPTH 2
static QueueHandle_t s_frame_queue;

/* ----------------------------------------------------------------
 * nanortc event callback
 * ---------------------------------------------------------------- */
static void on_event(nanortc_t *rtc, const nanortc_event_t *evt, void *userdata)
{
    (void)rtc;
    (void)userdata;

    switch (evt->type) {
    case NANORTC_EV_ICE_STATE_CHANGE:
        if (evt->ice_state == NANORTC_ICE_STATE_CONNECTED)
            ESP_LOGI(TAG, "ICE connected");
        break;

    case NANORTC_EV_CONNECTED:
        ESP_LOGI(TAG, "Connected — starting audio+video stream");
        s_connected = 1;
        encoder_request_keyframe();
        break;

    case NANORTC_EV_KEYFRAME_REQUEST:
        ESP_LOGI(TAG, "Keyframe requested (mid=%d)", evt->keyframe_request.mid);
        encoder_request_keyframe();
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
 * POST /offer handler — full nanortc session lifecycle
 * ---------------------------------------------------------------- */
static int handle_offer(const char *offer, char *answer, size_t answer_size,
                        size_t *answer_len, void *userdata)
{
    (void)userdata;

    /* Tear down previous session */
    s_connected = 0;
    nano_run_loop_destroy(&s_loop);
    nanortc_destroy(&s_rtc);

    /* Drain any leftover frames in the queues */
    frame_msg_t stale;
    while (xQueueReceive(s_frame_queue, &stale, 0) == pdTRUE) {
        heap_caps_free(stale.data);
    }
    mic_msg_t mic_stale;
    while (xQueueReceive(s_mic_queue, &mic_stale, 0) == pdTRUE) {
        free(mic_stale.data);
    }

    /* Initialize nanortc */
    nanortc_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.crypto = nanortc_crypto_mbedtls();
    cfg.role = NANORTC_ROLE_CONTROLLED;
    cfg.log.callback = nanortc_log_cb;
    cfg.log.level = NANORTC_LOG_DEBUG;

    int rc = nanortc_init(&s_rtc, &cfg);
    if (rc != NANORTC_OK) {
        ESP_LOGE(TAG, "nanortc_init failed: %d", rc);
        return rc;
    }

    /* Add tracks: audio first, video second (must match browser SDP m-line order) */
    s_mic_mid = nanortc_add_audio_track(&s_rtc, NANORTC_DIR_SENDONLY,
                                         NANORTC_CODEC_OPUS, 48000, 1);
    if (s_mic_mid < 0) {
        ESP_LOGE(TAG, "nanortc_add_audio_track failed: %d", s_mic_mid);
        return s_mic_mid;
    }

    s_video_mid = nanortc_add_video_track(&s_rtc, NANORTC_DIR_SENDONLY, NANORTC_CODEC_H264);
    if (s_video_mid < 0) {
        ESP_LOGE(TAG, "nanortc_add_video_track failed: %d", s_video_mid);
        return s_video_mid;
    }

    rc = nano_run_loop_init(&s_loop, &s_rtc, NULL, CONFIG_EXAMPLE_UDP_PORT);
    if (rc < 0) {
        ESP_LOGE(TAG, "Failed to bind UDP port");
        return rc;
    }
    nanortc_add_local_candidate(&s_rtc, s_local_ip, CONFIG_EXAMPLE_UDP_PORT);
    nano_run_loop_set_event_cb(&s_loop, on_event, NULL);
    s_loop.max_poll_ms = 20;

    rc = nanortc_accept_offer(&s_rtc, offer, answer, answer_size, answer_len);
    if (rc != NANORTC_OK) {
        ESP_LOGE(TAG, "nanortc_accept_offer failed: %d (%s)", rc, nanortc_err_name(rc));
        return rc;
    }

    s_loop.running = 1;
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
                     "running=%d fd=%d connected=%d video_mid=%d mic_mid=%d\n"
                     "ice.remote_candidates=%d ice.state=%d\n"
                     "state=%d steps=%lu alive=%lu\n",
                     s_loop.running, s_loop.fd, (int)s_connected, s_video_mid, s_mic_mid,
                     s_rtc.ice.remote_candidate_count, s_rtc.ice.state, s_rtc.state,
                     (unsigned long)s_step_count, (unsigned long)s_task_alive);
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, buf, n);
    return ESP_OK;
}

/* ----------------------------------------------------------------
 * Camera capture + encode task (runs on core 1)
 * ---------------------------------------------------------------- */
static void camera_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "Camera task started on core %d", xPortGetCoreID());

    if (camera_start_streaming() != 0) {
        ESP_LOGE(TAG, "Camera streaming failed, task exiting");
        vTaskDelete(NULL);
        return;
    }

    uint32_t grab_count = 0, enc_count = 0, enc_err = 0;
    uint32_t last_heap_log_ms = 0;

    for (;;) {
        uint8_t *yuv_buf = NULL;
        size_t yuv_len = 0;
        int grab_rc = camera_grab_frame(&yuv_buf, &yuv_len);
        if (grab_rc != 0) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        grab_count++;

        /* Periodic heap log */
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
        if (now_ms - last_heap_log_ms >= 5000) {
            last_heap_log_ms = now_ms;
            ESP_LOGI(TAG, "[heap] internal: %lu free, %lu min | PSRAM: %lu free, %lu min",
                     (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned long)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                     (unsigned long)heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM));
            ESP_LOGI(TAG, "[stack HWM] webrtc=%lu mic=%lu camera=%lu words free",
                     (unsigned long)uxTaskGetStackHighWaterMark(s_webrtc_handle),
                     (unsigned long)uxTaskGetStackHighWaterMark(s_mic_handle),
                     (unsigned long)uxTaskGetStackHighWaterMark(s_camera_handle));
        }

        if (!s_connected) {
            camera_release_frame();
            continue;
        }

        /* Encode YUV to H264 */
        uint8_t *h264_data = NULL;
        size_t h264_len = 0;
        bool is_keyframe = false;
        int rc = encoder_encode(yuv_buf, yuv_len, &h264_data, &h264_len, &is_keyframe);
        camera_release_frame();

        if (rc != 0 || h264_len == 0) {
            enc_err++;
            if (enc_err <= 3 || enc_err % 100 == 0)
                ESP_LOGW(TAG, "[cam] encode err rc=%d len=%u err#%"PRIu32, rc, (unsigned)h264_len, enc_err);
            continue;
        }
        enc_count++;
        if (enc_count <= 3 || enc_count % 100 == 0)
            ESP_LOGI(TAG, "[cam] enc#%"PRIu32" h264=%u kf=%d", enc_count, (unsigned)h264_len, is_keyframe);

        /* Enqueue for WebRTC task */
        uint8_t *copy = heap_caps_malloc(h264_len, MALLOC_CAP_SPIRAM);
        if (!copy) {
            ESP_LOGW(TAG, "Frame alloc failed (%u bytes), dropping", (unsigned)h264_len);
            continue;
        }
        memcpy(copy, h264_data, h264_len);

        frame_msg_t msg = {
            .data = copy,
            .len = h264_len,
            .pts_ms = (uint32_t)(esp_timer_get_time() / 1000),
        };
        if (xQueueSend(s_frame_queue, &msg, pdMS_TO_TICKS(50)) != pdTRUE) {
            heap_caps_free(copy);
        }
    }
}

/* ----------------------------------------------------------------
 * Microphone capture task (runs on core 0)
 *
 * Polls esp_capture for Opus frames and enqueues them for the
 * webrtc_task. esp_capture handles I2S capture + Opus encoding
 * internally in its own worker threads.
 * ---------------------------------------------------------------- */
static void microphone_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "Microphone task started on core %d", xPortGetCoreID());

    for (;;) {
        uint8_t *opus_data = NULL;
        size_t opus_len = 0;
        uint32_t pts_ms = 0;

        if (microphone_acquire_frame(&opus_data, &opus_len, &pts_ms) != 0) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        if (s_connected && s_mic_mid >= 0) {
            uint8_t *copy = malloc(opus_len);
            if (copy) {
                memcpy(copy, opus_data, opus_len);
                mic_msg_t msg = {
                    .data = copy,
                    .len = opus_len,
                    .pts_ms = pts_ms,
                };
                if (xQueueSend(s_mic_queue, &msg, 0) != pdTRUE) {
                    free(copy);
                }
            }
        }

        microphone_release_frame();
    }
}

/* ----------------------------------------------------------------
 * WebRTC event loop task (runs on core 0)
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

            /* Send pending video frames */
            frame_msg_t msg;
            while (xQueueReceive(s_frame_queue, &msg, 0) == pdTRUE) {
                if (s_connected) {
                    nanortc_send_video(&s_rtc, (uint8_t)s_video_mid,
                                       msg.pts_ms, msg.data, msg.len);
                }
                heap_caps_free(msg.data);
            }

            /* Send pending microphone frames */
            mic_msg_t mic_msg;
            while (xQueueReceive(s_mic_queue, &mic_msg, 0) == pdTRUE) {
                if (s_connected && s_mic_mid >= 0) {
                    nanortc_send_audio(&s_rtc, (uint8_t)s_mic_mid,
                                       mic_msg.pts_ms, mic_msg.data, mic_msg.len);
                }
                free(mic_msg.data);
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
    ESP_LOGI(TAG, "nanortc ESP32-P4 Camera — live H264 + Opus stream, %dx%d @%dfps",
             CONFIG_EXAMPLE_VIDEO_WIDTH, CONFIG_EXAMPLE_VIDEO_HEIGHT,
             CONFIG_EXAMPLE_VIDEO_FPS);

    /* 1. NVS init */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* 2. Network init */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(example_connect());

    esp_netif_t *netif = get_example_netif();
    if (!netif) {
        ESP_LOGE(TAG, "Failed to get network interface");
        return;
    }
    esp_netif_ip_info_t ip_info;
    ESP_ERROR_CHECK(esp_netif_get_ip_info(netif, &ip_info));
    esp_ip4addr_ntoa(&ip_info.ip, s_local_ip, sizeof(s_local_ip));
    ESP_LOGI(TAG, "Station IP: %s", s_local_ip);

    /* 3. Board hardware init via esp_board_manager
     *    Handles I2C, I2S, ES8311, XCLK, MIPI LDO, and esp_video
     *    based on boards/esp32_p4_nano/ YAML configuration. */
    esp_board_periph_init(ESP_BOARD_PERIPH_NAME_LDO_MIPI);
    if (esp_board_device_init(ESP_BOARD_DEVICE_NAME_CAMERA) != ESP_OK) {
        ESP_LOGE(TAG, "Board camera device init failed");
        return;
    }
    if (esp_board_device_init(ESP_BOARD_DEVICE_NAME_AUDIO_ADC) != ESP_OK) {
        ESP_LOGW(TAG, "Board audio ADC init failed — video-only mode");
    }

    /* 4. Camera V4L2 setup (board manager already initialized the sensor) */
    if (camera_init(CONFIG_EXAMPLE_VIDEO_WIDTH, CONFIG_EXAMPLE_VIDEO_HEIGHT,
                    CONFIG_EXAMPLE_VIDEO_FPS) != 0) {
        ESP_LOGE(TAG, "Camera init failed");
        return;
    }

    /* 5. H264 encoder */
    if (encoder_init(CONFIG_EXAMPLE_VIDEO_WIDTH, CONFIG_EXAMPLE_VIDEO_HEIGHT,
                     CONFIG_EXAMPLE_VIDEO_FPS, CONFIG_EXAMPLE_H264_GOP,
                     CONFIG_EXAMPLE_H264_BITRATE_KBPS) != 0) {
        ESP_LOGE(TAG, "Encoder init failed");
        return;
    }

    /* 6. Microphone (esp_capture Opus pipeline, codec from board manager) */
    if (microphone_init(48000) != 0) {
        ESP_LOGW(TAG, "Microphone init failed — video-only mode");
    } else {
        microphone_start();
    }

    /* 7. Frame queues */
    s_frame_queue = xQueueCreate(FRAME_QUEUE_DEPTH, sizeof(frame_msg_t));
    s_mic_queue = xQueueCreate(MIC_QUEUE_DEPTH, sizeof(mic_msg_t));
    if (!s_frame_queue || !s_mic_queue) {
        ESP_LOGE(TAG, "Failed to create queues");
        return;
    }

    /* 8. Run loop state (not started until POST /offer) */
    memset(&s_loop, 0, sizeof(s_loop));
    s_loop.fd = -1;

    /* 9. HTTP server */
    nano_webserver_config_t wscfg;
    memset(&wscfg, 0, sizeof(wscfg));
    wscfg.html_start = index_html_start;
    wscfg.html_end = index_html_end;
    wscfg.offer_handler = handle_offer;
    wscfg.tag = TAG;

    httpd_handle_t server = nano_webserver_start(&wscfg);
    if (!server)
        return;

    httpd_uri_t uri_debug = {
        .uri = "/debug",
        .method = HTTP_GET,
        .handler = http_get_debug,
    };
    httpd_register_uri_handler(server, &uri_debug);

    /* 10. Start tasks */
    xTaskCreatePinnedToCore(webrtc_task, "webrtc", 6 * 1024, NULL, 5, &s_webrtc_handle, 0);
    xTaskCreatePinnedToCore(microphone_task, "mic", 3 * 1024, NULL, 7, &s_mic_handle, 0);
    xTaskCreatePinnedToCore(camera_task, "camera", 4 * 1024, NULL, 6, &s_camera_handle, 1);

    ESP_LOGI(TAG, "Open http://%s/ in your browser", s_local_ip);
}
