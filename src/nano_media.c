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
        if (codec == NANORTC_CODEC_H265) {
            h265_depkt_init(&m->track.video.u.h265_depkt);
        } else {
            h264_depkt_init(&m->track.video.u.h264_depkt);
        }
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
