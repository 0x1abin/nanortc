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
    0x80,                       /* V=2, P=0, X=0, CC=0 */
    0x6F,                       /* M=0, PT=111 */
    0x04, 0xD2,                 /* seq=1234 */
    0x12, 0x34, 0x56, 0x78,     /* timestamp */
    0xDE, 0xAD, 0xBE, 0xEF,     /* SSRC */
    'h',  'e',  'l',  'l',  'o' /* payload */
};

/* RTP packet with marker bit set */
static const uint8_t rtp_marker_vector[] = {
    0x80,                   /* V=2, P=0, X=0, CC=0 */
    0xEF,                   /* M=1, PT=111 */
    0x00, 0x01,             /* seq=1 */
    0x00, 0x00, 0x03, 0xE8, /* timestamp=1000 */
    0x00, 0x00, 0x00, 0x01, /* SSRC=1 */
    0xAA, 0xBB              /* payload */
};

/* ---- Tests ---- */

TEST(test_rtp_unpack_vector)
{
    uint8_t pt;
    uint16_t seq;
    uint32_t ts, ssrc;
    const uint8_t *payload;
    size_t payload_len;

    ASSERT_OK(
        rtp_unpack(rtp_vector, sizeof(rtp_vector), &pt, &seq, &ts, &ssrc, &payload, &payload_len));
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

    ASSERT_OK(rtp_unpack(rtp_marker_vector, sizeof(rtp_marker_vector), &pt, &seq, &ts, &ssrc,
                         &payload, &payload_len));
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

    ASSERT_OK(rtp_pack(&rtp, 0x12345678, (const uint8_t *)"hello", 5, buf, sizeof(buf), &out_len));
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

    ASSERT_OK(rtp_pack(&rtp, 1000, (const uint8_t *)"\xAA\xBB", 2, buf, sizeof(buf), &out_len));
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

    ASSERT_OK(rtp_pack(&rtp, 48000, payload, sizeof(payload), buf, sizeof(buf), &out_len));

    uint8_t pt;
    uint16_t seq;
    uint32_t ts, ssrc;
    const uint8_t *out_payload;
    size_t out_payload_len;

    ASSERT_OK(rtp_unpack(buf, out_len, &pt, &seq, &ts, &ssrc, &out_payload, &out_payload_len));
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

    ASSERT_EQ(rtp_pack(&rtp, 0, (const uint8_t *)"x", 1, buf, sizeof(buf), &out_len),
              NANORTC_ERR_BUFFER_TOO_SMALL);
}

/* ================================================================
 * RFC 3550 §5 — Independent byte-level test vectors
 * ================================================================ */

/*
 * RFC 3550 §5.1: RTP packet with CSRC list (CC=2)
 * V=2, P=0, X=0, CC=2, M=0, PT=96, seq=100, ts=160000, SSRC=0x01020304
 * CSRC[0]=0x0A0B0C0D, CSRC[1]=0x0E0F1011
 * Payload: {0xAA, 0xBB}
 */
TEST(test_rtp_csrc_list)
{
    static const uint8_t pkt[] = {
        0x82,                   /* V=2, P=0, X=0, CC=2 */
        0x60,                   /* M=0, PT=96 */
        0x00, 0x64,             /* seq=100 */
        0x00, 0x02, 0x71, 0x00, /* ts=160000 */
        0x01, 0x02, 0x03, 0x04, /* SSRC */
        0x0A, 0x0B, 0x0C, 0x0D, /* CSRC[0] */
        0x0E, 0x0F, 0x10, 0x11, /* CSRC[1] */
        0xAA, 0xBB,             /* payload */
    };

    uint8_t pt;
    uint16_t seq;
    uint32_t ts, ssrc;
    const uint8_t *payload;
    size_t payload_len;

    ASSERT_OK(rtp_unpack(pkt, sizeof(pkt), &pt, &seq, &ts, &ssrc, &payload, &payload_len));
    ASSERT_EQ(pt, 96);
    ASSERT_EQ(seq, 100);
    ASSERT_EQ(ts, 160000u);
    ASSERT_EQ(ssrc, 0x01020304u);
    /* Payload starts after 12-byte header + 2*4 = 8 bytes CSRC = offset 20 */
    ASSERT_EQ(payload_len, 2);
    ASSERT_EQ(payload[0], 0xAA);
    ASSERT_EQ(payload[1], 0xBB);
}

/*
 * RFC 3550 §5.3.1: RTP packet with extension header (X=1)
 * V=2, P=0, X=1, CC=0, M=1, PT=111, seq=5000, ts=0xAABBCCDD, SSRC=0x11111111
 * Extension: profile-specific=0xBEDE, length=1 (1 x 32-bit word)
 * Extension data: 0x12345678
 * Payload: {0xFF}
 */
TEST(test_rtp_extension_header)
{
    static const uint8_t pkt[] = {
        0x90, /* V=2, P=0, X=1, CC=0 */
        0xEF, /* M=1, PT=111 */
        0x13,
        0x88, /* seq=5000 */
        0xAA,
        0xBB,
        0xCC,
        0xDD, /* ts */
        0x11,
        0x11,
        0x11,
        0x11, /* SSRC */
        /* Extension header */
        0xBE,
        0xDE, /* defined by profile (RTP header extension) */
        0x00,
        0x01, /* length = 1 (32-bit word) */
        0x12,
        0x34,
        0x56,
        0x78, /* extension data */
        /* Payload */
        0xFF,
    };

    uint8_t pt;
    uint16_t seq;
    uint32_t ts, ssrc;
    const uint8_t *payload;
    size_t payload_len;

    ASSERT_OK(rtp_unpack(pkt, sizeof(pkt), &pt, &seq, &ts, &ssrc, &payload, &payload_len));
    ASSERT_EQ(pt, 111);
    ASSERT_EQ(seq, 5000);
    ASSERT_EQ(ts, 0xAABBCCDDu);
    ASSERT_EQ(ssrc, 0x11111111u);
    /* Payload starts after 12 + 4(ext hdr) + 4(ext data) = 20 */
    ASSERT_EQ(payload_len, 1);
    ASSERT_EQ(payload[0], 0xFF);
}

/*
 * RFC 3550 §5.1: RTP packet with both CSRC (CC=1) and extension (X=1)
 * Verifies combined header length calculation.
 */
TEST(test_rtp_csrc_and_extension)
{
    static const uint8_t pkt[] = {
        0x91, /* V=2, P=0, X=1, CC=1 */
        0x60, /* M=0, PT=96 */
        0x00,
        0x01, /* seq=1 */
        0x00,
        0x00,
        0x00,
        0x01, /* ts=1 */
        0x00,
        0x00,
        0x00,
        0x01, /* SSRC=1 */
        0x00,
        0x00,
        0x00,
        0x02, /* CSRC[0]=2 */
        /* Extension header (starts after CSRC) */
        0xBE,
        0xDE,
        0x00,
        0x01, /* length=1 word */
        0xAA,
        0xBB,
        0xCC,
        0xDD,
        /* Payload */
        0x42,
    };

    uint8_t pt;
    uint16_t seq;
    const uint8_t *payload;
    size_t payload_len;

    ASSERT_OK(rtp_unpack(pkt, sizeof(pkt), &pt, &seq, NULL, NULL, &payload, &payload_len));
    ASSERT_EQ(pt, 96);
    /* Header = 12 + 4(CSRC) + 4(ext hdr) + 4(ext data) = 24 */
    ASSERT_EQ(payload_len, 1);
    ASSERT_EQ(payload[0], 0x42);
}

/*
 * RFC 3550 §5.1: Padding bit (P=1)
 * The last byte of the payload indicates padding length.
 * Note: rtp_unpack does NOT strip padding — the caller must handle it.
 * This test verifies the raw payload includes the padding bytes.
 */
TEST(test_rtp_padding_bit)
{
    static const uint8_t pkt[] = {
        0xA0, /* V=2, P=1, X=0, CC=0 */
        0x60, /* M=0, PT=96 */
        0x00,
        0x0A, /* seq=10 */
        0x00,
        0x00,
        0x03,
        0xE8, /* ts=1000 */
        0xDE,
        0xAD,
        0xBE,
        0xEF, /* SSRC */
        /* Payload: 2 real bytes + 2 padding bytes (last byte = 0x02 = pad count) */
        0x01,
        0x02,
        0x00,
        0x02,
    };

    uint8_t pt;
    const uint8_t *payload;
    size_t payload_len;

    ASSERT_OK(rtp_unpack(pkt, sizeof(pkt), &pt, NULL, NULL, NULL, &payload, &payload_len));
    ASSERT_EQ(pt, 96);
    /* Raw payload includes padding bytes (caller responsibility to strip) */
    ASSERT_EQ(payload_len, 4);
    /* Padding bit was set in header */
    ASSERT_TRUE(pkt[0] & 0x20);
}

/*
 * Transport-wide CC (TWCC) header extension (RFC 8285 one-byte, draft-holmer-rmcat-twcc-01).
 *
 * When nano_rtp_t.twcc_ext_id is non-zero, rtp_pack must:
 *   - set the X bit in byte 0
 *   - emit profile=0xBEDE, length=1 at the start of the extension area
 *   - emit ID|len + 16-bit seq + pad for exactly 8 bytes of overhead
 *   - auto-increment twcc_seq after each packet
 */
TEST(test_rtp_pack_twcc_extension)
{
    nano_rtp_t rtp;
    rtp_init(&rtp, 0xDEADBEEF, 96);
    rtp.marker = 0; /* clear talk-spurt marker from rtp_init for deterministic bit check */
    rtp.seq = 0x1000;
    rtp.twcc_ext_id = 3;
    rtp.twcc_seq = 0x0042;

    uint8_t buf[64];
    size_t out_len = 0;
    const uint8_t payload[] = {'H', 'I'};
    ASSERT_OK(rtp_pack(&rtp, 0x11223344, payload, sizeof(payload), buf, sizeof(buf), &out_len));
    ASSERT_EQ(out_len, (size_t)(RTP_HEADER_SIZE + RTP_TWCC_EXT_OVERHEAD + sizeof(payload)));

    /* Fixed RTP header with X bit set. */
    ASSERT_EQ(buf[0], 0x90); /* V=2, P=0, X=1, CC=0 */
    ASSERT_EQ(buf[1], 96);   /* M=0, PT=96 */

    /* Extension profile + length. */
    ASSERT_EQ(buf[12], 0xBE);
    ASSERT_EQ(buf[13], 0xDE);
    ASSERT_EQ(buf[14], 0x00);
    ASSERT_EQ(buf[15], 0x01); /* length = 1 word */

    /* One-byte header: (ID=3 << 4) | (len=1) = 0x31. */
    ASSERT_EQ(buf[16], 0x31);
    ASSERT_EQ(buf[17], 0x00);
    ASSERT_EQ(buf[18], 0x42); /* seq = 0x0042 */
    ASSERT_EQ(buf[19], 0x00); /* pad */

    /* Payload follows. */
    ASSERT_EQ(buf[20], 'H');
    ASSERT_EQ(buf[21], 'I');

    /* twcc_seq auto-incremented. */
    ASSERT_EQ(rtp.twcc_seq, 0x0043);
}

TEST(test_rtp_pack_no_twcc_when_id_zero)
{
    nano_rtp_t rtp;
    rtp_init(&rtp, 0x1, 96);
    rtp.twcc_ext_id = 0;

    uint8_t buf[64];
    size_t out_len = 0;
    ASSERT_OK(rtp_pack(&rtp, 1, (const uint8_t *)"x", 1, buf, sizeof(buf), &out_len));
    ASSERT_EQ(out_len, (size_t)(RTP_HEADER_SIZE + 1));
    ASSERT_EQ(buf[0], 0x80); /* X bit cleared */
}

TEST(test_rtp_pack_twcc_rejects_invalid_id)
{
    /* IDs outside 1..14 are reserved per RFC 8285; treat as disabled. */
    nano_rtp_t rtp;
    rtp_init(&rtp, 0x1, 96);
    rtp.twcc_ext_id = 15;

    uint8_t buf[64];
    size_t out_len = 0;
    ASSERT_OK(rtp_pack(&rtp, 1, (const uint8_t *)"x", 1, buf, sizeof(buf), &out_len));
    ASSERT_EQ(out_len, (size_t)(RTP_HEADER_SIZE + 1));
    ASSERT_EQ(buf[0] & 0x10, 0); /* X bit cleared */
}

TEST(test_rtp_pack_twcc_seq_increments_per_packet)
{
    nano_rtp_t rtp;
    rtp_init(&rtp, 0x1, 96);
    rtp.twcc_ext_id = 5;
    rtp.twcc_seq = 100;

    uint8_t buf[64];
    size_t out_len = 0;
    for (int i = 0; i < 4; i++) {
        ASSERT_OK(rtp_pack(&rtp, (uint32_t)(1000 + i), (const uint8_t *)"y", 1, buf, sizeof(buf),
                           &out_len));
        uint16_t seq_in_ext = (uint16_t)((buf[17] << 8) | buf[18]);
        ASSERT_EQ(seq_in_ext, (uint16_t)(100 + i));
    }
    ASSERT_EQ(rtp.twcc_seq, 104);
}

TEST(test_rtp_unpack_skips_twcc_extension)
{
    /* A packet with TWCC extension should still yield the correct payload
     * pointer via rtp_unpack (existing X-bit logic). */
    nano_rtp_t rtp;
    rtp_init(&rtp, 0xABCD, 97);
    rtp.twcc_ext_id = 7;
    rtp.twcc_seq = 1;

    uint8_t buf[64];
    size_t out_len = 0;
    const uint8_t original[] = {0xCA, 0xFE, 0xBA, 0xBE};
    ASSERT_OK(rtp_pack(&rtp, 1, original, sizeof(original), buf, sizeof(buf), &out_len));

    uint8_t pt;
    uint16_t seq;
    uint32_t ts, ssrc;
    const uint8_t *pl;
    size_t pl_len;
    ASSERT_OK(rtp_unpack(buf, out_len, &pt, &seq, &ts, &ssrc, &pl, &pl_len));
    ASSERT_EQ(pt, 97);
    ASSERT_EQ(ssrc, 0xABCDu);
    ASSERT_EQ(pl_len, sizeof(original));
    ASSERT_MEM_EQ(pl, original, sizeof(original));
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
/* RFC 3550 §5 — independent byte-level vectors */
RUN(test_rtp_csrc_list);
RUN(test_rtp_extension_header);
RUN(test_rtp_csrc_and_extension);
RUN(test_rtp_padding_bit);
/* Transport-wide CC header extension (RFC 8285, draft-holmer-rmcat-twcc-01) */
RUN(test_rtp_pack_twcc_extension);
RUN(test_rtp_pack_no_twcc_when_id_zero);
RUN(test_rtp_pack_twcc_rejects_invalid_id);
RUN(test_rtp_pack_twcc_seq_increments_per_packet);
RUN(test_rtp_unpack_skips_twcc_extension);
TEST_MAIN_END
