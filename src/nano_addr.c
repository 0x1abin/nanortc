/*
 * nanortc — IP address string/binary conversion utilities
 *
 * Sans I/O: no platform headers, no heap allocation, explicit (buf, len) interface.
 * IPv4 parsing is always compiled; IPv6 guarded by NANORTC_FEATURE_IPV6.
 *
 * SPDX-License-Identifier: MIT
 */

#include "nano_addr.h"

#include <string.h>
#include <stdint.h>
#include <stddef.h>

/* Error codes (match nanortc.h without including it) */
#define ADDR_OK                0
#define ADDR_ERR_INVALID_PARAM -1
#define ADDR_ERR_BUFFER_SMALL  -2
#define ADDR_ERR_NOT_IMPL      -6
#define ADDR_ERR_PARSE         -7

/* ----------------------------------------------------------------
 * IPv4
 * ---------------------------------------------------------------- */

int addr_parse_ipv4(const char *str, size_t len, uint8_t out[4])
{
    if (!str || len == 0 || !out) {
        return ADDR_ERR_INVALID_PARAM;
    }

    int octet = 0;
    uint16_t val = 0;

    for (size_t i = 0; i <= len; i++) {
        if (i == len || str[i] == '.') {
            if (octet >= 4 || val > 255) {
                return ADDR_ERR_PARSE;
            }
            out[octet++] = (uint8_t)val;
            val = 0;
        } else if (str[i] >= '0' && str[i] <= '9') {
            val = val * 10 + (uint16_t)(str[i] - '0');
            if (val > 255) {
                return ADDR_ERR_PARSE;
            }
        } else {
            return ADDR_ERR_PARSE;
        }
    }

    if (octet != 4) {
        return ADDR_ERR_PARSE;
    }
    return ADDR_OK;
}

int addr_format_ipv4(const uint8_t addr[4], char *buf, size_t buf_len, size_t *out_len)
{
    if (!addr || !buf || !out_len) {
        return ADDR_ERR_INVALID_PARAM;
    }

    /* Max: "255.255.255.255" = 15 chars + NUL */
    size_t pos = 0;
    for (int i = 0; i < 4; i++) {
        if (i > 0) {
            if (pos >= buf_len) {
                return ADDR_ERR_BUFFER_SMALL;
            }
            buf[pos++] = '.';
        }
        /* Write up to 3 digits */
        uint8_t v = addr[i];
        if (v >= 100) {
            if (pos >= buf_len) {
                return ADDR_ERR_BUFFER_SMALL;
            }
            buf[pos++] = (char)('0' + v / 100);
            v %= 100;
            if (pos >= buf_len) {
                return ADDR_ERR_BUFFER_SMALL;
            }
            buf[pos++] = (char)('0' + v / 10);
            if (pos >= buf_len) {
                return ADDR_ERR_BUFFER_SMALL;
            }
            buf[pos++] = (char)('0' + v % 10);
        } else if (v >= 10) {
            if (pos >= buf_len) {
                return ADDR_ERR_BUFFER_SMALL;
            }
            buf[pos++] = (char)('0' + v / 10);
            if (pos >= buf_len) {
                return ADDR_ERR_BUFFER_SMALL;
            }
            buf[pos++] = (char)('0' + v % 10);
        } else {
            if (pos >= buf_len) {
                return ADDR_ERR_BUFFER_SMALL;
            }
            buf[pos++] = (char)('0' + v);
        }
    }

    if (pos >= buf_len) {
        return ADDR_ERR_BUFFER_SMALL;
    }
    buf[pos] = '\0';
    *out_len = pos;
    return ADDR_OK;
}

/* ----------------------------------------------------------------
 * IPv6 (RFC 4291 / RFC 5952)
 * ---------------------------------------------------------------- */

#if NANORTC_FEATURE_IPV6

/* Max groups in a full IPv6 address */
#define IPV6_MAX_GROUPS 8

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return 10 + (c - 'a');
    }
    if (c >= 'A' && c <= 'F') {
        return 10 + (c - 'A');
    }
    return -1;
}

int addr_parse_ipv6(const char *str, size_t len, uint8_t out[16])
{
    if (!str || len == 0 || !out) {
        return ADDR_ERR_INVALID_PARAM;
    }

    uint16_t groups[IPV6_MAX_GROUPS];
    int group_count = 0;
    int dcolon_pos = -1; /* position in groups[] where :: was seen */

    memset(groups, 0, sizeof(groups));
    memset(out, 0, 16);

    size_t i = 0;

    /* Handle leading :: */
    if (len >= 2 && str[0] == ':' && str[1] == ':') {
        dcolon_pos = 0;
        i = 2;
        if (i == len) {
            /* The address is just "::" (all zeros) */
            return ADDR_OK;
        }
    }

    while (i < len) {
        if (group_count >= IPV6_MAX_GROUPS) {
            return ADDR_ERR_PARSE;
        }

        /* Check for IPv4-mapped tail: last segment might be dotted-decimal.
         * Detect by scanning ahead for a '.' before the next ':' or end. */
        {
            int has_dot = 0;
            for (size_t j = i; j < len; j++) {
                if (str[j] == '.') {
                    has_dot = 1;
                    break;
                }
                if (str[j] == ':') {
                    break;
                }
            }
            if (has_dot) {
                /* Parse remaining as IPv4 into last 4 bytes → 2 groups */
                if (group_count > 6) {
                    return ADDR_ERR_PARSE; /* no room for 2 more groups */
                }
                uint8_t ipv4[4];
                int rc = addr_parse_ipv4(str + i, len - i, ipv4);
                if (rc != ADDR_OK) {
                    return rc;
                }
                groups[group_count++] = (uint16_t)((uint16_t)ipv4[0] << 8 | ipv4[1]);
                groups[group_count++] = (uint16_t)((uint16_t)ipv4[2] << 8 | ipv4[3]);
                i = len; /* consumed everything */
                break;
            }
        }

        /* Parse one hex group */
        uint32_t val = 0;
        int digits = 0;
        while (i < len && str[i] != ':') {
            int d = hex_digit(str[i]);
            if (d < 0) {
                return ADDR_ERR_PARSE;
            }
            val = (val << 4) | (uint32_t)d;
            if (val > 0xFFFF) {
                return ADDR_ERR_PARSE;
            }
            digits++;
            if (digits > 4) {
                return ADDR_ERR_PARSE;
            }
            i++;
        }

        if (digits == 0) {
            /* Empty group not at :: — invalid */
            return ADDR_ERR_PARSE;
        }

        groups[group_count++] = (uint16_t)val;

        /* Advance past ':' */
        if (i < len) {
            if (str[i] != ':') {
                return ADDR_ERR_PARSE;
            }
            i++;

            /* Check for :: */
            if (i < len && str[i] == ':') {
                if (dcolon_pos >= 0) {
                    return ADDR_ERR_PARSE; /* double :: */
                }
                dcolon_pos = group_count;
                i++;
                if (i == len) {
                    break; /* trailing :: */
                }
            } else if (i == len) {
                /* Trailing single colon — invalid */
                return ADDR_ERR_PARSE;
            }
        }
    }

    /* Expand :: into the 16-byte output */
    if (dcolon_pos >= 0) {
        int missing = IPV6_MAX_GROUPS - group_count;
        if (missing < 0) {
            return ADDR_ERR_PARSE;
        }
        /* Write groups before :: */
        for (int g = 0; g < dcolon_pos; g++) {
            out[g * 2] = (uint8_t)(groups[g] >> 8);
            out[g * 2 + 1] = (uint8_t)(groups[g] & 0xFF);
        }
        /* Zeros in the :: gap are already 0 from memset */
        /* Write groups after :: */
        int after_count = group_count - dcolon_pos;
        for (int g = 0; g < after_count; g++) {
            int out_idx = (dcolon_pos + missing + g) * 2;
            out[out_idx] = (uint8_t)(groups[dcolon_pos + g] >> 8);
            out[out_idx + 1] = (uint8_t)(groups[dcolon_pos + g] & 0xFF);
        }
    } else {
        /* No :: — must have exactly 8 groups */
        if (group_count != IPV6_MAX_GROUPS) {
            return ADDR_ERR_PARSE;
        }
        for (int g = 0; g < IPV6_MAX_GROUPS; g++) {
            out[g * 2] = (uint8_t)(groups[g] >> 8);
            out[g * 2 + 1] = (uint8_t)(groups[g] & 0xFF);
        }
    }

    return ADDR_OK;
}

int addr_format_ipv6(const uint8_t addr[16], char *buf, size_t buf_len, size_t *out_len)
{
    if (!addr || !buf || !out_len) {
        return ADDR_ERR_INVALID_PARAM;
    }

    /* Decode 8 groups from binary */
    uint16_t groups[IPV6_MAX_GROUPS];
    for (int g = 0; g < IPV6_MAX_GROUPS; g++) {
        groups[g] = (uint16_t)((uint16_t)addr[g * 2] << 8 | addr[g * 2 + 1]);
    }

    /* RFC 5952 §4.2.3: find the first longest run of consecutive zero groups */
    int best_start = -1;
    int best_len = 0;
    int cur_start = -1;
    int cur_len = 0;

    for (int g = 0; g < IPV6_MAX_GROUPS; g++) {
        if (groups[g] == 0) {
            if (cur_start < 0) {
                cur_start = g;
                cur_len = 1;
            } else {
                cur_len++;
            }
        } else {
            if (cur_len > best_len) {
                best_start = cur_start;
                best_len = cur_len;
            }
            cur_start = -1;
            cur_len = 0;
        }
    }
    if (cur_len > best_len) {
        best_start = cur_start;
        best_len = cur_len;
    }

    /* RFC 5952 §4.2.2: do not compress a single 16-bit 0 field */
    if (best_len <= 1) {
        best_start = -1;
        best_len = 0;
    }

    /* Format into buffer */
    static const char hex_chars[] = "0123456789abcdef";
    size_t pos = 0;

#define EMIT_CHAR(c)                      \
    do {                                  \
        if (pos >= buf_len)               \
            return ADDR_ERR_BUFFER_SMALL; \
        buf[pos++] = (c);                 \
    } while (0)

    for (int g = 0; g < IPV6_MAX_GROUPS;) {
        /* Handle :: compression at the start of the compressed range */
        if (g == best_start) {
            EMIT_CHAR(':');
            EMIT_CHAR(':');
            g += best_len;
            continue;
        }

        /* Separator colon between groups (not after ::) */
        if (g > 0 && g != best_start + best_len) {
            EMIT_CHAR(':');
        }

        /* Emit group without leading zeros (RFC 5952 §4.1) */
        uint16_t v = groups[g];
        if (v >= 0x1000) {
            EMIT_CHAR(hex_chars[(v >> 12) & 0xF]);
            EMIT_CHAR(hex_chars[(v >> 8) & 0xF]);
            EMIT_CHAR(hex_chars[(v >> 4) & 0xF]);
            EMIT_CHAR(hex_chars[v & 0xF]);
        } else if (v >= 0x100) {
            EMIT_CHAR(hex_chars[(v >> 8) & 0xF]);
            EMIT_CHAR(hex_chars[(v >> 4) & 0xF]);
            EMIT_CHAR(hex_chars[v & 0xF]);
        } else if (v >= 0x10) {
            EMIT_CHAR(hex_chars[(v >> 4) & 0xF]);
            EMIT_CHAR(hex_chars[v & 0xF]);
        } else {
            EMIT_CHAR(hex_chars[v & 0xF]);
        }
        g++;
    }

#undef EMIT_CHAR

    if (pos >= buf_len) {
        return ADDR_ERR_BUFFER_SMALL;
    }
    buf[pos] = '\0';
    *out_len = pos;
    return ADDR_OK;
}

#endif /* NANORTC_FEATURE_IPV6 */

/* ----------------------------------------------------------------
 * Auto-detect helpers
 * ---------------------------------------------------------------- */

int addr_parse_auto(const char *str, size_t len, uint8_t out[16], uint8_t *family)
{
    if (!str || len == 0 || !out || !family) {
        return ADDR_ERR_INVALID_PARAM;
    }

    /* Detect IPv6: any colon in the string */
    int is_v6 = 0;
    for (size_t i = 0; i < len; i++) {
        if (str[i] == ':') {
            is_v6 = 1;
            break;
        }
    }

    if (is_v6) {
#if NANORTC_FEATURE_IPV6
        *family = 6;
        return addr_parse_ipv6(str, len, out);
#else
        (void)out;
        return ADDR_ERR_NOT_IMPL;
#endif
    }

    /* IPv4 */
    *family = 4;
    memset(out, 0, 16);
    return addr_parse_ipv4(str, len, out);
}

int addr_format(const uint8_t *addr, uint8_t family, char *buf, size_t buf_len, size_t *out_len)
{
    if (!addr || !buf || !out_len) {
        return ADDR_ERR_INVALID_PARAM;
    }

    if (family == 4) {
        return addr_format_ipv4(addr, buf, buf_len, out_len);
    }
#if NANORTC_FEATURE_IPV6
    if (family == 6) {
        return addr_format_ipv6(addr, buf, buf_len, out_len);
    }
#endif
    return ADDR_ERR_INVALID_PARAM;
}
