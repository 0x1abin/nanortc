/*
 * nanortc — SRTP encryption/decryption (RFC 3711)
 *
 * SPDX-License-Identifier: MIT
 */

#include "nano_srtp.h"
#include <string.h>

int srtp_init(nano_srtp_t *srtp)
{
    if (!srtp) {
        return -1;
    }
    memset(srtp, 0, sizeof(*srtp));
    return 0;
}

int srtp_derive_keys(nano_srtp_t *srtp, const uint8_t *keying_material, size_t len)
{
    (void)srtp;
    (void)keying_material;
    (void)len;
    return -1;
}

int srtp_protect(nano_srtp_t *srtp, uint8_t *packet, size_t len, size_t *out_len)
{
    (void)srtp;
    (void)packet;
    (void)len;
    (void)out_len;
    return -1;
}

int srtp_unprotect(nano_srtp_t *srtp, uint8_t *packet, size_t len, size_t *out_len)
{
    (void)srtp;
    (void)packet;
    (void)len;
    (void)out_len;
    return -1;
}
