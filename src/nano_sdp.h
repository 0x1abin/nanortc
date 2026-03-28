/*
 * nanortc — SDP parser/generator internal interface (RFC 8866)
 *
 * Reference: RFC 8829 (WebRTC SDP).
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef NANORTC_SDP_H_
#define NANORTC_SDP_H_

#include "nanortc_config.h"
#include "nanortc.h"

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

    /* Local candidate (for SDP answer generation) */
    char local_candidate_ip[NANORTC_IPV6_STR_SIZE];
    uint16_t local_candidate_port;
    bool has_local_candidate;

    bool parsed; /* true after successful parse */

#if NANORTC_HAVE_MEDIA_TRANSPORT
    /* Audio m-line fields (parsed from remote / configured locally) */
    bool has_audio;
    uint8_t audio_pt;           /* Payload type number (e.g. 111 for Opus) */
    uint32_t audio_sample_rate; /* e.g. 48000 */
    uint8_t audio_channels;     /* e.g. 2 for stereo */
    nanortc_direction_t audio_direction;
#endif
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
 * Generate a WebRTC DataChannel SDP answer.
 *
 * @param sdp     SDP state (local fields must be set).
 * @param buf     Output buffer.
 * @param buf_len Buffer size.
 * @param out_len Actual output length.
 * @return NANORTC_OK on success.
 */
int sdp_generate_answer(nano_sdp_t *sdp, char *buf, size_t buf_len, size_t *out_len);

#endif /* NANORTC_SDP_H_ */
