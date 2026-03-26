/*
 * nanortc — SRTP encryption/decryption internal interface
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef NANO_SRTP_H_
#define NANO_SRTP_H_

#include <stdint.h>
#include <stddef.h>

typedef struct nano_srtp {
    uint8_t send_key[16];
    uint8_t send_salt[14];
    uint8_t recv_key[16];
    uint8_t recv_salt[14];
    uint32_t roc; /* rollover counter */
} nano_srtp_t;

int srtp_init(nano_srtp_t *srtp);
int srtp_derive_keys(nano_srtp_t *srtp, const uint8_t *keying_material, size_t len);
int srtp_protect(nano_srtp_t *srtp, uint8_t *packet, size_t len, size_t *out_len);
int srtp_unprotect(nano_srtp_t *srtp, uint8_t *packet, size_t len, size_t *out_len);

#endif /* NANO_SRTP_H_ */
