/*
 * nanortc — Bandwidth estimation tests (REMB parsing + EMA smoothing)
 *
 * SPDX-License-Identifier: MIT
 */

#include "nanortc.h"
#include "nano_bwe.h"
#include "nano_test.h"
#include <string.h>

/* ================================================================
 * Helper: build a REMB packet
 *
 * draft-alvestrand-rmcat-remb-03 §2:
 *   V=2, P=0, FMT=15, PT=206
 *   SSRC of sender, SSRC of media (0)
 *   "REMB" unique identifier
 *   Num SSRC | BR Exp (6) | BR Mantissa (18)
 *   SSRC feedback entries
 * ================================================================ */

static size_t build_remb(uint8_t *buf, size_t buf_len, uint32_t sender_ssrc, uint32_t bitrate,
                         uint32_t feedback_ssrc)
{
    /* Minimum 24 bytes: header(4) + sender_ssrc(4) + media_ssrc(4) +
     * REMB_id(4) + br_fields(4) + feedback_ssrc(4) */
    if (buf_len < 24) {
        return 0;
    }

    memset(buf, 0, 24);

    /* RTCP header: V=2, P=0, FMT=15, PT=206 */
    buf[0] = (2 << 6) | 15; /* V=2, FMT=15 */
    buf[1] = 206;           /* PT=PSFB */
    /* length in 32-bit words minus 1: (24/4 - 1) = 5 */
    nanortc_write_u16be(buf + 2, 5);

    /* Sender SSRC */
    nanortc_write_u32be(buf + 4, sender_ssrc);

    /* Media source SSRC (0 for REMB) */
    nanortc_write_u32be(buf + 8, 0);

    /* "REMB" unique identifier */
    buf[12] = 'R';
    buf[13] = 'E';
    buf[14] = 'M';
    buf[15] = 'B';

    /* Encode bitrate: find smallest exp such that mantissa fits in 18 bits */
    uint8_t exp = 0;
    uint32_t mantissa = bitrate;
    while (mantissa > 0x3FFFF && exp < 63) { /* 0x3FFFF = 18-bit max */
        mantissa >>= 1;
        exp++;
    }

    /* Num SSRC = 1 */
    buf[16] = 1;

    /* BR Exp (6 bits) | BR Mantissa high (2 bits) */
    buf[17] = (uint8_t)((exp << 2) | ((mantissa >> 16) & 0x03));

    /* BR Mantissa low (16 bits) */
    buf[18] = (uint8_t)((mantissa >> 8) & 0xFF);
    buf[19] = (uint8_t)(mantissa & 0xFF);

    /* Feedback SSRC */
    nanortc_write_u32be(buf + 20, feedback_ssrc);

    return 24;
}

/* ================================================================
 * Init tests
 * ================================================================ */

TEST(test_bwe_init)
{
    nano_bwe_t bwe;
    ASSERT_OK(bwe_init(&bwe));
    ASSERT_EQ(bwe.estimated_bitrate, NANORTC_BWE_INITIAL_BITRATE);
    ASSERT_EQ(bwe.last_remb_bitrate, 0);
    ASSERT_EQ(bwe.last_update_ms, 0);
    ASSERT_EQ(bwe.remb_count, 0);
}

TEST(test_bwe_init_null)
{
    ASSERT_FAIL(bwe_init(NULL));
}

/* ================================================================
 * REMB parse tests
 * ================================================================ */

TEST(test_bwe_parse_remb_basic)
{
    /* Build REMB with 500 kbps */
    uint8_t buf[32];
    size_t len = build_remb(buf, sizeof(buf), 0x11223344, 500000, 0xAABBCCDD);
    ASSERT_TRUE(len > 0);

    uint32_t bitrate = 0;
    ASSERT_OK(bwe_parse_remb(buf, len, &bitrate));
    /* Due to mantissa encoding, we may lose precision. Check within 1% */
    ASSERT_TRUE(bitrate >= 490000 && bitrate <= 510000);
}

TEST(test_bwe_parse_remb_exact_power_of_two)
{
    /* 1 Mbps = 1000000; mantissa should encode exactly for certain values */
    uint8_t buf[32];
    size_t len = build_remb(buf, sizeof(buf), 0, 262144, 0); /* 2^18 = 262144 */
    ASSERT_TRUE(len > 0);

    uint32_t bitrate = 0;
    ASSERT_OK(bwe_parse_remb(buf, len, &bitrate));
    ASSERT_EQ(bitrate, 262144);
}

TEST(test_bwe_parse_remb_zero)
{
    uint8_t buf[32];
    size_t len = build_remb(buf, sizeof(buf), 0, 0, 0);
    ASSERT_TRUE(len > 0);

    uint32_t bitrate = 0;
    ASSERT_OK(bwe_parse_remb(buf, len, &bitrate));
    ASSERT_EQ(bitrate, 0);
}

TEST(test_bwe_parse_remb_high_bitrate)
{
    /* 10 Mbps */
    uint8_t buf[32];
    size_t len = build_remb(buf, sizeof(buf), 0, 10000000, 0);
    ASSERT_TRUE(len > 0);

    uint32_t bitrate = 0;
    ASSERT_OK(bwe_parse_remb(buf, len, &bitrate));
    /* Within 1% of 10 Mbps due to mantissa truncation */
    ASSERT_TRUE(bitrate >= 9900000 && bitrate <= 10100000);
}

TEST(test_bwe_parse_remb_null_params)
{
    uint8_t buf[32];
    uint32_t bitrate;
    ASSERT_FAIL(bwe_parse_remb(NULL, 24, &bitrate));
    ASSERT_FAIL(bwe_parse_remb(buf, 24, NULL));
}

TEST(test_bwe_parse_remb_too_short)
{
    uint8_t buf[10] = {0};
    uint32_t bitrate;
    ASSERT_FAIL(bwe_parse_remb(buf, 10, &bitrate));
}

TEST(test_bwe_parse_remb_wrong_fmt)
{
    uint8_t buf[32];
    size_t len = build_remb(buf, sizeof(buf), 0, 500000, 0);
    /* Change FMT to 1 (PLI instead of REMB) */
    buf[0] = (2 << 6) | 1;
    uint32_t bitrate;
    ASSERT_FAIL(bwe_parse_remb(buf, len, &bitrate));
}

TEST(test_bwe_parse_remb_wrong_pt)
{
    uint8_t buf[32];
    size_t len = build_remb(buf, sizeof(buf), 0, 500000, 0);
    /* Change PT to 200 (SR instead of PSFB) */
    buf[1] = 200;
    uint32_t bitrate;
    ASSERT_FAIL(bwe_parse_remb(buf, len, &bitrate));
}

TEST(test_bwe_parse_remb_wrong_uid)
{
    uint8_t buf[32];
    size_t len = build_remb(buf, sizeof(buf), 0, 500000, 0);
    /* Corrupt "REMB" identifier */
    buf[12] = 'X';
    uint32_t bitrate;
    ASSERT_FAIL(bwe_parse_remb(buf, len, &bitrate));
}

TEST(test_bwe_parse_remb_bad_version)
{
    uint8_t buf[32];
    size_t len = build_remb(buf, sizeof(buf), 0, 500000, 0);
    /* Change version to 1 */
    buf[0] = (1 << 6) | 15;
    uint32_t bitrate;
    ASSERT_FAIL(bwe_parse_remb(buf, len, &bitrate));
}

/* ================================================================
 * Feedback handler tests (EMA smoothing)
 * ================================================================ */

TEST(test_bwe_feedback_first_remb)
{
    nano_bwe_t bwe;
    bwe_init(&bwe);

    /* First REMB should jump directly to the value (no smoothing) */
    uint8_t buf[32];
    size_t len = build_remb(buf, sizeof(buf), 0, 500000, 0);
    ASSERT_OK(bwe_on_rtcp_feedback(&bwe, buf, len, 1000));

    /* First REMB: estimated should match (within encoding precision) */
    uint32_t est = bwe_get_bitrate(&bwe);
    ASSERT_TRUE(est >= 490000 && est <= 510000);
    ASSERT_EQ(bwe.remb_count, 1);
    ASSERT_EQ(bwe.last_update_ms, 1000);
}

TEST(test_bwe_feedback_smoothing)
{
    nano_bwe_t bwe;
    bwe_init(&bwe);

    /* First REMB: 500 kbps — jumps directly */
    uint8_t buf[32];
    size_t plen = build_remb(buf, sizeof(buf), 0, 500000, 0);
    bwe_on_rtcp_feedback(&bwe, buf, plen, 1000);

    uint32_t first_est = bwe.estimated_bitrate;

    /* Second REMB: 1 Mbps — should be smoothed, not jump to 1M */
    plen = build_remb(buf, sizeof(buf), 0, 1000000, 0);
    bwe_on_rtcp_feedback(&bwe, buf, plen, 2000);

    uint32_t second_est = bwe.estimated_bitrate;
    /* EMA: new = (204 * 1M + 52 * ~500k) / 256 ≈ 900k
     * Should be between first and 1M, closer to 1M (alpha=0.8) */
    ASSERT_TRUE(second_est > first_est);
    ASSERT_TRUE(second_est < 1000000);
    ASSERT_EQ(bwe.remb_count, 2);
}

TEST(test_bwe_feedback_min_clamp)
{
    nano_bwe_t bwe;
    bwe_init(&bwe);

    /* REMB with very low bitrate — should clamp to min */
    uint8_t buf[32];
    size_t plen = build_remb(buf, sizeof(buf), 0, 100, 0);
    bwe_on_rtcp_feedback(&bwe, buf, plen, 1000);

    ASSERT_EQ(bwe.estimated_bitrate, NANORTC_BWE_MIN_BITRATE);
}

TEST(test_bwe_feedback_max_clamp)
{
    nano_bwe_t bwe;
    bwe_init(&bwe);

    /* REMB with very high bitrate — should clamp to max */
    uint8_t buf[32];
    size_t plen = build_remb(buf, sizeof(buf), 0, 100000000, 0);
    bwe_on_rtcp_feedback(&bwe, buf, plen, 1000);

    ASSERT_EQ(bwe.estimated_bitrate, NANORTC_BWE_MAX_BITRATE);
}

TEST(test_bwe_feedback_not_remb)
{
    nano_bwe_t bwe;
    bwe_init(&bwe);

    /* Send a non-REMB packet — should return error, not update state */
    uint8_t buf[32];
    build_remb(buf, sizeof(buf), 0, 500000, 0);
    buf[0] = (2 << 6) | 1; /* Change FMT to PLI */

    int rc = bwe_on_rtcp_feedback(&bwe, buf, 24, 1000);
    ASSERT_FAIL(rc);
    ASSERT_EQ(bwe.remb_count, 0);
    ASSERT_EQ(bwe.estimated_bitrate, NANORTC_BWE_INITIAL_BITRATE);
}

TEST(test_bwe_feedback_null_params)
{
    nano_bwe_t bwe;
    uint8_t buf[32];
    ASSERT_FAIL(bwe_on_rtcp_feedback(NULL, buf, 24, 0));
    ASSERT_FAIL(bwe_on_rtcp_feedback(&bwe, NULL, 24, 0));
}

TEST(test_bwe_get_bitrate_null)
{
    ASSERT_EQ(bwe_get_bitrate(NULL), 0);
}

/* ================================================================
 * Real-world REMB byte vector test
 *
 * Manually crafted REMB for 512000 bps (512 kbps):
 *   mantissa = 512000 = 0x7D000
 *   0x7D000 > 0x3FFFF (18-bit max), so shift: 512000 >> 1 = 256000 = 0x3E800
 *   0x3E800 > 0x3FFFF? No. So exp=1, mantissa=0x3E800
 *
 *   Actually: 512000 = 0b1111101000000000000 (19 bits)
 *   Need 18 bits: 512000 >> 1 = 256000 = 0x3E800, exp=1
 *   Decode: 256000 << 1 = 512000 ✓
 * ================================================================ */

TEST(test_bwe_parse_remb_byte_vector)
{
    /* Hand-crafted REMB: 512 kbps, sender SSRC=1, feedback SSRC=2 */
    uint8_t pkt[] = {
        0xAF, 0xCE, 0x00, 0x05, /* V=2, P=0, FMT=15, PT=206, length=5 */
        0x00, 0x00, 0x00, 0x01, /* Sender SSRC = 1 */
        0x00, 0x00, 0x00, 0x00, /* Media SSRC = 0 */
        0x52, 0x45, 0x4D, 0x42, /* "REMB" */
        0x01, 0x07, 0xA1, 0x20, /* Num=1, Exp=1, Mantissa=0x3A120 (not 0x3E800 — see below) */
        0x00, 0x00, 0x00, 0x02  /* Feedback SSRC = 2 */
    };

    /* Recompute: let's encode 512000 properly.
     * 512000 in binary: need to find exp s.t. mantissa fits 18 bits.
     * 512000 = 0x7D000. Bits needed: 19.
     * Shift by 1: 256000 = 0x3E800. Fits 18 bits. exp=1.
     * Byte 17: (1 << 2) | (0x3E800 >> 16 & 0x03) = 4 | 3 = 7 → 0x07
     * Byte 18: (0x3E800 >> 8) & 0xFF = 0xE8
     * Byte 19: 0x3E800 & 0xFF = 0x00 */
    pkt[17] = 0x07; /* exp=1 << 2 | mantissa_hi=3 */
    pkt[18] = 0xE8; /* mantissa_mid */
    pkt[19] = 0x00; /* mantissa_lo */

    uint32_t bitrate = 0;
    ASSERT_OK(bwe_parse_remb(pkt, sizeof(pkt), &bitrate));
    ASSERT_EQ(bitrate, 512000);
}

/* ================================================================
 * Event threshold tests
 * ================================================================ */

TEST(test_bwe_event_threshold_large_change)
{
    nano_bwe_t bwe;
    bwe_init(&bwe);

    /* First REMB: jump to 500 kbps */
    uint8_t buf[32];
    size_t plen = build_remb(buf, sizeof(buf), 0, 500000, 0);
    bwe_on_rtcp_feedback(&bwe, buf, plen, 1000);

    /* prev_event_bitrate was set to initial (300k), now estimated ~500k.
     * Change is ~67% — well above 15% threshold */
    ASSERT_TRUE(bwe_should_emit_event(&bwe));

    /* Immediately after: prev_event_bitrate updated, no change → no event */
    ASSERT_FALSE(bwe_should_emit_event(&bwe));
}

TEST(test_bwe_event_threshold_small_change)
{
    nano_bwe_t bwe;
    bwe_init(&bwe);

    /* Set to 500k */
    uint8_t buf[32];
    size_t plen = build_remb(buf, sizeof(buf), 0, 500000, 0);
    bwe_on_rtcp_feedback(&bwe, buf, plen, 1000);
    /* Consume the initial event */
    bwe_should_emit_event(&bwe);

    /* Feed 510k — only ~2% change, below 15% threshold */
    plen = build_remb(buf, sizeof(buf), 0, 510000, 0);
    bwe_on_rtcp_feedback(&bwe, buf, plen, 2000);
    ASSERT_FALSE(bwe_should_emit_event(&bwe));
}

TEST(test_bwe_event_threshold_decrease)
{
    nano_bwe_t bwe;
    bwe_init(&bwe);

    /* Set to 1 Mbps */
    uint8_t buf[32];
    size_t plen = build_remb(buf, sizeof(buf), 0, 1000000, 0);
    bwe_on_rtcp_feedback(&bwe, buf, plen, 1000);
    bwe_should_emit_event(&bwe); /* consume */

    /* Drop to 500k — 50% decrease, should trigger */
    plen = build_remb(buf, sizeof(buf), 0, 500000, 0);
    bwe_on_rtcp_feedback(&bwe, buf, plen, 2000);
    /* After EMA smoothing, won't drop to 500k immediately but still >15% change */
    ASSERT_TRUE(bwe_should_emit_event(&bwe));
}

TEST(test_bwe_event_null)
{
    ASSERT_FALSE(bwe_should_emit_event(NULL));
}

/* ================================================================
 * Public API tests
 * ================================================================ */

TEST(test_bwe_public_get_estimated_bitrate)
{
    /* nanortc_get_estimated_bitrate is only available with VIDEO feature.
     * Here we just verify bwe_get_bitrate (the underlying getter) works
     * correctly after init and after feedback. */
    nano_bwe_t bwe;
    bwe_init(&bwe);
    ASSERT_EQ(bwe_get_bitrate(&bwe), NANORTC_BWE_INITIAL_BITRATE);

    uint8_t buf[32];
    size_t plen = build_remb(buf, sizeof(buf), 0, 800000, 0);
    bwe_on_rtcp_feedback(&bwe, buf, plen, 1000);

    uint32_t est = bwe_get_bitrate(&bwe);
    ASSERT_TRUE(est >= 780000 && est <= 820000);
}

TEST(test_bwe_init_sets_prev_event_bitrate)
{
    nano_bwe_t bwe;
    bwe_init(&bwe);
    ASSERT_EQ(bwe.prev_event_bitrate, NANORTC_BWE_INITIAL_BITRATE);
}

/* ================================================================
 * Runner
 * ================================================================ */

TEST_MAIN_BEGIN("BWE bandwidth estimation tests")
RUN(test_bwe_init);
RUN(test_bwe_init_null);
RUN(test_bwe_parse_remb_basic);
RUN(test_bwe_parse_remb_exact_power_of_two);
RUN(test_bwe_parse_remb_zero);
RUN(test_bwe_parse_remb_high_bitrate);
RUN(test_bwe_parse_remb_null_params);
RUN(test_bwe_parse_remb_too_short);
RUN(test_bwe_parse_remb_wrong_fmt);
RUN(test_bwe_parse_remb_wrong_pt);
RUN(test_bwe_parse_remb_wrong_uid);
RUN(test_bwe_parse_remb_bad_version);
RUN(test_bwe_parse_remb_byte_vector);
RUN(test_bwe_feedback_first_remb);
RUN(test_bwe_feedback_smoothing);
RUN(test_bwe_feedback_min_clamp);
RUN(test_bwe_feedback_max_clamp);
RUN(test_bwe_feedback_not_remb);
RUN(test_bwe_feedback_null_params);
RUN(test_bwe_get_bitrate_null);
RUN(test_bwe_event_threshold_large_change);
RUN(test_bwe_event_threshold_small_change);
RUN(test_bwe_event_threshold_decrease);
RUN(test_bwe_event_null);
RUN(test_bwe_public_get_estimated_bitrate);
RUN(test_bwe_init_sets_prev_event_bitrate);
TEST_MAIN_END
