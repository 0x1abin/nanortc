/*
 * capture.h — V4L2 camera capture + H.264 encoding interface
 *
 * Backend-agnostic API for capturing video from a V4L2 device and
 * encoding it to H.264 Annex-B. Two implementations are provided:
 *
 *   - capture_gstreamer.c  (GStreamer, compile with RK3588_CAPTURE_GSTREAMER)
 *   - capture_ffmpeg.c     (FFmpeg libav*, compile with RK3588_CAPTURE_FFMPEG)
 *
 * Encoded H.264 access units are delivered through the capture_encoder_cb
 * callback on the capture thread. The application must copy the buffer
 * before returning.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef CAPTURE_H_
#define CAPTURE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Encoded H.264 frame callback.
 *
 * Called from the capture/encoding thread for every encoded access unit.
 * The @p annex_b buffer is valid only for the duration of the call.
 *
 * @param ctx          Opaque pointer from capture_config_t.userdata.
 * @param annex_b      H.264 Annex-B byte stream (NAL start codes + data).
 * @param len          Length of @p annex_b in bytes.
 * @param pts_ms       Presentation timestamp in milliseconds.
 * @param is_keyframe  True if the access unit contains an IDR.
 */
typedef void (*capture_encoder_cb)(void *ctx, const uint8_t *annex_b, size_t len,
                                   uint32_t pts_ms, bool is_keyframe);

/**
 * @brief Check if Annex-B data contains an IDR keyframe (NAL type 5, 7, or 8).
 */
static inline bool capture_annex_b_is_keyframe(const uint8_t *data, size_t len)
{
    size_t i = 0;
    while (i + 4 < len) {
        size_t sc = 0;
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 0 && data[i + 3] == 1)
            sc = 4;
        else if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1)
            sc = 3;
        if (sc > 0) {
            uint8_t nal_type = data[i + sc] & 0x1f;
            if (nal_type == 5 || nal_type == 7 || nal_type == 8) return true;
            i += sc + 1;
        } else {
            i++;
        }
    }
    return false;
}

/**
 * @brief Capture + encode configuration.
 */
typedef struct {
    const char *device;          /**< V4L2 device node, e.g. "/dev/video2". */
    int width;                   /**< Capture width in pixels. */
    int height;                  /**< Capture height in pixels. */
    int fps;                     /**< Capture frame rate. */
    int bitrate_bps;             /**< H.264 target bitrate in bits/s. */
    int keyframe_interval_s;     /**< Max seconds between forced keyframes. */
    const char *encoder;         /**< Backend-specific encoder name.
                                      GStreamer: "mpph264enc" (default), "openh264enc".
                                      FFmpeg:   "h264_rkmpp" (default), "libx264". */
    capture_encoder_cb callback; /**< Encoded-frame callback. */
    void *userdata;              /**< User pointer passed to @ref callback. */
} capture_config_t;

/**
 * @brief Start the capture + encode pipeline.
 * @return 0 on success, -1 on failure.
 */
int capture_start(const capture_config_t *cfg);

/**
 * @brief Stop the pipeline and release resources.
 */
void capture_stop(void);

/**
 * @brief Force the encoder to emit a keyframe on the next frame.
 */
void capture_force_keyframe(void);

#ifdef __cplusplus
}
#endif

#endif /* CAPTURE_H_ */
