/*
 * nanortc ESP32-P4 Camera example — H264 hardware encoder
 *
 * Uses the esp_h264 component's hardware encoder on ESP32-P4.
 * Input: YUV420 frames from V4L2 camera capture.
 * Output: H264 Annex-B bitstream (start codes included).
 *
 * Reference: KVS esp_h264_hw_enc.c
 *
 * SPDX-License-Identifier: MIT
 */

#include "encoder.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_cache.h"

#include "esp_h264_enc_single.h"
#include "esp_h264_enc_single_hw.h"
#include "esp_h264_types.h"
#include "esp_h264_alloc.h"

static const char *TAG = "encoder";

typedef struct {
    esp_h264_enc_handle_t enc;
    esp_h264_enc_cfg_t cfg;
    esp_h264_enc_in_frame_t in_frame;
    esp_h264_enc_out_frame_t out_frame;
    volatile bool keyframe_requested;
    volatile bool bitrate_changed;
    uint32_t new_bitrate_bps;
} encoder_state_t;

static encoder_state_t s_enc;

static int create_encoder(void)
{
    esp_h264_err_t ret = esp_h264_enc_hw_new(&s_enc.cfg, &s_enc.enc);
    if (ret != ESP_H264_ERR_OK) {
        ESP_LOGE(TAG, "esp_h264_enc_hw_new failed: %d", ret);
        return -1;
    }

    ret = esp_h264_enc_open(s_enc.enc);
    if (ret != ESP_H264_ERR_OK) {
        ESP_LOGE(TAG, "esp_h264_enc_open failed: %d", ret);
        esp_h264_enc_del(s_enc.enc);
        s_enc.enc = NULL;
        return -1;
    }
    return 0;
}

static void destroy_encoder(void)
{
    if (s_enc.enc) {
        esp_h264_enc_close(s_enc.enc);
        esp_h264_enc_del(s_enc.enc);
        s_enc.enc = NULL;
    }
}

int encoder_init(uint16_t width, uint16_t height, uint8_t fps,
                 uint8_t gop, uint32_t bitrate_kbps)
{
    memset(&s_enc, 0, sizeof(s_enc));

    s_enc.cfg.pic_type = ESP_H264_RAW_FMT_O_UYY_E_VYY;
    s_enc.cfg.gop = gop;
    s_enc.cfg.fps = fps;
    s_enc.cfg.res.width = width;
    s_enc.cfg.res.height = height;
    s_enc.cfg.rc.bitrate = (uint32_t)bitrate_kbps * 1024;
    s_enc.cfg.rc.qp_min = 26;
    s_enc.cfg.rc.qp_max = 42;

    /* Round up to macroblock boundary (16 pixels) */
    uint16_t aligned_w = ((width + 15) >> 4) << 4;
    uint16_t aligned_h = ((height + 15) >> 4) << 4;
    /* YUV420 (O_UYY_E_VYY): 1.5 bytes per pixel */
    size_t frame_size = aligned_w * aligned_h + (aligned_w * aligned_h >> 1);

    /* Allocate output buffer using esp_h264 allocator (handles alignment + caps) */
    uint32_t actual_size = 0;
    s_enc.out_frame.raw_data.buffer = esp_h264_aligned_calloc(
        16, 1, frame_size, &actual_size, ESP_H264_MEM_SPIRAM);
    if (!s_enc.out_frame.raw_data.buffer) {
        ESP_LOGE(TAG, "Failed to allocate output buffer (%u bytes)", (unsigned)frame_size);
        return -1;
    }
    s_enc.out_frame.raw_data.len = actual_size;

    if (create_encoder() != 0) {
        esp_h264_free(s_enc.out_frame.raw_data.buffer);
        s_enc.out_frame.raw_data.buffer = NULL;
        return -1;
    }

    ESP_LOGI(TAG, "H264 HW encoder initialized: %dx%d @%dfps, GOP=%d, bitrate=%"PRIu32" kbps",
             width, height, fps, gop, bitrate_kbps);
    return 0;
}

int encoder_encode(const uint8_t *yuv, size_t yuv_len,
                   uint8_t **h264_out, size_t *out_len, bool *is_keyframe)
{
    if (!s_enc.enc)
        return -1;

    /* Handle pending keyframe request by resetting encoder */
    if (s_enc.keyframe_requested) {
        s_enc.keyframe_requested = false;
        ESP_LOGI(TAG, "Forcing IDR: resetting encoder");
        destroy_encoder();
        if (create_encoder() != 0)
            return -1;
    }

    /* Handle pending bitrate change */
    if (s_enc.bitrate_changed) {
        s_enc.bitrate_changed = false;
        esp_h264_enc_param_hw_handle_t param_hd = NULL;
        esp_h264_err_t ret = esp_h264_enc_hw_get_param_hd(s_enc.enc, &param_hd);
        if (ret == ESP_H264_ERR_OK) {
            esp_h264_enc_set_bitrate(&param_hd->base, s_enc.new_bitrate_bps);
            ESP_LOGI(TAG, "Bitrate changed to %"PRIu32" bps", s_enc.new_bitrate_bps);
        }
    }

    /* Set input frame (V4L2 mmap buffer, already in memory) */
    s_enc.in_frame.raw_data.buffer = (uint8_t *)yuv;
    s_enc.in_frame.raw_data.len = yuv_len;

    /* Encode (blocking — waits for HW completion via ISR) */
    esp_h264_err_t ret = esp_h264_enc_process(s_enc.enc, &s_enc.in_frame, &s_enc.out_frame);
    if (ret != ESP_H264_ERR_OK) {
        ESP_LOGE(TAG, "esp_h264_enc_process failed: %d", ret);
        return -1;
    }

    /* Invalidate CPU cache to read HW-written output (align to 64 bytes) */
    esp_cache_msync(s_enc.out_frame.raw_data.buffer,
                    (s_enc.out_frame.length + 63) & ~63,
                    ESP_CACHE_MSYNC_FLAG_DIR_M2C);

    *h264_out = s_enc.out_frame.raw_data.buffer;
    *out_len = s_enc.out_frame.length;
    *is_keyframe = (s_enc.out_frame.frame_type == ESP_H264_FRAME_TYPE_IDR);

    return 0;
}

void encoder_request_keyframe(void)
{
    s_enc.keyframe_requested = true;
}

void encoder_set_bitrate(uint32_t kbps)
{
    s_enc.new_bitrate_bps = kbps * 1024;
    s_enc.bitrate_changed = true;
}

void encoder_deinit(void)
{
    destroy_encoder();
    if (s_enc.out_frame.raw_data.buffer) {
        esp_h264_free(s_enc.out_frame.raw_data.buffer);
        s_enc.out_frame.raw_data.buffer = NULL;
    }
    ESP_LOGI(TAG, "Encoder deinitialized");
}
