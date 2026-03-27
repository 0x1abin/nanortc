/*
 * nanortc — RTCP report tests (RFC 3550 / RFC 4585)
 *
 * SPDX-License-Identifier: MIT
 */

#include "nanortc.h"
#include "nano_rtcp.h"
#include "nano_test.h"
#include <string.h>

/* ================================================================
 * Sender Report tests
 * ================================================================ */

TEST(test_rtcp_sr_basic)
{
    nano_rtcp_t rtcp;
    ASSERT_OK(rtcp_init(&rtcp, 0x12345678));
    rtcp.packets_sent = 100;
    rtcp.octets_sent = 16000;

    uint8_t buf[64];
    size_t out_len = 0;
    ASSERT_OK(rtcp_generate_sr(&rtcp, 0xAABBCCDD, 0x11223344, 0x55667788,
                                buf, sizeof(buf), &out_len));
    ASSERT_EQ(out_len, RTCP_SR_SIZE); /* 28 bytes */

    /* Verify RTCP header */
    ASSERT_EQ((buf[0] >> 6) & 0x03, 2);   /* V=2 */
    ASSERT_EQ(buf[0] & 0x1F, 0);          /* RC=0 */
    ASSERT_EQ(buf[1], RTCP_SR);           /* PT=200 */
    ASSERT_EQ(nano_read_u16be(buf + 2), 6);    /* length=6 (words) */
    ASSERT_EQ(nano_read_u32be(buf + 4), 0x12345678); /* SSRC */

    /* Verify sender info */
    ASSERT_EQ(nano_read_u32be(buf + 8), 0xAABBCCDD);  /* NTP sec */
    ASSERT_EQ(nano_read_u32be(buf + 12), 0x11223344); /* NTP frac */
    ASSERT_EQ(nano_read_u32be(buf + 16), 0x55667788); /* RTP ts */
    ASSERT_EQ(nano_read_u32be(buf + 20), 100);        /* packet count */
    ASSERT_EQ(nano_read_u32be(buf + 24), 16000);      /* octet count */
}

TEST(test_rtcp_sr_buffer_too_small)
{
    nano_rtcp_t rtcp;
    rtcp_init(&rtcp, 1);

    uint8_t buf[16]; /* Too small for 28-byte SR */
    size_t out_len = 0;
    ASSERT_FAIL(rtcp_generate_sr(&rtcp, 0, 0, 0, buf, sizeof(buf), &out_len));
}

TEST(test_rtcp_sr_null_params)
{
    nano_rtcp_t rtcp;
    rtcp_init(&rtcp, 1);
    uint8_t buf[64];
    size_t out_len;

    ASSERT_FAIL(rtcp_generate_sr(NULL, 0, 0, 0, buf, sizeof(buf), &out_len));
    ASSERT_FAIL(rtcp_generate_sr(&rtcp, 0, 0, 0, NULL, sizeof(buf), &out_len));
    ASSERT_FAIL(rtcp_generate_sr(&rtcp, 0, 0, 0, buf, sizeof(buf), NULL));
}

/* ================================================================
 * Receiver Report tests
 * ================================================================ */

TEST(test_rtcp_rr_basic)
{
    nano_rtcp_t rtcp;
    ASSERT_OK(rtcp_init(&rtcp, 0xAABBCCDD));
    rtcp.packets_received = 95;
    rtcp.packets_lost = 5;
    rtcp.max_seq = 100;
    rtcp.jitter = 320;

    uint8_t buf[64];
    size_t out_len = 0;
    ASSERT_OK(rtcp_generate_rr(&rtcp, 0x12345678, buf, sizeof(buf), &out_len));
    ASSERT_EQ(out_len, RTCP_RR_SIZE); /* 32 bytes */

    /* Verify RTCP header */
    ASSERT_EQ((buf[0] >> 6) & 0x03, 2);   /* V=2 */
    ASSERT_EQ(buf[0] & 0x1F, 1);          /* RC=1 */
    ASSERT_EQ(buf[1], RTCP_RR);           /* PT=201 */
    ASSERT_EQ(nano_read_u16be(buf + 2), 7);    /* length=7 */
    ASSERT_EQ(nano_read_u32be(buf + 4), 0xAABBCCDD); /* Reporter SSRC */

    /* Report block */
    ASSERT_EQ(nano_read_u32be(buf + 8), 0x12345678); /* Reported SSRC */
    ASSERT_TRUE(buf[12] > 0); /* Fraction lost > 0 */
    /* Cumulative lost = 5 */
    uint32_t clost = ((uint32_t)buf[13] << 16) | ((uint32_t)buf[14] << 8) | buf[15];
    ASSERT_EQ(clost, 5);
    /* Extended highest seq */
    ASSERT_EQ(nano_read_u32be(buf + 16), 100);
    /* Jitter */
    ASSERT_EQ(nano_read_u32be(buf + 20), 320);
}

TEST(test_rtcp_rr_no_loss)
{
    nano_rtcp_t rtcp;
    rtcp_init(&rtcp, 1);
    rtcp.packets_received = 100;
    rtcp.packets_lost = 0;

    uint8_t buf[64];
    size_t out_len = 0;
    ASSERT_OK(rtcp_generate_rr(&rtcp, 2, buf, sizeof(buf), &out_len));
    ASSERT_EQ(out_len, RTCP_RR_SIZE);

    /* Fraction lost = 0 */
    ASSERT_EQ(buf[12], 0);
}

TEST(test_rtcp_rr_buffer_too_small)
{
    nano_rtcp_t rtcp;
    rtcp_init(&rtcp, 1);

    uint8_t buf[16];
    size_t out_len = 0;
    ASSERT_FAIL(rtcp_generate_rr(&rtcp, 2, buf, sizeof(buf), &out_len));
}

/* ================================================================
 * NACK tests
 * ================================================================ */

TEST(test_rtcp_nack_basic)
{
    uint8_t buf[32];
    size_t out_len = 0;
    ASSERT_OK(rtcp_generate_nack(0xAAAAAAAA, 0xBBBBBBBB, 42, buf, sizeof(buf), &out_len));
    ASSERT_EQ(out_len, RTCP_NACK_SIZE); /* 16 bytes */

    /* Header */
    ASSERT_EQ((buf[0] >> 6) & 0x03, 2);    /* V=2 */
    ASSERT_EQ(buf[0] & 0x1F, 1);           /* FMT=1 (Generic NACK) */
    ASSERT_EQ(buf[1], RTCP_RTPFB);         /* PT=205 */
    ASSERT_EQ(nano_read_u16be(buf + 2), 3);     /* length=3 */
    ASSERT_EQ(nano_read_u32be(buf + 4), 0xAAAAAAAA); /* Sender SSRC */

    /* Media SSRC */
    ASSERT_EQ(nano_read_u32be(buf + 8), 0xBBBBBBBB);

    /* FCI: PID=42, BLP=0 */
    ASSERT_EQ(nano_read_u16be(buf + 12), 42);
    ASSERT_EQ(nano_read_u16be(buf + 14), 0);
}

TEST(test_rtcp_nack_buffer_too_small)
{
    uint8_t buf[8];
    size_t out_len = 0;
    ASSERT_FAIL(rtcp_generate_nack(1, 2, 3, buf, sizeof(buf), &out_len));
}

/* ================================================================
 * Parser tests
 * ================================================================ */

TEST(test_rtcp_parse_sr)
{
    /* Generate an SR, then parse it back */
    nano_rtcp_t rtcp;
    rtcp_init(&rtcp, 0xDEADBEEF);
    rtcp.packets_sent = 50;
    rtcp.octets_sent = 8000;

    uint8_t buf[64];
    size_t out_len = 0;
    ASSERT_OK(rtcp_generate_sr(&rtcp, 1000, 2000, 3000, buf, sizeof(buf), &out_len));

    nano_rtcp_info_t info;
    ASSERT_OK(rtcp_parse(buf, out_len, &info));
    ASSERT_EQ(info.type, RTCP_SR);
    ASSERT_EQ(info.ssrc, 0xDEADBEEF);
    ASSERT_EQ(info.ntp_sec, 1000);
    ASSERT_EQ(info.ntp_frac, 2000);
    ASSERT_EQ(info.rtp_ts, 3000);
    ASSERT_EQ(info.sr_packets, 50);
    ASSERT_EQ(info.sr_octets, 8000);
}

TEST(test_rtcp_parse_rr)
{
    nano_rtcp_t rtcp;
    rtcp_init(&rtcp, 0x11111111);

    uint8_t buf[64];
    size_t out_len = 0;
    ASSERT_OK(rtcp_generate_rr(&rtcp, 0x22222222, buf, sizeof(buf), &out_len));

    nano_rtcp_info_t info;
    ASSERT_OK(rtcp_parse(buf, out_len, &info));
    ASSERT_EQ(info.type, RTCP_RR);
    ASSERT_EQ(info.ssrc, 0x11111111);
}

TEST(test_rtcp_parse_nack)
{
    uint8_t buf[32];
    size_t out_len = 0;
    ASSERT_OK(rtcp_generate_nack(0x33333333, 0x44444444, 999, buf, sizeof(buf), &out_len));

    nano_rtcp_info_t info;
    ASSERT_OK(rtcp_parse(buf, out_len, &info));
    ASSERT_EQ(info.type, RTCP_RTPFB);
    ASSERT_EQ(info.ssrc, 0x33333333);
    ASSERT_EQ(info.nack_pid, 999);
    ASSERT_EQ(info.nack_blp, 0);
}

TEST(test_rtcp_parse_truncated)
{
    uint8_t buf[4] = {0x80, RTCP_SR, 0x00, 0x06};
    nano_rtcp_info_t info;
    /* Header says length=6 words (28 bytes) but buffer is only 4 bytes */
    ASSERT_FAIL(rtcp_parse(buf, sizeof(buf), &info));
}

TEST(test_rtcp_parse_bad_version)
{
    uint8_t buf[28] = {0};
    buf[0] = 0x00; /* V=0, invalid */
    buf[1] = RTCP_SR;
    nano_rtcp_info_t info;
    ASSERT_FAIL(rtcp_parse(buf, sizeof(buf), &info));
}

TEST(test_rtcp_parse_null_params)
{
    uint8_t buf[28];
    nano_rtcp_info_t info;
    ASSERT_FAIL(rtcp_parse(NULL, 28, &info));
    ASSERT_FAIL(rtcp_parse(buf, 28, NULL));
    ASSERT_FAIL(rtcp_parse(buf, 0, &info));
}

TEST(test_rtcp_init_null)
{
    ASSERT_FAIL(rtcp_init(NULL, 0));
}

/* ================================================================
 * Test runner
 * ================================================================ */

TEST_MAIN_BEGIN("test_rtcp")
RUN(test_rtcp_sr_basic);
RUN(test_rtcp_sr_buffer_too_small);
RUN(test_rtcp_sr_null_params);
RUN(test_rtcp_rr_basic);
RUN(test_rtcp_rr_no_loss);
RUN(test_rtcp_rr_buffer_too_small);
RUN(test_rtcp_nack_basic);
RUN(test_rtcp_nack_buffer_too_small);
RUN(test_rtcp_parse_sr);
RUN(test_rtcp_parse_rr);
RUN(test_rtcp_parse_nack);
RUN(test_rtcp_parse_truncated);
RUN(test_rtcp_parse_bad_version);
RUN(test_rtcp_parse_null_params);
RUN(test_rtcp_init_null);
TEST_MAIN_END
