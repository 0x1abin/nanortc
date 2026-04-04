/*
 * nanortc ESP32-P4 Camera example — microphone capture module
 *
 * Uses esp_capture framework with board-managed ES8311 codec to
 * capture audio from the onboard microphone on ESP32-P4-Nano.
 * The board manager handles I2C, I2S, and codec initialization;
 * this module only sets up the esp_capture pipeline for Opus encoding.
 *
 * SPDX-License-Identifier: MIT
 */

#include "microphone.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "esp_log.h"

#include "esp_board_device.h"
#include "esp_board_manager_defs.h"
#include "dev_audio_codec.h"
#include "esp_codec_dev.h"

#include "esp_audio_enc.h"
#include "esp_audio_enc_default.h"

#include "esp_capture.h"
#include "esp_capture_sink.h"
#include "esp_capture_defaults.h"

static const char *TAG = "microphone";

/* ----------------------------------------------------------------
 * State
 * ---------------------------------------------------------------- */
static esp_capture_handle_t s_capture;
static esp_capture_sink_handle_t s_sink;
static esp_capture_stream_frame_t s_last_frame;

/* ----------------------------------------------------------------
 * Thread scheduler: esp_capture worker threads use PSRAM stack
 * ---------------------------------------------------------------- */
static void capture_thread_scheduler(const char *name,
                                     esp_capture_thread_schedule_cfg_t *cfg)
{
    cfg->stack_size = 32 * 1024;
    cfg->priority = 7;
    cfg->core_id = 0;
    cfg->stack_in_ext = 1;
}

/* ----------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------- */

int microphone_init(uint32_t sample_rate)
{
    /* Register audio encoders before any esp_capture operations */
    esp_audio_enc_register_default();

    /* Get codec handle from board manager (already initialized in main) */
    dev_audio_codec_handles_t *codec_handle = NULL;
    esp_err_t ret = esp_board_device_get_handle(ESP_BOARD_DEVICE_NAME_AUDIO_ADC,
                                                 (void **)&codec_handle);
    if (ret != ESP_OK || codec_handle == NULL) {
        ESP_LOGE(TAG, "Failed to get audio ADC handle: %s", esp_err_to_name(ret));
        return -1;
    }

    /* Set microphone input gain (ES8311 range: 0–42 dB, 6 dB steps) */
    esp_codec_dev_set_in_gain(codec_handle->codec_dev, 42.0);

    /* Audio source from board-managed codec device */
    esp_capture_audio_dev_src_cfg_t src_cfg = {
        .record_handle = codec_handle->codec_dev,
    };
    esp_capture_audio_src_if_t *mic_src = esp_capture_new_audio_dev_src(&src_cfg);
    if (!mic_src) {
        ESP_LOGE(TAG, "Failed to create audio dev source");
        return -1;
    }

    esp_capture_set_thread_scheduler(capture_thread_scheduler);

    /* Open capture system (audio only) */
    esp_capture_cfg_t cap_cfg = {
        .sync_mode = ESP_CAPTURE_SYNC_MODE_NONE,
        .audio_src = mic_src,
        .video_src = NULL,
    };
    esp_capture_err_t err = esp_capture_open(&cap_cfg, &s_capture);
    if (err != ESP_CAPTURE_ERR_OK) {
        ESP_LOGE(TAG, "esp_capture_open failed: %d", err);
        return -1;
    }

    /* Sink: output as Opus */
    esp_capture_sink_cfg_t sink_cfg = {
        .audio_info = {
            .format_id = ESP_CAPTURE_FMT_ID_OPUS,
            .sample_rate = sample_rate,
            .channel = 1,
            .bits_per_sample = 16,
        },
    };
    err = esp_capture_sink_setup(s_capture, 0, &sink_cfg, &s_sink);
    if (err != ESP_CAPTURE_ERR_OK) {
        ESP_LOGE(TAG, "esp_capture_sink_setup failed: %d", err);
        return -1;
    }

    esp_capture_sink_disable_stream(s_sink, ESP_CAPTURE_STREAM_TYPE_VIDEO);
    esp_capture_sink_enable(s_sink, ESP_CAPTURE_RUN_MODE_ALWAYS);

    ESP_LOGI(TAG, "Microphone initialized: Opus %"PRIu32" Hz", sample_rate);
    return 0;
}

int microphone_start(void)
{
    if (!s_capture) {
        return -1;
    }
    esp_capture_err_t err = esp_capture_start(s_capture);
    if (err != ESP_CAPTURE_ERR_OK) {
        ESP_LOGE(TAG, "esp_capture_start failed: %d", err);
        return -1;
    }
    ESP_LOGI(TAG, "Microphone capture started");
    return 0;
}

int microphone_acquire_frame(uint8_t **data, size_t *len, uint32_t *pts_ms)
{
    if (!s_sink) {
        return -1;
    }
    memset(&s_last_frame, 0, sizeof(s_last_frame));
    s_last_frame.stream_type = ESP_CAPTURE_STREAM_TYPE_AUDIO;

    esp_capture_err_t err = esp_capture_sink_acquire_frame(s_sink, &s_last_frame, true);
    if (err != ESP_CAPTURE_ERR_OK) {
        return -1;
    }

    *data = s_last_frame.data;
    *len = (size_t)s_last_frame.size;
    *pts_ms = s_last_frame.pts;
    return 0;
}

void microphone_release_frame(void)
{
    if (s_sink && s_last_frame.data) {
        esp_capture_sink_release_frame(s_sink, &s_last_frame);
        s_last_frame.data = NULL;
    }
}

void microphone_stop(void)
{
    if (s_capture) {
        esp_capture_stop(s_capture);
        ESP_LOGI(TAG, "Microphone capture stopped");
    }
}

void microphone_deinit(void)
{
    if (s_capture) {
        esp_capture_stop(s_capture);
        esp_capture_close(s_capture);
        s_capture = NULL;
        s_sink = NULL;
    }
    ESP_LOGI(TAG, "Microphone deinitialized");
}
