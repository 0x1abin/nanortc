/*
 * nanortc — H.264 packetizer/depacketizer tests (RFC 6184)
 *
 * Test vectors derived from RFC 6184 §5.6-§5.8 packet format
 * and validated against str0m H.264 test cases.
 *
 * SPDX-License-Identifier: MIT
 */

#include "nanortc.h"
#include "nano_h264.h"
#include "nano_test.h"
#include <string.h>

/* ================================================================
 * Packetizer callback helper
 * ================================================================ */

#define MAX_FRAGMENTS 32
#define MAX_FRAG_SIZE 1500

typedef struct {
    uint8_t frags[MAX_FRAGMENTS][MAX_FRAG_SIZE];
    size_t frag_lens[MAX_FRAGMENTS];
    int markers[MAX_FRAGMENTS];
    int count;
} pkt_collector_t;

static int collect_cb(const uint8_t *payload, size_t len, int marker, void *userdata)
{
    pkt_collector_t *c = (pkt_collector_t *)userdata;
    if (c->count >= MAX_FRAGMENTS || len > MAX_FRAG_SIZE) {
        return NANORTC_ERR_BUFFER_TOO_SMALL;
    }
    memcpy(c->frags[c->count], payload, len);
    c->frag_lens[c->count] = len;
    c->markers[c->count] = marker;
    c->count++;
    return NANORTC_OK;
}

/* ================================================================
 * Packetizer tests
 * ================================================================ */

TEST(test_h264_pack_single_nal)
{
    /* NAL ≤ MTU: should be passed through as-is (Single NAL Unit mode) */
    uint8_t nalu[] = {0x65, 0x01, 0x02, 0x03, 0x04}; /* IDR slice */
    pkt_collector_t c;
    memset(&c, 0, sizeof(c));

    ASSERT_OK(h264_packetize(nalu, sizeof(nalu), 1200, collect_cb, &c));
    ASSERT_EQ(c.count, 1);
    ASSERT_EQ(c.frag_lens[0], sizeof(nalu));
    ASSERT_MEM_EQ(c.frags[0], nalu, sizeof(nalu));
    ASSERT_EQ(c.markers[0], 1); /* Marker set on single NAL */
}

TEST(test_h264_pack_exact_mtu)
{
    /* NAL exactly == MTU: single NAL unit, not fragmented */
    uint8_t nalu[10];
    memset(nalu, 0xAB, sizeof(nalu));
    nalu[0] = 0x41; /* non-IDR slice, NRI=2 */

    pkt_collector_t c;
    memset(&c, 0, sizeof(c));

    ASSERT_OK(h264_packetize(nalu, sizeof(nalu), 10, collect_cb, &c));
    ASSERT_EQ(c.count, 1);
    ASSERT_EQ(c.frag_lens[0], 10);
    ASSERT_EQ(c.markers[0], 1);
}

TEST(test_h264_pack_fua_basic)
{
    /* NAL > MTU: should be split into FU-A fragments.
     * RFC 6184 §5.8: FU indicator (1B) + FU header (1B) + data
     *
     * Use a 16-byte NAL with MTU=5 → max_frag = 5 - 2 = 3 bytes per fragment.
     * NAL data after header = 15 bytes → ceil(15/3) = 5 fragments. */
    uint8_t nalu[16];
    nalu[0] = 0x00; /* NAL header: NRI=0, type=0 (for testing FU-A) */
    for (int i = 1; i < 16; i++) {
        nalu[i] = (uint8_t)(i);
    }

    pkt_collector_t c;
    memset(&c, 0, sizeof(c));

    ASSERT_OK(h264_packetize(nalu, sizeof(nalu), 5, collect_cb, &c));
    ASSERT_EQ(c.count, 5);

    /* First fragment: S bit set */
    ASSERT_EQ(c.frags[0][0] & H264_NAL_TYPE_MASK, H264_NAL_FUA); /* FU indicator type=28 */
    ASSERT_TRUE(c.frags[0][1] & H264_FUA_S_BIT);                 /* Start bit */
    ASSERT_FALSE(c.frags[0][1] & H264_FUA_E_BIT);                /* No end bit */
    ASSERT_EQ(c.markers[0], 0);

    /* Middle fragments: no S/E bits */
    for (int i = 1; i < 4; i++) {
        ASSERT_EQ(c.frags[i][0] & H264_NAL_TYPE_MASK, H264_NAL_FUA);
        ASSERT_FALSE(c.frags[i][1] & H264_FUA_S_BIT);
        ASSERT_FALSE(c.frags[i][1] & H264_FUA_E_BIT);
        ASSERT_EQ(c.markers[i], 0);
    }

    /* Last fragment: E bit set, marker=1 */
    ASSERT_EQ(c.frags[4][0] & H264_NAL_TYPE_MASK, H264_NAL_FUA);
    ASSERT_FALSE(c.frags[4][1] & H264_FUA_S_BIT);
    ASSERT_TRUE(c.frags[4][1] & H264_FUA_E_BIT);
    ASSERT_EQ(c.markers[4], 1);
}

TEST(test_h264_pack_fua_nri_preserved)
{
    /* NRI bits from original NAL should be preserved in FU indicator */
    uint8_t nalu[10];
    nalu[0] = 0x65; /* NAL header: NRI=3 (0x60), type=5 (IDR) */
    memset(nalu + 1, 0xCC, 9);

    pkt_collector_t c;
    memset(&c, 0, sizeof(c));

    ASSERT_OK(h264_packetize(nalu, sizeof(nalu), 5, collect_cb, &c));
    ASSERT_TRUE(c.count > 1);

    /* FU indicator NRI should match original (0x60) */
    ASSERT_EQ(c.frags[0][0] & H264_NAL_REF_IDC_MASK, 0x60);

    /* FU header type should match original type (5 = IDR) */
    ASSERT_EQ(c.frags[0][1] & H264_NAL_TYPE_MASK, H264_NAL_IDR);
}

TEST(test_h264_pack_fua_str0m_vector)
{
    /* Test vector from str0m: packetize 16-byte NAL with MTU=5.
     * Expected output matches str0m's test_h264_payload large_payload_packetized. */
    uint8_t nalu[] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
                      0x08, 0x09, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15};

    pkt_collector_t c;
    memset(&c, 0, sizeof(c));

    ASSERT_OK(h264_packetize(nalu, sizeof(nalu), 5, collect_cb, &c));
    ASSERT_EQ(c.count, 5);

    /* Verify fragment contents match str0m expected output:
     * [0x1c, 0x80, 0x01, 0x02, 0x03]  ← S=1, type=0
     * [0x1c, 0x00, 0x04, 0x05, 0x06]
     * [0x1c, 0x00, 0x07, 0x08, 0x09]
     * [0x1c, 0x00, 0x10, 0x11, 0x12]
     * [0x1c, 0x40, 0x13, 0x14, 0x15]  ← E=1 */
    uint8_t expected[5][5] = {
        {0x1c, 0x80, 0x01, 0x02, 0x03}, {0x1c, 0x00, 0x04, 0x05, 0x06},
        {0x1c, 0x00, 0x07, 0x08, 0x09}, {0x1c, 0x00, 0x10, 0x11, 0x12},
        {0x1c, 0x40, 0x13, 0x14, 0x15},
    };
    for (int i = 0; i < 5; i++) {
        ASSERT_EQ(c.frag_lens[i], 5);
        ASSERT_MEM_EQ(c.frags[i], expected[i], 5);
    }
}

TEST(test_h264_pack_null_params)
{
    uint8_t nalu[] = {0x65, 0x00};
    pkt_collector_t c;
    memset(&c, 0, sizeof(c));

    ASSERT_FAIL(h264_packetize(NULL, 2, 1200, collect_cb, &c));
    ASSERT_FAIL(h264_packetize(nalu, 0, 1200, collect_cb, &c));
    ASSERT_FAIL(h264_packetize(nalu, 2, 2, collect_cb, &c)); /* mtu too small */
    ASSERT_FAIL(h264_packetize(nalu, 2, 1200, NULL, &c));
}

/* ================================================================
 * Depacketizer tests
 * ================================================================ */

TEST(test_h264_depkt_single_nal)
{
    nano_h264_depkt_t d;
    h264_depkt_init(&d);

    uint8_t payload[] = {0x65, 0xAA, 0xBB, 0xCC}; /* IDR slice */
    const uint8_t *out = NULL;
    size_t out_len = 0;

    ASSERT_OK(h264_depkt_push(&d, payload, sizeof(payload), 1, &out, &out_len));
    ASSERT_TRUE(out != NULL);
    ASSERT_EQ(out_len, sizeof(payload));
    ASSERT_MEM_EQ(out, payload, sizeof(payload));
}

TEST(test_h264_depkt_fua_roundtrip)
{
    /* Packetize a NAL, then depacketize — should get original NAL back */
    uint8_t nalu[16];
    nalu[0] = 0x65; /* IDR, NRI=3 */
    for (int i = 1; i < 16; i++) {
        nalu[i] = (uint8_t)(i * 3);
    }

    pkt_collector_t c;
    memset(&c, 0, sizeof(c));
    ASSERT_OK(h264_packetize(nalu, sizeof(nalu), 5, collect_cb, &c));
    ASSERT_TRUE(c.count > 1);

    /* Now depacketize all fragments */
    nano_h264_depkt_t d;
    h264_depkt_init(&d);

    const uint8_t *out = NULL;
    size_t out_len = 0;
    for (int i = 0; i < c.count; i++) {
        int rc = h264_depkt_push(&d, c.frags[i], c.frag_lens[i], c.markers[i], &out, &out_len);
        ASSERT_OK(rc);
        if (i < c.count - 1) {
            ASSERT_TRUE(out == NULL); /* Not complete yet */
        }
    }

    /* Final fragment should produce the complete NAL */
    ASSERT_TRUE(out != NULL);
    ASSERT_EQ(out_len, sizeof(nalu));
    ASSERT_MEM_EQ(out, nalu, sizeof(nalu));
}

TEST(test_h264_depkt_fua_str0m_vector)
{
    /* Depacketize str0m's FU-A test vectors → expect original NAL */
    uint8_t frags[5][5] = {
        {0x1c, 0x80, 0x01, 0x02, 0x03}, {0x1c, 0x00, 0x04, 0x05, 0x06},
        {0x1c, 0x00, 0x07, 0x08, 0x09}, {0x1c, 0x00, 0x10, 0x11, 0x12},
        {0x1c, 0x40, 0x13, 0x14, 0x15},
    };

    nano_h264_depkt_t d;
    h264_depkt_init(&d);

    const uint8_t *out = NULL;
    size_t out_len = 0;
    for (int i = 0; i < 5; i++) {
        int marker = (i == 4) ? 1 : 0;
        ASSERT_OK(h264_depkt_push(&d, frags[i], 5, marker, &out, &out_len));
        if (i < 4) {
            ASSERT_TRUE(out == NULL);
        }
    }

    /* Expected reassembled NAL: [NRI|type] + concatenated fragment data
     * NRI from FU indicator 0x1c = 0x00 (NRI=0)
     * Type from FU header = 0x00 (type=0, from first fragment S bit)
     * NAL header = 0x00 | 0x00 = 0x00 */
    uint8_t expected[] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
                          0x08, 0x09, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15};
    ASSERT_TRUE(out != NULL);
    ASSERT_EQ(out_len, sizeof(expected));
    ASSERT_MEM_EQ(out, expected, sizeof(expected));
}

TEST(test_h264_depkt_stapa)
{
    /* STAP-A packet from str0m test: contains SPS + PPS */
    uint8_t stapa[] = {
        0x78,                               /* STAP-A header (type=24, NRI=3) */
        0x00, 0x0f,                         /* NALU 1 length = 15 */
        0x67, 0x42, 0xc0, 0x1f, 0x1a, 0x32, /* NALU 1 data (SPS, type=7) */
        0x35, 0x01, 0x40, 0x7a, 0x40, 0x3c, 0x22, 0x11, 0xa8, 0x00, 0x05, /* NALU 2 length = 5 */
        0x68, 0x1a, 0x34, 0xe3, 0xc8, /* NALU 2 data (PPS, type=8) */
        0x00,                         /* trailing padding (ignored) */
    };

    nano_h264_depkt_t d;
    h264_depkt_init(&d);

    const uint8_t *out = NULL;
    size_t out_len = 0;
    ASSERT_OK(h264_depkt_push(&d, stapa, sizeof(stapa), 1, &out, &out_len));

    /* Should return the first sub-NAL (SPS) */
    ASSERT_TRUE(out != NULL);
    ASSERT_EQ(out_len, 15);
    ASSERT_EQ(out[0] & H264_NAL_TYPE_MASK, H264_NAL_SPS); /* type 7 */
}

TEST(test_h264_depkt_null_params)
{
    nano_h264_depkt_t d;
    h264_depkt_init(&d);
    uint8_t payload[] = {0x65};
    const uint8_t *out;
    size_t out_len;

    ASSERT_FAIL(h264_depkt_push(NULL, payload, 1, 0, &out, &out_len));
    ASSERT_FAIL(h264_depkt_push(&d, NULL, 1, 0, &out, &out_len));
    ASSERT_FAIL(h264_depkt_push(&d, payload, 0, 0, &out, &out_len));
    ASSERT_FAIL(h264_depkt_push(&d, payload, 1, 0, NULL, &out_len));
    ASSERT_FAIL(h264_depkt_push(&d, payload, 1, 0, &out, NULL));
}

/* ================================================================
 * Keyframe detection tests
 * ================================================================ */

TEST(test_h264_keyframe_empty)
{
    ASSERT_FALSE(h264_is_keyframe(NULL, 0));
    ASSERT_FALSE(h264_is_keyframe((uint8_t[]){0}, 0));
}

TEST(test_h264_keyframe_single_idr)
{
    /* IDR NAL unit (type 5): 0x65 = NRI=3, type=5 */
    ASSERT_TRUE(h264_is_keyframe((uint8_t[]){0x65, 0x00, 0x00}, 3));
}

TEST(test_h264_keyframe_single_non_idr)
{
    /* Non-IDR slice (type 1): 0x41 = NRI=2, type=1 */
    ASSERT_FALSE(h264_is_keyframe((uint8_t[]){0x41, 0x00, 0x00}, 3));
    /* SPS (type 7): not a keyframe */
    ASSERT_FALSE(h264_is_keyframe((uint8_t[]){0x67, 0x00, 0x00}, 3));
    /* PPS (type 8): not a keyframe */
    ASSERT_FALSE(h264_is_keyframe((uint8_t[]){0x68, 0x00, 0x00}, 3));
}

TEST(test_h264_keyframe_stapa_with_idr)
{
    /* STAP-A containing SPS + IDR */
    uint8_t stapa[] = {
        0x18,                   /* STAP-A (type=24) */
        0x00, 0x02, 0x67, 0xAA, /* SPS: size=2, type=7 */
        0x00, 0x02, 0x65, 0xBB, /* IDR: size=2, type=5 */
    };
    ASSERT_TRUE(h264_is_keyframe(stapa, sizeof(stapa)));
}

TEST(test_h264_keyframe_stapa_without_idr)
{
    uint8_t stapa[] = {
        0x18,                   /* STAP-A (type=24) */
        0x00, 0x02, 0x67, 0xAA, /* SPS: size=2, type=7 */
        0x00, 0x02, 0x68, 0xBB, /* PPS: size=2, type=8 */
    };
    ASSERT_FALSE(h264_is_keyframe(stapa, sizeof(stapa)));
}

TEST(test_h264_keyframe_fua_start_idr)
{
    /* FU-A start fragment with IDR type */
    uint8_t fua[] = {
        0x7C, /* FU indicator: NRI=3, type=28 (FU-A) */
        0x85, /* FU header: S=1, type=5 (IDR) */
        0x00,
        0x00,
    };
    ASSERT_TRUE(h264_is_keyframe(fua, sizeof(fua)));
}

TEST(test_h264_keyframe_fua_start_non_idr)
{
    uint8_t fua[] = {
        0x7C, /* FU indicator: type=28 */
        0x81, /* FU header: S=1, type=1 (non-IDR) */
        0x00,
        0x00,
    };
    ASSERT_FALSE(h264_is_keyframe(fua, sizeof(fua)));
}

TEST(test_h264_keyframe_fua_continuation)
{
    /* FU-A continuation (S=0) — should NOT detect as keyframe */
    uint8_t fua[] = {
        0x7C, /* FU indicator: type=28 */
        0x05, /* FU header: S=0, type=5 (IDR but no start bit) */
        0x00,
        0x00,
    };
    ASSERT_FALSE(h264_is_keyframe(fua, sizeof(fua)));
}

TEST(test_h264_keyframe_fua_too_short)
{
    /* FU-A with only 1 byte — too short */
    ASSERT_FALSE(h264_is_keyframe((uint8_t[]){0x7C}, 1));
}

/* ================================================================
 * Edge cases
 * ================================================================ */

TEST(test_h264_pack_two_byte_nal)
{
    /* Minimum valid NAL: just the header + 1 byte */
    uint8_t nalu[] = {0x41, 0xFF};
    pkt_collector_t c;
    memset(&c, 0, sizeof(c));

    ASSERT_OK(h264_packetize(nalu, sizeof(nalu), 1200, collect_cb, &c));
    ASSERT_EQ(c.count, 1);
    ASSERT_EQ(c.frag_lens[0], 2);
}

TEST(test_h264_depkt_fua_no_start)
{
    /* FU-A continuation without a prior start → should be ignored */
    nano_h264_depkt_t d;
    h264_depkt_init(&d);

    uint8_t fua_cont[] = {0x1c, 0x01, 0xAA, 0xBB}; /* S=0, E=0 */
    const uint8_t *out = NULL;
    size_t out_len = 0;
    ASSERT_OK(h264_depkt_push(&d, fua_cont, sizeof(fua_cont), 0, &out, &out_len));
    ASSERT_TRUE(out == NULL);
}

/* ================================================================
 * RFC 6184 edge case tests
 * ================================================================ */

/*
 * RFC 6184 §5.8: FU-A fragmentation — NAL exactly MTU+1 bytes.
 * Should produce exactly 2 fragments.
 */
TEST(test_h264_pack_fua_exact_mtu_plus_one)
{
    /* MTU=10, NAL=11 bytes: 1 byte NAL header + 10 bytes body
     * FU-A splits: first frag gets header(2) + some body, second frag gets rest */
    uint8_t nalu[11];
    memset(nalu, 0xCC, sizeof(nalu));
    nalu[0] = 0x65; /* IDR, NRI=3 */

    pkt_collector_t c;
    memset(&c, 0, sizeof(c));

    ASSERT_OK(h264_packetize(nalu, sizeof(nalu), 10, collect_cb, &c));
    ASSERT_EQ(c.count, 2);

    /* First fragment: S=1, E=0 */
    ASSERT_EQ(c.frags[0][1] & 0x80, 0x80); /* Start bit */
    ASSERT_EQ(c.frags[0][1] & 0x40, 0x00); /* No End bit */
    ASSERT_EQ(c.markers[0], 0);            /* Not last */

    /* Second fragment: S=0, E=1 */
    ASSERT_EQ(c.frags[1][1] & 0x80, 0x00); /* No Start bit */
    ASSERT_EQ(c.frags[1][1] & 0x40, 0x40); /* End bit */
    ASSERT_EQ(c.markers[1], 1);            /* Last fragment */
}

/*
 * RFC 6184 §1.3: Forbidden bit (F=1) in NAL header.
 * A NAL with F=1 (bit 7 set) indicates a syntax violation.
 * The packetizer should still handle it (pass-through).
 */
TEST(test_h264_forbidden_bit_passthrough)
{
    /* NAL header with F=1: 0x80 | type=5 (IDR) = 0x85 */
    uint8_t nalu[] = {0x85, 0x01, 0x02};

    pkt_collector_t c;
    memset(&c, 0, sizeof(c));

    /* Packetizer should not reject NALs with forbidden bit — it's informational */
    ASSERT_OK(h264_packetize(nalu, sizeof(nalu), 1200, collect_cb, &c));
    ASSERT_EQ(c.count, 1);
    /* Forbidden bit should be preserved */
    ASSERT_TRUE(c.frags[0][0] & 0x80);
}

/*
 * RFC 6184: Keyframe detection on various edge cases.
 * Empty payload should not be detected as keyframe.
 * Single byte payload = just NAL header, should work.
 */
TEST(test_h264_keyframe_single_byte)
{
    /* Just NAL header for IDR (type 5): 0x65 */
    uint8_t idr_nal[] = {0x65};
    ASSERT_TRUE(h264_is_keyframe(idr_nal, 1));

    /* Non-IDR (type 1): 0x41 */
    uint8_t non_idr_nal[] = {0x41};
    ASSERT_FALSE(h264_is_keyframe(non_idr_nal, 1));
}

/* ================================================================
 * Annex-B NAL finder tests (h264_annex_b_find_nal)
 * ================================================================ */

TEST(test_h264_annex_b_null_params)
{
    size_t offset = 0, nal_len = 0;
    ASSERT_EQ(h264_annex_b_find_nal(NULL, 10, &offset, &nal_len), NULL);
    uint8_t data[] = {0, 0, 1, 0x65};
    ASSERT_EQ(h264_annex_b_find_nal(data, 4, NULL, &nal_len), NULL);
    ASSERT_EQ(h264_annex_b_find_nal(data, 4, &offset, NULL), NULL);
}

TEST(test_h264_annex_b_3byte_start_code)
{
    /* 00 00 01 <IDR NAL=0x65> */
    uint8_t data[] = {0x00, 0x00, 0x01, 0x65, 0xAA, 0xBB};
    size_t offset = 0, nal_len = 0;
    const uint8_t *nal = h264_annex_b_find_nal(data, sizeof(data), &offset, &nal_len);
    ASSERT_TRUE(nal != NULL);
    ASSERT_EQ(nal[0], 0x65);
    ASSERT_EQ(nal_len, 3); /* 0x65, 0xAA, 0xBB */
}

TEST(test_h264_annex_b_4byte_start_code)
{
    /* 00 00 00 01 <SPS NAL=0x67> <data> */
    uint8_t data[] = {0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0xC0};
    size_t offset = 0, nal_len = 0;
    const uint8_t *nal = h264_annex_b_find_nal(data, sizeof(data), &offset, &nal_len);
    ASSERT_TRUE(nal != NULL);
    ASSERT_EQ(nal[0], 0x67);
    ASSERT_EQ(nal_len, 3); /* 0x67, 0x42, 0xC0 */
}

TEST(test_h264_annex_b_two_nals)
{
    /* NAL1 | 00 00 01 | NAL2 */
    uint8_t data[] = {0x00, 0x00, 0x01, 0x67, 0x42, /* NAL1 */
                      0x00, 0x00, 0x01, 0x68, 0xCE}; /* NAL2 */
    size_t offset = 0, nal_len = 0;

    /* First NAL */
    const uint8_t *nal1 = h264_annex_b_find_nal(data, sizeof(data), &offset, &nal_len);
    ASSERT_TRUE(nal1 != NULL);
    ASSERT_EQ(nal1[0], 0x67);
    ASSERT_EQ(nal_len, 2); /* 0x67, 0x42 (trailing zeros stripped) */

    /* Second NAL */
    const uint8_t *nal2 = h264_annex_b_find_nal(data, sizeof(data), &offset, &nal_len);
    ASSERT_TRUE(nal2 != NULL);
    ASSERT_EQ(nal2[0], 0x68);
    ASSERT_EQ(nal_len, 2); /* 0x68, 0xCE */
}

TEST(test_h264_annex_b_trailing_zeros_stripped)
{
    /* NAL followed by zero padding — trailing zeros between NALs should be stripped */
    uint8_t data[] = {0x00, 0x00, 0x01, 0x65, 0xAA, 0x00, 0x00, 0x00};
    size_t offset = 0, nal_len = 0;
    const uint8_t *nal = h264_annex_b_find_nal(data, sizeof(data), &offset, &nal_len);
    ASSERT_TRUE(nal != NULL);
    ASSERT_EQ(nal[0], 0x65);
    ASSERT_EQ(nal_len, 2); /* 0x65, 0xAA — trailing zeros stripped */
}

TEST(test_h264_annex_b_empty)
{
    uint8_t data[] = {0};
    size_t offset = 0, nal_len = 0;
    const uint8_t *nal = h264_annex_b_find_nal(data, 0, &offset, &nal_len);
    ASSERT_EQ(nal, NULL);
}

/* T-extra: depkt_init NULL */
TEST(test_h264_depkt_init_null)
{
    ASSERT_FAIL(h264_depkt_init(NULL));
}

/* T-extra: Unknown NAL type (e.g. type 30) ignored gracefully */
TEST(test_h264_depkt_unknown_nal_type)
{
    nano_h264_depkt_t d;
    h264_depkt_init(&d);

    /* NAL type 30 = 0x1E, with NRI=0 → byte = 0x1E */
    uint8_t payload[] = {0x1E, 0xAA, 0xBB};
    const uint8_t *out = NULL;
    size_t out_len = 0;
    ASSERT_OK(h264_depkt_push(&d, payload, sizeof(payload), 1, &out, &out_len));
    ASSERT_TRUE(out == NULL); /* Unknown NAL ignored */
}

/* T-extra: FU-A in progress, then single NAL interrupts reassembly */
TEST(test_h264_depkt_fua_interrupted_by_single_nal)
{
    nano_h264_depkt_t d;
    h264_depkt_init(&d);

    /* Start FU-A (S=1, E=0) for IDR type 5, NRI=3 → FU indicator: 0x7C, FU header: 0x85 */
    uint8_t fua_start[] = {0x7C, 0x85, 0x01, 0x02, 0x03};
    const uint8_t *out = NULL;
    size_t out_len = 0;
    ASSERT_OK(h264_depkt_push(&d, fua_start, sizeof(fua_start), 0, &out, &out_len));
    ASSERT_TRUE(out == NULL); /* Not complete yet */

    /* Now send a single NAL (type 1) — should interrupt FU-A */
    uint8_t single_nal[] = {0x41, 0xAA}; /* type=1, NRI=2 */
    ASSERT_OK(h264_depkt_push(&d, single_nal, sizeof(single_nal), 1, &out, &out_len));
    ASSERT_TRUE(out != NULL); /* Single NAL returned */
    ASSERT_EQ(out_len, 2);
    ASSERT_EQ(out[0], 0x41);
}

/* T-extra: Single NAL exceeds buffer (use impossibly large payload) */
TEST(test_h264_depkt_single_nal_exceeds_buffer)
{
    nano_h264_depkt_t d;
    h264_depkt_init(&d);

    /* We can't actually allocate NANORTC_VIDEO_NAL_BUF_SIZE+1 on stack.
     * Instead, lie about length while passing a valid small pointer.
     * The function checks len > NANORTC_VIDEO_NAL_BUF_SIZE before memcpy. */
    uint8_t payload[] = {0x65}; /* IDR NAL header */
    const uint8_t *out = NULL;
    size_t out_len = 0;
    int rc = h264_depkt_push(&d, payload, NANORTC_VIDEO_NAL_BUF_SIZE + 1, 1, &out, &out_len);
    ASSERT_EQ(rc, NANORTC_ERR_BUFFER_TOO_SMALL);
}

/* ================================================================
 * Test runner
 * ================================================================ */

TEST_MAIN_BEGIN("test_h264")
/* Packetizer */
RUN(test_h264_pack_single_nal);
RUN(test_h264_pack_exact_mtu);
RUN(test_h264_pack_fua_basic);
RUN(test_h264_pack_fua_nri_preserved);
RUN(test_h264_pack_fua_str0m_vector);
RUN(test_h264_pack_null_params);
/* Depacketizer */
RUN(test_h264_depkt_single_nal);
RUN(test_h264_depkt_fua_roundtrip);
RUN(test_h264_depkt_fua_str0m_vector);
RUN(test_h264_depkt_stapa);
RUN(test_h264_depkt_null_params);
RUN(test_h264_depkt_fua_no_start);
/* Keyframe detection */
RUN(test_h264_keyframe_empty);
RUN(test_h264_keyframe_single_idr);
RUN(test_h264_keyframe_single_non_idr);
RUN(test_h264_keyframe_stapa_with_idr);
RUN(test_h264_keyframe_stapa_without_idr);
RUN(test_h264_keyframe_fua_start_idr);
RUN(test_h264_keyframe_fua_start_non_idr);
RUN(test_h264_keyframe_fua_continuation);
RUN(test_h264_keyframe_fua_too_short);
/* Edge cases */
RUN(test_h264_pack_two_byte_nal);
RUN(test_h264_depkt_fua_no_start);
/* RFC 6184 edge case tests */
RUN(test_h264_pack_fua_exact_mtu_plus_one);
RUN(test_h264_forbidden_bit_passthrough);
RUN(test_h264_keyframe_single_byte);
/* Annex-B NAL finder */
RUN(test_h264_annex_b_null_params);
RUN(test_h264_annex_b_3byte_start_code);
RUN(test_h264_annex_b_4byte_start_code);
RUN(test_h264_annex_b_two_nals);
RUN(test_h264_annex_b_trailing_zeros_stripped);
RUN(test_h264_annex_b_empty);
/* Extra coverage tests */
RUN(test_h264_depkt_init_null);
RUN(test_h264_depkt_unknown_nal_type);
RUN(test_h264_depkt_fua_interrupted_by_single_nal);
RUN(test_h264_depkt_single_nal_exceeds_buffer);
TEST_MAIN_END
