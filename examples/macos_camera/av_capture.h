/*
 * av_capture — macOS AVFoundation camera + microphone capture
 *
 * C-callable interface wrapping AVCaptureSession for real-time
 * camera video and microphone audio capture.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AV_CAPTURE_H_
#define AV_CAPTURE_H_

#include <stddef.h>
#include <stdint.h>

#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Video frame callback. Called on a dedicated dispatch queue.
 * @param ctx       User context from av_capture_config_t.
 * @param pixbuf    Pixel buffer (NV12). Valid only during callback.
 * @param pts       Presentation timestamp from the capture device.
 */
typedef void (*av_capture_video_cb)(void *ctx, CVPixelBufferRef pixbuf, CMTime pts);

/**
 * @brief Audio samples callback. Called on a dedicated dispatch queue.
 * @param ctx           User context from av_capture_config_t.
 * @param pcm           Interleaved 16-bit PCM samples.
 * @param sample_count  Number of samples (per channel).
 * @param pts           Presentation timestamp from the capture device.
 */
typedef void (*av_capture_audio_cb)(void *ctx, const int16_t *pcm, size_t sample_count, CMTime pts);

typedef struct {
    int video_width;       /**< Desired width (1280 for 720p, 1920 for 1080p). */
    int video_height;      /**< Desired height (720 for 720p, 1080 for 1080p). */
    int video_fps;         /**< Desired frame rate (30 recommended). */
    int audio_sample_rate; /**< Audio sample rate (48000 for Opus). */
    int audio_channels;    /**< Audio channels (1 = mono recommended). */
    av_capture_video_cb video_cb;
    av_capture_audio_cb audio_cb;
    void *userdata;
} av_capture_config_t;

/**
 * @brief Start camera and microphone capture.
 * @return 0 on success, -1 on failure.
 */
int av_capture_start(const av_capture_config_t *cfg);

/**
 * @brief Stop capture and release resources.
 */
void av_capture_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* AV_CAPTURE_H_ */
