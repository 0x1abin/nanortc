/*
 * nanortc — Media track abstraction
 *
 * SPDX-License-Identifier: MIT
 */

#include "nano_media.h"
#include "nanortc.h"
#include <string.h>

int track_init(nanortc_track_t *m, uint8_t mid, nanortc_track_kind_t kind,
               nanortc_direction_t direction, uint8_t codec, uint32_t sample_rate, uint8_t channels,
               uint32_t jitter_depth_ms)
{
    if (!m) {
        return NANORTC_ERR_INVALID_PARAM;
    }

    memset(m, 0, sizeof(*m));
    m->mid = mid;
    m->kind = kind;
    m->direction = direction;
    m->active = true;
    m->codec = codec;
    m->sample_rate = sample_rate;
    m->channels = channels;

    rtp_init(&m->rtp, 0, 0);
    rtcp_init(&m->rtcp, 0);

#if NANORTC_FEATURE_AUDIO
    if (kind == NANORTC_TRACK_AUDIO) {
        jitter_init(&m->track.audio.jitter, jitter_depth_ms);
        m->track.audio.jitter_depth_ms = jitter_depth_ms;
    }
#endif

#if NANORTC_FEATURE_VIDEO
    if (kind == NANORTC_TRACK_VIDEO) {
        h264_depkt_init(&m->track.video.h264_depkt);
    }
#endif

    (void)jitter_depth_ms;
    return NANORTC_OK;
}

nanortc_track_t *track_find_by_mid(nanortc_track_t *media, uint8_t media_count, uint8_t mid)
{
    if (!media) {
        return NULL;
    }
    for (uint8_t i = 0; i < media_count; i++) {
        if (media[i].active && media[i].mid == mid) {
            return &media[i];
        }
    }
    return NULL;
}

int ssrc_map_register(nanortc_ssrc_entry_t *map, uint8_t map_size, uint32_t ssrc, uint8_t mid)
{
    if (!map) {
        return NANORTC_ERR_INVALID_PARAM;
    }

    /* Check if already registered */
    for (uint8_t i = 0; i < map_size; i++) {
        if (map[i].occupied && map[i].ssrc == ssrc) {
            map[i].mid = mid;
            return NANORTC_OK;
        }
    }

    /* Find free slot */
    for (uint8_t i = 0; i < map_size; i++) {
        if (!map[i].occupied) {
            map[i].ssrc = ssrc;
            map[i].mid = mid;
            map[i].occupied = true;
            return NANORTC_OK;
        }
    }

    return NANORTC_ERR_BUFFER_TOO_SMALL;
}

int ssrc_map_lookup(const nanortc_ssrc_entry_t *map, uint8_t map_size, uint32_t ssrc)
{
    if (!map) {
        return -1;
    }
    for (uint8_t i = 0; i < map_size; i++) {
        if (map[i].occupied && map[i].ssrc == ssrc) {
            return (int)map[i].mid;
        }
    }
    return -1;
}

/* ----------------------------------------------------------------
 * Rate window: reports the previous completed second. Integer-only.
 *
 * Rolling policy: when (now_ms - bucket_start_ms) >= 1000, freeze the
 * current bucket into prev_*, reset the current counters, and rebase
 * bucket_start_ms to now_ms. This means rates update once per second
 * and are accurate to within ~1 second of the actual traffic.
 * ---------------------------------------------------------------- */

void rate_window_roll(nano_rate_window_t *w, uint32_t now_ms)
{
    if (!w) {
        return;
    }
    /* First use: initialise the bucket epoch so we don't report a huge
     * rate on the first call just because bucket_start_ms is zero. */
    if (w->bucket_start_ms == 0) {
        w->bucket_start_ms = now_ms ? now_ms : 1;
        return;
    }
    uint32_t elapsed = now_ms - w->bucket_start_ms;
    if (elapsed >= 1000) {
        /* Bits/s = bytes * 8. At <= 2 Mbps × 1s the product fits in 32 bits. */
        w->prev_bps = w->cur_bytes * 8u;
        /* Q8.8 fixed point: fps × 256. 30 fps → 7680. uint16 holds up to 255.99 fps. */
        uint32_t fps_q8 = w->cur_frames * 256u;
        w->prev_fps_q8 = (fps_q8 > 0xFFFFu) ? 0xFFFFu : (uint16_t)fps_q8;
        w->cur_frames = 0;
        w->cur_bytes = 0;
        w->bucket_start_ms = now_ms;
    }
}

void rate_window_on_frame(nano_rate_window_t *w, uint32_t now_ms)
{
    if (!w) {
        return;
    }
    rate_window_roll(w, now_ms);
    w->cur_frames++;
}

void rate_window_on_bytes(nano_rate_window_t *w, uint32_t now_ms, uint32_t nbytes)
{
    if (!w) {
        return;
    }
    rate_window_roll(w, now_ms);
    w->cur_bytes += nbytes;
}
