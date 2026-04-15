/*
 * nanortc — Annex-B bitstream scanner tests
 *
 * Exercises nano_annex_b_find_nal() across 3-byte and 4-byte start codes,
 * multi-NAL walks via the in/out offset cursor, trailing zero-padding strip,
 * NULL parameter guards, and short / no-start-code buffers.
 *
 * SPDX-License-Identifier: MIT
 */

#include "nano_annex_b.h"
#include "nano_test.h"
#include "nanortc.h"
#include <stddef.h>
#include <stdint.h>

TEST(test_annex_b_null_guards)
{
    uint8_t buf[4] = {0, 0, 0, 1};
    size_t offset = 0;
    size_t nal_len = 0;

    ASSERT_TRUE(nano_annex_b_find_nal(NULL, sizeof(buf), &offset, &nal_len) == NULL);
    ASSERT_TRUE(nano_annex_b_find_nal(buf, sizeof(buf), NULL, &nal_len) == NULL);
    ASSERT_TRUE(nano_annex_b_find_nal(buf, sizeof(buf), &offset, NULL) == NULL);
}

TEST(test_annex_b_four_byte_start_code)
{
    /* 00 00 00 01 | AA BB CC */
    uint8_t buf[] = {0x00, 0x00, 0x00, 0x01, 0xAA, 0xBB, 0xCC};
    size_t offset = 0;
    size_t nal_len = 0;

    const uint8_t *nal = nano_annex_b_find_nal(buf, sizeof(buf), &offset, &nal_len);
    ASSERT_TRUE(nal != NULL);
    ASSERT_EQ(nal[0], 0xAA);
    ASSERT_EQ(nal[1], 0xBB);
    ASSERT_EQ(nal[2], 0xCC);
    ASSERT_EQ(nal_len, 3u);
    ASSERT_EQ(offset, sizeof(buf));
}

TEST(test_annex_b_three_byte_start_code)
{
    /* 00 00 01 | AA BB CC */
    uint8_t buf[] = {0x00, 0x00, 0x01, 0xAA, 0xBB, 0xCC};
    size_t offset = 0;
    size_t nal_len = 0;

    const uint8_t *nal = nano_annex_b_find_nal(buf, sizeof(buf), &offset, &nal_len);
    ASSERT_TRUE(nal != NULL);
    ASSERT_EQ(nal[0], 0xAA);
    ASSERT_EQ(nal[1], 0xBB);
    ASSERT_EQ(nal[2], 0xCC);
    ASSERT_EQ(nal_len, 3u);
    ASSERT_EQ(offset, sizeof(buf));
}

TEST(test_annex_b_two_nals_walk)
{
    /* 00 00 00 01 | AA BB | 00 00 01 | CC DD
     * First NAL via 4-byte start code, second NAL via 3-byte start code.
     * Walk both with a single advancing offset cursor. */
    uint8_t buf[] = {
        0x00, 0x00, 0x00, 0x01, 0xAA, 0xBB,
        0x00, 0x00, 0x01,       0xCC, 0xDD,
    };
    size_t offset = 0;
    size_t nal_len = 0;

    const uint8_t *nal1 = nano_annex_b_find_nal(buf, sizeof(buf), &offset, &nal_len);
    ASSERT_TRUE(nal1 != NULL);
    ASSERT_EQ(nal1[0], 0xAA);
    ASSERT_EQ(nal1[1], 0xBB);
    ASSERT_EQ(nal_len, 2u);

    const uint8_t *nal2 = nano_annex_b_find_nal(buf, sizeof(buf), &offset, &nal_len);
    ASSERT_TRUE(nal2 != NULL);
    ASSERT_EQ(nal2[0], 0xCC);
    ASSERT_EQ(nal2[1], 0xDD);
    ASSERT_EQ(nal_len, 2u);
    ASSERT_EQ(offset, sizeof(buf));

    const uint8_t *nal3 = nano_annex_b_find_nal(buf, sizeof(buf), &offset, &nal_len);
    ASSERT_TRUE(nal3 == NULL);
}

TEST(test_annex_b_trailing_zero_padding)
{
    /* 00 00 00 01 | AA BB | 00 00 00 (trailing pad) */
    uint8_t buf[] = {0x00, 0x00, 0x00, 0x01, 0xAA, 0xBB, 0x00, 0x00, 0x00};
    size_t offset = 0;
    size_t nal_len = 0;

    const uint8_t *nal = nano_annex_b_find_nal(buf, sizeof(buf), &offset, &nal_len);
    ASSERT_TRUE(nal != NULL);
    ASSERT_EQ(nal[0], 0xAA);
    ASSERT_EQ(nal[1], 0xBB);
    ASSERT_EQ(nal_len, 2u); /* trailing zeros stripped */
}

TEST(test_annex_b_only_zeros_after_first_nal)
{
    /* 00 00 00 01 | AA | 00 00 (clamp + strip on single-byte NAL) */
    uint8_t buf[] = {0x00, 0x00, 0x00, 0x01, 0xAA, 0x00, 0x00};
    size_t offset = 0;
    size_t nal_len = 0;

    const uint8_t *nal = nano_annex_b_find_nal(buf, sizeof(buf), &offset, &nal_len);
    ASSERT_TRUE(nal != NULL);
    ASSERT_EQ(nal[0], 0xAA);
    ASSERT_EQ(nal_len, 1u);
    ASSERT_EQ(offset, sizeof(buf));
}

TEST(test_annex_b_start_code_at_buffer_end)
{
    /* AA | 00 00 00 01 — start code consumes the rest of the buffer and
     * leaves no payload behind. The scanner advances past the start code,
     * hits i == len, and returns NULL via the early-return guard. */
    uint8_t buf[] = {0xAA, 0x00, 0x00, 0x00, 0x01};
    size_t offset = 0;
    size_t nal_len = 0;

    const uint8_t *nal = nano_annex_b_find_nal(buf, sizeof(buf), &offset, &nal_len);
    ASSERT_TRUE(nal == NULL);
}

TEST(test_annex_b_three_then_four_byte)
{
    /* 00 00 01 | AA | 00 00 00 01 | BB
     * Validates that the next-start-code search inside the first NAL hits
     * the 4-byte sub-condition. */
    uint8_t buf[] = {
        0x00, 0x00, 0x01,             0xAA,
        0x00, 0x00, 0x00, 0x01,       0xBB,
    };
    size_t offset = 0;
    size_t nal_len = 0;

    const uint8_t *nal1 = nano_annex_b_find_nal(buf, sizeof(buf), &offset, &nal_len);
    ASSERT_TRUE(nal1 != NULL);
    ASSERT_EQ(nal1[0], 0xAA);
    ASSERT_EQ(nal_len, 1u);

    const uint8_t *nal2 = nano_annex_b_find_nal(buf, sizeof(buf), &offset, &nal_len);
    ASSERT_TRUE(nal2 != NULL);
    ASSERT_EQ(nal2[0], 0xBB);
    ASSERT_EQ(nal_len, 1u);
    ASSERT_EQ(offset, sizeof(buf));
}

TEST_MAIN_BEGIN("test_annex_b")
RUN(test_annex_b_null_guards);
RUN(test_annex_b_four_byte_start_code);
RUN(test_annex_b_three_byte_start_code);
RUN(test_annex_b_two_nals_walk);
RUN(test_annex_b_trailing_zero_padding);
RUN(test_annex_b_only_zeros_after_first_nal);
RUN(test_annex_b_start_code_at_buffer_end);
RUN(test_annex_b_three_then_four_byte);
TEST_MAIN_END
