/*
 * nanortc ESP32-P4 Camera example — H264 hardware encoder
 *
 * Wraps the ESP32-P4 hardware H264 encoder (esp_h264) with a
 * simple encode-one-frame interface.
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize the ESP32-P4 hardware H264 encoder.
 *
 * @param width        Frame width in pixels.
 * @param height       Frame height in pixels.
 * @param fps          Target frame rate.
 * @param gop          GOP size (IDR every @p gop frames).
 * @param bitrate_kbps Target bitrate in kilobits/sec.
 * @return 0 on success, negative on error.
 */
int encoder_init(uint16_t width, uint16_t height, uint8_t fps,
                 uint8_t gop, uint32_t bitrate_kbps);

/**
 * Encode one YUV420 frame to H264 Annex-B.
 *
 * The output buffer is internal and valid until the next encode call.
 *
 * @param yuv          Raw YUV420 frame data.
 * @param yuv_len      Length of @p yuv in bytes.
 * @param[out] h264_out  Pointer to encoded H264 Annex-B data.
 * @param[out] out_len   Length of encoded data.
 * @param[out] is_keyframe  True if the frame is an IDR keyframe.
 * @return 0 on success, negative on error.
 */
int encoder_encode(const uint8_t *yuv, size_t yuv_len,
                   uint8_t **h264_out, size_t *out_len, bool *is_keyframe);

/**
 * Force the next encoded frame to be an IDR keyframe.
 * Achieved by closing and re-opening the encoder to reset the GOP counter.
 */
void encoder_request_keyframe(void);

/**
 * Update the target bitrate (takes effect at next encode).
 *
 * @param kbps New bitrate in kilobits/sec.
 */
void encoder_set_bitrate(uint32_t kbps);

/** Release encoder resources. */
void encoder_deinit(void);

#ifdef __cplusplus
}
#endif
