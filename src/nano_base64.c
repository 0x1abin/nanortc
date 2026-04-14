/*
 * nanortc — Base64 encoder (RFC 4648 §4 standard alphabet)
 *
 * RFC 4648 §4 defines the "base64" encoding with alphabet
 *   index:  0  1  2  3 .. 25 26 27 .. 51 52 53 .. 61 62 63
 *   char :  A  B  C  D .. Z  a  b  .. z  0  1  .. 9  +  /
 *
 * Padding (§3.2): trailing '=' characters so the encoded length is a
 * multiple of 4. One '=' when the input length mod 3 == 2; two '=' when
 * mod 3 == 1.
 *
 * The implementation consumes the input 3 bytes at a time and emits 4
 * output characters per group. No dynamic allocation, no platform
 * dependency, Sans I/O compatible.
 *
 * SPDX-License-Identifier: MIT
 */

#include "nano_base64.h"
#include "nanortc.h"

#include <stddef.h>
#include <stdint.h>

/* RFC 4648 §4 standard alphabet. 65th byte is the pad character '='. */
static const char kBase64Alphabet[65] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int nano_base64_encode(const uint8_t *src, size_t src_len, char *dst, size_t dst_cap,
                       size_t *out_len)
{
    if (!out_len) {
        return NANORTC_ERR_INVALID_PARAM;
    }
    *out_len = 0;
    if (!dst || (src_len > 0 && !src)) {
        return NANORTC_ERR_INVALID_PARAM;
    }

    /* Required capacity: ceil(src_len / 3) * 4 + 1 for NUL. */
    size_t needed = ((src_len + 2) / 3) * 4 + 1;
    if (dst_cap < needed) {
        return NANORTC_ERR_BUFFER_TOO_SMALL;
    }

    size_t si = 0;
    size_t di = 0;

    /* Process full 3-byte groups → 4 output characters each. */
    while (si + 3 <= src_len) {
        uint32_t triple =
            ((uint32_t)src[si] << 16) | ((uint32_t)src[si + 1] << 8) | (uint32_t)src[si + 2];
        dst[di + 0] = kBase64Alphabet[(triple >> 18) & 0x3F];
        dst[di + 1] = kBase64Alphabet[(triple >> 12) & 0x3F];
        dst[di + 2] = kBase64Alphabet[(triple >> 6) & 0x3F];
        dst[di + 3] = kBase64Alphabet[(triple >> 0) & 0x3F];
        si += 3;
        di += 4;
    }

    /* Tail: 1 or 2 remaining bytes → pad with '=' per RFC 4648 §3.2. */
    size_t tail = src_len - si;
    if (tail == 1) {
        uint32_t triple = (uint32_t)src[si] << 16;
        dst[di + 0] = kBase64Alphabet[(triple >> 18) & 0x3F];
        dst[di + 1] = kBase64Alphabet[(triple >> 12) & 0x3F];
        dst[di + 2] = '=';
        dst[di + 3] = '=';
        di += 4;
    } else if (tail == 2) {
        uint32_t triple = ((uint32_t)src[si] << 16) | ((uint32_t)src[si + 1] << 8);
        dst[di + 0] = kBase64Alphabet[(triple >> 18) & 0x3F];
        dst[di + 1] = kBase64Alphabet[(triple >> 12) & 0x3F];
        dst[di + 2] = kBase64Alphabet[(triple >> 6) & 0x3F];
        dst[di + 3] = '=';
        di += 4;
    }

    dst[di] = '\0';
    *out_len = di;
    return NANORTC_OK;
}
