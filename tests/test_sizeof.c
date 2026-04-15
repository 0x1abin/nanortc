/*
 * nanortc — Struct size regression tests
 *
 * Ensures key structs don't accidentally grow beyond expected limits.
 * Run as part of CI to catch unintended size regressions early.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include "unity.h"
#include "nanortc.h"

/* Limits are generous to allow minor additions without breaking CI,
 * but tight enough to catch accidental large buffer/array additions.
 * Values are for the host (64-bit) build; 32-bit ARM will be smaller. */

void setUp(void)
{
}
void tearDown(void)
{
}

static void test_sizeof_ice(void)
{
    TEST_ASSERT_LESS_OR_EQUAL_size_t(600, sizeof(nano_ice_t));
}

static void test_sizeof_dtls(void)
{
    TEST_ASSERT_LESS_OR_EQUAL_size_t(7000, sizeof(nano_dtls_t));
}

static void test_sizeof_sdp(void)
{
#if NANORTC_FEATURE_H265
    /* Per-mline overhead: sprop scratch (NANORTC_H265_SPROP_FMTP_SIZE) +
     * rtpmap_pt (1 B) + sprop_len (2 B) + struct padding. 16 B covers
     * alignment up to the next 8-B boundary on 64-bit targets. */
    const size_t h265_mline_overhead = NANORTC_H265_SPROP_FMTP_SIZE + 16;
    TEST_ASSERT_LESS_OR_EQUAL_size_t(1500 + h265_mline_overhead * NANORTC_MAX_MEDIA_TRACKS,
                                     sizeof(nano_sdp_t));
#else
    TEST_ASSERT_LESS_OR_EQUAL_size_t(1500, sizeof(nano_sdp_t));
#endif
}

#if NANORTC_FEATURE_TURN
static void test_sizeof_turn(void)
{
    TEST_ASSERT_LESS_OR_EQUAL_size_t(800, sizeof(nano_turn_t));
}
#endif

#if NANORTC_FEATURE_DATACHANNEL
static void test_sizeof_sctp(void)
{
    TEST_ASSERT_LESS_OR_EQUAL_size_t(15000, sizeof(nano_sctp_t));
}

static void test_sizeof_dc(void)
{
    TEST_ASSERT_LESS_OR_EQUAL_size_t(600, sizeof(nano_dc_t));
}
#endif

#if NANORTC_HAVE_MEDIA_TRANSPORT
static void test_sizeof_track(void)
{
    TEST_ASSERT_LESS_OR_EQUAL_size_t(50000, sizeof(nanortc_track_t));
}
#endif

static void test_sizeof_output(void)
{
    TEST_ASSERT_LESS_OR_EQUAL_size_t(80, sizeof(nanortc_output_t));
}

static void test_sizeof_stun_msg(void)
{
    TEST_ASSERT_LESS_OR_EQUAL_size_t(256, sizeof(stun_msg_t));
}

int main(void)
{
    UNITY_BEGIN();
    printf("nanortc sizeof tests (DC=%d AUDIO=%d VIDEO=%d TURN=%d)\n", NANORTC_FEATURE_DATACHANNEL,
#if NANORTC_FEATURE_AUDIO
           1,
#else
           0,
#endif
#if NANORTC_FEATURE_VIDEO
           1,
#else
           0,
#endif
           NANORTC_FEATURE_TURN);

    RUN_TEST(test_sizeof_ice);
    RUN_TEST(test_sizeof_dtls);
    RUN_TEST(test_sizeof_sdp);
#if NANORTC_FEATURE_TURN
    RUN_TEST(test_sizeof_turn);
#endif
#if NANORTC_FEATURE_DATACHANNEL
    RUN_TEST(test_sizeof_sctp);
    RUN_TEST(test_sizeof_dc);
#endif
#if NANORTC_HAVE_MEDIA_TRANSPORT
    RUN_TEST(test_sizeof_track);
#endif
    RUN_TEST(test_sizeof_output);
    RUN_TEST(test_sizeof_stun_msg);

    return UNITY_END();
}
