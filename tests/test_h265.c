/*
 * nanortc — H.265/HEVC packetizer/depacketizer tests (RFC 7798)
 *
 * All test vectors are generated independently from RFC 7798:
 *   - NAL headers are constructed from scratch using the §1.1.4 bit layout.
 *   - FU PayloadHdr / FU header bytes are constructed from scratch using
 *     the §4.4.3 bit layout.
 *   - Aggregation Packet framing is constructed from scratch using the
 *     §4.4.2 layout.
 *
 * No byte sequences are copied from libdatachannel, libwebrtc, or str0m.
 * Roundtrip checks (pack → unpack → byte-compare) are supplementary to
 * these hand-crafted vectors, as required by the project's RFC iron rule.
 *
 * SPDX-License-Identifier: MIT
 */

#include "nanortc.h"
#include "nano_h265.h"
#include "nano_test.h"
#include <string.h>

/* ================================================================
 * Packetizer callback helper (identical shape to test_h264.c)
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
 * Helper: build an H.265 NAL header (2 bytes) from fields
 * ================================================================ */

static void build_nal_header(uint8_t out[2], uint8_t type, uint8_t layer_id, uint8_t tid, int f_bit)
{
    out[0] = (uint8_t)((f_bit ? 0x80 : 0x00) | ((type & 0x3F) << 1) | ((layer_id >> 5) & 0x01));
    out[1] = (uint8_t)(((layer_id & 0x1F) << 3) | (tid & 0x07));
}

/* ================================================================
 * Packetizer — Single NAL Unit Packet tests (RFC 7798 §4.4.1)
 * ================================================================ */

TEST(test_h265_pack_single_nal)
{
    /* NAL ≤ MTU: emitted unchanged, marker=1. IDR_W_RADL (type=19). */
    uint8_t nalu[] = {
        0x26, 0x01,                   /* NAL header: type=19, layer=0, TID=1 */
        0xDE, 0xAD, 0xBE, 0xEF, 0x42, /* RBSP payload */
    };
    pkt_collector_t c = {0};

    ASSERT_OK(h265_packetize(nalu, sizeof(nalu), 1200, collect_cb, &c));
    ASSERT_EQ(c.count, 1);
    ASSERT_EQ(c.frag_lens[0], sizeof(nalu));
    ASSERT_MEM_EQ(c.frags[0], nalu, sizeof(nalu));
    ASSERT_EQ(c.markers[0], 1);
}

TEST(test_h265_pack_single_nal_header_type_extract)
{
    /* Sanity: encoded header byte 0 has type in bits 6..1.
     * For type=19 (IDR_W_RADL), layer=0, TID=1, F=0:
     *   byte0 = (19 << 1) = 0x26, byte1 = 0x01
     */
    uint8_t hdr[2];
    build_nal_header(hdr, 19, 0, 1, 0);
    ASSERT_EQ(hdr[0], 0x26);
    ASSERT_EQ(hdr[1], 0x01);
    ASSERT_EQ(H265_NAL_TYPE(hdr), 19);
    ASSERT_EQ(H265_NAL_LAYER_ID(hdr), 0);
    ASSERT_EQ(H265_NAL_TID(hdr), 1);
}

TEST(test_h265_pack_exact_mtu)
{
    /* NAL exactly at MTU size: should still emit a single Single-NAL packet. */
    uint8_t nalu[200];
    nalu[0] = 0x26; /* type=19, layer=0, LayerId_msb=0 */
    nalu[1] = 0x01; /* TID=1 */
    for (size_t i = 2; i < sizeof(nalu); ++i) {
        nalu[i] = (uint8_t)(i & 0xFF);
    }
    pkt_collector_t c = {0};

    ASSERT_OK(h265_packetize(nalu, sizeof(nalu), sizeof(nalu), collect_cb, &c));
    ASSERT_EQ(c.count, 1);
    ASSERT_EQ(c.frag_lens[0], sizeof(nalu));
    ASSERT_EQ(c.markers[0], 1);
}

/* ================================================================
 * Packetizer — Fragmentation Unit tests (RFC 7798 §4.4.3)
 * ================================================================ */

TEST(test_h265_pack_fu_basic)
{
    /* NAL = 2-byte header + 300 bytes payload; MTU = 100.
     * Max fragment payload = 100 - 3 (FU overhead: 2 PayloadHdr + 1 FU hdr) = 97.
     * Need ceil(300 / 97) = 4 fragments. */
    uint8_t nalu[302];
    build_nal_header(nalu, 19 /* IDR_W_RADL */, 0, 1, 0);
    for (size_t i = 2; i < sizeof(nalu); ++i) {
        nalu[i] = (uint8_t)(i - 2);
    }
    pkt_collector_t c = {0};

    ASSERT_OK(h265_packetize(nalu, sizeof(nalu), 100, collect_cb, &c));
    ASSERT_EQ(c.count, 4);

    /* Every fragment begins with PayloadHdr (type=49, layer=0, TID=1). */
    for (int i = 0; i < c.count; ++i) {
        /* PayloadHdr byte 0: F=0, type=49 (0x62 in bits 6..1), LayerId_msb=0 → 0x62 */
        ASSERT_EQ(c.frags[i][0], 0x62);
        ASSERT_EQ(c.frags[i][1], 0x01); /* LayerId_lsb=0, TID=1 */
    }

    /* Fragment 0: Start bit set, FuType=19 → FU hdr = 0x80 | 19 = 0x93 */
    ASSERT_EQ(c.frags[0][2], 0x93);
    ASSERT_EQ(c.markers[0], 0);

    /* Fragments 1 and 2: middle (S=0, E=0) → FU hdr = 0x13 */
    ASSERT_EQ(c.frags[1][2], 0x13);
    ASSERT_EQ(c.markers[1], 0);
    ASSERT_EQ(c.frags[2][2], 0x13);
    ASSERT_EQ(c.markers[2], 0);

    /* Fragment 3: End bit set → FU hdr = 0x40 | 19 = 0x53 */
    ASSERT_EQ(c.frags[3][2], 0x53);
    ASSERT_EQ(c.markers[3], 1);
}

TEST(test_h265_pack_fu_layerid_tid_preserved)
{
    /* RFC 7798 §4.4.3: "LayerId and TID MUST be equal to the LayerId and TID
     * of the fragmented NAL unit." Verify with non-zero LayerId (25) and
     * non-default TID (6). */
    uint8_t nalu[160];
    build_nal_header(nalu, 20 /* IDR_N_LP */, 25, 6, 0);
    for (size_t i = 2; i < sizeof(nalu); ++i) {
        nalu[i] = 0xAA;
    }
    pkt_collector_t c = {0};

    ASSERT_OK(h265_packetize(nalu, sizeof(nalu), 80, collect_cb, &c));
    ASSERT_TRUE(c.count >= 2);

    for (int i = 0; i < c.count; ++i) {
        /* PayloadHdr must preserve LayerId=25 and TID=6, with Type replaced by 49.
         * byte 0 = 0 (F) | (49<<1)=0x62 | LayerId_msb=(25>>5)&1=0 → 0x62
         * byte 1 = (25 & 0x1F) << 3 | 6 = (25 << 3) | 6 = 0xCE */
        ASSERT_EQ(c.frags[i][0], 0x62);
        ASSERT_EQ(c.frags[i][1], 0xCE);
        ASSERT_EQ(H265_NAL_LAYER_ID(c.frags[i]), 25);
        ASSERT_EQ(H265_NAL_TID(c.frags[i]), 6);
    }
}

TEST(test_h265_pack_fu_layerid_msb_nonzero)
{
    /* LayerId 33 (> 31) puts the MSB bit in byte[0] bit 0.
     * byte 0 = 0 | (49<<1) | ((33>>5)&1)=1 → 0x62 | 0x01 = 0x63
     * byte 1 = (33 & 0x1F) << 3 | 2 = (1<<3) | 2 = 0x0A */
    uint8_t nalu[160];
    build_nal_header(nalu, 19, 33, 2, 0);
    for (size_t i = 2; i < sizeof(nalu); ++i) {
        nalu[i] = 0x55;
    }
    pkt_collector_t c = {0};

    ASSERT_OK(h265_packetize(nalu, sizeof(nalu), 80, collect_cb, &c));
    ASSERT_TRUE(c.count >= 2);
    for (int i = 0; i < c.count; ++i) {
        ASSERT_EQ(c.frags[i][0], 0x63);
        ASSERT_EQ(c.frags[i][1], 0x0A);
        ASSERT_EQ(H265_NAL_LAYER_ID(c.frags[i]), 33);
        ASSERT_EQ(H265_NAL_TID(c.frags[i]), 2);
    }
}

TEST(test_h265_pack_fu_f_bit_preserved)
{
    /* F=1 in original NAL header must be preserved in every FU PayloadHdr. */
    uint8_t nalu[160];
    build_nal_header(nalu, 19, 0, 1, 1 /* F=1 */);
    for (size_t i = 2; i < sizeof(nalu); ++i) {
        nalu[i] = 0xFE;
    }
    pkt_collector_t c = {0};

    ASSERT_OK(h265_packetize(nalu, sizeof(nalu), 80, collect_cb, &c));
    ASSERT_TRUE(c.count >= 2);
    for (int i = 0; i < c.count; ++i) {
        /* byte 0 = 0x80 (F) | (49<<1) | 0 = 0xE2 */
        ASSERT_EQ(c.frags[i][0], 0xE2);
    }
}

TEST(test_h265_pack_fu_single_byte_tail)
{
    /* Construct a NAL where the last fragment carries exactly 1 payload byte.
     * MTU=20, FU overhead=3 → max_frag=17.
     *
     * For FU to kick in at all, nalu_len MUST exceed MTU (otherwise Single
     * NAL Unit Packet is used). Choose payload = 35 bytes so:
     *   frag1 = 17, frag2 = 17, frag3 = 1   (total 35)
     * Total NAL = 2 (header) + 35 = 37 bytes > MTU=20. */
    uint8_t nalu[37];
    build_nal_header(nalu, 1 /* TRAIL_N */, 0, 1, 0);
    for (size_t i = 2; i < sizeof(nalu); ++i) {
        nalu[i] = (uint8_t)i;
    }
    pkt_collector_t c = {0};

    ASSERT_OK(h265_packetize(nalu, sizeof(nalu), 20, collect_cb, &c));
    ASSERT_EQ(c.count, 3);
    /* First fragment: S=1, 17 bytes of data → total 3 + 17 = 20 */
    ASSERT_EQ(c.frag_lens[0], 20u);
    ASSERT_EQ(c.frags[0][2] & H265_FU_S_BIT, H265_FU_S_BIT);
    ASSERT_EQ(c.frags[0][2] & H265_FU_E_BIT, 0u);
    /* Second fragment: middle (S=0, E=0), 17 bytes of data */
    ASSERT_EQ(c.frag_lens[1], 20u);
    ASSERT_EQ(c.frags[1][2] & H265_FU_S_BIT, 0u);
    ASSERT_EQ(c.frags[1][2] & H265_FU_E_BIT, 0u);
    /* Third fragment: E=1, 1 byte of data → total 3 + 1 = 4 */
    ASSERT_EQ(c.frag_lens[2], 4u);
    ASSERT_EQ(c.frags[2][2] & H265_FU_E_BIT, H265_FU_E_BIT);
    ASSERT_EQ(c.markers[2], 1);
}

TEST(test_h265_pack_fu_mtu_too_small)
{
    /* MTU ≤ FU header (3) → cannot carry any payload bytes → invalid. */
    uint8_t nalu[100];
    build_nal_header(nalu, 19, 0, 1, 0);
    memset(nalu + 2, 0x55, sizeof(nalu) - 2);
    pkt_collector_t c = {0};

    int rc = h265_packetize(nalu, sizeof(nalu), 3, collect_cb, &c);
    ASSERT_FAIL(rc);
}

TEST(test_h265_pack_null_params)
{
    uint8_t nalu[4] = {0x26, 0x01, 0xAA, 0xBB};
    pkt_collector_t c = {0};

    ASSERT_FAIL(h265_packetize(NULL, sizeof(nalu), 1200, collect_cb, &c));
    ASSERT_FAIL(h265_packetize(nalu, 0, 1200, collect_cb, &c));
    ASSERT_FAIL(h265_packetize(nalu, 1, 1200, collect_cb, &c)); /* Below header size */
    ASSERT_FAIL(h265_packetize(nalu, sizeof(nalu), 1200, NULL, &c));
}

TEST(test_h265_pack_minimum_nal)
{
    /* 2-byte NAL (just the header) — no RBSP payload. Spec doesn't prohibit
     * this; treat as Single NAL passthrough. */
    uint8_t nalu[] = {0x26, 0x01};
    pkt_collector_t c = {0};

    ASSERT_OK(h265_packetize(nalu, sizeof(nalu), 1200, collect_cb, &c));
    ASSERT_EQ(c.count, 1);
    ASSERT_EQ(c.frag_lens[0], 2u);
    ASSERT_MEM_EQ(c.frags[0], nalu, 2);
    ASSERT_EQ(c.markers[0], 1);
}

/* ================================================================
 * Packetizer — Aggregation Packet tests (RFC 7798 §4.4.2)
 * ================================================================ */

TEST(test_h265_pack_ap_two_small)
{
    /* Two 10-byte NALs: total raw bytes = 20; AP overhead = 2 (hdr) + 2*2 (lengths) = 6.
     * Total AP = 26 bytes, fits easily in MTU=1200. */
    uint8_t vps[10];
    uint8_t sps[10];
    build_nal_header(vps, H265_NAL_VPS_NUT, 0, 1, 0);
    build_nal_header(sps, H265_NAL_SPS_NUT, 0, 1, 0);
    for (size_t i = 2; i < sizeof(vps); ++i) {
        vps[i] = 0x10 + (uint8_t)i;
    }
    for (size_t i = 2; i < sizeof(sps); ++i) {
        sps[i] = 0x20 + (uint8_t)i;
    }

    h265_nal_ref_t nals[2] = {
        {.data = vps, .len = sizeof(vps)},
        {.data = sps, .len = sizeof(sps)},
    };
    pkt_collector_t c = {0};

    ASSERT_OK(h265_packetize_au(nals, 2, 1200, collect_cb, &c));
    ASSERT_EQ(c.count, 1);
    ASSERT_EQ(c.markers[0], 1);
    ASSERT_EQ(c.frag_lens[0], 2u + 2u + 10u + 2u + 10u); /* hdr + len1 + nal1 + len2 + nal2 */

    /* PayloadHdr must have Type=48. byte 0 bits 6..1 = 48 = 0x60 → byte 0 = 0x60. */
    ASSERT_EQ(c.frags[0][0], 0x60);
    ASSERT_EQ(c.frags[0][1], 0x01);

    /* First length prefix: 10 */
    ASSERT_EQ(c.frags[0][2], 0x00);
    ASSERT_EQ(c.frags[0][3], 0x0A);
    /* First NAL payload */
    ASSERT_MEM_EQ(c.frags[0] + 4, vps, 10);

    /* Second length prefix */
    ASSERT_EQ(c.frags[0][14], 0x00);
    ASSERT_EQ(c.frags[0][15], 0x0A);
    ASSERT_MEM_EQ(c.frags[0] + 16, sps, 10);
}

TEST(test_h265_pack_ap_three_nals_vps_sps_pps)
{
    /* Typical access unit prelude: VPS (~24) + SPS (~48) + PPS (~12).
     * Total raw = 84; AP overhead = 2 + 2*3 = 8; total = 92 bytes in one AP. */
    uint8_t vps[24], sps[48], pps[12];
    build_nal_header(vps, H265_NAL_VPS_NUT, 0, 1, 0);
    build_nal_header(sps, H265_NAL_SPS_NUT, 0, 1, 0);
    build_nal_header(pps, H265_NAL_PPS_NUT, 0, 1, 0);
    memset(vps + 2, 0xA1, sizeof(vps) - 2);
    memset(sps + 2, 0xB2, sizeof(sps) - 2);
    memset(pps + 2, 0xC3, sizeof(pps) - 2);

    h265_nal_ref_t nals[3] = {
        {.data = vps, .len = sizeof(vps)},
        {.data = sps, .len = sizeof(sps)},
        {.data = pps, .len = sizeof(pps)},
    };
    pkt_collector_t c = {0};

    ASSERT_OK(h265_packetize_au(nals, 3, 1200, collect_cb, &c));
    ASSERT_EQ(c.count, 1);
    ASSERT_EQ(c.frag_lens[0], 2u + (2u + 24u) + (2u + 48u) + (2u + 12u));
    ASSERT_EQ(c.frags[0][0], 0x60);
    ASSERT_EQ(c.markers[0], 1);
}

TEST(test_h265_pack_ap_layerid_min)
{
    /* RFC 7798 §4.4.2: AP PayloadHdr LayerId MUST be the smallest LayerId
     * of the aggregated NAL units. Use NALs with LayerId 5 and 2 → min = 2. */
    uint8_t n1[20], n2[20];
    build_nal_header(n1, H265_NAL_VPS_NUT, 5, 3, 0);
    build_nal_header(n2, H265_NAL_SPS_NUT, 2, 5, 0);
    memset(n1 + 2, 0xAA, 18);
    memset(n2 + 2, 0xBB, 18);

    h265_nal_ref_t nals[2] = {
        {.data = n1, .len = sizeof(n1)},
        {.data = n2, .len = sizeof(n2)},
    };
    pkt_collector_t c = {0};

    ASSERT_OK(h265_packetize_au(nals, 2, 1200, collect_cb, &c));
    ASSERT_EQ(c.count, 1);
    /* PayloadHdr encodes LayerId=2, TID=min(3,5)=3 */
    ASSERT_EQ(H265_NAL_LAYER_ID(c.frags[0]), 2);
    ASSERT_EQ(H265_NAL_TID(c.frags[0]), 3);
}

TEST(test_h265_pack_ap_f_bit_union)
{
    /* F bit of AP PayloadHdr MUST be set iff any aggregated NAL has F=1. */
    uint8_t n1[20], n2[20];
    build_nal_header(n1, H265_NAL_VPS_NUT, 0, 1, 0); /* F=0 */
    build_nal_header(n2, H265_NAL_SPS_NUT, 0, 1, 1); /* F=1 */
    memset(n1 + 2, 0xAA, 18);
    memset(n2 + 2, 0xBB, 18);

    h265_nal_ref_t nals[2] = {
        {.data = n1, .len = sizeof(n1)},
        {.data = n2, .len = sizeof(n2)},
    };
    pkt_collector_t c = {0};

    ASSERT_OK(h265_packetize_au(nals, 2, 1200, collect_cb, &c));
    ASSERT_EQ(c.count, 1);
    ASSERT_EQ(c.frags[0][0] & H265_NAL_F_BIT, H265_NAL_F_BIT);
}

TEST(test_h265_pack_au_mixed_ap_plus_fu)
{
    /* Access unit: 3 small NALs (fit in AP) + 1 large NAL (FU).
     * Expect: 1 AP emission + N FU emissions. */
    uint8_t vps[16], sps[16], pps[16];
    uint8_t idr[500];
    build_nal_header(vps, H265_NAL_VPS_NUT, 0, 1, 0);
    build_nal_header(sps, H265_NAL_SPS_NUT, 0, 1, 0);
    build_nal_header(pps, H265_NAL_PPS_NUT, 0, 1, 0);
    build_nal_header(idr, H265_NAL_IDR_W_RADL, 0, 1, 0);
    memset(vps + 2, 0x11, 14);
    memset(sps + 2, 0x22, 14);
    memset(pps + 2, 0x33, 14);
    memset(idr + 2, 0x44, 498);

    h265_nal_ref_t nals[4] = {
        {.data = vps, .len = sizeof(vps)},
        {.data = sps, .len = sizeof(sps)},
        {.data = pps, .len = sizeof(pps)},
        {.data = idr, .len = sizeof(idr)},
    };
    pkt_collector_t c = {0};

    ASSERT_OK(h265_packetize_au(nals, 4, 200, collect_cb, &c));
    /* First emission: AP with 3 parameter sets */
    ASSERT_EQ(c.frags[0][0], 0x60); /* PayloadHdr Type=48 */
    ASSERT_EQ(c.markers[0], 0);

    /* Remaining emissions: FU fragments (type=49 in PayloadHdr, 0x62) */
    for (int i = 1; i < c.count; ++i) {
        ASSERT_EQ(c.frags[i][0], 0x62);
    }
    /* Only the final FU fragment has marker=1 */
    ASSERT_EQ(c.markers[c.count - 1], 1);
    for (int i = 0; i < c.count - 1; ++i) {
        ASSERT_EQ(c.markers[i], 0);
    }
}

TEST(test_h265_pack_au_all_large)
{
    /* All NALs large → each becomes its own FU sequence. */
    uint8_t n1[400], n2[400];
    build_nal_header(n1, H265_NAL_TRAIL_R, 0, 1, 0);
    build_nal_header(n2, H265_NAL_IDR_W_RADL, 0, 1, 0);
    memset(n1 + 2, 0x77, 398);
    memset(n2 + 2, 0x88, 398);

    h265_nal_ref_t nals[2] = {
        {.data = n1, .len = sizeof(n1)},
        {.data = n2, .len = sizeof(n2)},
    };
    pkt_collector_t c = {0};

    ASSERT_OK(h265_packetize_au(nals, 2, 200, collect_cb, &c));
    /* Every fragment must be an FU. */
    for (int i = 0; i < c.count; ++i) {
        ASSERT_EQ(c.frags[i][0] & 0x7E, (H265_PKT_FU << 1) & 0x7E);
    }
    /* Only final fragment gets marker=1. */
    ASSERT_EQ(c.markers[c.count - 1], 1);
}

TEST(test_h265_pack_au_single_nal_is_single)
{
    /* A lone small NAL is emitted as Single NAL Unit (not wrapped in AP). */
    uint8_t nalu[50];
    build_nal_header(nalu, H265_NAL_TRAIL_R, 0, 1, 0);
    memset(nalu + 2, 0x99, 48);

    h265_nal_ref_t nals[1] = {{.data = nalu, .len = sizeof(nalu)}};
    pkt_collector_t c = {0};

    ASSERT_OK(h265_packetize_au(nals, 1, 1200, collect_cb, &c));
    ASSERT_EQ(c.count, 1);
    ASSERT_EQ(c.frag_lens[0], sizeof(nalu));
    ASSERT_MEM_EQ(c.frags[0], nalu, sizeof(nalu));
    ASSERT_EQ(c.markers[0], 1);
}

TEST(test_h265_pack_au_ap_then_single)
{
    /* Three NALs where the greedy AP packer stops after aggregating the
     * first two because the third would overflow the MTU:
     *   n1+n2 AP  = 2 (hdr) + 2+10 + 2+10 = 26 bytes
     *   adding n3 = 26 + (2 + 900) = 928 bytes
     * Choose MTU=910 so 928 > 910; the packer stops the AP run at j=1,
     * then emits n3 as a Single NAL Unit Packet (900 ≤ 910). */
    uint8_t n1[10], n2[10], n3[900];
    build_nal_header(n1, H265_NAL_VPS_NUT, 0, 1, 0);
    build_nal_header(n2, H265_NAL_SPS_NUT, 0, 1, 0);
    build_nal_header(n3, H265_NAL_PPS_NUT, 0, 1, 0);
    memset(n1 + 2, 0xA, 8);
    memset(n2 + 2, 0xB, 8);
    memset(n3 + 2, 0xC, 898);

    h265_nal_ref_t nals[3] = {
        {.data = n1, .len = sizeof(n1)},
        {.data = n2, .len = sizeof(n2)},
        {.data = n3, .len = sizeof(n3)},
    };
    pkt_collector_t c = {0};

    ASSERT_OK(h265_packetize_au(nals, 3, 910, collect_cb, &c));
    ASSERT_EQ(c.count, 2);
    ASSERT_EQ(c.frags[0][0], 0x60); /* AP PayloadHdr Type=48 */
    ASSERT_EQ(c.frag_lens[0], 26u);
    ASSERT_EQ(c.markers[0], 0);
    ASSERT_EQ(c.frag_lens[1], 900u); /* Single NAL */
    ASSERT_EQ(c.markers[1], 1);
}

TEST(test_h265_pack_au_null_params)
{
    h265_nal_ref_t nals[1] = {{.data = NULL, .len = 0}};
    pkt_collector_t c = {0};

    ASSERT_FAIL(h265_packetize_au(NULL, 1, 1200, collect_cb, &c));
    ASSERT_FAIL(h265_packetize_au(nals, 0, 1200, collect_cb, &c));
    ASSERT_FAIL(h265_packetize_au(nals, 1, 1200, collect_cb, &c)); /* data=NULL */
}

/* ================================================================
 * Depacketizer — Single NAL Unit (§4.4.1)
 * ================================================================ */

TEST(test_h265_depkt_single_nal)
{
    nano_h265_depkt_t d;
    h265_depkt_init(&d);

    uint8_t payload[] = {0x26, 0x01, 0xDE, 0xAD, 0xBE, 0xEF}; /* type=19 IDR */
    const uint8_t *out = NULL;
    size_t out_len = 0;
    ASSERT_OK(h265_depkt_push(&d, payload, sizeof(payload), 1, &out, &out_len));
    ASSERT_TRUE(out != NULL);
    ASSERT_EQ(out_len, sizeof(payload));
    ASSERT_MEM_EQ(out, payload, sizeof(payload));
}

TEST(test_h265_depkt_null_params)
{
    nano_h265_depkt_t d;
    h265_depkt_init(&d);

    uint8_t payload[] = {0x26, 0x01};
    const uint8_t *out = NULL;
    size_t out_len = 0;
    ASSERT_FAIL(h265_depkt_push(NULL, payload, sizeof(payload), 1, &out, &out_len));
    ASSERT_FAIL(h265_depkt_push(&d, NULL, sizeof(payload), 1, &out, &out_len));
    ASSERT_FAIL(h265_depkt_push(&d, payload, 1, 1, &out, &out_len)); /* Below header size */
    ASSERT_FAIL(h265_depkt_push(&d, payload, sizeof(payload), 1, NULL, &out_len));
    ASSERT_FAIL(h265_depkt_push(&d, payload, sizeof(payload), 1, &out, NULL));
}

TEST(test_h265_depkt_init_null)
{
    ASSERT_FAIL(h265_depkt_init(NULL));
}

/* ================================================================
 * Depacketizer — FU reassembly (§4.4.3) + roundtrip
 * ================================================================ */

TEST(test_h265_depkt_fu_roundtrip)
{
    /* Pack a large NAL, then depacketize the fragments — bytes must match. */
    uint8_t original[600];
    build_nal_header(original, H265_NAL_IDR_W_RADL, 12, 4, 0);
    for (size_t i = 2; i < sizeof(original); ++i) {
        original[i] = (uint8_t)(i ^ 0x5A);
    }

    pkt_collector_t c = {0};
    ASSERT_OK(h265_packetize(original, sizeof(original), 200, collect_cb, &c));
    ASSERT_TRUE(c.count >= 2);

    nano_h265_depkt_t d;
    h265_depkt_init(&d);

    const uint8_t *out = NULL;
    size_t out_len = 0;
    for (int i = 0; i < c.count; ++i) {
        ASSERT_OK(h265_depkt_push(&d, c.frags[i], c.frag_lens[i], c.markers[i], &out, &out_len));
        if (i < c.count - 1) {
            ASSERT_TRUE(out == NULL); /* Not complete yet */
        }
    }
    ASSERT_TRUE(out != NULL);
    ASSERT_EQ(out_len, sizeof(original));
    ASSERT_MEM_EQ(out, original, sizeof(original));
}

TEST(test_h265_depkt_fu_hand_crafted)
{
    /* Hand-crafted 3-fragment FU sequence:
     * Original NAL: [0x26 0x01] (type=19 IDR_W_RADL, layer=0, TID=1)
     *               followed by 6 bytes of RBSP: 0xAA 0xBB 0xCC 0xDD 0xEE 0xFF.
     *
     * PayloadHdr for FU (type=49, layer=0, TID=1):
     *   byte0 = 0x00 (F=0) | 0x62 (49<<1) | 0x00 (LayerId_msb) = 0x62
     *   byte1 = 0x01 (LayerId_lsb=0, TID=1)
     *
     * FU header bytes:
     *   start  (S=1, E=0, FuType=19)  → 0x80 | 0x13 = 0x93
     *   middle (S=0, E=0, FuType=19)  → 0x13
     *   end    (S=0, E=1, FuType=19)  → 0x40 | 0x13 = 0x53
     */
    uint8_t f1[] = {0x62, 0x01, 0x93, 0xAA, 0xBB};
    uint8_t f2[] = {0x62, 0x01, 0x13, 0xCC, 0xDD};
    uint8_t f3[] = {0x62, 0x01, 0x53, 0xEE, 0xFF};
    uint8_t expected_nal[] = {0x26, 0x01, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};

    nano_h265_depkt_t d;
    h265_depkt_init(&d);

    const uint8_t *out = NULL;
    size_t out_len = 0;

    ASSERT_OK(h265_depkt_push(&d, f1, sizeof(f1), 0, &out, &out_len));
    ASSERT_TRUE(out == NULL);
    ASSERT_OK(h265_depkt_push(&d, f2, sizeof(f2), 0, &out, &out_len));
    ASSERT_TRUE(out == NULL);
    ASSERT_OK(h265_depkt_push(&d, f3, sizeof(f3), 1, &out, &out_len));
    ASSERT_TRUE(out != NULL);
    ASSERT_EQ(out_len, sizeof(expected_nal));
    ASSERT_MEM_EQ(out, expected_nal, sizeof(expected_nal));
}

TEST(test_h265_depkt_fu_continuation_without_start)
{
    nano_h265_depkt_t d;
    h265_depkt_init(&d);

    /* Middle fragment without a preceding Start — silently dropped. */
    uint8_t middle[] = {0x62, 0x01, 0x13, 0xAA, 0xBB};
    const uint8_t *out = NULL;
    size_t out_len = 0;
    ASSERT_OK(h265_depkt_push(&d, middle, sizeof(middle), 0, &out, &out_len));
    ASSERT_TRUE(out == NULL);
}

TEST(test_h265_depkt_fu_abort_by_single)
{
    nano_h265_depkt_t d;
    h265_depkt_init(&d);

    /* FU Start in progress, then a Single NAL arrives → discard FU, return Single. */
    uint8_t fu_start[] = {0x62, 0x01, 0x93, 0x11, 0x22, 0x33};
    const uint8_t *out = NULL;
    size_t out_len = 0;
    ASSERT_OK(h265_depkt_push(&d, fu_start, sizeof(fu_start), 0, &out, &out_len));
    ASSERT_TRUE(out == NULL);

    uint8_t single[] = {0x02, 0x01, 0xAA, 0xBB}; /* type=1 TRAIL_R */
    ASSERT_OK(h265_depkt_push(&d, single, sizeof(single), 1, &out, &out_len));
    ASSERT_TRUE(out != NULL);
    ASSERT_EQ(out_len, sizeof(single));
    ASSERT_MEM_EQ(out, single, sizeof(single));
}

TEST(test_h265_depkt_fu_abort_by_ap)
{
    nano_h265_depkt_t d;
    h265_depkt_init(&d);

    uint8_t fu_start[] = {0x62, 0x01, 0x93, 0x77};
    const uint8_t *out = NULL;
    size_t out_len = 0;
    ASSERT_OK(h265_depkt_push(&d, fu_start, sizeof(fu_start), 0, &out, &out_len));
    ASSERT_TRUE(out == NULL);

    /* AP with one NAL of 4 bytes. */
    uint8_t ap[] = {0x60, 0x01, 0x00, 0x04, 0x42, 0x01, 0xAA, 0xBB};
    ASSERT_OK(h265_depkt_push(&d, ap, sizeof(ap), 1, &out, &out_len));
    ASSERT_TRUE(out != NULL);
    ASSERT_EQ(out_len, 4u);
    /* The returned NAL is the inner one from the AP. */
    ASSERT_EQ(out[0], 0x42);
}

TEST(test_h265_depkt_fu_short_packet)
{
    nano_h265_depkt_t d;
    h265_depkt_init(&d);

    /* Len = 2 (PayloadHdr only, no FU header) → parse error. */
    uint8_t tiny[] = {0x62, 0x01};
    const uint8_t *out = NULL;
    size_t out_len = 0;
    int rc = h265_depkt_push(&d, tiny, sizeof(tiny), 0, &out, &out_len);
    ASSERT_EQ(rc, NANORTC_ERR_PARSE);
}

/* ================================================================
 * Depacketizer — Aggregation Packet (§4.4.2)
 * ================================================================ */

TEST(test_h265_depkt_ap_first_nal)
{
    /* Hand-crafted AP with 2 sub-NALs of 4 bytes each.
     * PayloadHdr: type=48 (0x60), layer=0, TID=1 → 0x60 0x01
     * First sub-NAL: length=4, bytes {0x40 0x01 0xAA 0xBB} (VPS, type=32)
     * Second sub-NAL: length=4, bytes {0x42 0x01 0xCC 0xDD} (SPS, type=33)
     */
    uint8_t ap[] = {0x60, 0x01, 0x00, 0x04, 0x40, 0x01, 0xAA,
                    0xBB, 0x00, 0x04, 0x42, 0x01, 0xCC, 0xDD};

    nano_h265_depkt_t d;
    h265_depkt_init(&d);

    const uint8_t *out = NULL;
    size_t out_len = 0;
    ASSERT_OK(h265_depkt_push(&d, ap, sizeof(ap), 1, &out, &out_len));
    ASSERT_TRUE(out != NULL);
    /* First NAL returned */
    ASSERT_EQ(out_len, 4u);
    ASSERT_EQ(out[0], 0x40);
    ASSERT_EQ(out[1], 0x01);
}

TEST(test_h265_depkt_ap_malformed_length)
{
    /* Declared sub-NAL size exceeds remaining packet. */
    uint8_t ap[] = {0x60, 0x01, 0x00, 0xFF, 0x40, 0x01, 0xAA};
    nano_h265_depkt_t d;
    h265_depkt_init(&d);

    const uint8_t *out = NULL;
    size_t out_len = 0;
    int rc = h265_depkt_push(&d, ap, sizeof(ap), 1, &out, &out_len);
    ASSERT_EQ(rc, NANORTC_ERR_PARSE);
}

TEST(test_h265_depkt_ap_zero_length_nal)
{
    /* Zero-length inner NAL → break inner loop, no output returned. */
    uint8_t ap[] = {0x60, 0x01, 0x00, 0x00};
    nano_h265_depkt_t d;
    h265_depkt_init(&d);

    const uint8_t *out = NULL;
    size_t out_len = 0;
    ASSERT_OK(h265_depkt_push(&d, ap, sizeof(ap), 1, &out, &out_len));
    ASSERT_TRUE(out == NULL);
}

TEST(test_h265_depkt_ap_roundtrip)
{
    /* pack → unpack for an AP with 3 small NALs, verify first NAL. */
    uint8_t vps[8], sps[10], pps[12];
    build_nal_header(vps, H265_NAL_VPS_NUT, 0, 1, 0);
    build_nal_header(sps, H265_NAL_SPS_NUT, 0, 1, 0);
    build_nal_header(pps, H265_NAL_PPS_NUT, 0, 1, 0);
    memset(vps + 2, 0x11, 6);
    memset(sps + 2, 0x22, 8);
    memset(pps + 2, 0x33, 10);

    h265_nal_ref_t nals[3] = {
        {.data = vps, .len = sizeof(vps)},
        {.data = sps, .len = sizeof(sps)},
        {.data = pps, .len = sizeof(pps)},
    };
    pkt_collector_t c = {0};
    ASSERT_OK(h265_packetize_au(nals, 3, 1200, collect_cb, &c));
    ASSERT_EQ(c.count, 1);

    nano_h265_depkt_t d;
    h265_depkt_init(&d);

    const uint8_t *out = NULL;
    size_t out_len = 0;
    ASSERT_OK(h265_depkt_push(&d, c.frags[0], c.frag_lens[0], 1, &out, &out_len));
    ASSERT_TRUE(out != NULL);
    ASSERT_EQ(out_len, sizeof(vps));
    ASSERT_MEM_EQ(out, vps, sizeof(vps));
}

/* ================================================================
 * Depacketizer — PACI & unknown types (§4.4.4 / reserved)
 * ================================================================ */

TEST(test_h265_depkt_paci_ignored)
{
    nano_h265_depkt_t d;
    h265_depkt_init(&d);

    /* PACI packet (type=50): byte0 = 50 << 1 = 0x64 */
    uint8_t paci[] = {0x64, 0x01, 0xDE, 0xAD};
    const uint8_t *out = NULL;
    size_t out_len = 0;
    ASSERT_OK(h265_depkt_push(&d, paci, sizeof(paci), 1, &out, &out_len));
    ASSERT_TRUE(out == NULL);
}

TEST(test_h265_depkt_reserved_type)
{
    nano_h265_depkt_t d;
    h265_depkt_init(&d);

    /* Type 51 (reserved): byte0 = 51 << 1 = 0x66 */
    uint8_t rsv[] = {0x66, 0x01, 0xDE, 0xAD};
    const uint8_t *out = NULL;
    size_t out_len = 0;
    ASSERT_OK(h265_depkt_push(&d, rsv, sizeof(rsv), 1, &out, &out_len));
    ASSERT_TRUE(out == NULL);
}

/* ================================================================
 * Keyframe detection (stateless)
 * ================================================================ */

TEST(test_h265_keyframe_empty_null)
{
    ASSERT_EQ(h265_is_keyframe(NULL, 0), 0);
    ASSERT_EQ(h265_is_keyframe(NULL, 100), 0);
    uint8_t buf[1] = {0x26};
    ASSERT_EQ(h265_is_keyframe(buf, 1), 0); /* Shorter than NAL header */
}

TEST(test_h265_keyframe_idr_w_radl)
{
    /* type=19 → byte0 = 19 << 1 = 0x26 */
    uint8_t p[] = {0x26, 0x01, 0xAA};
    ASSERT_EQ(h265_is_keyframe(p, sizeof(p)), 1);
}

TEST(test_h265_keyframe_idr_n_lp)
{
    /* type=20 → byte0 = 20 << 1 = 0x28 */
    uint8_t p[] = {0x28, 0x01, 0xBB};
    ASSERT_EQ(h265_is_keyframe(p, sizeof(p)), 1);
}

TEST(test_h265_keyframe_cra_nut)
{
    /* type=21 → byte0 = 21 << 1 = 0x2A */
    uint8_t p[] = {0x2A, 0x01, 0xCC};
    ASSERT_EQ(h265_is_keyframe(p, sizeof(p)), 1);
}

TEST(test_h265_keyframe_bla_w_lp)
{
    /* type=16 → byte0 = 16 << 1 = 0x20 */
    uint8_t p[] = {0x20, 0x01, 0xDD};
    ASSERT_EQ(h265_is_keyframe(p, sizeof(p)), 1);
}

TEST(test_h265_keyframe_rsv_irap_vcl23)
{
    /* type=23 → byte0 = 23 << 1 = 0x2E */
    uint8_t p[] = {0x2E, 0x01, 0xEE};
    ASSERT_EQ(h265_is_keyframe(p, sizeof(p)), 1);
}

TEST(test_h265_keyframe_non_irap_trail_r)
{
    /* type=1 (TRAIL_R) → byte0 = 0x02 */
    uint8_t p[] = {0x02, 0x01, 0xAA};
    ASSERT_EQ(h265_is_keyframe(p, sizeof(p)), 0);
}

TEST(test_h265_keyframe_non_irap_radl_r)
{
    /* type=7 (RADL_R) → byte0 = 7 << 1 = 0x0E */
    uint8_t p[] = {0x0E, 0x01, 0xFF};
    ASSERT_EQ(h265_is_keyframe(p, sizeof(p)), 0);
}

TEST(test_h265_keyframe_fu_start_idr)
{
    /* FU payload:
     *   byte0 = (49<<1) = 0x62
     *   byte1 = 0x01
     *   FU header = 0x80 | 0x13 (S=1, FuType=19) = 0x93
     */
    uint8_t p[] = {0x62, 0x01, 0x93, 0xAA};
    ASSERT_EQ(h265_is_keyframe(p, sizeof(p)), 1);
}

TEST(test_h265_keyframe_fu_start_non_idr)
{
    /* FU start with FuType=1 (TRAIL_R) → not a keyframe. */
    uint8_t p[] = {0x62, 0x01, 0x81, 0xAA};
    ASSERT_EQ(h265_is_keyframe(p, sizeof(p)), 0);
}

TEST(test_h265_keyframe_fu_middle_idr)
{
    /* FU continuation fragment (S=0) → we cannot reliably know the original
     * type from a non-start fragment → report 0. */
    uint8_t p[] = {0x62, 0x01, 0x13, 0xAA}; /* S=0, E=0, FuType=19 */
    ASSERT_EQ(h265_is_keyframe(p, sizeof(p)), 0);
}

TEST(test_h265_keyframe_fu_too_short)
{
    /* FU packet without even an FU header → false. */
    uint8_t p[] = {0x62, 0x01};
    ASSERT_EQ(h265_is_keyframe(p, sizeof(p)), 0);
}

TEST(test_h265_keyframe_ap_with_idr_inner)
{
    /* AP containing VPS (type=32, not IRAP), SPS (33), PPS (34), IDR (19).
     * Even though IDR is last, is_keyframe must return 1. */
    uint8_t ap[] = {
        0x60, 0x01,             /* AP PayloadHdr, type=48 */
        0x00, 0x04,             /* VPS length = 4 */
        0x40, 0x01, 0x00, 0x01, /* VPS NAL */
        0x00, 0x04,             /* SPS length = 4 */
        0x42, 0x01, 0x00, 0x02, /* SPS NAL */
        0x00, 0x04,             /* PPS length = 4 */
        0x44, 0x01, 0x00, 0x03, /* PPS NAL */
        0x00, 0x04,             /* IDR length = 4 */
        0x26, 0x01, 0xAA, 0xBB, /* IDR NAL (type=19) */
    };
    ASSERT_EQ(h265_is_keyframe(ap, sizeof(ap)), 1);
}

TEST(test_h265_keyframe_ap_without_idr)
{
    /* AP of only parameter sets (no VCL): should NOT be a keyframe.
     * The stream is only "usable for random access" when an IRAP NAL
     * follows the parameter sets. */
    uint8_t ap[] = {
        0x60, 0x01, 0x00, 0x04, 0x40, 0x01, 0x00, 0x01, 0x00, 0x04,
        0x42, 0x01, 0x00, 0x02, 0x00, 0x04, 0x44, 0x01, 0x00, 0x03,
    };
    ASSERT_EQ(h265_is_keyframe(ap, sizeof(ap)), 0);
}

TEST(test_h265_keyframe_paci_not_keyframe)
{
    uint8_t paci[] = {0x64, 0x01, 0xAA};
    ASSERT_EQ(h265_is_keyframe(paci, sizeof(paci)), 0);
}

/* ================================================================
 * Annex-B scanner regression (two-byte NAL header handling)
 * ================================================================ */

TEST(test_h265_annex_b_two_byte_header)
{
    /* Annex-B stream with start code + HEVC VPS NAL (type=32, layer=0, TID=1).
     * The scanner is codec-agnostic — it must return 2 bytes for the header
     * plus whatever payload follows. */
    uint8_t data[] = {0x00, 0x00, 0x00, 0x01, 0x40, 0x01, 0x0C, 0x01, 0xFF};
    size_t offset = 0, nal_len = 0;
    const uint8_t *nal = nano_annex_b_find_nal(data, sizeof(data), &offset, &nal_len);
    ASSERT_TRUE(nal != NULL);
    ASSERT_EQ(nal[0], 0x40); /* HEVC NAL byte 0 for VPS */
    ASSERT_EQ(nal[1], 0x01);
    ASSERT_EQ(nal_len, 5u);
}

/* ================================================================
 * Test runner
 * ================================================================ */

TEST_MAIN_BEGIN("test_h265")
/* Packetizer — Single NAL (§4.4.1) */
RUN(test_h265_pack_single_nal);
RUN(test_h265_pack_single_nal_header_type_extract);
RUN(test_h265_pack_exact_mtu);
/* Packetizer — FU (§4.4.3) */
RUN(test_h265_pack_fu_basic);
RUN(test_h265_pack_fu_layerid_tid_preserved);
RUN(test_h265_pack_fu_layerid_msb_nonzero);
RUN(test_h265_pack_fu_f_bit_preserved);
RUN(test_h265_pack_fu_single_byte_tail);
RUN(test_h265_pack_fu_mtu_too_small);
RUN(test_h265_pack_null_params);
RUN(test_h265_pack_minimum_nal);
/* Packetizer — AP (§4.4.2) */
RUN(test_h265_pack_ap_two_small);
RUN(test_h265_pack_ap_three_nals_vps_sps_pps);
RUN(test_h265_pack_ap_layerid_min);
RUN(test_h265_pack_ap_f_bit_union);
/* Packetizer — access unit mixed cases */
RUN(test_h265_pack_au_mixed_ap_plus_fu);
RUN(test_h265_pack_au_all_large);
RUN(test_h265_pack_au_single_nal_is_single);
RUN(test_h265_pack_au_ap_then_single);
RUN(test_h265_pack_au_null_params);
/* Depacketizer — Single NAL */
RUN(test_h265_depkt_single_nal);
RUN(test_h265_depkt_null_params);
RUN(test_h265_depkt_init_null);
/* Depacketizer — FU */
RUN(test_h265_depkt_fu_roundtrip);
RUN(test_h265_depkt_fu_hand_crafted);
RUN(test_h265_depkt_fu_continuation_without_start);
RUN(test_h265_depkt_fu_abort_by_single);
RUN(test_h265_depkt_fu_abort_by_ap);
RUN(test_h265_depkt_fu_short_packet);
/* Depacketizer — AP */
RUN(test_h265_depkt_ap_first_nal);
RUN(test_h265_depkt_ap_malformed_length);
RUN(test_h265_depkt_ap_zero_length_nal);
RUN(test_h265_depkt_ap_roundtrip);
/* Depacketizer — PACI / reserved */
RUN(test_h265_depkt_paci_ignored);
RUN(test_h265_depkt_reserved_type);
/* Keyframe detection */
RUN(test_h265_keyframe_empty_null);
RUN(test_h265_keyframe_idr_w_radl);
RUN(test_h265_keyframe_idr_n_lp);
RUN(test_h265_keyframe_cra_nut);
RUN(test_h265_keyframe_bla_w_lp);
RUN(test_h265_keyframe_rsv_irap_vcl23);
RUN(test_h265_keyframe_non_irap_trail_r);
RUN(test_h265_keyframe_non_irap_radl_r);
RUN(test_h265_keyframe_fu_start_idr);
RUN(test_h265_keyframe_fu_start_non_idr);
RUN(test_h265_keyframe_fu_middle_idr);
RUN(test_h265_keyframe_fu_too_short);
RUN(test_h265_keyframe_ap_with_idr_inner);
RUN(test_h265_keyframe_ap_without_idr);
RUN(test_h265_keyframe_paci_not_keyframe);
/* Annex-B scanner (H.265 regression) */
RUN(test_h265_annex_b_two_byte_header);
TEST_MAIN_END
