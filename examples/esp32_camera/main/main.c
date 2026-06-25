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
 *   webrtc_task    (core 0, pri 8): nanortc run loop -> dequeue -> RTP send
 *
 * webrtc_task outranks the mic task: a preempted RTP dispatch loop is what
 * lets the output queue back up under multi-fragment video bursts. lwIP
 * (18) and WiFi (23) still outrank everything here.
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
#if CONFIG_EXAMPLE_CONNECT_WIFI
#include "esp_wifi.h"
#endif

#include "esp_board_device.h"
#include "esp_board_periph.h"
#include "esp_board_manager_defs.h"

#include "nanortc.h"
#include "ice_server_resolve.h"
#include "nanortc_crypto.h"
#include "run_loop.h"
#include "webserver.h"
#include "bwe_coordinator.h"

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

/* Video-path health counters (sampled via /debug + the 5 s stats line). */
static volatile uint32_t s_vid_send_err;    /* nanortc_send_video failures (after retry) */
static volatile uint32_t s_vid_send_err_kf; /* ... of which were keyframes */
static volatile uint32_t s_frame_drop;      /* frames dropped before send (queue policy) */
static volatile uint32_t s_kf_max_bytes;    /* largest IDR seen this session */

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
    bool is_keyframe;
} frame_msg_t;

#define FRAME_QUEUE_DEPTH 2
static QueueHandle_t s_frame_queue;

/* ----------------------------------------------------------------
 * Forced-IDR debounce
 *
 * PLI storms (and local frame drops) must not turn into IDR storms:
 * IDRs are exactly the frames big enough to stress the output queue,
 * so answering every PLI with a fresh IDR re-triggers the loss that
 * caused the PLI. One forced IDR per second is plenty for recovery.
 * ---------------------------------------------------------------- */
#define IDR_FORCE_MIN_INTERVAL_MS 1000
static volatile uint32_t s_last_idr_force_ms;

static void request_keyframe_debounced(void)
{
    uint32_t now = nano_get_millis();
    uint32_t last = s_last_idr_force_ms;
    if (last != 0 && (uint32_t)(now - last) < IDR_FORCE_MIN_INTERVAL_MS) {
        return;
    }
    s_last_idr_force_ms = now;
    encoder_request_keyframe();
}

/* ----------------------------------------------------------------
 * BWE → encoder closed loop
 *
 * The library's TWCC/REMB loss-based estimator only reports; driving
 * the hardware encoder's rate control is the application's job. Without
 * this loop the encoder pushes its configured bitrate into a congested
 * link forever and the stream degrades into a PLI/freeze cycle.
 * Single viewer → the aggregate estimate is just this session's.
 * ---------------------------------------------------------------- */
#define BWE_APPLY_INTERVAL_MS 1000
#define BWE_APPLY_DAMPEN_PCT  5

static bwe_coordinator_t s_bwe;

static int camera_bwe_apply(uint32_t new_bps, void *ctx)
{
    (void)ctx;
    /* encoder_set_bitrate takes kbps and multiplies by 1024 internally —
     * mirror that convention when converting down from bps. */
    encoder_set_bitrate(new_bps / 1024);
    return 0;
}

static void apply_bwe_estimate(void)
{
    uint32_t est = nanortc_get_estimated_bitrate(&s_rtc);
    if (est == 0) {
        return;
    }
    int rc = bwe_coordinator_try_apply(&s_bwe, est, 1, nano_get_millis());
    if (rc == BWE_APPLY_OK) {
        ESP_LOGI(TAG, "[bwe] encoder target -> %lu kbps", (unsigned long)(est / 1024));
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
        if (evt->ice_state == NANORTC_ICE_STATE_CONNECTED)
            ESP_LOGI(TAG, "ICE connected");
        break;

    case NANORTC_EV_CONNECTED:
        ESP_LOGI(TAG, "Connected — starting audio+video stream");
        s_connected = 1;
        s_last_idr_force_ms = nano_get_millis();
        encoder_request_keyframe();
        break;

    case NANORTC_EV_KEYFRAME_REQUEST:
        ESP_LOGI(TAG, "Keyframe requested (mid=%d)", evt->keyframe_request.mid);
        request_keyframe_debounced();
        break;

    case NANORTC_EV_BITRATE_ESTIMATE:
        ESP_LOGI(TAG, "BWE %s via %s: %lu kbps (was %lu kbps)",
                 bwe_dir_str(evt->bitrate_estimate.direction),
                 bwe_src_str(evt->bitrate_estimate.source),
                 (unsigned long)(evt->bitrate_estimate.bitrate_bps / 1024),
                 (unsigned long)(evt->bitrate_estimate.prev_bitrate_bps / 1024));
        apply_bwe_estimate();
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
 * Track setup callback — audio first, video second.
 * ---------------------------------------------------------------- */
static int setup_camera_tracks(nanortc_t *rtc, void *userdata)
{
    (void)userdata;

    s_mic_mid = nanortc_add_audio_track(rtc, NANORTC_DIR_SENDONLY, NANORTC_CODEC_OPUS, 48000, 1);
    if (s_mic_mid < 0) {
        ESP_LOGE(TAG, "nanortc_add_audio_track failed: %d", s_mic_mid);
        return s_mic_mid;
    }

    s_video_mid = nanortc_add_video_track(rtc, NANORTC_DIR_SENDONLY, NANORTC_CODEC_H264);
    if (s_video_mid < 0) {
        ESP_LOGE(TAG, "nanortc_add_video_track failed: %d", s_video_mid);
        return s_video_mid;
    }

    /* Clamp BWE to what the hardware encoder is configured to deliver and
     * seed the initial estimate, so pre-feedback packets go out at a sane
     * rate and the estimator never suggests an unreachable ceiling. */
    nanortc_set_bitrate_bounds(rtc, 300000, (uint32_t)CONFIG_EXAMPLE_H264_BITRATE_KBPS * 1024);
    nanortc_set_initial_bitrate(rtc, (uint32_t)CONFIG_EXAMPLE_H264_BITRATE_KBPS * 1024);
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

    /* Drain any leftover frames in the queues */
    frame_msg_t stale;
    while (xQueueReceive(s_frame_queue, &stale, 0) == pdTRUE) {
        heap_caps_free(stale.data);
    }
    mic_msg_t mic_stale;
    while (xQueueReceive(s_mic_queue, &mic_stale, 0) == pdTRUE) {
        free(mic_stale.data);
    }

    /* Fresh session: reset the BWE apply state (a stale applied_bps from a
     * dead session would dampen-reject the first new estimate) and the
     * per-session health counters. */
    bwe_coordinator_reset(&s_bwe);
    s_last_idr_force_ms = 0;
    s_vid_send_err = 0;
    s_vid_send_err_kf = 0;
    s_frame_drop = 0;
    s_kf_max_bytes = 0;

    nanortc_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.crypto = nanortc_crypto_mbedtls();
    cfg.role = NANORTC_ROLE_CONTROLLED;
    cfg.log.callback = nanortc_log_cb;
    cfg.log.level = NANORTC_LOG_DEBUG;

    /* ICE servers: Google STUN + PeerJS public TURN */
    static const char *stun_url = "stun:stun.l.google.com:19302";
    static const char *turn_url = "turn:eu-0.turn.peerjs.com:3478";
    static char ice_scratch[512];
    nanortc_ice_server_t ice_servers[] = {
        {.urls = &stun_url, .url_count = 1},
        {.urls = &turn_url, .url_count = 1, .username = "peerjs", .credential = "peerjsp"},
    };
    nano_resolve_ice_servers(ice_servers, 2, ice_scratch, sizeof(ice_scratch));
    cfg.ice_servers = ice_servers;
    cfg.ice_server_count = 2;

    nano_accept_offer_params_t params = {
        .rtc_cfg = &cfg,
        .track_setup = setup_camera_tracks,
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
    char buf[1024];
    /* PR-2 lifetime audit signals — exposed over HTTP so the bench operator
     * can sample them without a working serial monitor. */
    uint32_t pkt_overrun = __atomic_load_n(&s_rtc.stats_pkt_ring_overrun, __ATOMIC_RELAXED);
    uint32_t tx_full = __atomic_load_n(&s_rtc.stats_tx_queue_full, __ATOMIC_RELAXED);
#if NANORTC_FEATURE_TURN
    uint32_t wrap_drop = __atomic_load_n(&s_rtc.stats_wrap_dropped, __ATOMIC_RELAXED);
    uint32_t via_turn = __atomic_load_n(&s_rtc.stats_enqueue_via_turn, __ATOMIC_RELAXED);
    uint32_t direct = __atomic_load_n(&s_rtc.stats_enqueue_direct, __ATOMIC_RELAXED);
#else
    uint32_t wrap_drop = 0, via_turn = 0, direct = 0;
#endif
#if NANORTC_FEATURE_VIDEO && NANORTC_FEATURE_VIDEO_PACING
    /* Send-pacer health: `paced` rises with every metered fragment; a rising
     * `pace_catchup` means the link is slower than the encoder's output (the
     * pacer hit its latency cap and flushed the backlog — drop encoder bitrate). */
    uint32_t paced = __atomic_load_n(&s_rtc.stats_paced_packets, __ATOMIC_RELAXED);
    uint32_t pace_catchup = __atomic_load_n(&s_rtc.stats_pace_catchup, __ATOMIC_RELAXED);
#else
    uint32_t paced = 0, pace_catchup = 0;
#endif
    nanortc_track_stats_t vstats;
    memset(&vstats, 0, sizeof(vstats));
    if (s_connected && s_video_mid >= 0) {
        nanortc_get_track_stats(&s_rtc, (uint8_t)s_video_mid, &vstats);
    }
    int n = snprintf(buf, sizeof(buf),
                     "running=%d fd=%d connected=%d video_mid=%d mic_mid=%d\n"
                     "ice.remote_candidates=%d ice.state=%d\n"
                     "state=%d steps=%lu alive=%lu\n"
                     "lifetime out_q=%u/%u pkt_overrun=%lu wrap_drop=%lu tx_full=%lu "
                     "via_turn=%lu direct=%lu\n"
                     "video send_err=%lu (kf=%lu) frame_drop=%lu kf_max=%lu B\n"
                     "pacer paced=%lu catchup=%lu\n"
                     "sock send_retry=%lu send_drop=%lu\n"
                     "bwe est=%lu kbps applied=%lu kbps send=%lu kbps lost=%u/255\n",
                     s_loop.running, s_loop.fds[0], (int)s_connected, s_video_mid, s_mic_mid,
                     s_rtc.ice.remote_candidate_count, s_rtc.ice.state, s_rtc.state,
                     (unsigned long)s_step_count, (unsigned long)s_task_alive,
                     (unsigned)(s_rtc.out_tail - s_rtc.out_head), (unsigned)NANORTC_OUT_QUEUE_SIZE,
                     (unsigned long)pkt_overrun, (unsigned long)wrap_drop,
                     (unsigned long)tx_full, (unsigned long)via_turn, (unsigned long)direct,
                     (unsigned long)s_vid_send_err, (unsigned long)s_vid_send_err_kf,
                     (unsigned long)s_frame_drop, (unsigned long)s_kf_max_bytes,
                     (unsigned long)paced, (unsigned long)pace_catchup,
                     (unsigned long)s_loop.stats_send_retry, (unsigned long)s_loop.stats_send_drop,
                     (unsigned long)(vstats.estimated_bitrate_bps / 1024),
                     (unsigned long)(s_bwe.applied_bps / 1024),
                     (unsigned long)(vstats.send_bitrate_bps / 1024),
                     (unsigned)vstats.fraction_lost);
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
                ESP_LOGW(TAG, "[cam] encode err rc=%d len=%u err#%" PRIu32, rc, (unsigned)h264_len,
                         enc_err);
            continue;
        }
        enc_count++;
        if (enc_count <= 3 || enc_count % 100 == 0)
            ESP_LOGI(TAG, "[cam] enc#%" PRIu32 " h264=%u kf=%d", enc_count, (unsigned)h264_len,
                     is_keyframe);

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
            .is_keyframe = is_keyframe,
        };
        if (xQueueSend(s_frame_queue, &msg, 0) != pdTRUE) {
            /* Queue full — webrtc_task is behind. Drop the OLDEST frame so
             * latency stays bounded and the freshest frame ships (blocking
             * here for 50 ms only deepened the backlog). Any local drop
             * breaks the H264 reference chain, so re-anchor with a
             * debounced IDR request instead of waiting for the receiver's
             * PLI round-trip. */
            frame_msg_t old;
            if (xQueueReceive(s_frame_queue, &old, 0) == pdTRUE) {
                heap_caps_free(old.data);
                s_frame_drop++;
            }
            bool enqueued = (xQueueSend(s_frame_queue, &msg, 0) == pdTRUE);
            if (!enqueued) {
                heap_caps_free(msg.data); /* still full (race) — drop new */
                s_frame_drop++;
            }
            /* A keyframe that made it in re-anchors the chain by itself. */
            if (!enqueued || !msg.is_keyframe) {
                request_keyframe_debounced();
            }
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

    /* PR-2 lifetime audit cadence — 5 s suffices to spot drift without
     * flooding the serial console. */
    int64_t next_stats_us = 0;

    for (;;) {
        s_task_alive++;
        if (s_loop.running) {
            nano_run_loop_step(&s_loop);
            s_step_count++;

            /* Send at most ONE video frame per loop step, so every
             * multi-fragment burst is followed by a full dispatch pass.
             * Phase 10 found back-to-back sends under FreeRTOS scheduling
             * jitter are what overflow the output queue; at 30 fps
             * (33 ms/frame) one frame per ≤20 ms step still keeps up. */
            frame_msg_t msg;
            if (xQueueReceive(s_frame_queue, &msg, 0) == pdTRUE) {
                if (s_connected) {
                    int rc = nanortc_send_video(&s_rtc, (uint8_t)s_video_mid, msg.pts_ms, msg.data,
                                                msg.len);
                    if (rc == NANORTC_ERR_WOULD_BLOCK) {
                        /* Flush pending packets onto the wire and retry
                         * once. Admission is atomic: the retry either ships
                         * the whole frame or rejects it again untouched. */
                        nano_run_loop_drain(&s_loop);
                        rc = nanortc_send_video(&s_rtc, (uint8_t)s_video_mid, msg.pts_ms, msg.data,
                                                msg.len);
                    }
                    if (rc != NANORTC_OK) {
                        /* Frame dropped whole — reference chain is broken
                         * until the next IDR, so request one (debounced). */
                        s_vid_send_err++;
                        if (msg.is_keyframe) {
                            s_vid_send_err_kf++;
                        }
                        request_keyframe_debounced();
                    }
                    if (msg.is_keyframe) {
                        if (msg.len > s_kf_max_bytes) {
                            s_kf_max_bytes = msg.len;
                        }
                        /* One line answers "do IDRs fit the queue/ring":
                         * frags is the worst-case packet count, q_free the
                         * post-send queue headroom. */
                        ESP_LOGI(TAG, "IDR %u B -> %u frags rc=%d (q_free=%u/%u)",
                                 (unsigned)msg.len,
                                 (unsigned)((msg.len + NANORTC_VIDEO_MTU - 1) / NANORTC_VIDEO_MTU),
                                 rc, (unsigned)nanortc_output_free_slots(&s_rtc),
                                 (unsigned)NANORTC_OUT_QUEUE_SIZE);
                    }
                }
                heap_caps_free(msg.data);
            }

            /* Send pending microphone frames */
            mic_msg_t mic_msg;
            while (xQueueReceive(s_mic_queue, &mic_msg, 0) == pdTRUE) {
                if (s_connected && s_mic_mid >= 0) {
                    nanortc_send_audio(&s_rtc, (uint8_t)s_mic_mid, mic_msg.pts_ms, mic_msg.data,
                                       mic_msg.len);
                }
                free(mic_msg.data);
            }

            /* PR-2 audit: when streaming, dump the lifetime/queue counters
             * every 5 s. All-zero across a run is the success signal — any
             * non-zero entry pinpoints under-sized rings or caller-side
             * drain bugs before they reach the wire. */
            int64_t now_us = esp_timer_get_time();
            if (s_connected && now_us >= next_stats_us) {
                next_stats_us = now_us + 5LL * 1000LL * 1000LL;
                uint32_t pkt_overrun =
                    __atomic_load_n(&s_rtc.stats_pkt_ring_overrun, __ATOMIC_RELAXED);
                uint32_t tx_full = __atomic_load_n(&s_rtc.stats_tx_queue_full, __ATOMIC_RELAXED);

                /* Video-path health + BWE tracking. send/est diverging with
                 * fraction_lost > 0 is the encoder outrunning the link. */
                nanortc_track_stats_t vstats;
                memset(&vstats, 0, sizeof(vstats));
                if (s_video_mid >= 0) {
                    nanortc_get_track_stats(&s_rtc, (uint8_t)s_video_mid, &vstats);
                }
                ESP_LOGI(TAG,
                         "video send=%lu est=%lu applied=%lu kbps lost=%u/255 "
                         "send_err=%lu (kf=%lu) frame_drop=%lu kf_max=%lu "
                         "sock_retry=%lu sock_drop=%lu",
                         (unsigned long)(vstats.send_bitrate_bps / 1024),
                         (unsigned long)(vstats.estimated_bitrate_bps / 1024),
                         (unsigned long)(s_bwe.applied_bps / 1024), (unsigned)vstats.fraction_lost,
                         (unsigned long)s_vid_send_err, (unsigned long)s_vid_send_err_kf,
                         (unsigned long)s_frame_drop, (unsigned long)s_kf_max_bytes,
                         (unsigned long)s_loop.stats_send_retry,
                         (unsigned long)s_loop.stats_send_drop);

                /* Periodic BWE convergence: estimates that move slowly may
                 * never cross the event threshold — converge here too. */
                apply_bwe_estimate();
#if NANORTC_FEATURE_TURN
                uint32_t wrap_drop = __atomic_load_n(&s_rtc.stats_wrap_dropped, __ATOMIC_RELAXED);
                uint32_t via_turn =
                    __atomic_load_n(&s_rtc.stats_enqueue_via_turn, __ATOMIC_RELAXED);
                uint32_t direct = __atomic_load_n(&s_rtc.stats_enqueue_direct, __ATOMIC_RELAXED);
                ESP_LOGI(TAG,
                         "lifetime out_q=%u/%u pkt_overrun=%lu wrap_drop=%lu tx_full=%lu "
                         "via_turn=%lu direct=%lu",
                         (unsigned)(s_rtc.out_tail - s_rtc.out_head),
                         (unsigned)NANORTC_OUT_QUEUE_SIZE, (unsigned long)pkt_overrun,
                         (unsigned long)wrap_drop, (unsigned long)tx_full,
                         (unsigned long)via_turn, (unsigned long)direct);
#else
                ESP_LOGI(TAG, "lifetime out_q=%u/%u pkt_overrun=%lu tx_full=%lu",
                         (unsigned)(s_rtc.out_tail - s_rtc.out_head),
                         (unsigned)NANORTC_OUT_QUEUE_SIZE, (unsigned long)pkt_overrun,
                         (unsigned long)tx_full);
#endif
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
             CONFIG_EXAMPLE_VIDEO_WIDTH, CONFIG_EXAMPLE_VIDEO_HEIGHT, CONFIG_EXAMPLE_VIDEO_FPS);

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

#if CONFIG_EXAMPLE_CONNECT_WIFI
    /* Modem power-save (the default) wakes only at DTIM beacons, adding
     * periodic 100–300 ms TX latency spikes — a classic source of rhythmic
     * stutter in live video. Trade idle power for stable latency. */
    esp_wifi_set_ps(WIFI_PS_NONE);
#endif

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

    /* 8. Run loop state (not started until POST /offer) + BWE→encoder glue */
    memset(&s_loop, 0, sizeof(s_loop));
    bwe_coordinator_init(&s_bwe, BWE_APPLY_INTERVAL_MS, BWE_APPLY_DAMPEN_PCT, camera_bwe_apply,
                         NULL);

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

    /* 10. Start tasks. webrtc_task outranks the mic task (see header):
     * RTP dispatch must not be preempted mid-burst by audio capture, or
     * the output queue backs up exactly when a video frame needs it. */
    xTaskCreatePinnedToCore(webrtc_task, "webrtc", 6 * 1024, NULL, 8, &s_webrtc_handle, 0);
    xTaskCreatePinnedToCore(microphone_task, "mic", 3 * 1024, NULL, 7, &s_mic_handle, 0);
    xTaskCreatePinnedToCore(camera_task, "camera", 4 * 1024, NULL, 6, &s_camera_handle, 1);

    ESP_LOGI(TAG, "Open http://%s/ in your browser", s_local_ip);
}
