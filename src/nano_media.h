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

#if NANORTC_FEATURE_H265
#include "nano_h265.h"
#endif

#if NANORTC_FEATURE_VIDEO && NANORTC_FEATURE_VIDEO_REORDER
#include "nano_reorder.h"
#endif

#include <stdbool.h>
#include <stdint.h>

/** @brief Media track kind (audio or video). */
typedef enum {
    NANORTC_TRACK_AUDIO = 0,
    NANORTC_TRACK_VIDEO = 1,
} nanortc_track_kind_t;

/**
 * @brief 1-second rate window for send frame / byte counting.
 *
 * Reports the rate of the *previous completed second*. This trades a
 * one-second reporting latency for trivial integer math and avoids the
 * floating-point or multi-bucket ring that a true sliding window needs.
 * Adequate for IoT monitoring / encoder-driving feedback loops.
 *
 * Both counters advance whenever bytes or frames are added; the
 * bucket is rolled when `now_ms - bucket_start_ms >= 1000`.
 */
typedef struct nano_rate_window {
    uint32_t bucket_start_ms; /**< Monotonic start of the current bucket. */
    uint32_t cur_frames;      /**< Frames added in the current bucket. */
    uint32_t cur_bytes;       /**< Bytes added in the current bucket. */
    uint32_t prev_bps;        /**< Bits-per-second from the last completed second. */
    uint16_t prev_fps_q8;     /**< Frames-per-second as Q8.8 from the last completed second. */
    bool bucket_valid;        /**< True once bucket_start_ms has been initialized. */
} nano_rate_window_t;

/** Roll the window forward if at least one second has elapsed. */
void rate_window_roll(nano_rate_window_t *w, uint32_t now_ms);

/** Record one frame sent at @p now_ms (also rolls the window). */
void rate_window_on_frame(nano_rate_window_t *w, uint32_t now_ms);

/** Record @p nbytes of payload sent at @p now_ms (also rolls the window). */
void rate_window_on_bytes(nano_rate_window_t *w, uint32_t now_ms, uint32_t nbytes);

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

    /* Outgoing send-rate + fps window used to populate stats. */
    nano_rate_window_t rate_window;

    /* Latest fraction_lost byte copied from inbound RTCP RR. */
    uint8_t fraction_lost;

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
            /** Per-codec depacketizer state (selected by `codec` field). */
            union {
                nano_h264_depkt_t h264; /**< H.264 FU-A reassembly (RFC 6184). */
#if NANORTC_FEATURE_H265
                nano_h265_depkt_t h265; /**< H.265 FU reassembly (RFC 7798). */
#endif
            } depkt;
#if NANORTC_FEATURE_VIDEO_AUTO_PLI
            /** Receive-side loss detection for auto-PLI keyframe recovery. */
            uint16_t recv_last_seq;    /**< Highest in-order RTP seq received. */
            bool recv_seq_inited;      /**< First packet seen (recv_last_seq valid). */
            bool recv_lost_pending;    /**< Forward gap seen since last delivered frame. */
            uint32_t recv_last_pli_ms; /**< Last auto-PLI emit time (debounce). */
            bool recv_last_pli_valid;  /**< True once recv_last_pli_ms has been set. */
#endif
#if NANORTC_FEATURE_VIDEO_REORDER
            /** Receive-side reorder buffer (heals reordering before depkt). */
            nano_reorder_t reorder;
#endif
#if NANORTC_FEATURE_VIDEO_NACK_RX
            /** Receiver NACK gap detection (independent of recv_last_seq). */
            uint16_t recv_nack_seq; /**< Highest seq seen for NACK gap detection. */
            bool recv_nack_inited;  /**< recv_nack_seq valid. */
#endif
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
