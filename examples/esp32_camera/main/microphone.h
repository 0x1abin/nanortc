/*
 * nanortc ESP32-P4 Camera example — microphone capture module
 *
 * Uses esp_capture with ES8311 codec to capture audio from the
 * onboard microphone on ESP32-P4-Nano. The esp_capture pipeline
 * handles I2S capture and Opus encoding internally.
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize microphone capture pipeline (ES8311 + I2S + Opus encoder).
 *
 * Sets up esp_codec_dev for the ES8311 codec, creates an esp_capture
 * audio source, and configures the pipeline to output Opus frames.
 *
 * @param sample_rate  Audio sample rate in Hz (48000 for Opus).
 * @return 0 on success, negative on error.
 */
int microphone_init(uint32_t sample_rate);

/**
 * Start the microphone capture pipeline.
 * @return 0 on success, negative on error.
 */
int microphone_start(void);

/**
 * Acquire one encoded Opus frame (non-blocking).
 *
 * @param[out] data    Pointer to Opus frame data (owned by esp_capture).
 * @param[out] len     Opus frame size in bytes.
 * @param[out] pts_ms  Presentation timestamp in milliseconds.
 * @return 0 on success, negative if no frame available.
 */
int microphone_acquire_frame(uint8_t **data, size_t *len, uint32_t *pts_ms);

/**
 * Release the last acquired frame back to esp_capture.
 * Must be called after each successful microphone_acquire_frame().
 */
void microphone_release_frame(void);

/**
 * Stop the microphone capture pipeline.
 */
void microphone_stop(void);

/** Release all microphone resources. */
void microphone_deinit(void);

#ifdef __cplusplus
}
#endif
