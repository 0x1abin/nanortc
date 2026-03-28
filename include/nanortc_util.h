/*
 * nanortc — Utility functions
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef NANORTC_UTIL_H_
#define NANORTC_UTIL_H_

#include <stddef.h>
#include <stdint.h>

/**
 * @brief Bounded NUL scan — safe alternative to strlen().
 *
 * C99-compatible equivalent of POSIX strnlen().
 *
 * @param s      String to scan (must not be NULL).
 * @param maxlen Maximum number of bytes to examine.
 * @return Index of the first NUL byte, or @p maxlen if none found.
 */
static inline size_t nanortc_strnlen(const char *s, size_t maxlen)
{
    size_t i = 0;
    while (i < maxlen && s[i] != '\0')
        i++;
    return i;
}

/**
 * @brief Secure zeroing — prevents compiler from optimizing away the clear.
 *
 * Use for key material, HMAC contexts, and cipher state.
 *
 * @param buf  Buffer to zero.
 * @param len  Number of bytes to clear.
 */
static inline void nanortc_memzero(void *buf, size_t len)
{
    volatile uint8_t *p = (volatile uint8_t *)buf;
    for (size_t i = 0; i < len; i++)
        p[i] = 0;
}

/**
 * @brief Safe unsigned integer parsing — replaces atoi/atol.
 *
 * Rejects empty input, leading zeros (except "0"), and overflow beyond UINT32_MAX.
 *
 * @param buf  Decimal digit string (not necessarily NUL-terminated).
 * @param len  Number of characters to parse.
 * @param out  Receives the parsed value on success.
 * @return 0 on success, -1 on error.
 */
static inline int nanortc_parse_uint32(const char *buf, size_t len, uint32_t *out)
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
