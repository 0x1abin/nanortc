/*
 * nanortc — H.265/HEVC packetizer/depacketizer tests (RFC 7798)
 *
 * Test vectors derived from RFC 7798 §1.1.4 (NAL header layout) and
 * §4.4 (payload structures). Each vector documents the bit positions
 * it derives from so a reviewer can verify against the RFC text.
 *
 * SPDX-License-Identifier: MIT
 */

#include "nanortc.h"
#include "nano_h264.h" /* h264_annex_b_find_nal is codec-agnostic */
#include "nano_h265.h"
#include "nano_test.h"
#include <stdio.h>
#include <string.h>

/* ================================================================
 * Packetizer callback helper
 * ================================================================ */

#define MAX_FRAGMENTS 64
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
 * Vector A — Single NAL direct pass-through (RFC 7798 §4.4.1)
 *
 * TRAIL_R NAL (type=1), F=0, LayerId=0, TID=1, 3 payload bytes.
 *   byte0 = (1 << 1) | 0 = 0x02
 *   byte1 = (0 << 3) | 1 = 0x01
 * ================================================================ */

TEST(test_h265_pack_single_nal)
{
    uint8_t nalu[] = {0x02, 0x01, 0xAA, 0xBB, 0xCC};
    pkt_collector_t c;
    memset(&c, 0, sizeof(c));

    ASSERT_OK(h265_packetize(nalu, sizeof(nalu), 1200, collect_cb, &c));
    ASSERT_EQ(c.count, 1);
    ASSERT_EQ(c.frag_lens[0], sizeof(nalu));
    ASSERT_MEM_EQ(c.frags[0], nalu, sizeof(nalu));
    ASSERT_EQ(c.markers[0], 1);

    /* Roundtrip: feed the single packet back into depkt. */
    nano_h265_depkt_t d;
    h265_depkt_init(&d);
    const uint8_t *out = NULL;
    size_t out_len = 0;
    ASSERT_OK(h265_depkt_push(&d, c.frags[0], c.frag_lens[0], 1, &out, &out_len));
    ASSERT_EQ(out_len, sizeof(nalu));
    ASSERT_MEM_EQ(out, nalu, sizeof(nalu));
}

TEST(test_h265_pack_exact_mtu)
{
    /* NAL exactly == MTU: single NAL unit, not fragmented. */
    uint8_t nalu[10];
    memset(nalu, 0xAB, sizeof(nalu));
    nalu[0] = 0x02; /* TRAIL_R */
    nalu[1] = 0x01;

    pkt_collector_t c;
    memset(&c, 0, sizeof(c));

    ASSERT_OK(h265_packetize(nalu, sizeof(nalu), 10, collect_cb, &c));
    ASSERT_EQ(c.count, 1);
    ASSERT_EQ(c.frag_lens[0], 10);
    ASSERT_EQ(c.markers[0], 1);
}

/* ================================================================
 * Vector B — FU fragmentation byte-exact (RFC 7798 §4.4.3)
 *
 * 9-byte IDR_W_RADL NAL (type=19), TID=1:
 *   byte0 = (19 << 1) | 0 = 0x26
 *   byte1 = (0  << 3) | 1 = 0x01
 *   body  = {0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16}
 *
 * MTU=6  →  max_frag = 6 - 3 = 3  →  3 FU packets.
 *
 * Reconstructed FU PayloadHdr:
 *   hdr0 = (0x26 & 0x81) | (49 << 1)
 *        =  0x00        | 0x62          = 0x62
 *   hdr1 = 0x01  (TID preserved — critical LayerId/TID bug check)
 *
 * FU header:
 *   frag1: S|19 = 0x80 | 19 = 0x93
 *   frag2:   19 =               0x13
 *   frag3: E|19 = 0x40 | 19 = 0x53
 * ================================================================ */

TEST(test_h265_pack_fu_byte_exact)
{
    uint8_t nalu[] = {0x26, 0x01, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16};
    pkt_collector_t c;
    memset(&c, 0, sizeof(c));

    ASSERT_OK(h265_packetize(nalu, sizeof(nalu), 6, collect_cb, &c));
    ASSERT_EQ(c.count, 3);

    /* Fragment 1: [0x62, 0x01, 0x93, 0x10, 0x11, 0x12] */
    uint8_t exp1[] = {0x62, 0x01, 0x93, 0x10, 0x11, 0x12};
    ASSERT_EQ(c.frag_lens[0], sizeof(exp1));
    ASSERT_MEM_EQ(c.frags[0], exp1, sizeof(exp1));
    ASSERT_EQ(c.markers[0], 0);

    /* Fragment 2: [0x62, 0x01, 0x13, 0x13, 0x14, 0x15] */
    uint8_t exp2[] = {0x62, 0x01, 0x13, 0x13, 0x14, 0x15};
    ASSERT_EQ(c.frag_lens[1], sizeof(exp2));
    ASSERT_MEM_EQ(c.frags[1], exp2, sizeof(exp2));
    ASSERT_EQ(c.markers[1], 0);

    /* Fragment 3: [0x62, 0x01, 0x53, 0x16] with marker=1 */
    uint8_t exp3[] = {0x62, 0x01, 0x53, 0x16};
    ASSERT_EQ(c.frag_lens[2], sizeof(exp3));
    ASSERT_MEM_EQ(c.frags[2], exp3, sizeof(exp3));
    ASSERT_EQ(c.markers[2], 1);
}

TEST(test_h265_fu_roundtrip)
{
    /* Same input as pack_fu_byte_exact — reassemble and assert byte-equal. */
    uint8_t nalu[] = {0x26, 0x01, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16};
    pkt_collector_t c;
    memset(&c, 0, sizeof(c));
    ASSERT_OK(h265_packetize(nalu, sizeof(nalu), 6, collect_cb, &c));
    ASSERT_EQ(c.count, 3);

    nano_h265_depkt_t d;
    h265_depkt_init(&d);

    const uint8_t *out = NULL;
    size_t out_len = 0;

    /* Start fragment: in progress, no output yet. */
    ASSERT_OK(h265_depkt_push(&d, c.frags[0], c.frag_lens[0], 0, &out, &out_len));
    ASSERT_TRUE(out == NULL);
    ASSERT_EQ(out_len, 0);

    /* Middle fragment: still in progress. */
    ASSERT_OK(h265_depkt_push(&d, c.frags[1], c.frag_lens[1], 0, &out, &out_len));
    ASSERT_TRUE(out == NULL);

    /* End fragment: complete NAL reassembled. */
    ASSERT_OK(h265_depkt_push(&d, c.frags[2], c.frag_lens[2], 1, &out, &out_len));
    ASSERT_EQ(out_len, sizeof(nalu));
    ASSERT_MEM_EQ(out, nalu, sizeof(nalu));

    /* Critical: LayerId/TID must survive the FU envelope unchanged. */
    ASSERT_EQ(out[1], 0x01);
}

TEST(test_h265_fu_large_nal)
{
    /* 800-byte IDR_N_LP (type=20) with MTU=200 → max_frag = 197 → 5 FU packets. */
    uint8_t nalu[800];
    nalu[0] = 0x28; /* (20 << 1) | 0 */
    nalu[1] = 0x01;
    for (size_t i = 2; i < sizeof(nalu); i++) {
        nalu[i] = (uint8_t)(i & 0xFF);
    }

    pkt_collector_t c;
    memset(&c, 0, sizeof(c));
    ASSERT_OK(h265_packetize(nalu, sizeof(nalu), 200, collect_cb, &c));
    ASSERT_EQ(c.count, 5);

    /* First packet: S bit set. */
    ASSERT_EQ(c.frags[0][0], 0x62);
    ASSERT_EQ(c.frags[0][1], 0x01);
    ASSERT_EQ(c.frags[0][2] & 0x80, 0x80); /* S */
    ASSERT_EQ(c.frags[0][2] & 0x3F, 20);   /* type 20 */
    ASSERT_EQ(c.markers[0], 0);

    /* Last packet: E bit set, marker=1. */
    ASSERT_EQ(c.frags[4][2] & 0x40, 0x40); /* E */
    ASSERT_EQ(c.markers[4], 1);

    /* Middle packets: no S, no E. */
    for (int i = 1; i < 4; i++) {
        ASSERT_EQ(c.frags[i][2] & 0xC0, 0x00);
        ASSERT_EQ(c.markers[i], 0);
    }

    /* Roundtrip. */
    nano_h265_depkt_t d;
    h265_depkt_init(&d);
    const uint8_t *out = NULL;
    size_t out_len = 0;
    for (int i = 0; i < c.count; i++) {
        int is_last = (i == c.count - 1);
        ASSERT_OK(h265_depkt_push(&d, c.frags[i], c.frag_lens[i], is_last, &out, &out_len));
    }
    ASSERT_EQ(out_len, sizeof(nalu));
    ASSERT_MEM_EQ(out, nalu, sizeof(nalu));
}

/* ================================================================
 * Vector C — Aggregation Packet with VPS+SPS+PPS (RFC 7798 §4.4.2)
 *
 * PayloadHdr: type=48 (AP), F=0, LayerId=0, TID=1 → byte0=0x60, byte1=0x01
 *
 * Sub-NALs:
 *   VPS (type=32): byte0 = (32<<1) = 0x40, byte1 = 0x01, body 0xAA
 *   SPS (type=33): byte0 = (33<<1) = 0x42, byte1 = 0x01, body 0xBB 0xCC
 *   PPS (type=34): byte0 = (34<<1) = 0x44, byte1 = 0x01, body 0xDD
 * ================================================================ */

TEST(test_h265_depkt_ap_first_nal)
{
    uint8_t ap[] = {
        0x60, 0x01,                         /* PayloadHdr: AP, TID=1 */
        0x00, 0x03, 0x40, 0x01, 0xAA,       /* VPS len=3 */
        0x00, 0x04, 0x42, 0x01, 0xBB, 0xCC, /* SPS len=4 */
        0x00, 0x03, 0x44, 0x01, 0xDD        /* PPS len=3 */
    };

    nano_h265_depkt_t d;
    h265_depkt_init(&d);
    const uint8_t *out = NULL;
    size_t out_len = 0;

    ASSERT_OK(h265_depkt_push(&d, ap, sizeof(ap), 0, &out, &out_len));
    ASSERT_EQ(out_len, 3); /* First sub-NAL = VPS (3 bytes) */
    uint8_t exp_vps[] = {0x40, 0x01, 0xAA};
    ASSERT_MEM_EQ(out, exp_vps, 3);
}

TEST(test_h265_ap_keyframe_detection)
{
    /* AP with no IRAP sub-NAL → not keyframe. */
    uint8_t ap_no_irap[] = {
        0x60, 0x01, 0x00, 0x03, 0x40, 0x01, 0xAA, /* VPS */
        0x00, 0x03, 0x42, 0x01, 0xBB,             /* SPS */
    };
    ASSERT_EQ(h265_is_keyframe(ap_no_irap, sizeof(ap_no_irap)), 0);

    /* AP containing IDR_W_RADL (type=19) → keyframe.
     * IDR byte0 = (19<<1)|0 = 0x26 */
    uint8_t ap_with_idr[] = {
        0x60, 0x01, 0x00, 0x03, 0x40, 0x01, 0xAA, /* VPS */
        0x00, 0x04, 0x26, 0x01, 0xDE, 0xAD,       /* IDR_W_RADL */
    };
    ASSERT_EQ(h265_is_keyframe(ap_with_idr, sizeof(ap_with_idr)), 1);
}

/* ================================================================
 * Vector D — Keyframe classification (RFC 7798 §1.1.4 Table 1)
 * ================================================================ */

TEST(test_h265_is_keyframe_single_nal)
{
    /* IRAP set: types 16..21 → keyframe */
    for (uint8_t t = 16; t <= 21; t++) {
        uint8_t nal[] = {(uint8_t)(t << 1), 0x01, 0xAA};
        char msg[64];
        snprintf(msg, sizeof(msg), "type %u should be keyframe", t);
        TEST_ASSERT_TRUE_MESSAGE(h265_is_keyframe(nal, sizeof(nal)) == 1, msg);
    }

    /* Non-IRAP: TRAIL_R=1, VPS=32, SPS=33, PPS=34 */
    uint8_t tr[] = {0x02, 0x01, 0xAA};
    ASSERT_EQ(h265_is_keyframe(tr, sizeof(tr)), 0);
    uint8_t vps[] = {0x40, 0x01, 0xAA};
    ASSERT_EQ(h265_is_keyframe(vps, sizeof(vps)), 0);
    uint8_t sps[] = {0x42, 0x01, 0xAA};
    ASSERT_EQ(h265_is_keyframe(sps, sizeof(sps)), 0);
    uint8_t pps[] = {0x44, 0x01, 0xAA};
    ASSERT_EQ(h265_is_keyframe(pps, sizeof(pps)), 0);

    /* Boundary: type 15 (RSV), type 22 (RSV) → not IRAP */
    uint8_t rsv15[] = {(uint8_t)(15 << 1), 0x01, 0xAA};
    ASSERT_EQ(h265_is_keyframe(rsv15, sizeof(rsv15)), 0);
    uint8_t rsv22[] = {(uint8_t)(22 << 1), 0x01, 0xAA};
    ASSERT_EQ(h265_is_keyframe(rsv22, sizeof(rsv22)), 0);
}

TEST(test_h265_is_keyframe_fu)
{
    /* FU start with IDR_W_RADL (type=19) → keyframe */
    uint8_t fu_start_idr[] = {0x62, 0x01, 0x93, 0xAA};
    ASSERT_EQ(h265_is_keyframe(fu_start_idr, sizeof(fu_start_idr)), 1);

    /* FU start with CRA_NUT (type=21) → keyframe */
    uint8_t fu_start_cra[] = {0x62, 0x01, 0x95, 0xAA}; /* 0x95 = S|21 */
    ASSERT_EQ(h265_is_keyframe(fu_start_cra, sizeof(fu_start_cra)), 1);

    /* FU continuation (no S bit) → cannot classify, return 0 */
    uint8_t fu_cont_idr[] = {0x62, 0x01, 0x13, 0xAA};
    ASSERT_EQ(h265_is_keyframe(fu_cont_idr, sizeof(fu_cont_idr)), 0);

    /* FU start with TRAIL_R → not keyframe */
    uint8_t fu_start_trail[] = {0x62, 0x01, 0x81, 0xAA}; /* 0x81 = S|1 */
    ASSERT_EQ(h265_is_keyframe(fu_start_trail, sizeof(fu_start_trail)), 0);
}

/* ================================================================
 * Vector E — Real sample frame smoke test
 *
 * Parse examples/sample_data/h265SampleFrames/frame-0001.h265 with the
 * codec-agnostic Annex-B scanner and verify the first NAL is VPS (32)
 * and the second is SPS (33). This guards against byte-order mistakes
 * in the Annex-B loop and confirms the enum constants on real data.
 * ================================================================ */

TEST(test_h265_real_frame_annex_b)
{
    /* Paths: prefer CMake-provided SAMPLE_DATA_DIR if defined, fall back
     * to a relative path from the build tree. */
#ifndef H265_SAMPLE_DIR
#define H265_SAMPLE_DIR "examples/sample_data/h265SampleFrames"
#endif

    const char *path_candidates[] = {
        H265_SAMPLE_DIR "/frame-0001.h265",
        "../examples/sample_data/h265SampleFrames/frame-0001.h265",
        "../../examples/sample_data/h265SampleFrames/frame-0001.h265",
    };

    FILE *f = NULL;
    for (size_t i = 0; i < sizeof(path_candidates) / sizeof(path_candidates[0]); i++) {
        f = fopen(path_candidates[i], "rb");
        if (f) {
            break;
        }
    }
    if (!f) {
        TEST_IGNORE_MESSAGE("h265 sample frame not reachable from CWD — skipping");
        return;
    }

    uint8_t buf[16384];
    size_t n = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    ASSERT_TRUE(n > 32);

    size_t offset = 0;
    size_t nal_len = 0;

    const uint8_t *nal1 = h264_annex_b_find_nal(buf, n, &offset, &nal_len);
    TEST_ASSERT_NOT_NULL(nal1);
    ASSERT_TRUE(nal_len >= 2);
    ASSERT_EQ((nal1[0] >> 1) & 0x3F, H265_NAL_VPS);

    const uint8_t *nal2 = h264_annex_b_find_nal(buf, n, &offset, &nal_len);
    TEST_ASSERT_NOT_NULL(nal2);
    ASSERT_TRUE(nal_len >= 2);
    ASSERT_EQ((nal2[0] >> 1) & 0x3F, H265_NAL_SPS);
}

/* ================================================================
 * Vector F — Buffer overflow safety
 * ================================================================ */

TEST(test_h265_depkt_single_nal_too_large)
{
    /* Single NAL > NANORTC_VIDEO_NAL_BUF_SIZE → refuse and return error. */
    static uint8_t oversized[NANORTC_VIDEO_NAL_BUF_SIZE + 16];
    oversized[0] = 0x02; /* TRAIL_R */
    oversized[1] = 0x01;

    nano_h265_depkt_t d;
    h265_depkt_init(&d);
    const uint8_t *out = NULL;
    size_t out_len = 0;
    int rc = h265_depkt_push(&d, oversized, sizeof(oversized), 1, &out, &out_len);
    ASSERT_FAIL(rc);
    /* State must remain safe to reuse. */
    ASSERT_EQ(d.in_progress, 0);
}

TEST(test_h265_depkt_fu_oversized_reassembly)
{
    /* FU reassembly that exceeds the buffer returns error and resets state. */
    nano_h265_depkt_t d;
    h265_depkt_init(&d);

    /* Start fragment with nearly-max payload. */
    static uint8_t big_start[NANORTC_VIDEO_NAL_BUF_SIZE];
    big_start[0] = 0x62; /* FU PayloadHdr */
    big_start[1] = 0x01;
    big_start[2] = 0x93; /* S | IDR_W_RADL */
    memset(big_start + 3, 0xAB, sizeof(big_start) - 3);

    const uint8_t *out = NULL;
    size_t out_len = 0;
    int rc = h265_depkt_push(&d, big_start, sizeof(big_start), 0, &out, &out_len);
    /* Start itself may or may not fit — accept either outcome.
     * The critical test is the continuation that pushes past the limit. */
    if (rc == NANORTC_OK) {
        static uint8_t big_cont[256];
        big_cont[0] = 0x62;
        big_cont[1] = 0x01;
        big_cont[2] = 0x13; /* no S/E */
        memset(big_cont + 3, 0xCD, sizeof(big_cont) - 3);
        rc = h265_depkt_push(&d, big_cont, sizeof(big_cont), 0, &out, &out_len);
    }
    ASSERT_FAIL(rc);
    ASSERT_EQ(d.in_progress, 0);
}

/* ================================================================
 * Vector G — Invalid input rejection
 * ================================================================ */

TEST(test_h265_pack_rejects_fu_input)
{
    /* Input already has type=49 (FU). RFC 7798 §4.4.2 forbids re-aggregation. */
    uint8_t fu[] = {0x62, 0x01, 0x93, 0xAA};
    pkt_collector_t c;
    memset(&c, 0, sizeof(c));
    int rc = h265_packetize(fu, sizeof(fu), 1200, collect_cb, &c);
    ASSERT_FAIL(rc);
    ASSERT_EQ(c.count, 0);
}

TEST(test_h265_pack_rejects_ap_input)
{
    /* Input already has type=48 (AP). Forbid nesting. */
    uint8_t ap[] = {0x60, 0x01, 0x00, 0x03, 0x40, 0x01, 0xAA};
    pkt_collector_t c;
    memset(&c, 0, sizeof(c));
    int rc = h265_packetize(ap, sizeof(ap), 1200, collect_cb, &c);
    ASSERT_FAIL(rc);
}

TEST(test_h265_pack_rejects_short_nal)
{
    /* NAL shorter than 2-byte header → invalid. */
    uint8_t short_nal[] = {0x02};
    pkt_collector_t c;
    memset(&c, 0, sizeof(c));
    int rc = h265_packetize(short_nal, sizeof(short_nal), 1200, collect_cb, &c);
    ASSERT_FAIL(rc);
}

TEST(test_h265_depkt_rejects_null)
{
    nano_h265_depkt_t d;
    h265_depkt_init(&d);
    const uint8_t *out = NULL;
    size_t out_len = 0;
    int rc = h265_depkt_push(&d, NULL, 5, 0, &out, &out_len);
    ASSERT_FAIL(rc);
}

/* ================================================================
 * FU continuation without start — graceful handling
 * ================================================================ */

TEST(test_h265_depkt_fu_continuation_without_start)
{
    /* Continuation fragment arriving before any S=1 start: log + ignore. */
    uint8_t frag[] = {0x62, 0x01, 0x13, 0xAA, 0xBB};
    nano_h265_depkt_t d;
    h265_depkt_init(&d);
    const uint8_t *out = NULL;
    size_t out_len = 0;
    ASSERT_OK(h265_depkt_push(&d, frag, sizeof(frag), 0, &out, &out_len));
    ASSERT_TRUE(out == NULL);
    ASSERT_EQ(out_len, 0);
    ASSERT_EQ(d.in_progress, 0);
}

/* ================================================================
 * Test runner
 * ================================================================ */

TEST_MAIN_BEGIN("test_h265")
RUN(test_h265_pack_single_nal);
RUN(test_h265_pack_exact_mtu);
RUN(test_h265_pack_fu_byte_exact);
RUN(test_h265_fu_roundtrip);
RUN(test_h265_fu_large_nal);
RUN(test_h265_depkt_ap_first_nal);
RUN(test_h265_ap_keyframe_detection);
RUN(test_h265_is_keyframe_single_nal);
RUN(test_h265_is_keyframe_fu);
RUN(test_h265_real_frame_annex_b);
RUN(test_h265_depkt_single_nal_too_large);
RUN(test_h265_depkt_fu_oversized_reassembly);
RUN(test_h265_pack_rejects_fu_input);
RUN(test_h265_pack_rejects_ap_input);
RUN(test_h265_pack_rejects_short_nal);
RUN(test_h265_depkt_rejects_null);
RUN(test_h265_depkt_fu_continuation_without_start);
TEST_MAIN_END
