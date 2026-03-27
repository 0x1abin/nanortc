/*
 * nanortc — SDP parser/generator internal interface (RFC 8866)
 *
 * Reference: RFC 8829 (WebRTC SDP).
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef NANO_SDP_H_
#define NANO_SDP_H_

#include "nanortc_config.h"
#include "nanortc.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* DTLS setup role */
typedef enum {
    NANO_SDP_SETUP_ACTPASS, /* offerer default */
    NANO_SDP_SETUP_ACTIVE,  /* DTLS client */
    NANO_SDP_SETUP_PASSIVE, /* DTLS server */
} nano_sdp_setup_t;

/* ICE candidate parsed from SDP a=candidate: line (RFC 8839 §5.1) */
typedef struct {
    char addr[NANO_IPV6_STR_SIZE]; /* IP address string */
    uint16_t port;
} nano_sdp_candidate_t;

typedef struct nano_sdp {
    /* Parsed from remote SDP */
    char remote_ufrag[NANO_ICE_REMOTE_UFRAG_SIZE];
    char remote_pwd[NANO_ICE_REMOTE_PWD_SIZE];
    char remote_fingerprint[NANO_SDP_FINGERPRINT_SIZE]; /* "sha-256 AA:BB:CC:..." */
    uint16_t remote_sctp_port;
    nano_sdp_setup_t remote_setup;

    /* Remote ICE candidates embedded in SDP (RFC 8839) */
    nano_sdp_candidate_t remote_candidates[NANO_SDP_MAX_CANDIDATES];
    uint8_t candidate_count;

    /* Local SDP fields */
    char local_ufrag[NANO_ICE_UFRAG_SIZE];
    char local_pwd[NANO_ICE_PWD_SIZE];
    char local_fingerprint[NANO_SDP_FINGERPRINT_SIZE];
    uint16_t local_sctp_port;
    nano_sdp_setup_t local_setup;

    /* Local candidate (for SDP answer generation) */
    char local_candidate_ip[NANO_IPV6_STR_SIZE];
    uint16_t local_candidate_port;
    bool has_local_candidate;

    bool parsed; /* true after successful parse */
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
 * @return NANO_OK on success.
 */
int sdp_parse(nano_sdp_t *sdp, const char *sdp_str, size_t len);

/**
 * Generate a WebRTC DataChannel SDP answer.
 *
 * @param sdp     SDP state (local fields must be set).
 * @param buf     Output buffer.
 * @param buf_len Buffer size.
 * @param out_len Actual output length.
 * @return NANO_OK on success.
 */
int sdp_generate_answer(nano_sdp_t *sdp, char *buf, size_t buf_len, size_t *out_len);

#endif /* NANO_SDP_H_ */
