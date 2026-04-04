/*
 * nanortc ESP32-P4 Camera example — camera capture module
 *
 * Initializes OV5647 via MIPI CSI and captures YUV420 frames
 * through the esp_video V4L2 interface.
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
 * Initialize the MIPI CSI camera (OV5647) and prepare V4L2 capture.
 * Does NOT start streaming — call camera_start_streaming() separately.
 *
 * @param width   Capture width in pixels.
 * @param height  Capture height in pixels.
 * @param fps     Target frame rate.
 * @return 0 on success, negative on error.
 */
int camera_init(uint16_t width, uint16_t height, uint8_t fps);

/**
 * Start V4L2 streaming. Must be called after camera_init().
 * @return 0 on success, negative on error.
 */
int camera_start_streaming(void);

/**
 * Grab one YUV420 frame (blocking — waits for camera to deliver).
 *
 * The returned buffer is owned by the V4L2 driver. The caller must
 * call camera_release_frame() when finished with the buffer.
 *
 * @param[out] buf  Pointer to frame data.
 * @param[out] len  Frame data length in bytes.
 * @return 0 on success, negative on error.
 */
int camera_grab_frame(uint8_t **buf, size_t *len);

/**
 * Return the current frame buffer to the V4L2 queue.
 * Must be called after each successful camera_grab_frame().
 */
void camera_release_frame(void);

/** Stop capture and release camera resources. */
void camera_deinit(void);

#ifdef __cplusplus
}
#endif
