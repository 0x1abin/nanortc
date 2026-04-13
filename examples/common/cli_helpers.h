/*
 * nanortc examples — Minimal CLI parsing helpers
 *
 * Shared utilities for example programs that accept "host[:port]"
 * style command-line arguments. Keeps the parsing logic in one place
 * so every example doesn't need its own copy of the strrchr/memcpy
 * dance.
 *
 * Header-only (static inline) — no .c needed.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef NANORTC_CLI_HELPERS_H_
#define NANORTC_CLI_HELPERS_H_

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Parse a "host[:port]" CLI argument.
 *
 * Splits @p str at the LAST ':' to cover both `host:port` and bare
 * `host`. IPv6 literal support would require bracket syntax
 * (`[::1]:8765`) and is not currently used by any example; keep it
 * simple.
 *
 * @param str        Input string (e.g. "localhost:8765" or "example.com").
 * @param host_out   Buffer receiving the host portion (null-terminated,
 *                   truncated if the buffer is too small).
 * @param host_len   Size of @p host_out in bytes, must be > 0.
 * @param port_out   On success, receives the parsed port. LEFT UNCHANGED
 *                   when @p str contains no ':' — this lets callers
 *                   preserve a CLI default port value.
 *
 * @return 0 on success, -1 on invalid argument (NULL pointers, zero-length
 *         host buffer, or empty host).
 */
static inline int nano_parse_host_port(const char *str, char *host_out, size_t host_len,
                                       uint16_t *port_out)
{
    if (!str || !host_out || host_len == 0) {
        return -1;
    }

    const char *colon = strrchr(str, ':');
    size_t hlen;
    if (colon) {
        hlen = (size_t)(colon - str);
        if (port_out) {
            *port_out = (uint16_t)atoi(colon + 1);
        }
    } else {
        hlen = strlen(str);
    }

    if (hlen == 0) {
        return -1; /* empty host portion */
    }
    if (hlen >= host_len) {
        hlen = host_len - 1;
    }
    memcpy(host_out, str, hlen);
    host_out[hlen] = '\0';
    return 0;
}

#ifdef __cplusplus
}
#endif

#endif /* NANORTC_CLI_HELPERS_H_ */
