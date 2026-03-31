/*
 * nanortc — Address parsing/formatting tests
 *
 * Test vectors from RFC 4291 §2.2, RFC 5952, and common edge cases.
 *
 * SPDX-License-Identifier: MIT
 */

#include "nanortc.h"
#include "nano_addr.h"
#include "nano_test.h"
#include <string.h>

/* ================================================================
 * IPv4 parsing
 * ================================================================ */

TEST(test_ipv4_parse_normal)
{
    uint8_t out[4];
    ASSERT_OK(addr_parse_ipv4("192.168.1.1", 11, out));
    ASSERT_EQ(out[0], 192);
    ASSERT_EQ(out[1], 168);
    ASSERT_EQ(out[2], 1);
    ASSERT_EQ(out[3], 1);
}

TEST(test_ipv4_parse_zeros)
{
    uint8_t out[4];
    ASSERT_OK(addr_parse_ipv4("0.0.0.0", 7, out));
    ASSERT_EQ(out[0], 0);
    ASSERT_EQ(out[1], 0);
    ASSERT_EQ(out[2], 0);
    ASSERT_EQ(out[3], 0);
}

TEST(test_ipv4_parse_max)
{
    uint8_t out[4];
    ASSERT_OK(addr_parse_ipv4("255.255.255.255", 15, out));
    ASSERT_EQ(out[0], 255);
    ASSERT_EQ(out[1], 255);
    ASSERT_EQ(out[2], 255);
    ASSERT_EQ(out[3], 255);
}

TEST(test_ipv4_parse_loopback)
{
    uint8_t out[4];
    ASSERT_OK(addr_parse_ipv4("127.0.0.1", 9, out));
    ASSERT_EQ(out[0], 127);
    ASSERT_EQ(out[1], 0);
    ASSERT_EQ(out[2], 0);
    ASSERT_EQ(out[3], 1);
}

TEST(test_ipv4_parse_octet_overflow)
{
    uint8_t out[4];
    ASSERT_FAIL(addr_parse_ipv4("256.0.0.1", 9, out));
}

TEST(test_ipv4_parse_too_few_octets)
{
    uint8_t out[4];
    ASSERT_FAIL(addr_parse_ipv4("1.2.3", 5, out));
}

TEST(test_ipv4_parse_too_many_octets)
{
    uint8_t out[4];
    ASSERT_FAIL(addr_parse_ipv4("1.2.3.4.5", 9, out));
}

TEST(test_ipv4_parse_empty)
{
    uint8_t out[4];
    ASSERT_FAIL(addr_parse_ipv4("", 0, out));
}

TEST(test_ipv4_parse_alpha)
{
    uint8_t out[4];
    ASSERT_FAIL(addr_parse_ipv4("1.2.3.x", 7, out));
}

TEST(test_ipv4_parse_null)
{
    uint8_t out[4];
    ASSERT_FAIL(addr_parse_ipv4(NULL, 0, out));
    ASSERT_FAIL(addr_parse_ipv4("1.2.3.4", 7, NULL));
}

/* ================================================================
 * IPv4 formatting
 * ================================================================ */

TEST(test_ipv4_format_normal)
{
    const uint8_t addr[] = {192, 168, 1, 1};
    char buf[16];
    size_t len;
    ASSERT_OK(addr_format_ipv4(addr, buf, sizeof(buf), &len));
    ASSERT_EQ(len, 11u);
    ASSERT_MEM_EQ(buf, "192.168.1.1", 11);
}

TEST(test_ipv4_format_zeros)
{
    const uint8_t addr[] = {0, 0, 0, 0};
    char buf[16];
    size_t len;
    ASSERT_OK(addr_format_ipv4(addr, buf, sizeof(buf), &len));
    ASSERT_MEM_EQ(buf, "0.0.0.0", 7);
}

TEST(test_ipv4_format_max)
{
    const uint8_t addr[] = {255, 255, 255, 255};
    char buf[16];
    size_t len;
    ASSERT_OK(addr_format_ipv4(addr, buf, sizeof(buf), &len));
    ASSERT_MEM_EQ(buf, "255.255.255.255", 15);
}

TEST(test_ipv4_format_buffer_too_small)
{
    const uint8_t addr[] = {255, 255, 255, 255};
    char buf[10]; /* too small for "255.255.255.255\0" */
    size_t len;
    ASSERT_FAIL(addr_format_ipv4(addr, buf, sizeof(buf), &len));
}

TEST(test_ipv4_roundtrip)
{
    const char *addrs[] = {"0.0.0.0", "127.0.0.1", "192.168.1.1",
                           "10.0.0.1", "255.255.255.255"};
    for (size_t i = 0; i < sizeof(addrs) / sizeof(addrs[0]); i++) {
        uint8_t bin[4];
        const char *s = addrs[i];
        size_t slen = 0;
        while (s[slen]) slen++;
        ASSERT_OK(addr_parse_ipv4(s, slen, bin));
        char out[16];
        size_t out_len;
        ASSERT_OK(addr_format_ipv4(bin, out, sizeof(out), &out_len));
        ASSERT_EQ(out_len, slen);
        ASSERT_MEM_EQ(out, s, slen);
    }
}

/* ================================================================
 * IPv6 parsing (RFC 4291 §2.2 / RFC 5952)
 * ================================================================ */

#if NANORTC_FEATURE_IPV6

TEST(test_ipv6_parse_full_form)
{
    /* 2001:0db8:0000:0000:0000:0000:0000:0001 */
    uint8_t out[16];
    ASSERT_OK(addr_parse_ipv6("2001:0db8:0000:0000:0000:0000:0000:0001", 39, out));
    const uint8_t expected[] = {0x20, 0x01, 0x0d, 0xb8, 0x00, 0x00, 0x00, 0x00,
                                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01};
    ASSERT_MEM_EQ(out, expected, 16);
}

TEST(test_ipv6_parse_compressed)
{
    /* 2001:db8:85a3::8a2e:370:7334 */
    uint8_t out[16];
    ASSERT_OK(addr_parse_ipv6("2001:db8:85a3::8a2e:370:7334", 28, out));
    const uint8_t expected[] = {0x20, 0x01, 0x0d, 0xb8, 0x85, 0xa3, 0x00, 0x00,
                                0x00, 0x00, 0x8a, 0x2e, 0x03, 0x70, 0x73, 0x34};
    ASSERT_MEM_EQ(out, expected, 16);
}

TEST(test_ipv6_parse_loopback)
{
    uint8_t out[16];
    ASSERT_OK(addr_parse_ipv6("::1", 3, out));
    const uint8_t expected[] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01};
    ASSERT_MEM_EQ(out, expected, 16);
}

TEST(test_ipv6_parse_all_zeros)
{
    uint8_t out[16];
    ASSERT_OK(addr_parse_ipv6("::", 2, out));
    const uint8_t expected[16] = {0};
    ASSERT_MEM_EQ(out, expected, 16);
}

TEST(test_ipv6_parse_link_local)
{
    /* fe80::1 */
    uint8_t out[16];
    ASSERT_OK(addr_parse_ipv6("fe80::1", 7, out));
    const uint8_t expected[] = {0xfe, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01};
    ASSERT_MEM_EQ(out, expected, 16);
}

TEST(test_ipv6_parse_multicast)
{
    /* ff02::1 */
    uint8_t out[16];
    ASSERT_OK(addr_parse_ipv6("ff02::1", 7, out));
    ASSERT_EQ(out[0], 0xff);
    ASSERT_EQ(out[1], 0x02);
    ASSERT_EQ(out[15], 0x01);
    /* bytes 2-14 should be zero */
    for (int i = 2; i < 15; i++) {
        ASSERT_EQ(out[i], 0);
    }
}

TEST(test_ipv6_parse_trailing_dcolon)
{
    /* 2001:db8:: */
    uint8_t out[16];
    ASSERT_OK(addr_parse_ipv6("2001:db8::", 10, out));
    const uint8_t expected[] = {0x20, 0x01, 0x0d, 0xb8, 0x00, 0x00, 0x00, 0x00,
                                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    ASSERT_MEM_EQ(out, expected, 16);
}

TEST(test_ipv6_parse_mid_dcolon)
{
    /* 2001:db8:0:1::1 */
    uint8_t out[16];
    ASSERT_OK(addr_parse_ipv6("2001:db8:0:1::1", 15, out));
    const uint8_t expected[] = {0x20, 0x01, 0x0d, 0xb8, 0x00, 0x00, 0x00, 0x01,
                                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01};
    ASSERT_MEM_EQ(out, expected, 16);
}

TEST(test_ipv6_parse_ipv4_mapped)
{
    /* ::ffff:192.168.1.1 */
    uint8_t out[16];
    ASSERT_OK(addr_parse_ipv6("::ffff:192.168.1.1", 18, out));
    const uint8_t expected[] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                0x00, 0x00, 0xff, 0xff, 0xc0, 0xa8, 0x01, 0x01};
    ASSERT_MEM_EQ(out, expected, 16);
}

TEST(test_ipv6_parse_ipv4_mapped_10)
{
    /* ::ffff:10.0.0.1 */
    uint8_t out[16];
    ASSERT_OK(addr_parse_ipv6("::ffff:10.0.0.1", 15, out));
    const uint8_t expected[] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                0x00, 0x00, 0xff, 0xff, 0x0a, 0x00, 0x00, 0x01};
    ASSERT_MEM_EQ(out, expected, 16);
}

/* RFC 5769 §2.3 address: 2001:db8:1234:5678:11:2233:4455:6677 */
TEST(test_ipv6_parse_rfc5769)
{
    uint8_t out[16];
    ASSERT_OK(addr_parse_ipv6("2001:db8:1234:5678:11:2233:4455:6677", 36, out));
    const uint8_t expected[] = {0x20, 0x01, 0x0d, 0xb8, 0x12, 0x34, 0x56, 0x78,
                                0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77};
    ASSERT_MEM_EQ(out, expected, 16);
}

TEST(test_ipv6_parse_uppercase)
{
    uint8_t out[16];
    ASSERT_OK(addr_parse_ipv6("FE80::ABCD", 10, out));
    ASSERT_EQ(out[0], 0xfe);
    ASSERT_EQ(out[1], 0x80);
    ASSERT_EQ(out[14], 0xab);
    ASSERT_EQ(out[15], 0xcd);
}

/* ---- Negative cases ---- */

TEST(test_ipv6_parse_double_dcolon)
{
    uint8_t out[16];
    ASSERT_FAIL(addr_parse_ipv6("2001:db8::1::2", 14, out));
}

TEST(test_ipv6_parse_too_many_groups)
{
    uint8_t out[16];
    ASSERT_FAIL(addr_parse_ipv6("2001:db8:85a3:0:0:0:0:0:1", 26, out));
}

TEST(test_ipv6_parse_invalid_hex)
{
    uint8_t out[16];
    ASSERT_FAIL(addr_parse_ipv6("2001:db8:gggg::1", 16, out));
}

TEST(test_ipv6_parse_group_overflow)
{
    uint8_t out[16];
    ASSERT_FAIL(addr_parse_ipv6("12345::1", 8, out));
}

TEST(test_ipv6_parse_bad_mapped_ipv4)
{
    uint8_t out[16];
    ASSERT_FAIL(addr_parse_ipv6("::ffff:999.168.1.1", 18, out));
}

TEST(test_ipv6_parse_empty)
{
    uint8_t out[16];
    ASSERT_FAIL(addr_parse_ipv6("", 0, out));
}

TEST(test_ipv6_parse_null)
{
    uint8_t out[16];
    ASSERT_FAIL(addr_parse_ipv6(NULL, 0, out));
}

TEST(test_ipv6_parse_trailing_colon)
{
    uint8_t out[16];
    ASSERT_FAIL(addr_parse_ipv6("2001:db8:", 9, out));
}

/* ================================================================
 * IPv6 formatting (RFC 5952 canonical)
 * ================================================================ */

TEST(test_ipv6_format_all_zeros)
{
    const uint8_t addr[16] = {0};
    char buf[NANORTC_IPV6_STR_SIZE];
    size_t len;
    ASSERT_OK(addr_format_ipv6(addr, buf, sizeof(buf), &len));
    ASSERT_MEM_EQ(buf, "::", 2);
    ASSERT_EQ(len, 2u);
}

TEST(test_ipv6_format_loopback)
{
    const uint8_t addr[] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01};
    char buf[NANORTC_IPV6_STR_SIZE];
    size_t len;
    ASSERT_OK(addr_format_ipv6(addr, buf, sizeof(buf), &len));
    ASSERT_MEM_EQ(buf, "::1", 3);
    ASSERT_EQ(len, 3u);
}

TEST(test_ipv6_format_no_compression_needed)
{
    /* 2001:db8:1234:5678:11:2233:4455:6677 — no consecutive zero groups */
    const uint8_t addr[] = {0x20, 0x01, 0x0d, 0xb8, 0x12, 0x34, 0x56, 0x78,
                            0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77};
    char buf[NANORTC_IPV6_STR_SIZE];
    size_t len;
    ASSERT_OK(addr_format_ipv6(addr, buf, sizeof(buf), &len));
    ASSERT_MEM_EQ(buf, "2001:db8:1234:5678:11:2233:4455:6677", 36);
}

TEST(test_ipv6_format_longest_zero_run)
{
    /* 2001:db8::1 = 2001:0db8:0000:0000:0000:0000:0000:0001 */
    const uint8_t addr[] = {0x20, 0x01, 0x0d, 0xb8, 0x00, 0x00, 0x00, 0x00,
                            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01};
    char buf[NANORTC_IPV6_STR_SIZE];
    size_t len;
    ASSERT_OK(addr_format_ipv6(addr, buf, sizeof(buf), &len));
    ASSERT_MEM_EQ(buf, "2001:db8::1", 11);
}

TEST(test_ipv6_format_single_zero_no_compress)
{
    /* RFC 5952 §4.2.2: single 0 group must NOT be compressed */
    /* 2001:db8:0:1:2:3:4:5 */
    const uint8_t addr[] = {0x20, 0x01, 0x0d, 0xb8, 0x00, 0x00, 0x00, 0x01,
                            0x00, 0x02, 0x00, 0x03, 0x00, 0x04, 0x00, 0x05};
    char buf[NANORTC_IPV6_STR_SIZE];
    size_t len;
    ASSERT_OK(addr_format_ipv6(addr, buf, sizeof(buf), &len));
    ASSERT_MEM_EQ(buf, "2001:db8:0:1:2:3:4:5", 21);
}

TEST(test_ipv6_format_buffer_too_small)
{
    const uint8_t addr[16] = {0};
    char buf[1]; /* way too small */
    size_t len;
    ASSERT_FAIL(addr_format_ipv6(addr, buf, sizeof(buf), &len));
}

/* ================================================================
 * IPv6 roundtrip (parse → format → parse)
 * ================================================================ */

TEST(test_ipv6_roundtrip)
{
    /* Canonical form strings that should survive roundtrip */
    const char *addrs[] = {
        "::",
        "::1",
        "fe80::1",
        "2001:db8::1",
        "2001:db8:85a3::8a2e:370:7334",
        "2001:db8:1234:5678:11:2233:4455:6677",
        "ff02::1",
    };
    for (size_t i = 0; i < sizeof(addrs) / sizeof(addrs[0]); i++) {
        uint8_t bin[16];
        const char *s = addrs[i];
        size_t slen = 0;
        while (s[slen]) slen++;
        ASSERT_OK(addr_parse_ipv6(s, slen, bin));
        char out[NANORTC_IPV6_STR_SIZE];
        size_t out_len;
        ASSERT_OK(addr_format_ipv6(bin, out, sizeof(out), &out_len));
        ASSERT_EQ(out_len, slen);
        ASSERT_MEM_EQ(out, s, slen);
    }
}

#endif /* NANORTC_FEATURE_IPV6 */

/* ================================================================
 * Auto-detect (addr_parse_auto)
 * ================================================================ */

TEST(test_auto_parse_ipv4)
{
    uint8_t out[16] = {0};
    uint8_t family = 0;
    ASSERT_OK(addr_parse_auto("10.0.0.1", 8, out, &family));
    ASSERT_EQ(family, 4);
    ASSERT_EQ(out[0], 10);
    ASSERT_EQ(out[1], 0);
    ASSERT_EQ(out[2], 0);
    ASSERT_EQ(out[3], 1);
}

#if NANORTC_FEATURE_IPV6
TEST(test_auto_parse_ipv6)
{
    uint8_t out[16] = {0};
    uint8_t family = 0;
    ASSERT_OK(addr_parse_auto("::1", 3, out, &family));
    ASSERT_EQ(family, 6);
    ASSERT_EQ(out[15], 1);
}
#endif

TEST(test_auto_parse_null)
{
    uint8_t out[16];
    uint8_t family;
    ASSERT_FAIL(addr_parse_auto(NULL, 0, out, &family));
}

/* ================================================================
 * Format dispatch (addr_format)
 * ================================================================ */

TEST(test_format_ipv4)
{
    const uint8_t addr[] = {127, 0, 0, 1};
    char buf[16];
    size_t len;
    ASSERT_OK(addr_format(addr, 4, buf, sizeof(buf), &len));
    ASSERT_MEM_EQ(buf, "127.0.0.1", 9);
}

#if NANORTC_FEATURE_IPV6
TEST(test_format_ipv6)
{
    const uint8_t addr[] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01};
    char buf[NANORTC_IPV6_STR_SIZE];
    size_t len;
    ASSERT_OK(addr_format(addr, 6, buf, sizeof(buf), &len));
    ASSERT_MEM_EQ(buf, "::1", 3);
}
#endif

TEST(test_format_bad_family)
{
    const uint8_t addr[16] = {0};
    char buf[16];
    size_t len;
    ASSERT_FAIL(addr_format(addr, 99, buf, sizeof(buf), &len));
}

/* ================================================================ */

TEST_MAIN_BEGIN("nanortc address tests")
/* IPv4 parsing */
RUN(test_ipv4_parse_normal);
RUN(test_ipv4_parse_zeros);
RUN(test_ipv4_parse_max);
RUN(test_ipv4_parse_loopback);
RUN(test_ipv4_parse_octet_overflow);
RUN(test_ipv4_parse_too_few_octets);
RUN(test_ipv4_parse_too_many_octets);
RUN(test_ipv4_parse_empty);
RUN(test_ipv4_parse_alpha);
RUN(test_ipv4_parse_null);
/* IPv4 formatting */
RUN(test_ipv4_format_normal);
RUN(test_ipv4_format_zeros);
RUN(test_ipv4_format_max);
RUN(test_ipv4_format_buffer_too_small);
RUN(test_ipv4_roundtrip);
#if NANORTC_FEATURE_IPV6
/* IPv6 parsing */
RUN(test_ipv6_parse_full_form);
RUN(test_ipv6_parse_compressed);
RUN(test_ipv6_parse_loopback);
RUN(test_ipv6_parse_all_zeros);
RUN(test_ipv6_parse_link_local);
RUN(test_ipv6_parse_multicast);
RUN(test_ipv6_parse_trailing_dcolon);
RUN(test_ipv6_parse_mid_dcolon);
RUN(test_ipv6_parse_ipv4_mapped);
RUN(test_ipv6_parse_ipv4_mapped_10);
RUN(test_ipv6_parse_rfc5769);
RUN(test_ipv6_parse_uppercase);
/* IPv6 negative */
RUN(test_ipv6_parse_double_dcolon);
RUN(test_ipv6_parse_too_many_groups);
RUN(test_ipv6_parse_invalid_hex);
RUN(test_ipv6_parse_group_overflow);
RUN(test_ipv6_parse_bad_mapped_ipv4);
RUN(test_ipv6_parse_empty);
RUN(test_ipv6_parse_null);
RUN(test_ipv6_parse_trailing_colon);
/* IPv6 formatting */
RUN(test_ipv6_format_all_zeros);
RUN(test_ipv6_format_loopback);
RUN(test_ipv6_format_no_compression_needed);
RUN(test_ipv6_format_longest_zero_run);
RUN(test_ipv6_format_single_zero_no_compress);
RUN(test_ipv6_format_buffer_too_small);
/* IPv6 roundtrip */
RUN(test_ipv6_roundtrip);
#endif /* NANORTC_FEATURE_IPV6 */
/* Auto-detect */
RUN(test_auto_parse_ipv4);
#if NANORTC_FEATURE_IPV6
RUN(test_auto_parse_ipv6);
#endif
RUN(test_auto_parse_null);
/* Format dispatch */
RUN(test_format_ipv4);
#if NANORTC_FEATURE_IPV6
RUN(test_format_ipv6);
#endif
RUN(test_format_bad_family);
TEST_MAIN_END
