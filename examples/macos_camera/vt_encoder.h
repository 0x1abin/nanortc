/*
 * vt_encoder — macOS VideoToolbox H.264 hardware encoder
 *
 * C-callable interface wrapping VTCompressionSession for real-time
 * H.264 encoding with Annex-B output format.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef VT_ENCODER_H_
#define VT_ENCODER_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Encoded frame callback. Called on the VideoToolbox callback queue.
 * @param ctx         User context from vt_encoder_config_t.
 * @param annex_b     H.264 Annex-B data (start codes + NALs, SPS/PPS prepended for keyframes).
 * @param len         Length of annex_b in bytes.
 * @param pts_ms      Presentation timestamp in milliseconds.
 * @param is_keyframe True if the frame is an IDR keyframe.
 */
typedef void (*vt_encoder_cb)(void *ctx, const uint8_t *annex_b, size_t len, uint32_t pts_ms,
                              bool is_keyframe);

typedef struct {
    int width;              /**< Frame width (1280 default). */
    int height;             /**< Frame height (720 default). */
    int fps;                /**< Frame rate (30 default). */
    int bitrate_kbps;       /**< Average bitrate in kbps (3000 default). */
    int keyframe_interval_s; /**< Max seconds between keyframes (2 default). */
    vt_encoder_cb callback;
    void *userdata;
} vt_encoder_config_t;

/**
 * @brief Initialize the VideoToolbox H.264 encoder.
 * @return 0 on success, -1 on failure.
 */
int vt_encoder_init(const vt_encoder_config_t *cfg);

/**
 * @brief Encode a video frame.
 * @param pixbuf  CVPixelBuffer from AVFoundation (NV12 format).
 * @param pts     Presentation timestamp.
 */
void vt_encoder_encode(CVPixelBufferRef pixbuf, CMTime pts);

/**
 * @brief Force the next frame to be a keyframe (for PLI response).
 */
void vt_encoder_force_keyframe(void);

/**
 * @brief Destroy the encoder and release resources.
 */
void vt_encoder_destroy(void);

#ifdef __cplusplus
}
#endif

#endif /* VT_ENCODER_H_ */
