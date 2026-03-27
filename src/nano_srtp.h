/*
 * nanortc — SRTP encryption/decryption internal interface (RFC 3711)
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef NANO_SRTP_H_
#define NANO_SRTP_H_

#include <stdint.h>
#include <stddef.h>

/* Forward declaration for crypto provider */
#ifndef NANO_CRYPTO_PROVIDER_T_DECLARED
#define NANO_CRYPTO_PROVIDER_T_DECLARED
typedef struct nano_crypto_provider nano_crypto_provider_t;
#endif

/* SRTP key/salt sizes per RFC 3711, AES-128-CM-HMAC-SHA1-80 */
#define NANO_SRTP_KEY_SIZE      16 /* AES-128 cipher key */
#define NANO_SRTP_AUTH_KEY_SIZE 20 /* HMAC-SHA1 auth key */
#define NANO_SRTP_SALT_SIZE     14 /* Session salt */
#define NANO_SRTP_AUTH_TAG_SIZE 10 /* HMAC-SHA1-80 tag */

typedef struct nano_srtp {
    /* RTP session keys */
    uint8_t send_key[NANO_SRTP_KEY_SIZE];
    uint8_t send_auth_key[NANO_SRTP_AUTH_KEY_SIZE];
    uint8_t send_salt[NANO_SRTP_SALT_SIZE];
    uint8_t recv_key[NANO_SRTP_KEY_SIZE];
    uint8_t recv_auth_key[NANO_SRTP_AUTH_KEY_SIZE];
    uint8_t recv_salt[NANO_SRTP_SALT_SIZE];

    /* RTCP session keys */
    uint8_t send_rtcp_key[NANO_SRTP_KEY_SIZE];
    uint8_t send_rtcp_auth_key[NANO_SRTP_AUTH_KEY_SIZE];
    uint8_t send_rtcp_salt[NANO_SRTP_SALT_SIZE];
    uint8_t recv_rtcp_key[NANO_SRTP_KEY_SIZE];
    uint8_t recv_rtcp_auth_key[NANO_SRTP_AUTH_KEY_SIZE];
    uint8_t recv_rtcp_salt[NANO_SRTP_SALT_SIZE];

    /* State */
    uint32_t send_roc;         /* Send rollover counter */
    uint32_t recv_roc;         /* Receive rollover counter */
    uint16_t send_seq;         /* Highest sent sequence number */
    uint16_t recv_seq_max;     /* Highest received sequence number */
    uint32_t srtcp_send_index; /* SRTCP index counter */

    /* References */
    const nano_crypto_provider_t *crypto;
    int is_client; /* DTLS client role (determines key direction) */
    int ready;     /* Keys derived and ready for use */
} nano_srtp_t;

int srtp_init(nano_srtp_t *srtp, const nano_crypto_provider_t *crypto, int is_client);
int srtp_derive_keys(nano_srtp_t *srtp, const uint8_t *keying_material, size_t len);
int srtp_protect(nano_srtp_t *srtp, uint8_t *packet, size_t len, size_t *out_len);
int srtp_unprotect(nano_srtp_t *srtp, uint8_t *packet, size_t len, size_t *out_len);

#endif /* NANO_SRTP_H_ */
