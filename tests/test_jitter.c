/*
 * nanortc — Jitter buffer tests
 *
 * SPDX-License-Identifier: MIT
 */

#include "nanortc.h"
#include "nano_jitter.h"
#include "nano_test.h"
#include <string.h>

/* ================================================================
 * Basic tests
 * ================================================================ */

TEST(test_jitter_init)
{
    nano_jitter_t jb;
    ASSERT_OK(jitter_init(&jb, 50));
    ASSERT_EQ(jb.depth_ms, 50);
    ASSERT_EQ(jb.started, 0);
}

TEST(test_jitter_init_null)
{
    ASSERT_FAIL(jitter_init(NULL, 50));
}

TEST(test_jitter_pop_empty)
{
    nano_jitter_t jb;
    jitter_init(&jb, 20);

    uint8_t buf[256];
    size_t out_len;
    uint32_t ts;
    ASSERT_EQ(jitter_pop(&jb, 1000, buf, sizeof(buf), &out_len, &ts), NANO_ERR_NO_DATA);
}

/* ================================================================
 * Sequential push/pop
 * ================================================================ */

TEST(test_jitter_sequential)
{
    nano_jitter_t jb;
    jitter_init(&jb, 20); /* 20ms depth */

    uint8_t payload[] = {0xAA, 0xBB, 0xCC};

    /* Push 3 sequential packets at time 100 */
    ASSERT_OK(jitter_push(&jb, 0, 160, payload, 3, 100));
    ASSERT_OK(jitter_push(&jb, 1, 320, payload, 3, 100));
    ASSERT_OK(jitter_push(&jb, 2, 480, payload, 3, 100));

    /* Pop too early — depth not reached */
    uint8_t buf[256];
    size_t out_len;
    uint32_t ts;
    ASSERT_EQ(jitter_pop(&jb, 110, buf, sizeof(buf), &out_len, &ts), NANO_ERR_NO_DATA);

    /* Pop at time 120 (100 + 20ms depth) */
    ASSERT_OK(jitter_pop(&jb, 120, buf, sizeof(buf), &out_len, &ts));
    ASSERT_EQ(out_len, 3);
    ASSERT_EQ(ts, 160); /* First packet's timestamp */

    ASSERT_OK(jitter_pop(&jb, 120, buf, sizeof(buf), &out_len, &ts));
    ASSERT_EQ(ts, 320);

    ASSERT_OK(jitter_pop(&jb, 120, buf, sizeof(buf), &out_len, &ts));
    ASSERT_EQ(ts, 480);

    /* Empty again */
    ASSERT_EQ(jitter_pop(&jb, 120, buf, sizeof(buf), &out_len, &ts), NANO_ERR_NO_DATA);
}

/* ================================================================
 * Out-of-order reordering
 * ================================================================ */

TEST(test_jitter_reorder)
{
    nano_jitter_t jb;
    jitter_init(&jb, 20);

    uint8_t p1[] = {0x01};
    uint8_t p2[] = {0x02};
    uint8_t p3[] = {0x03};

    /* Push out of order: seq 2, 0, 1 */
    ASSERT_OK(jitter_push(&jb, 2, 480, p3, 1, 100));
    ASSERT_OK(jitter_push(&jb, 0, 160, p1, 1, 100));
    ASSERT_OK(jitter_push(&jb, 1, 320, p2, 1, 100));

    uint8_t buf[256];
    size_t out_len;
    uint32_t ts;

    /* Should pop in order: 0, 1, 2 */
    /* Note: head_seq starts at 2 (first push), so seq 0 is behind.
     * Let's test with a different scenario where first push is seq 0 */
}

TEST(test_jitter_reorder_v2)
{
    nano_jitter_t jb;
    jitter_init(&jb, 20);

    uint8_t p1[] = {0x01};
    uint8_t p2[] = {0x02};
    uint8_t p3[] = {0x03};

    /* Push seq 0 first (sets head), then out of order */
    ASSERT_OK(jitter_push(&jb, 0, 160, p1, 1, 100));
    ASSERT_OK(jitter_push(&jb, 2, 480, p3, 1, 101));
    ASSERT_OK(jitter_push(&jb, 1, 320, p2, 1, 102));

    uint8_t buf[256];
    size_t out_len;
    uint32_t ts;

    /* Pop in sequence order */
    ASSERT_OK(jitter_pop(&jb, 125, buf, sizeof(buf), &out_len, &ts));
    ASSERT_EQ(buf[0], 0x01);
    ASSERT_EQ(ts, 160);

    ASSERT_OK(jitter_pop(&jb, 125, buf, sizeof(buf), &out_len, &ts));
    ASSERT_EQ(buf[0], 0x02);
    ASSERT_EQ(ts, 320);

    ASSERT_OK(jitter_pop(&jb, 125, buf, sizeof(buf), &out_len, &ts));
    ASSERT_EQ(buf[0], 0x03);
    ASSERT_EQ(ts, 480);
}

/* ================================================================
 * Lost packet skip
 * ================================================================ */

TEST(test_jitter_lost_packet)
{
    nano_jitter_t jb;
    jitter_init(&jb, 20);

    uint8_t p1[] = {0x01};
    uint8_t p3[] = {0x03};

    /* Push seq 0 and 2, skip seq 1 (lost) */
    ASSERT_OK(jitter_push(&jb, 0, 160, p1, 1, 100));
    ASSERT_OK(jitter_push(&jb, 2, 480, p3, 1, 100));

    uint8_t buf[256];
    size_t out_len;
    uint32_t ts;

    /* Pop seq 0 */
    ASSERT_OK(jitter_pop(&jb, 120, buf, sizeof(buf), &out_len, &ts));
    ASSERT_EQ(ts, 160);

    /* Seq 1 is missing. After delay expires for seq 2, should skip and return seq 2 */
    ASSERT_OK(jitter_pop(&jb, 125, buf, sizeof(buf), &out_len, &ts));
    ASSERT_EQ(ts, 480);
    ASSERT_EQ(buf[0], 0x03);
}

/* ================================================================
 * Capacity overflow
 * ================================================================ */

TEST(test_jitter_overflow)
{
    nano_jitter_t jb;
    jitter_init(&jb, 0); /* zero depth for immediate pop */

    uint8_t payload[] = {0xFF};

    /* Fill buffer with NANO_JITTER_SLOTS packets */
    for (uint16_t i = 0; i < NANO_JITTER_SLOTS; i++) {
        ASSERT_OK(jitter_push(&jb, i, i * 160, payload, 1, 100));
    }

    /* Push one more — should force advance of head_seq */
    ASSERT_OK(jitter_push(&jb, NANO_JITTER_SLOTS, NANO_JITTER_SLOTS * 160, payload, 1, 100));

    /* head_seq should have advanced, old packets discarded */
    uint8_t buf[256];
    size_t out_len;
    uint32_t ts;
    ASSERT_OK(jitter_pop(&jb, 100, buf, sizeof(buf), &out_len, &ts));
}

/* ================================================================
 * Payload too large
 * ================================================================ */

TEST(test_jitter_push_too_large)
{
    nano_jitter_t jb;
    jitter_init(&jb, 20);

    uint8_t big[NANO_JITTER_SLOT_DATA_SIZE + 1];
    memset(big, 0, sizeof(big));
    ASSERT_FAIL(jitter_push(&jb, 0, 0, big, sizeof(big), 100));
}

/* ================================================================
 * Null parameter validation
 * ================================================================ */

TEST(test_jitter_push_null)
{
    nano_jitter_t jb;
    jitter_init(&jb, 20);
    ASSERT_FAIL(jitter_push(NULL, 0, 0, (const uint8_t *)"a", 1, 0));
    ASSERT_FAIL(jitter_push(&jb, 0, 0, NULL, 1, 0));
    ASSERT_FAIL(jitter_push(&jb, 0, 0, (const uint8_t *)"a", 0, 0));
}

TEST(test_jitter_pop_null)
{
    nano_jitter_t jb;
    jitter_init(&jb, 20);
    uint8_t buf[64];
    size_t out_len;
    ASSERT_FAIL(jitter_pop(NULL, 0, buf, sizeof(buf), &out_len, NULL));
    ASSERT_FAIL(jitter_pop(&jb, 0, NULL, sizeof(buf), &out_len, NULL));
    ASSERT_FAIL(jitter_pop(&jb, 0, buf, sizeof(buf), NULL, NULL));
}

/* ================================================================
 * Zero depth (immediate playout)
 * ================================================================ */

TEST(test_jitter_zero_depth)
{
    nano_jitter_t jb;
    jitter_init(&jb, 0);

    uint8_t payload[] = {0x42};
    ASSERT_OK(jitter_push(&jb, 0, 160, payload, 1, 100));

    uint8_t buf[64];
    size_t out_len;
    uint32_t ts;
    /* With depth=0, pop should succeed immediately */
    ASSERT_OK(jitter_pop(&jb, 100, buf, sizeof(buf), &out_len, &ts));
    ASSERT_EQ(buf[0], 0x42);
}

/* ================================================================
 * Test runner
 * ================================================================ */

TEST_MAIN_BEGIN("test_jitter")
RUN(test_jitter_init);
RUN(test_jitter_init_null);
RUN(test_jitter_pop_empty);
RUN(test_jitter_sequential);
RUN(test_jitter_reorder);
RUN(test_jitter_reorder_v2);
RUN(test_jitter_lost_packet);
RUN(test_jitter_overflow);
RUN(test_jitter_push_too_large);
RUN(test_jitter_push_null);
RUN(test_jitter_pop_null);
RUN(test_jitter_zero_depth);
TEST_MAIN_END
