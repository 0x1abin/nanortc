/*
 * capture.h — V4L2 camera capture + H.264/H.265 encoding interface
 *
 * Backend-agnostic API for capturing video from a V4L2 device and
 * encoding it to H.264 or H.265 Annex-B. Two implementations are
 * provided (CMake selects one):
 *
 *   - capture_ffmpeg.c     (FFmpeg libav*; libx264 / NVENC / h264_rkmpp)
 *   - capture_gstreamer.c  (GStreamer; Rockchip MPP mpph264enc, opt-in)
 *
 * The camera is captured as MJPEG (default) or raw YUYV, decoded,
 * colour-converted to NV12 and handed to the encoder named in
 * capture_config_t.encoder. Encoded Annex-B access units are
 * delivered through the capture_encoder_cb callback on the capture
 * thread. The application must copy the buffer before returning.
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
 * @brief Encoded video frame callback.
 *
 * Called from the capture/encoding thread for every encoded access unit.
 * The @p annex_b buffer is valid only for the duration of the call.
 *
 * @param ctx          Opaque pointer from capture_config_t.userdata.
 * @param annex_b      H.264/H.265 Annex-B byte stream (NAL start codes + data).
 * @param len          Length of @p annex_b in bytes.
 * @param pts_ms       Presentation timestamp in milliseconds.
 * @param is_keyframe  True if the access unit contains an IDR/IRAP.
 */
typedef void (*capture_encoder_cb)(void *ctx, const uint8_t *annex_b, size_t len, uint32_t pts_ms,
                                   bool is_keyframe);

/**
 * @brief Check if H.264 Annex-B data contains an IDR keyframe (NAL type 5, 7, or 8).
 */
static inline bool capture_annex_b_is_keyframe_h264(const uint8_t *data, size_t len)
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
            if (nal_type == 5 || nal_type == 7 || nal_type == 8)
                return true;
            i += sc + 1;
        } else {
            i++;
        }
    }
    return false;
}

/**
 * @brief Check if H.265 Annex-B data contains an IRAP keyframe.
 *
 * H.265 NAL type lives in bits 6..1 of the first NAL header byte
 * ((byte >> 1) & 0x3F). IRAP pictures (BLA/IDR/CRA) are types 16..23;
 * VPS/SPS/PPS (32..34) also mark a parameter-set-carrying keyframe AU.
 */
static inline bool capture_annex_b_is_keyframe_h265(const uint8_t *data, size_t len)
{
    size_t i = 0;
    while (i + 4 < len) {
        size_t sc = 0;
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 0 && data[i + 3] == 1)
            sc = 4;
        else if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1)
            sc = 3;
        if (sc > 0) {
            uint8_t nal_type = (uint8_t)((data[i + sc] >> 1) & 0x3f);
            if ((nal_type >= 16 && nal_type <= 23) || (nal_type >= 32 && nal_type <= 34))
                return true;
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
    const char *device;          /**< V4L2 device node, e.g. "/dev/video0". */
    int width;                   /**< Capture width in pixels. */
    int height;                  /**< Capture height in pixels. */
    int fps;                     /**< Capture frame rate. */
    int bitrate_bps;             /**< Target bitrate in bits/s. */
    int keyframe_interval_s;     /**< Max seconds between forced keyframes. */
    const char *encoder;         /**< FFmpeg encoder name:
                                      "hevc_nvenc" (default, H.265 GPU),
                                      "h264_nvenc" (H.264 GPU),
                                      "libx264" (software fallback). */
    const char *input_format;    /**< V4L2 pixel format: "mjpeg" (default) or
                                      "yuyv422". MJPEG is required for many
                                      USB3 4K cameras at >=720p. */
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

/**
 * @brief Update the encoder target bitrate at runtime (bits/s).
 *
 * Safe to call from any thread after @ref capture_start. Intended for
 * BWE-driven adaptation: the caller computes a new target from RTCP
 * feedback and this function stages it; the capture thread applies it
 * (in-place NVENC reconfigure) just before the next encode, avoiding a
 * cross-thread race on the encoder context.
 *
 * Returns 0 on success, -1 if the encoder is not running or @p bps <= 0.
 */
int capture_set_bitrate(int bps);

/**
 * @brief Update the encoder output resolution + frame rate at runtime.
 *
 * Intended for adaptive spec control (Phase 14): the SDK's rate controller
 * recommends a capability-ladder rung {width, height, fps, bitrate} and the
 * application applies the geometry here and the bitrate via
 * @ref capture_set_bitrate.
 *
 * Like @ref capture_set_bitrate, the change is staged and applied by the
 * capture thread before the next encode (no cross-thread race). The FFmpeg
 * backend reopens the encoder at the new geometry (downscaling from the fixed
 * capture resolution via swscale) and drops input frames to hit @p fps, then
 * forces an IDR so the receiver re-syncs on the new in-band parameter sets.
 *
 * @p width / @p height must be even and <= the capture resolution; @p fps must
 * be in 1..capture-fps. The GStreamer backend bakes caps at launch and returns
 * -1 (unsupported) — bitrate adaptation still applies.
 *
 * Returns 0 on success, -1 if the encoder is not running, the backend does not
 * support runtime layout changes, or the parameters are out of range.
 */
int capture_set_layout(int width, int height, int fps);

/**
 * @brief Fetch the H.265 VPS/SPS/PPS parameter sets parsed from the
 *        encoder, as RAW NAL units (no Annex-B start code).
 *
 * Valid after @ref capture_start for an H.265 encoder. The returned
 * pointers reference module-owned storage that lives for the capture
 * session; do not free them. Pass them straight to
 * nanortc_video_set_h265_parameter_sets().
 *
 * @return 0 on success, -1 if unavailable (H.264 encoder, or parameter
 *         sets not yet produced).
 */
int capture_get_h265_parameter_sets(const uint8_t **vps, size_t *vps_len, const uint8_t **sps,
                                    size_t *sps_len, const uint8_t **pps, size_t *pps_len);

#ifdef __cplusplus
}
#endif

#endif /* CAPTURE_H_ */
