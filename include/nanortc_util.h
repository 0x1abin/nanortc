/*
 * nanortc — Utility functions
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef NANORTC_UTIL_H_
#define NANORTC_UTIL_H_

#include <stddef.h>
#include <stdint.h>

/* Bounded NUL scan -- safe alternative to strlen() for internal use.
 * Returns the index of the first NUL byte, or maxlen if none found.
 * C99-compatible equivalent of POSIX strnlen(). */
static inline size_t nano_strnlen(const char *s, size_t maxlen)
{
    size_t i = 0;
    while (i < maxlen && s[i] != '\0')
        i++;
    return i;
}

/* Secure zeroing — prevents compiler from optimizing away the clear.
 * Use for key material, HMAC contexts, cipher state. */
static inline void nano_memzero(void *buf, size_t len)
{
    volatile uint8_t *p = (volatile uint8_t *)buf;
    for (size_t i = 0; i < len; i++)
        p[i] = 0;
}

/* Safe unsigned integer parsing — replaces atoi/atol.
 * Parses decimal string (buf, len) into *out. Returns 0 on success, -1 on error.
 * Rejects empty input, leading zeros (except "0"), overflow beyond UINT32_MAX. */
static inline int nano_parse_uint32(const char *buf, size_t len, uint32_t *out)
{
    if (!buf || len == 0 || !out)
        return -1;
    if (len > 1 && buf[0] == '0')
        return -1; /* reject leading zeros */
    uint32_t val = 0;
    for (size_t i = 0; i < len; i++) {
        if (buf[i] < '0' || buf[i] > '9')
            return -1;
        uint32_t digit = (uint32_t)(buf[i] - '0');
        if (val > (UINT32_MAX - digit) / 10)
            return -1; /* overflow */
        val = val * 10 + digit;
    }
    *out = val;
    return 0;
}

#endif /* NANORTC_UTIL_H_ */
