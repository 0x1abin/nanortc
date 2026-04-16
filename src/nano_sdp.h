/*
 * nanortc — SDP parser/generator internal interface (RFC 8866)
 * @internal Not part of the public API.
 *
 * Reference: RFC 8829 (WebRTC SDP).
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef NANORTC_SDP_H_
#define NANORTC_SDP_H_

#include "nanortc_config.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* DTLS setup role */
typedef enum {
    NANORTC_SDP_SETUP_ACTPASS, /* offerer default */
    NANORTC_SDP_SETUP_ACTIVE,  /* DTLS client */
    NANORTC_SDP_SETUP_PASSIVE, /* DTLS server */
} nano_sdp_setup_t;

/* ICE candidate parsed from SDP a=candidate: line (RFC 8839 §5.1) */
typedef struct {
    char addr[NANORTC_IPV6_STR_SIZE]; /* IP address string */
    uint16_t port;
} nano_sdp_candidate_t;

/* m-line type constants */
#define SDP_MLINE_NONE        0
#define SDP_MLINE_APPLICATION 1
#define SDP_MLINE_AUDIO       2
#define SDP_MLINE_VIDEO       3

#if NANORTC_HAVE_MEDIA_TRANSPORT
/**
 * @brief Per-m-line state for multi-track SDP.
 *
 * Each entry corresponds to one audio or video m-line in the SDP.
 * The MID index is the universal track identifier (matches media[] in nanortc_t).
 */
typedef struct nano_sdp_mline {
    uint8_t kind;                         /**< SDP_MLINE_AUDIO or SDP_MLINE_VIDEO. */
    uint8_t mid;                          /**< MID index (position in SDP). */
    uint8_t pt;                           /**< Local payload type number. */
    uint8_t remote_pt;                    /**< First PT from remote m=line. */
    uint32_t sample_rate;                 /**< Audio sample rate (0 for video). */
    uint8_t channels;                     /**< Audio channels (0 for video). */
    uint8_t codec;                        /**< nanortc_codec_t value. */
    nanortc_direction_t direction;        /**< Local direction. */
    nanortc_direction_t remote_direction; /**< Parsed remote direction. */
    bool active;                          /**< True if this m-line slot is in use. */

    /* H264 cross-validation */
    uint8_t video_h264_rtpmap_pt; /**< PT confirmed via a=rtpmap H264 (0=not yet). */
#if NANORTC_FEATURE_H265
    /* H265 cross-validation */
    uint8_t video_h265_rtpmap_pt; /**< PT confirmed via a=rtpmap H265 (0=not yet). */
    /* Pre-formatted "sprop-vps=..;sprop-sps=..;sprop-pps=.." fmtp fragment.
     * Populated by nanortc_video_set_h265_parameter_sets(). Not zero-terminated. */
    char h265_sprop_fmtp[NANORTC_H265_SPROP_FMTP_SIZE];
    uint16_t h265_sprop_fmtp_len;
    /* H.265 profile-tier-level extracted from the VPS NAL by
     * nanortc_video_set_h265_parameter_sets(). When level_id == 0, the SDP
     * emitter falls back to the compile-time default. Safari WebRTC is
     * strict about level — a stream-SDP mismatch (e.g. emitting 3.1 while
     * the stream is 4.0) causes the HEVC decoder to drop all frames. */
    uint8_t h265_profile_id;
    uint8_t h265_tier_flag;
    uint8_t h265_level_id;
#endif
} nano_sdp_mline_t;
#endif

typedef struct nano_sdp {
    /* Parsed from remote SDP */
    char remote_ufrag[NANORTC_ICE_REMOTE_UFRAG_SIZE];
    char remote_pwd[NANORTC_ICE_REMOTE_PWD_SIZE];
    char remote_fingerprint[NANORTC_SDP_FINGERPRINT_SIZE]; /* "sha-256 AA:BB:CC:..." */
    uint16_t remote_sctp_port;
    nano_sdp_setup_t remote_setup;

    /* Remote ICE candidates embedded in SDP (RFC 8839) */
    nano_sdp_candidate_t remote_candidates[NANORTC_SDP_MAX_CANDIDATES];
    uint8_t candidate_count;

    /* Local SDP fields */
    char local_ufrag[NANORTC_ICE_UFRAG_SIZE];
    char local_pwd[NANORTC_ICE_PWD_SIZE];
    char local_fingerprint[NANORTC_SDP_FINGERPRINT_SIZE];
    uint16_t local_sctp_port;
    nano_sdp_setup_t local_setup;

    /* Local host candidates (for SDP answer/offer generation) */
    nano_sdp_candidate_t local_candidates[NANORTC_MAX_LOCAL_CANDIDATES];
    uint8_t local_candidate_count;

    /* Server-reflexive candidate (from STUN server) */
    char srflx_candidate_ip[NANORTC_IPV6_STR_SIZE];
    uint16_t srflx_candidate_port;
    bool has_srflx_candidate;

    /* Relay candidate (from TURN allocation) */
    char relay_candidate_ip[NANORTC_IPV6_STR_SIZE];
    uint16_t relay_candidate_port;
    bool has_relay_candidate;

    bool parsed;            /* true after successful parse */
    bool end_of_candidates; /* remote signaled a=end-of-candidates (RFC 8838) */

#if NANORTC_HAVE_MEDIA_TRANSPORT
    /** Media m-lines (audio + video tracks). */
    nano_sdp_mline_t mlines[NANORTC_MAX_MEDIA_TRACKS];
    uint8_t mline_count; /**< Number of active media m-lines. */
#endif

    /* DataChannel m-line (separate — not a "media track") */
    bool has_datachannel;
    uint8_t dc_mid; /**< MID index for datachannel m-line. */

    /** Total MID count (media + DC), used for MID assignment. */
    uint8_t mid_count;
} nano_sdp_t;

/** Initialize SDP state. */
int sdp_init(nano_sdp_t *sdp);

/**
 * Parse a WebRTC SDP offer/answer string.
 *
 * Extracts: ice-ufrag, ice-pwd, fingerprint, sctp-port, setup.
 *
 * @param sdp     SDP state to fill.
 * @param sdp_str SDP string (may or may not be null-terminated).
 * @param len     Length of sdp_str.
 * @return NANORTC_OK on success.
 */
int sdp_parse(nano_sdp_t *sdp, const char *sdp_str, size_t len);

/**
 * Generate a WebRTC SDP answer (or offer).
 *
 * @param sdp     SDP state (local fields must be set).
 * @param buf     Output buffer.
 * @param buf_len Buffer size.
 * @param out_len Actual output length.
 * @return NANORTC_OK on success.
 */
int sdp_generate_answer(nano_sdp_t *sdp, char *buf, size_t buf_len, size_t *out_len);

#if NANORTC_HAVE_MEDIA_TRANSPORT
/** Find an SDP m-line by MID. Returns NULL if not found. */
nano_sdp_mline_t *sdp_find_mline(nano_sdp_t *sdp, uint8_t mid);

/** Add a media m-line. Returns the MID on success, or negative error. */
int sdp_add_mline(nano_sdp_t *sdp, uint8_t kind, uint8_t codec, uint8_t pt, uint32_t sample_rate,
                  uint8_t channels, nanortc_direction_t direction);
#endif

#endif /* NANORTC_SDP_H_ */
