/*
 * nanortc — STUN codec tests (RFC 8489)
 *
 * Stub for Phase 1 TDD. Add test vectors from RFC appendix
 * and browser pcap captures.
 *
 * SPDX-License-Identifier: MIT
 */

#include "nanortc.h"
#include "nano_stun.h"
#include "nano_test.h"
#include <string.h>

/* ---- Tests ---- */

TEST(test_stun_header_size)
{
    ASSERT_EQ(STUN_HEADER_SIZE, 20);
    ASSERT_EQ(STUN_MAGIC_COOKIE, 0x2112A442);
}

TEST(test_stun_parse_too_short)
{
    uint8_t data[10] = {0};
    stun_msg_t msg;
    /* Should fail: buffer too short for STUN header */
    ASSERT_FAIL(stun_parse(data, sizeof(data), &msg));
}

/* ---- Runner ---- */

TEST_MAIN_BEGIN("nanortc STUN tests")
    RUN(test_stun_header_size);
    RUN(test_stun_parse_too_short);
TEST_MAIN_END
