/*
 * nanortc — SDP parser/generator internal interface (RFC 8866)
 *
 * Reference: libpeer src/sdp.c (format), RFC 8829 (WebRTC SDP).
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef NANO_SDP_H_
#define NANO_SDP_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* DTLS setup role */
typedef enum {
    NANO_SDP_SETUP_ACTPASS, /* offerer default */
    NANO_SDP_SETUP_ACTIVE,  /* DTLS client */
    NANO_SDP_SETUP_PASSIVE, /* DTLS server */
} nano_sdp_setup_t;

typedef struct nano_sdp {
    /* Parsed from remote SDP */
    char remote_ufrag[32];
    char remote_pwd[128];
    char remote_fingerprint[128]; /* "sha-256 AA:BB:CC:..." */
    uint16_t remote_sctp_port;
    nano_sdp_setup_t remote_setup;

    /* Local SDP fields */
    char local_ufrag[8];
    char local_pwd[32];
    char local_fingerprint[128];
    uint16_t local_sctp_port;
    nano_sdp_setup_t local_setup;

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
int sdp_generate_answer(nano_sdp_t *sdp, char *buf, size_t buf_len,
                        size_t *out_len);

#endif /* NANO_SDP_H_ */
