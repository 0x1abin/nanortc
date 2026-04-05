/*
 * nanortc — Media track abstraction (str0m-inspired multi-track)
 * Per-track media state used by nanortc_t.
 *
 * Each nanortc_track_t represents one SDP m-line / WebRTC transceiver.
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
    NANORTC_TRACK_AUDIO = 0,
    NANORTC_TRACK_VIDEO = 1,
} nanortc_track_kind_t;

/**
 * @brief Per-track media state.
 *
 * One instance per SDP m-line. Contains all RTP/RTCP state, codec info,
 * and (for audio) jitter buffer / (for video) depacketizer.
 */
typedef struct nano_media {
    uint8_t mid;                   /**< MID index (= position in media[] array). */
    nanortc_track_kind_t kind;     /**< Audio or Video. */
    nanortc_direction_t direction; /**< Negotiated direction. */
    bool active;                   /**< True if this slot is in use. */

    nano_rtp_t rtp;   /**< Per-track RTP state (SSRC, seq, PT). */
    nano_rtcp_t rtcp; /**< Per-track RTCP stats. */

    /* Codec configuration */
    uint8_t codec;        /**< nanortc_codec_t value. */
    uint32_t sample_rate; /**< Audio sample rate (0 for video). */
    uint8_t channels;     /**< Audio channels (0 for video). */

#if NANORTC_HAVE_MEDIA_TRANSPORT
    /** Audio/video-specific state (union — a track is one or the other). */
    union {
#if NANORTC_FEATURE_AUDIO
        struct {
            nano_jitter_t jitter;     /**< Audio jitter buffer. */
            uint32_t jitter_depth_ms; /**< Jitter buffer depth. */
        } audio;
#endif
#if NANORTC_FEATURE_VIDEO
        struct {
            nano_h264_depkt_t h264_depkt; /**< H.264 FU-A reassembly. */
        } video;
#endif
    } track;
#endif /* NANORTC_HAVE_MEDIA_TRANSPORT */

    /** Per-track scratch buffer for RTP packing + SRTP. */
    uint8_t media_buf[NANORTC_MEDIA_BUF_SIZE];
} nanortc_track_t;

/**
 * @brief SSRC → MID lookup entry for RTP demuxing.
 */
typedef struct nano_ssrc_entry {
    uint32_t ssrc;
    uint8_t mid;
    bool occupied;
} nanortc_ssrc_entry_t;

/** Initialize a media track slot. */
int track_init(nanortc_track_t *m, uint8_t mid, nanortc_track_kind_t kind,
               nanortc_direction_t direction, uint8_t codec, uint32_t sample_rate, uint8_t channels,
               uint32_t jitter_depth_ms);

/** Find a media track by MID. Returns NULL if not found or inactive. */
nanortc_track_t *track_find_by_mid(nanortc_track_t *media, uint8_t media_count, uint8_t mid);

/** Register an SSRC→MID mapping. Returns 0 on success, negative on table full. */
int ssrc_map_register(nanortc_ssrc_entry_t *map, uint8_t map_size, uint32_t ssrc, uint8_t mid);

/** Lookup MID by SSRC. Returns mid on success, or -1 if not found. */
int ssrc_map_lookup(const nanortc_ssrc_entry_t *map, uint8_t map_size, uint32_t ssrc);

#endif /* NANORTC_MEDIA_H_ */
