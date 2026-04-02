/*
 * nanortc examples — Embedded blob media frame source
 *
 * Reads frames from a binary blob embedded in flash (via EMBED_FILES).
 * The blob is produced by tools/pack_frames.py.
 *
 * Blob format (all little-endian):
 *   [uint32  frame_count]
 *   [frame_count × {uint32 offset, uint32 len}]  (offset relative to data start)
 *   [frame data bytes ...]
 *
 * Set src->blob and src->blob_len before calling nano_media_source_init().
 *
 * SPDX-License-Identifier: MIT
 */

#include "media_source.h"

#include <string.h>

#define VIDEO_FPS      25
#define VIDEO_INTERVAL (1000 / VIDEO_FPS) /* 40 ms */
#define AUDIO_INTERVAL 20                 /* 20 ms */

/* Read a uint32 little-endian from memory */
static inline uint32_t read_u32le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

int nano_media_source_init(nano_media_source_t *src, nanortc_track_type_t type,
                           const char *sample_dir)
{
    (void)sample_dir;

    if (!src || !src->blob || src->blob_len < 4) {
        return -1;
    }

    uint32_t frame_count = read_u32le(src->blob);
    uint32_t header_size = 4 + frame_count * 8;

    if (src->blob_len < header_size || frame_count == 0) {
        return -1;
    }

    src->type = type;
    src->frame_count = (int)frame_count;
    src->frame_index = 0;
    src->timestamp_ms = 0;

    switch (type) {
    case NANORTC_MEDIA_H264:
    case NANORTC_MEDIA_H265:
        src->frame_interval_ms = VIDEO_INTERVAL;
        break;
    case NANORTC_MEDIA_OPUS:
        src->frame_interval_ms = AUDIO_INTERVAL;
        break;
    }

    return 0;
}

int nano_media_source_next_frame(nano_media_source_t *src, uint8_t *buf, size_t buf_len,
                                 size_t *frame_len, uint32_t *timestamp_ms)
{
    if (!src || !buf || !frame_len || !src->blob) {
        return -1;
    }

    uint32_t count = read_u32le(src->blob);
    uint32_t header_size = 4 + count * 8;
    const uint8_t *index_base = src->blob + 4;
    const uint8_t *data_base = src->blob + header_size;

    int idx = src->frame_index;
    if (idx < 0 || idx >= (int)count) {
        idx = 0;
    }

    /* Read offset and length from index table */
    const uint8_t *entry = index_base + (uint32_t)idx * 8;
    uint32_t offset = read_u32le(entry);
    uint32_t len = read_u32le(entry + 4);

    if (len > buf_len || (header_size + offset + len) > src->blob_len) {
        return -1;
    }

    memcpy(buf, data_base + offset, len);
    *frame_len = len;

    if (timestamp_ms) {
        *timestamp_ms = src->timestamp_ms;
    }

    /* Advance */
    src->timestamp_ms += src->frame_interval_ms;
    src->frame_index++;
    if (src->frame_index >= (int)count) {
        src->frame_index = 0;
    }

    return 0;
}

void nano_media_source_reset(nano_media_source_t *src)
{
    if (!src) {
        return;
    }
    src->frame_index = 0;
    src->timestamp_ms = 0;
}

void nano_media_source_destroy(nano_media_source_t *src)
{
    (void)src;
}
