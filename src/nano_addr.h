/*
 * nanortc — IP address string/binary conversion utilities
 * @internal Not part of the public API.
 *
 * Sans I/O: no platform headers, no heap allocation, explicit (buf, len) interface.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef NANORTC_ADDR_H_
#define NANORTC_ADDR_H_

#include "nanortc_config.h"

#include <stdint.h>
#include <stddef.h>

/* ---- IPv4 (always available) ---- */

/**
 * Parse an IPv4 dotted-decimal address string into 4-byte binary.
 * @param str  Address string (not necessarily NUL-terminated).
 * @param len  String length.
 * @param out  Output: 4 bytes in network order.
 * @return 0 on success, negative error code on failure.
 */
int addr_parse_ipv4(const char *str, size_t len, uint8_t out[4]);

/**
 * Format a 4-byte IPv4 address as dotted-decimal string.
 * @param addr     4 bytes in network order.
 * @param buf      Output buffer (minimum 16 bytes).
 * @param buf_len  Buffer size.
 * @param out_len  Output: actual string length excluding NUL.
 * @return 0 on success, negative error code on buffer overflow.
 */
int addr_format_ipv4(const uint8_t addr[4], char *buf, size_t buf_len, size_t *out_len);

/* ---- IPv6 (guarded by feature flag) ---- */

#if NANORTC_FEATURE_IPV6

/**
 * Parse an IPv6 address string (RFC 4291 / RFC 5952) into 16-byte binary.
 * Supports: full form, compressed (::), mixed IPv4-mapped (::ffff:a.b.c.d).
 * @param str  Address string (not necessarily NUL-terminated).
 * @param len  String length.
 * @param out  Output: 16 bytes in network order.
 * @return 0 on success, negative error code on failure.
 */
int addr_parse_ipv6(const char *str, size_t len, uint8_t out[16]);

/**
 * Format a 16-byte IPv6 address in RFC 5952 canonical form.
 * @param addr     16 bytes in network order.
 * @param buf      Output buffer (minimum NANORTC_IPV6_STR_SIZE bytes).
 * @param buf_len  Buffer size.
 * @param out_len  Output: actual string length excluding NUL.
 * @return 0 on success, negative error code on buffer overflow.
 */
int addr_format_ipv6(const uint8_t addr[16], char *buf, size_t buf_len, size_t *out_len);

#endif /* NANORTC_FEATURE_IPV6 */

/* ---- Auto-detect helpers ---- */

/**
 * Auto-detect family and parse an IP address string.
 * Detection heuristic: string containing ':' → IPv6, otherwise → IPv4.
 * @param str     Address string (not necessarily NUL-terminated).
 * @param len     String length.
 * @param out     Output: up to 16 bytes in network order.
 * @param family  Output: 4 for IPv4, 6 for IPv6.
 * @return 0 on success, NANORTC_ERR_PARSE on malformed input,
 *         NANORTC_ERR_NOT_IMPLEMENTED if IPv6 but feature disabled.
 */
int addr_parse_auto(const char *str, size_t len, uint8_t out[16], uint8_t *family);

/**
 * Format a binary IP address to string, dispatching on family.
 * @param addr     Binary address (4 or 16 bytes in network order).
 * @param family   4 for IPv4, 6 for IPv6.
 * @param buf      Output buffer.
 * @param buf_len  Buffer size.
 * @param out_len  Output: actual string length excluding NUL.
 * @return 0 on success, negative error code on failure.
 */
int addr_format(const uint8_t *addr, uint8_t family, char *buf, size_t buf_len, size_t *out_len);

#endif /* NANORTC_ADDR_H_ */
