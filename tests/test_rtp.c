/*
 * nanortc — RTP pack/unpack tests (RFC 3550)
 *
 * SPDX-License-Identifier: MIT
 */

#include "nanortc.h"
#include "nano_rtp.h"
#include "nano_test.h"
#include <string.h>

/* ---- Hand-crafted byte vector ---- */

/*
 * RTP packet: V=2, P=0, X=0, CC=0, M=0, PT=111, seq=1234, ts=0x12345678,
 *             SSRC=0xDEADBEEF, payload="hello"
 */
static const uint8_t rtp_vector[] = {
    0x80,       /* V=2, P=0, X=0, CC=0 */
    0x6F,       /* M=0, PT=111 */
    0x04, 0xD2, /* seq=1234 */
    0x12, 0x34, 0x56, 0x78, /* timestamp */
    0xDE, 0xAD, 0xBE, 0xEF, /* SSRC */
    'h', 'e', 'l', 'l', 'o' /* payload */
};

/* RTP packet with marker bit set */
static const uint8_t rtp_marker_vector[] = {
    0x80,       /* V=2, P=0, X=0, CC=0 */
    0xEF,       /* M=1, PT=111 */
    0x00, 0x01, /* seq=1 */
    0x00, 0x00, 0x03, 0xE8, /* timestamp=1000 */
    0x00, 0x00, 0x00, 0x01, /* SSRC=1 */
    0xAA, 0xBB  /* payload */
};

/* ---- Tests ---- */

TEST(test_rtp_unpack_vector)
{
    uint8_t pt;
    uint16_t seq;
    uint32_t ts, ssrc;
    const uint8_t *payload;
    size_t payload_len;

    ASSERT_OK(rtp_unpack(rtp_vector, sizeof(rtp_vector),
                         &pt, &seq, &ts, &ssrc, &payload, &payload_len));
    ASSERT_EQ(pt, 111);
    ASSERT_EQ(seq, 1234);
    ASSERT_EQ(ts, 0x12345678);
    ASSERT_EQ(ssrc, 0xDEADBEEF);
    ASSERT_EQ(payload_len, 5);
    ASSERT_MEM_EQ(payload, "hello", 5);
}

TEST(test_rtp_unpack_marker)
{
    uint8_t pt;
    uint16_t seq;
    uint32_t ts, ssrc;
    const uint8_t *payload;
    size_t payload_len;

    ASSERT_OK(rtp_unpack(rtp_marker_vector, sizeof(rtp_marker_vector),
                         &pt, &seq, &ts, &ssrc, &payload, &payload_len));
    ASSERT_EQ(pt, 111);
    ASSERT_EQ(seq, 1);
    ASSERT_EQ(ts, 1000);
    ASSERT_EQ(ssrc, 1);
    ASSERT_EQ(payload_len, 2);
}

TEST(test_rtp_pack_basic)
{
    nano_rtp_t rtp;
    rtp_init(&rtp, 0xDEADBEEF, 111);
    rtp.seq = 1234;
    rtp.marker = 0; /* clear first-packet marker for vector comparison */

    uint8_t buf[64];
    size_t out_len;

    ASSERT_OK(rtp_pack(&rtp, 0x12345678, (const uint8_t *)"hello", 5,
                       buf, sizeof(buf), &out_len));
    ASSERT_EQ(out_len, 17);
    ASSERT_MEM_EQ(buf, rtp_vector, sizeof(rtp_vector));

    /* seq should have incremented */
    ASSERT_EQ(rtp.seq, 1235);
}

TEST(test_rtp_pack_marker)
{
    nano_rtp_t rtp;
    rtp_init(&rtp, 1, 111);
    rtp.seq = 1;
    rtp.marker = 1;

    uint8_t buf[64];
    size_t out_len;

    ASSERT_OK(rtp_pack(&rtp, 1000, (const uint8_t *)"\xAA\xBB", 2,
                       buf, sizeof(buf), &out_len));
    ASSERT_EQ(out_len, 14);
    ASSERT_MEM_EQ(buf, rtp_marker_vector, sizeof(rtp_marker_vector));

    /* marker should be cleared after pack */
    ASSERT_EQ(rtp.marker, 0);
}

TEST(test_rtp_roundtrip)
{
    nano_rtp_t rtp;
    rtp_init(&rtp, 0x11223344, 96);

    const uint8_t payload[] = {1, 2, 3, 4, 5, 6, 7, 8};
    uint8_t buf[64];
    size_t out_len;

    ASSERT_OK(rtp_pack(&rtp, 48000, payload, sizeof(payload),
                       buf, sizeof(buf), &out_len));

    uint8_t pt;
    uint16_t seq;
    uint32_t ts, ssrc;
    const uint8_t *out_payload;
    size_t out_payload_len;

    ASSERT_OK(rtp_unpack(buf, out_len,
                         &pt, &seq, &ts, &ssrc, &out_payload, &out_payload_len));
    ASSERT_EQ(pt, 96);
    ASSERT_EQ(seq, 0);
    ASSERT_EQ(ts, 48000);
    ASSERT_EQ(ssrc, 0x11223344);
    ASSERT_EQ(out_payload_len, 8);
    ASSERT_MEM_EQ(out_payload, payload, 8);
}

TEST(test_rtp_unpack_too_short)
{
    uint8_t data[8] = {0x80, 0x00};
    ASSERT_FAIL(rtp_unpack(data, 8, NULL, NULL, NULL, NULL, NULL, NULL));
}

TEST(test_rtp_unpack_bad_version)
{
    uint8_t data[12];
    memset(data, 0, sizeof(data));
    data[0] = 0x40; /* V=1, invalid */
    ASSERT_FAIL(rtp_unpack(data, sizeof(data), NULL, NULL, NULL, NULL, NULL, NULL));
}

TEST(test_rtp_pack_buffer_too_small)
{
    nano_rtp_t rtp;
    rtp_init(&rtp, 1, 96);

    uint8_t buf[10]; /* too small for 12-byte header */
    size_t out_len;

    ASSERT_EQ(rtp_pack(&rtp, 0, (const uint8_t *)"x", 1,
                       buf, sizeof(buf), &out_len), NANORTC_ERR_BUFFER_TOO_SMALL);
}

/* ---- Runner ---- */

TEST_MAIN_BEGIN("RTP tests")
    RUN(test_rtp_unpack_vector);
    RUN(test_rtp_unpack_marker);
    RUN(test_rtp_pack_basic);
    RUN(test_rtp_pack_marker);
    RUN(test_rtp_roundtrip);
    RUN(test_rtp_unpack_too_short);
    RUN(test_rtp_unpack_bad_version);
    RUN(test_rtp_pack_buffer_too_small);
TEST_MAIN_END
