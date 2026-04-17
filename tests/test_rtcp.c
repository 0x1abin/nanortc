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
    ASSERT_OK(
        rtcp_generate_sr(&rtcp, 0xAABBCCDD, 0x11223344, 0x55667788, buf, sizeof(buf), &out_len));
    ASSERT_EQ(out_len, RTCP_SR_SIZE); /* 28 bytes */

    /* Verify RTCP header */
    ASSERT_EQ((buf[0] >> 6) & 0x03, 2);                 /* V=2 */
    ASSERT_EQ(buf[0] & 0x1F, 0);                        /* RC=0 */
    ASSERT_EQ(buf[1], RTCP_SR);                         /* PT=200 */
    ASSERT_EQ(nanortc_read_u16be(buf + 2), 6);          /* length=6 (words) */
    ASSERT_EQ(nanortc_read_u32be(buf + 4), 0x12345678); /* SSRC */

    /* Verify sender info */
    ASSERT_EQ(nanortc_read_u32be(buf + 8), 0xAABBCCDD);  /* NTP sec */
    ASSERT_EQ(nanortc_read_u32be(buf + 12), 0x11223344); /* NTP frac */
    ASSERT_EQ(nanortc_read_u32be(buf + 16), 0x55667788); /* RTP ts */
    ASSERT_EQ(nanortc_read_u32be(buf + 20), 100);        /* packet count */
    ASSERT_EQ(nanortc_read_u32be(buf + 24), 16000);      /* octet count */
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
    ASSERT_EQ((buf[0] >> 6) & 0x03, 2);                 /* V=2 */
    ASSERT_EQ(buf[0] & 0x1F, 1);                        /* RC=1 */
    ASSERT_EQ(buf[1], RTCP_RR);                         /* PT=201 */
    ASSERT_EQ(nanortc_read_u16be(buf + 2), 7);          /* length=7 */
    ASSERT_EQ(nanortc_read_u32be(buf + 4), 0xAABBCCDD); /* Reporter SSRC */

    /* Report block */
    ASSERT_EQ(nanortc_read_u32be(buf + 8), 0x12345678); /* Reported SSRC */
    ASSERT_TRUE(buf[12] > 0);                           /* Fraction lost > 0 */
    /* Cumulative lost = 5 */
    uint32_t clost = ((uint32_t)buf[13] << 16) | ((uint32_t)buf[14] << 8) | buf[15];
    ASSERT_EQ(clost, 5);
    /* Extended highest seq */
    ASSERT_EQ(nanortc_read_u32be(buf + 16), 100);
    /* Jitter */
    ASSERT_EQ(nanortc_read_u32be(buf + 20), 320);
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
    ASSERT_EQ((buf[0] >> 6) & 0x03, 2);                 /* V=2 */
    ASSERT_EQ(buf[0] & 0x1F, 1);                        /* FMT=1 (Generic NACK) */
    ASSERT_EQ(buf[1], RTCP_RTPFB);                      /* PT=205 */
    ASSERT_EQ(nanortc_read_u16be(buf + 2), 3);          /* length=3 */
    ASSERT_EQ(nanortc_read_u32be(buf + 4), 0xAAAAAAAA); /* Sender SSRC */

    /* Media SSRC */
    ASSERT_EQ(nanortc_read_u32be(buf + 8), 0xBBBBBBBB);

    /* FCI: PID=42, BLP=0 */
    ASSERT_EQ(nanortc_read_u16be(buf + 12), 42);
    ASSERT_EQ(nanortc_read_u16be(buf + 14), 0);
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
 * PLI tests (RFC 4585 §6.3.1)
 * ================================================================ */

TEST(test_rtcp_pli_basic)
{
    uint8_t buf[32];
    size_t out_len = 0;
    ASSERT_OK(rtcp_generate_pli(0xAAAAAAAA, 0xBBBBBBBB, buf, sizeof(buf), &out_len));
    ASSERT_EQ(out_len, RTCP_PLI_SIZE); /* 12 bytes */

    /* Header: V=2, P=0, FMT=1, PT=206 (PSFB), length=2 */
    ASSERT_EQ((buf[0] >> 6) & 0x03, 2);                 /* V=2 */
    ASSERT_EQ(buf[0] & 0x1F, 1);                        /* FMT=1 (PLI) */
    ASSERT_EQ(buf[1], RTCP_PSFB);                       /* PT=206 */
    ASSERT_EQ(nanortc_read_u16be(buf + 2), 2);          /* length=2 words */
    ASSERT_EQ(nanortc_read_u32be(buf + 4), 0xAAAAAAAA); /* Sender SSRC */
    ASSERT_EQ(nanortc_read_u32be(buf + 8), 0xBBBBBBBB); /* Media SSRC */
}

TEST(test_rtcp_pli_buffer_too_small)
{
    uint8_t buf[8];
    size_t out_len = 0;
    ASSERT_FAIL(rtcp_generate_pli(1, 2, buf, sizeof(buf), &out_len));
}

TEST(test_rtcp_pli_null_params)
{
    uint8_t buf[32];
    size_t out_len;
    ASSERT_FAIL(rtcp_generate_pli(1, 2, NULL, 32, &out_len));
    ASSERT_FAIL(rtcp_generate_pli(1, 2, buf, 32, NULL));
}

TEST(test_rtcp_pli_parse_roundtrip)
{
    /* Generate PLI, then parse it back */
    uint8_t buf[32];
    size_t out_len = 0;
    ASSERT_OK(rtcp_generate_pli(0x11111111, 0x22222222, buf, sizeof(buf), &out_len));

    nano_rtcp_info_t info;
    ASSERT_OK(rtcp_parse(buf, out_len, &info));
    ASSERT_EQ(info.type, RTCP_PSFB);
    ASSERT_EQ(info.ssrc, 0x11111111);
}

/* ================================================================
 * RFC 3550 MUST/SHOULD requirement tests
 * ================================================================ */

/*
 * RFC 3550 §6.4.1: SR header verification — version, padding, RC, PT fields.
 * Independent byte-level verification of generated SR packet structure.
 */
TEST(test_rtcp_sr_header_fields)
{
    nano_rtcp_t rtcp;
    rtcp_init(&rtcp, 0xABCDEF01);
    rtcp.packets_sent = 200;
    rtcp.octets_sent = 32000;

    uint8_t buf[64];
    size_t out_len = 0;
    ASSERT_OK(rtcp_generate_sr(&rtcp, 0x12345678, 0x9ABCDEF0, 160000, buf, sizeof(buf), &out_len));
    ASSERT_EQ(out_len, RTCP_SR_SIZE); /* 28 bytes */

    /* RFC 3550 §6.4.1: Byte 0 = V(2)|P(1)|RC(5) */
    ASSERT_EQ((buf[0] >> 6) & 0x03, 2); /* V=2 MUST */
    ASSERT_EQ((buf[0] >> 5) & 0x01, 0); /* P=0 (no padding) */
    ASSERT_EQ(buf[0] & 0x1F, 0);        /* RC=0 (no report blocks in basic SR) */

    /* PT=200 (SR) */
    ASSERT_EQ(buf[1], RTCP_SR);

    /* Length field: (28/4 - 1) = 6 */
    ASSERT_EQ(nanortc_read_u16be(buf + 2), 6);

    /* SSRC of sender */
    ASSERT_EQ(nanortc_read_u32be(buf + 4), 0xABCDEF01u);

    /* NTP timestamp */
    ASSERT_EQ(nanortc_read_u32be(buf + 8), 0x12345678u);  /* NTP seconds */
    ASSERT_EQ(nanortc_read_u32be(buf + 12), 0x9ABCDEF0u); /* NTP fraction */

    /* RTP timestamp */
    ASSERT_EQ(nanortc_read_u32be(buf + 16), 160000u);

    /* Sender packet count and octet count */
    ASSERT_EQ(nanortc_read_u32be(buf + 20), 200u);
    ASSERT_EQ(nanortc_read_u32be(buf + 24), 32000u);
}

/*
 * RFC 3550 §6.4.1: Extended highest sequence number in RR report block.
 * The "extended highest sequence number" is cycles(16) << 16 | max_seq(16).
 * With our implementation, max_seq maps directly (no cycle tracking yet).
 */
TEST(test_rtcp_rr_extended_seq_field)
{
    nano_rtcp_t rtcp;
    rtcp_init(&rtcp, 0x11111111);
    rtcp.packets_received = 50;
    rtcp.packets_lost = 0;
    rtcp.max_seq = 0xFFFF; /* maximum 16-bit seq */

    uint8_t buf[64];
    size_t out_len = 0;
    ASSERT_OK(rtcp_generate_rr(&rtcp, 0x22222222, buf, sizeof(buf), &out_len));

    /* Report block extended highest seq is at offset 16 in RR packet */
    uint32_t ext_seq = nanortc_read_u32be(buf + 16);
    /* Should contain at least the max_seq value */
    ASSERT_TRUE((ext_seq & 0xFFFF) == 0xFFFF);
}

/*
 * RFC 4585 §6.2.1: NACK with bitmask for multiple lost packets.
 * PID=100, BLP=0x0005 → lost packets: 100, 101, 103 (bit 0 and bit 2 set).
 *
 * Build NACK manually to test parser's BLP interpretation.
 */
TEST(test_rtcp_nack_bitmask_vector)
{
    /* Hand-craft NACK packet with PID=100 and BLP=0x0005 */
    static const uint8_t nack_pkt[] = {
        /* V=2, P=0, FMT=1, PT=205 (RTPFB) */
        0x81,
        0xCD,
        /* Length = 3 (in 32-bit words minus 1) */
        0x00,
        0x03,
        /* Sender SSRC = 0x11111111 */
        0x11,
        0x11,
        0x11,
        0x11,
        /* Media SSRC = 0x22222222 */
        0x22,
        0x22,
        0x22,
        0x22,
        /* FCI: PID=100, BLP=0x0005 */
        0x00,
        0x64,
        0x00,
        0x05,
    };

    nano_rtcp_info_t info;
    ASSERT_OK(rtcp_parse(nack_pkt, sizeof(nack_pkt), &info));
    ASSERT_EQ(info.type, RTCP_RTPFB);
    ASSERT_EQ(info.ssrc, 0x11111111u);
    ASSERT_EQ(info.nack_pid, 100);
    ASSERT_EQ(info.nack_blp, 0x0005);
    /* BLP 0x0005 = bits 0 and 2 → lost packets: PID+1=101, PID+3=103 */
}

/*
 * RFC 3550 §6.4.1: SR parse — independent byte-level vector.
 * Verify we can parse an SR packet we didn't generate.
 */
TEST(test_rtcp_sr_independent_vector)
{
    static const uint8_t sr_pkt[] = {
        /* V=2, P=0, RC=0, PT=200 (SR) */
        0x80,
        0xC8,
        /* Length=6 */
        0x00,
        0x06,
        /* Sender SSRC = 0xCAFEBABE */
        0xCA,
        0xFE,
        0xBA,
        0xBE,
        /* NTP seconds = 1000 (0x000003E8) */
        0x00,
        0x00,
        0x03,
        0xE8,
        /* NTP fraction = 0 */
        0x00,
        0x00,
        0x00,
        0x00,
        /* RTP timestamp = 48000 */
        0x00,
        0x00,
        0xBB,
        0x80,
        /* Sender packet count = 42 */
        0x00,
        0x00,
        0x00,
        0x2A,
        /* Sender octet count = 8400 */
        0x00,
        0x00,
        0x20,
        0xD0,
    };

    nano_rtcp_info_t info;
    ASSERT_OK(rtcp_parse(sr_pkt, sizeof(sr_pkt), &info));
    ASSERT_EQ(info.type, RTCP_SR);
    ASSERT_EQ(info.ssrc, 0xCAFEBABEu);
    ASSERT_EQ(info.ntp_sec, 1000u);
    ASSERT_EQ(info.ntp_frac, 0u);
    ASSERT_EQ(info.rtp_ts, 48000u);
    ASSERT_EQ(info.sr_packets, 42u);
    ASSERT_EQ(info.sr_octets, 8400u);
}

/* ================================================================
 * Report-block extraction from RR/SR (PR-5)
 * ================================================================ */

TEST(test_rtcp_parse_rr_fraction_lost)
{
    /* Build a minimal RR with one report block carrying fraction_lost=0x40. */
    nano_rtcp_t rtcp;
    rtcp_init(&rtcp, 0x11111111);
    rtcp.packets_lost = 0;
    rtcp.packets_received = 100;
    rtcp.jitter = 50;

    uint8_t buf[64];
    size_t out_len = 0;
    ASSERT_OK(rtcp_generate_rr(&rtcp, 0x22222222, buf, sizeof(buf), &out_len));
    /* Overwrite the fraction_lost byte (first byte of the report block at offset 12). */
    buf[12] = 0x40;

    nano_rtcp_info_t info;
    ASSERT_OK(rtcp_parse(buf, out_len, &info));
    ASSERT_EQ(info.type, (uint8_t)RTCP_RR);
    ASSERT_TRUE(info.rb_valid);
    ASSERT_EQ(info.rb_source_ssrc, 0x22222222u);
    ASSERT_EQ(info.rb_fraction_lost, 0x40);
}

TEST(test_rtcp_parse_sr_with_report_block)
{
    /* SR with RC=1 and one report block. Parser must surface fraction_lost. */
    uint8_t buf[RTCP_SR_WITH_RB_SIZE] = {0};
    buf[0] = (2 << 6) | 1; /* V=2, RC=1 */
    buf[1] = RTCP_SR;
    buf[2] = 0x00;
    buf[3] = (RTCP_SR_WITH_RB_SIZE / 4) - 1; /* length field */
    /* Sender SSRC */
    nanortc_write_u32be(buf + 4, 0xAAAAAAAAu);
    /* Sender info (20 bytes) — values not checked here, just nonzero. */
    nanortc_write_u32be(buf + 8, 1);
    nanortc_write_u32be(buf + 12, 2);
    nanortc_write_u32be(buf + 16, 3);
    nanortc_write_u32be(buf + 20, 4);
    nanortc_write_u32be(buf + 24, 5);
    /* Report block (24 bytes): source SSRC, fraction lost, cumulative loss... */
    nanortc_write_u32be(buf + 28, 0xBBBBBBBBu);
    buf[32] = 0xA0; /* fraction lost ≈ 62.5 % */
    /* cumulative loss (3B) + ext seq + jitter + LSR + DLSR — zeros OK */

    nano_rtcp_info_t info;
    ASSERT_OK(rtcp_parse(buf, sizeof(buf), &info));
    ASSERT_EQ(info.type, (uint8_t)RTCP_SR);
    ASSERT_TRUE(info.rb_valid);
    ASSERT_EQ(info.rb_source_ssrc, 0xBBBBBBBBu);
    ASSERT_EQ(info.rb_fraction_lost, 0xA0);
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
RUN(test_rtcp_pli_basic);
RUN(test_rtcp_pli_buffer_too_small);
RUN(test_rtcp_pli_null_params);
RUN(test_rtcp_pli_parse_roundtrip);
/* RFC 3550 MUST/SHOULD requirement tests */
RUN(test_rtcp_sr_header_fields);
RUN(test_rtcp_rr_extended_seq_field);
RUN(test_rtcp_nack_bitmask_vector);
RUN(test_rtcp_sr_independent_vector);
/* PR-5: report-block (fraction_lost) extraction */
RUN(test_rtcp_parse_rr_fraction_lost);
RUN(test_rtcp_parse_sr_with_report_block);
TEST_MAIN_END
