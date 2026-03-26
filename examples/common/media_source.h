/*
 * nanortc examples — Media frame source interface
 *
 * Reads sample media frames from disk for testing.
 * Loops back to the first frame after reaching the end.
 *
 * Sample data format (from Amazon Kinesis WebRTC SDK):
 *   H.264: frame-NNNN.h264 (raw NAL with 00 00 00 01 start codes), 1500 frames, 25fps
 *   H.265: frame-NNNN.h265 (same format), 1500 frames, 25fps
 *   Opus:  sample-NNN.opus (raw Opus frames, no Ogg container), 619 frames, 20ms each
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef NANO_MEDIA_SOURCE_H_
#define NANO_MEDIA_SOURCE_H_

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NANO_MEDIA_H264,
    NANO_MEDIA_H265,
    NANO_MEDIA_OPUS,
} nano_media_type_t;

#define NANO_MEDIA_MAX_FRAME_SIZE 16384 /* 16 KB max frame */
#define NANO_MEDIA_MAX_PATH       512

typedef struct nano_media_source {
    nano_media_type_t type;
    char sample_dir[NANO_MEDIA_MAX_PATH];
    int frame_index;   /* current frame (1-based for video, 0-based for opus) */
    int frame_count;   /* total frames available */
    uint32_t frame_interval_ms; /* ms between frames */
    uint32_t timestamp_ms;      /* running timestamp */
} nano_media_source_t;

/* Initialize: point to sample data directory */
int nano_media_source_init(nano_media_source_t *src,
                           nano_media_type_t type,
                           const char *sample_dir);

/* Read next frame. Loops when end is reached.
 * Returns 0 on success, fills buf/frame_len/timestamp_ms.
 * Returns negative on error (e.g., file not found). */
int nano_media_source_next_frame(nano_media_source_t *src,
                                 uint8_t *buf, size_t buf_len,
                                 size_t *frame_len,
                                 uint32_t *timestamp_ms);

/* Reset to first frame */
void nano_media_source_reset(nano_media_source_t *src);

void nano_media_source_destroy(nano_media_source_t *src);

/* Convenience: frame interval for timing loops */
static inline uint32_t nano_media_source_interval_ms(const nano_media_source_t *src)
{
    return src ? src->frame_interval_ms : 0;
}

#ifdef __cplusplus
}
#endif

#endif /* NANO_MEDIA_SOURCE_H_ */
