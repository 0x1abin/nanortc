/*
 * audio_capture.h — Microphone capture + Opus encoding interface
 *
 * Backend-agnostic API for capturing audio from a local input device
 * and encoding it to Opus (RFC 6716). One implementation is provided:
 *
 *   - audio_capture_alsa.c  (ALSA + libopus)
 *
 * Encoded Opus packets are delivered through the audio_encoder_cb
 * callback on a dedicated capture thread. The application must copy
 * the buffer before returning.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AUDIO_CAPTURE_H_
#define AUDIO_CAPTURE_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Encoded Opus frame callback.
 *
 * Called from the audio capture/encode thread for every 20 ms Opus packet.
 * The @p data buffer is valid only for the duration of the call.
 *
 * @param ctx     Opaque pointer from audio_config_t.userdata.
 * @param data    Opus packet payload (no RTP headers).
 * @param len     Length of @p data in bytes.
 * @param pts_ms  Presentation timestamp in milliseconds (capture-end).
 */
typedef void (*audio_encoder_cb)(void *ctx, const uint8_t *data, size_t len,
                                 uint32_t pts_ms);

/**
 * @brief Audio capture + encode configuration.
 */
typedef struct {
    const char *device;          /**< ALSA PCM name, e.g. "plughw:CARD=U4K,DEV=0". */
    int sample_rate;             /**< Sample rate in Hz (Opus: 48000). */
    int channels;                /**< Channel count (1=mono, 2=stereo). */
    int frame_ms;                /**< Frame duration in ms (typically 20). */
    int bitrate_bps;             /**< Opus target bitrate in bits/s. */
    audio_encoder_cb callback;   /**< Encoded-frame callback. */
    void *userdata;              /**< User pointer passed to @ref callback. */
} audio_config_t;

/**
 * @brief Start the audio capture + encode pipeline.
 * @return 0 on success, -1 on failure.
 */
int audio_start(const audio_config_t *cfg);

/**
 * @brief Stop the pipeline and release resources.
 *
 * Uses pthread_kill(SIGUSR1) to wake the blocking snd_pcm_readi(),
 * then joins the capture thread. Safe to call even if audio_start failed.
 */
void audio_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* AUDIO_CAPTURE_H_ */
