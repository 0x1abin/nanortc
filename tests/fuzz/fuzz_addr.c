/*
 * Fuzz harness for address parsers — nano_addr.c
 *
 * Targets: addr_parse_ipv4(), addr_parse_ipv6(), addr_parse_auto(), addr_format()
 * Attack surface: Text-to-binary address parsing with complex IPv6 rules
 *                 (RFC 4291/5952 compression, embedded IPv4, etc.).
 *
 * SPDX-License-Identifier: MIT
 */

#include "nano_addr.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    /* Treat fuzz input as a string (may not be null-terminated — len is explicit) */
    const char *str = (const char *)data;

    /* IPv4 parse */
    uint8_t ipv4[4];
    addr_parse_ipv4(str, size, ipv4);

#if NANORTC_FEATURE_IPV6
    /* IPv6 parse */
    uint8_t ipv6[16];
    addr_parse_ipv6(str, size, ipv6);
#endif

    /* Auto-detect parse */
    uint8_t addr[16];
    uint8_t family = 0;
    addr_parse_auto(str, size, addr, &family);

    /* Format roundtrip: if auto-parse succeeded, format back */
    if (family != 0) {
        char buf[64];
        size_t out_len = 0;
        addr_format(addr, family, buf, sizeof(buf), &out_len);
    }

    return 0;
}
