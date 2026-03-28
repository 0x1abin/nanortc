/*
 * nanortc examples — File-based media frame source
 *
 * Reads sample frames from disk matching the Amazon Kinesis SDK format:
 *   H.264: sample_dir/frame-NNNN.h264 (1-based, 1500 frames)
 *   H.265: sample_dir/frame-NNNN.h265 (1-based, 1500 frames)
 *   Opus:  sample_dir/sample-NNN.opus  (0-based, 619 frames)
 *
 * SPDX-License-Identifier: MIT
 */

#include "media_source.h"

#include <stdio.h>
#include <string.h>

#define H264_FRAME_COUNT 1500
#define H265_FRAME_COUNT 1500
#define OPUS_FRAME_COUNT 619

#define VIDEO_FPS        25
#define VIDEO_INTERVAL   (1000 / VIDEO_FPS)  /* 40 ms */
#define AUDIO_INTERVAL   20                   /* 20 ms */

static int read_file(const char *path, uint8_t *buf, size_t buf_len, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        return -1;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size <= 0 || (size_t)size > buf_len) {
        fclose(f);
        return -1;
    }

    size_t nread = fread(buf, 1, (size_t)size, f);
    fclose(f);

    if (nread != (size_t)size) {
        return -1;
    }

    *out_len = nread;
    return 0;
}

int nano_media_source_init(nano_media_source_t *src,
                           nano_media_type_t type,
                           const char *sample_dir)
{
    if (!src || !sample_dir) {
        return -1;
    }

    memset(src, 0, sizeof(*src));
    src->type = type;
    strncpy(src->sample_dir, sample_dir, NANORTC_MEDIA_MAX_PATH - 1);

    switch (type) {
    case NANORTC_MEDIA_H264:
        src->frame_count = H264_FRAME_COUNT;
        src->frame_interval_ms = VIDEO_INTERVAL;
        src->frame_index = 1; /* 1-based */
        break;
    case NANORTC_MEDIA_H265:
        src->frame_count = H265_FRAME_COUNT;
        src->frame_interval_ms = VIDEO_INTERVAL;
        src->frame_index = 1;
        break;
    case NANORTC_MEDIA_OPUS:
        src->frame_count = OPUS_FRAME_COUNT;
        src->frame_interval_ms = AUDIO_INTERVAL;
        src->frame_index = 0; /* 0-based */
        break;
    }

    /* Verify first frame is accessible */
    char path[NANORTC_MEDIA_MAX_PATH + 32];
    uint8_t probe[16];
    size_t probe_len;

    switch (type) {
    case NANORTC_MEDIA_H264:
        snprintf(path, sizeof(path), "%s/frame-0001.h264", sample_dir);
        break;
    case NANORTC_MEDIA_H265:
        snprintf(path, sizeof(path), "%s/frame-0001.h265", sample_dir);
        break;
    case NANORTC_MEDIA_OPUS:
        snprintf(path, sizeof(path), "%s/sample-000.opus", sample_dir);
        break;
    }

    if (read_file(path, probe, sizeof(probe), &probe_len) < 0) {
        fprintf(stderr, "media_source: cannot open %s\n", path);
        return -1;
    }

    return 0;
}

int nano_media_source_next_frame(nano_media_source_t *src,
                                 uint8_t *buf, size_t buf_len,
                                 size_t *frame_len,
                                 uint32_t *timestamp_ms)
{
    if (!src || !buf || !frame_len) {
        return -1;
    }

    char path[NANORTC_MEDIA_MAX_PATH + 32];

    switch (src->type) {
    case NANORTC_MEDIA_H264:
        snprintf(path, sizeof(path), "%s/frame-%04d.h264",
                 src->sample_dir, src->frame_index);
        break;
    case NANORTC_MEDIA_H265:
        snprintf(path, sizeof(path), "%s/frame-%04d.h265",
                 src->sample_dir, src->frame_index);
        break;
    case NANORTC_MEDIA_OPUS:
        snprintf(path, sizeof(path), "%s/sample-%03d.opus",
                 src->sample_dir, src->frame_index);
        break;
    }

    int rc = read_file(path, buf, buf_len, frame_len);
    if (rc < 0) {
        /* File not found — wrap around */
        nano_media_source_reset(src);
        return nano_media_source_next_frame(src, buf, buf_len, frame_len, timestamp_ms);
    }

    if (timestamp_ms) {
        *timestamp_ms = src->timestamp_ms;
    }

    /* Advance to next frame */
    src->timestamp_ms += src->frame_interval_ms;
    src->frame_index++;

    /* Wrap around */
    switch (src->type) {
    case NANORTC_MEDIA_H264:
    case NANORTC_MEDIA_H265:
        if (src->frame_index > src->frame_count) {
            src->frame_index = 1;
        }
        break;
    case NANORTC_MEDIA_OPUS:
        if (src->frame_index >= src->frame_count) {
            src->frame_index = 0;
        }
        break;
    }

    return 0;
}

void nano_media_source_reset(nano_media_source_t *src)
{
    if (!src) {
        return;
    }
    src->timestamp_ms = 0;
    switch (src->type) {
    case NANORTC_MEDIA_H264:
    case NANORTC_MEDIA_H265:
        src->frame_index = 1;
        break;
    case NANORTC_MEDIA_OPUS:
        src->frame_index = 0;
        break;
    }
}

void nano_media_source_destroy(nano_media_source_t *src)
{
    (void)src;
}
