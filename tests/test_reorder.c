/*
 * nanortc — Video receive reorder buffer tests (NANORTC_FEATURE_VIDEO_REORDER)
 *
 * Drives src/nano_reorder.c in isolation with deterministic sequences:
 *   - in-order passthrough (no loss),
 *   - reorder healing (a swapped pair is delivered in order, no loss),
 *   - late/duplicate drop,
 *   - timeout skip (a missing packet is declared lost after the latency cap),
 *   - far-future force-advance (a jump past the window declares the gap lost),
 *   - 16-bit sequence wraparound (incl. a reorder across 0xFFFF→0x0000).
 *
 * SPDX-License-Identifier: MIT
 */

#include "nano_reorder.h"
#include "nanortc.h"
#include "nano_test.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#if NANORTC_FEATURE_VIDEO_REORDER

static nano_reorder_t g_r; /* ~10 KB — keep off the stack */

/* Push a packet whose payload encodes its seq, and ts = seq*1000, so pops can
 * be content-verified. */
static int rpush(uint16_t seq, uint32_t now)
{
    uint8_t buf[4];
    buf[0] = (uint8_t)seq;
    buf[1] = (uint8_t)(seq >> 8);
    buf[2] = 0xAB;
    buf[3] = 0xCD;
    return reorder_push(&g_r, seq, (uint32_t)seq * 1000u, (uint8_t)(seq & 1u), buf, sizeof(buf),
                        now);
}

/* Pop and content-verify; returns the rc, sets *seq / *lost on NANORTC_OK. */
static int rpop(uint32_t now, uint16_t *seq, bool *lost)
{
    uint32_t ts = 0;
    uint8_t mk = 0;
    const uint8_t *d = NULL;
    size_t len = 0;
    int rc = reorder_pop(&g_r, now, seq, &ts, &mk, &d, &len, lost);
    if (rc == NANORTC_OK) {
        ASSERT_EQ(len, (size_t)4);
        ASSERT_EQ(d[0], (uint8_t)(*seq));
        ASSERT_EQ(d[1], (uint8_t)((*seq) >> 8));
        ASSERT_EQ(ts, (uint32_t)(*seq) * 1000u);
        ASSERT_EQ(mk, (uint8_t)((*seq) & 1u));
    }
    return rc;
}

TEST(test_reorder_in_order_passthrough)
{
    reorder_init(&g_r);
    ASSERT_OK(rpush(100, 0));
    ASSERT_OK(rpush(101, 0));
    ASSERT_OK(rpush(102, 0));

    uint16_t s;
    bool lost;
    ASSERT_EQ(rpop(0, &s, &lost), NANORTC_OK);
    ASSERT_TRUE(s == 100 && !lost);
    ASSERT_EQ(rpop(0, &s, &lost), NANORTC_OK);
    ASSERT_TRUE(s == 101 && !lost);
    ASSERT_EQ(rpop(0, &s, &lost), NANORTC_OK);
    ASSERT_TRUE(s == 102 && !lost);
    ASSERT_EQ(rpop(0, &s, &lost), NANORTC_ERR_NO_DATA);
}

TEST(test_reorder_heals_swap_no_loss)
{
    reorder_init(&g_r);
    uint16_t s;
    bool lost;

    ASSERT_OK(rpush(100, 0));
    ASSERT_EQ(rpop(0, &s, &lost), NANORTC_OK);
    ASSERT_TRUE(s == 100 && !lost);

    /* 102 arrives before 101 — held, not delivered, no loss declared. */
    ASSERT_OK(rpush(102, 0));
    ASSERT_EQ(rpop(0, &s, &lost), NANORTC_ERR_NO_DATA);

    /* 101 fills the gap → both released in order with no loss. */
    ASSERT_OK(rpush(101, 0));
    ASSERT_EQ(rpop(0, &s, &lost), NANORTC_OK);
    ASSERT_TRUE(s == 101 && !lost);
    ASSERT_EQ(rpop(0, &s, &lost), NANORTC_OK);
    ASSERT_TRUE(s == 102 && !lost);
    ASSERT_EQ(rpop(0, &s, &lost), NANORTC_ERR_NO_DATA);
}

TEST(test_reorder_drops_late_duplicate)
{
    reorder_init(&g_r);
    uint16_t s;
    bool lost;
    ASSERT_OK(rpush(100, 0));
    ASSERT_EQ(rpop(0, &s, &lost), NANORTC_OK);
    ASSERT_OK(rpush(101, 0));
    ASSERT_EQ(rpop(0, &s, &lost), NANORTC_OK);

    /* 100 is now behind next_seq (102) — dropped, nothing to pop. */
    ASSERT_OK(rpush(100, 0));
    ASSERT_EQ(rpop(0, &s, &lost), NANORTC_ERR_NO_DATA);
}

TEST(test_reorder_timeout_skip_declares_loss)
{
    reorder_init(&g_r);
    uint16_t s;
    bool lost;
    ASSERT_OK(rpush(100, 10));
    ASSERT_EQ(rpop(10, &s, &lost), NANORTC_OK); /* next_seq = 101 */

    /* 102 arrives at t=10; 101 is missing. Hold until the cap elapses. */
    ASSERT_OK(rpush(102, 10));
    ASSERT_EQ(rpop(10, &s, &lost), NANORTC_ERR_NO_DATA);
    ASSERT_EQ(rpop(10 + NANORTC_VIDEO_REORDER_MAX_WAIT_MS - 1, &s, &lost), NANORTC_ERR_NO_DATA);

    /* At the cap, 101 is skipped (lost) and 102 is released with lost=true. */
    ASSERT_EQ(rpop(10 + NANORTC_VIDEO_REORDER_MAX_WAIT_MS, &s, &lost), NANORTC_OK);
    ASSERT_TRUE(s == 102 && lost);
    ASSERT_EQ(rpop(10 + NANORTC_VIDEO_REORDER_MAX_WAIT_MS, &s, &lost), NANORTC_ERR_NO_DATA);
}

TEST(test_reorder_far_future_force_advance)
{
    reorder_init(&g_r);
    uint16_t s;
    bool lost;
    ASSERT_OK(rpush(100, 0));
    ASSERT_EQ(rpop(0, &s, &lost), NANORTC_OK); /* next_seq = 101 */

    /* A packet far beyond the window force-advances next_seq; the jumped span is
     * lost. It is still held until the skip-over packets age out the cap. */
    uint16_t far = (uint16_t)(101 + NANORTC_VIDEO_REORDER_SLOTS + 5);
    ASSERT_OK(rpush(far, 0));
    ASSERT_EQ(rpop(0, &s, &lost), NANORTC_ERR_NO_DATA);
    ASSERT_EQ(rpop(NANORTC_VIDEO_REORDER_MAX_WAIT_MS, &s, &lost), NANORTC_OK);
    ASSERT_TRUE(s == far && lost);
}

TEST(test_reorder_seq_wraparound)
{
    reorder_init(&g_r);
    uint16_t s;
    bool lost;

    /* Straight run across the 0xFFFF → 0x0000 boundary. */
    ASSERT_OK(rpush(0xFFFE, 0));
    ASSERT_EQ(rpop(0, &s, &lost), NANORTC_OK);
    ASSERT_TRUE(s == 0xFFFE && !lost);
    ASSERT_OK(rpush(0xFFFF, 0));
    ASSERT_EQ(rpop(0, &s, &lost), NANORTC_OK);
    ASSERT_TRUE(s == 0xFFFF && !lost);

    /* Reorder across the wrap: 0x0001 before 0x0000, then healed. */
    ASSERT_OK(rpush(0x0001, 0));
    ASSERT_EQ(rpop(0, &s, &lost), NANORTC_ERR_NO_DATA);
    ASSERT_OK(rpush(0x0000, 0));
    ASSERT_EQ(rpop(0, &s, &lost), NANORTC_OK);
    ASSERT_TRUE(s == 0x0000 && !lost);
    ASSERT_EQ(rpop(0, &s, &lost), NANORTC_OK);
    ASSERT_TRUE(s == 0x0001 && !lost);
}

#endif /* NANORTC_FEATURE_VIDEO_REORDER */

TEST_MAIN_BEGIN("nanortc video reorder tests")
#if NANORTC_FEATURE_VIDEO_REORDER
RUN(test_reorder_in_order_passthrough);
RUN(test_reorder_heals_swap_no_loss);
RUN(test_reorder_drops_late_duplicate);
RUN(test_reorder_timeout_skip_declares_loss);
RUN(test_reorder_far_future_force_advance);
RUN(test_reorder_seq_wraparound);
#endif
TEST_MAIN_END
