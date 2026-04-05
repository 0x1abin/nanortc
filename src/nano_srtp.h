/*
 * nanortc — SRTP encryption/decryption internal interface (RFC 3711)
 * @internal Not part of the public API.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef NANORTC_SRTP_H_
#define NANORTC_SRTP_H_

#include "nanortc_config.h"
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/* Forward declaration for crypto provider */
#ifndef NANORTC_CRYPTO_PROVIDER_T_DECLARED
#define NANORTC_CRYPTO_PROVIDER_T_DECLARED
typedef struct nanortc_crypto_provider nanortc_crypto_provider_t;
#endif

/* SRTP key/salt sizes per RFC 3711, AES-128-CM-HMAC-SHA1-80 */
#define NANORTC_SRTP_KEY_SIZE      16 /* AES-128 cipher key */
#define NANORTC_SRTP_AUTH_KEY_SIZE 20 /* HMAC-SHA1 auth key */
#define NANORTC_SRTP_SALT_SIZE     14 /* Session salt */
#define NANORTC_SRTP_AUTH_TAG_SIZE 10 /* HMAC-SHA1-80 tag */

/**
 * @brief Per-SSRC ROC/seq state for SRTP replay protection (RFC 3711 §3.3).
 *
 * In BUNDLE, all tracks share session keys, but each SSRC has its own
 * rollover counter and sequence tracking.
 */
typedef struct nano_srtp_ssrc_state {
    uint32_t ssrc;
    uint32_t roc;
    uint16_t seq_max;
    bool active;
} nano_srtp_ssrc_state_t;

typedef struct nano_srtp {
    /* RTP session keys (shared across all tracks in BUNDLE) */
    uint8_t send_key[NANORTC_SRTP_KEY_SIZE];
    uint8_t send_auth_key[NANORTC_SRTP_AUTH_KEY_SIZE];
    uint8_t send_salt[NANORTC_SRTP_SALT_SIZE];
    uint8_t recv_key[NANORTC_SRTP_KEY_SIZE];
    uint8_t recv_auth_key[NANORTC_SRTP_AUTH_KEY_SIZE];
    uint8_t recv_salt[NANORTC_SRTP_SALT_SIZE];

    /* RTCP session keys */
    uint8_t send_rtcp_key[NANORTC_SRTP_KEY_SIZE];
    uint8_t send_rtcp_auth_key[NANORTC_SRTP_AUTH_KEY_SIZE];
    uint8_t send_rtcp_salt[NANORTC_SRTP_SALT_SIZE];
    uint8_t recv_rtcp_key[NANORTC_SRTP_KEY_SIZE];
    uint8_t recv_rtcp_auth_key[NANORTC_SRTP_AUTH_KEY_SIZE];
    uint8_t recv_rtcp_salt[NANORTC_SRTP_SALT_SIZE];

    /* Per-SSRC ROC/seq state (replaces single send_roc/recv_roc) */
    nano_srtp_ssrc_state_t ssrc_states[NANORTC_MAX_SSRC_MAP];
    uint32_t srtcp_send_index; /* SRTCP index counter */

    /* References */
    const nanortc_crypto_provider_t *crypto;
    int is_client; /* DTLS client role (determines key direction) */
    int ready;     /* Keys derived and ready for use */
} nano_srtp_t;

int nano_srtp_init(nano_srtp_t *srtp, const nanortc_crypto_provider_t *crypto, int is_client);
int nano_srtp_derive_keys(nano_srtp_t *srtp, const uint8_t *keying_material, size_t len);
int nano_srtp_protect(nano_srtp_t *srtp, uint8_t *packet, size_t len, size_t *out_len);
int nano_srtp_unprotect(nano_srtp_t *srtp, uint8_t *packet, size_t len, size_t *out_len);

/* SRTCP protect/unprotect (RFC 3711 §3.4) */
int nano_srtp_protect_rtcp(nano_srtp_t *srtp, uint8_t *packet, size_t len, size_t *out_len);
int nano_srtp_unprotect_rtcp(nano_srtp_t *srtp, uint8_t *packet, size_t len, size_t *out_len);

/* SRTCP overhead: 4 bytes (E-flag + index) + 10 bytes (auth tag) */
#define NANORTC_SRTCP_OVERHEAD (4 + NANORTC_SRTP_AUTH_TAG_SIZE)

#endif /* NANORTC_SRTP_H_ */
