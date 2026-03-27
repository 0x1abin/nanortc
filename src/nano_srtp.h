/*
 * nanortc — SRTP encryption/decryption internal interface
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef NANO_SRTP_H_
#define NANO_SRTP_H_

#include <stdint.h>
#include <stddef.h>

/* SRTP key/salt sizes (RFC 3711, AES-128-CM) */
#define NANO_SRTP_KEY_SIZE  16 /* RFC 3711 AES-128 key */
#define NANO_SRTP_SALT_SIZE 14 /* RFC 3711 session salt */

typedef struct nano_srtp {
    uint8_t send_key[NANO_SRTP_KEY_SIZE];
    uint8_t send_salt[NANO_SRTP_SALT_SIZE];
    uint8_t recv_key[NANO_SRTP_KEY_SIZE];
    uint8_t recv_salt[NANO_SRTP_SALT_SIZE];
    uint32_t roc; /* rollover counter */
} nano_srtp_t;

int srtp_init(nano_srtp_t *srtp);
int srtp_derive_keys(nano_srtp_t *srtp, const uint8_t *keying_material, size_t len);
int srtp_protect(nano_srtp_t *srtp, uint8_t *packet, size_t len, size_t *out_len);
int srtp_unprotect(nano_srtp_t *srtp, uint8_t *packet, size_t len, size_t *out_len);

#endif /* NANO_SRTP_H_ */
