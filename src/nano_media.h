/*
 * nanortc — Media track abstraction (str0m-inspired multi-track)
 * @internal Not part of the public API.
 *
 * Each nano_media_t represents one SDP m-line / WebRTC transceiver.
 * The MID (media ID) is the universal track identifier, mapping 1:1
 * to the index in the media[] array inside nanortc_t.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef NANORTC_MEDIA_H_
#define NANORTC_MEDIA_H_

#include "nanortc_config.h"
#include "nano_rtp.h"
#include "nano_rtcp.h"

#if NANORTC_FEATURE_AUDIO
#include "nano_jitter.h"
#endif

#if NANORTC_FEATURE_VIDEO
#include "nano_h264.h"
#endif

#include <stdbool.h>
#include <stdint.h>

/** @brief Media track kind (audio or video). */
typedef enum {
    NANO_MEDIA_AUDIO = 0,
    NANO_MEDIA_VIDEO = 1,
} nano_media_kind_t;

/**
 * @brief Per-track media state.
 *
 * One instance per SDP m-line. Contains all RTP/RTCP state, codec info,
 * and (for audio) jitter buffer / (for video) depacketizer.
 */
typedef struct nano_media {
    uint8_t mid;                   /**< MID index (= position in media[] array). */
    nano_media_kind_t kind;        /**< Audio or Video. */
    nanortc_direction_t direction; /**< Negotiated direction. */
    bool active;                   /**< True if this slot is in use. */

    nano_rtp_t rtp;   /**< Per-track RTP state (SSRC, seq, PT). */
    nano_rtcp_t rtcp; /**< Per-track RTCP stats. */

    /* Codec configuration */
    uint8_t codec;        /**< nanortc_codec_t value. */
    uint32_t sample_rate; /**< Audio sample rate (0 for video). */
    uint8_t channels;     /**< Audio channels (0 for video). */

#if NANORTC_FEATURE_AUDIO
    nano_jitter_t jitter;     /**< Audio jitter buffer. */
    uint32_t jitter_depth_ms; /**< Jitter buffer depth. */
#endif

#if NANORTC_FEATURE_VIDEO
    nano_h264_depkt_t h264_depkt; /**< H.264 FU-A reassembly. */
#endif

    /** Per-track scratch buffer for RTP packing + SRTP. */
    uint8_t media_buf[NANORTC_MEDIA_BUF_SIZE];
} nano_media_t;

/**
 * @brief SSRC → MID lookup entry for RTP demuxing.
 */
typedef struct nano_ssrc_entry {
    uint32_t ssrc;
    uint8_t mid;
    bool occupied;
} nano_ssrc_entry_t;

/** Initialize a media track slot. */
int media_init(nano_media_t *m, uint8_t mid, nano_media_kind_t kind, nanortc_direction_t direction,
               uint8_t codec, uint32_t sample_rate, uint8_t channels, uint32_t jitter_depth_ms);

/** Find a media track by MID. Returns NULL if not found or inactive. */
nano_media_t *media_find_by_mid(nano_media_t *media, uint8_t media_count, uint8_t mid);

/** Register an SSRC→MID mapping. Returns 0 on success, negative on table full. */
int ssrc_map_register(nano_ssrc_entry_t *map, uint8_t map_size, uint32_t ssrc, uint8_t mid);

/** Lookup MID by SSRC. Returns mid on success, or -1 if not found. */
int ssrc_map_lookup(const nano_ssrc_entry_t *map, uint8_t map_size, uint32_t ssrc);

#endif /* NANORTC_MEDIA_H_ */
