/*
 * nanortc — ULPFEC codec tests (NANORTC_FEATURE_VIDEO_FEC)
 *
 * Validates nano_fec.c to a high bar without a browser:
 *   - STRUCTURAL: the encoded FEC packet's fields equal the hand-computed
 *     RFC-5109 values (SN base, mask, TS / length / PT recovery), not merely
 *     self-consistent roundtrip;
 *   - ROUNDTRIP: encode a group, drop EACH member in turn, recover byte-exact;
 *   - variable payload lengths (zero-padding), wraparound SNs, and the
 *     unrecoverable cases (0 lost, 2 lost).
 *
 * SPDX-License-Identifier: MIT
 */

#include "nano_fec.h"
#include "nanortc.h"
#include "nano_test.h"

#include <stdint.h>
#include <string.h>

#if NANORTC_FEATURE_VIDEO_FEC

#define SSRC 0x12345678u

static uint16_t build_rtp(uint8_t *buf, uint16_t seq, uint32_t ts, uint8_t pt, uint8_t marker,
                          uint8_t fill, uint16_t plen)
{
    buf[0] = 0x80; /* V=2, P=0, X=0, CC=0 */
    buf[1] = (uint8_t)((marker ? 0x80u : 0u) | (pt & 0x7Fu));
    nanortc_write_u16be(buf + 2, seq);
    nanortc_write_u32be(buf + 4, ts);
    nanortc_write_u32be(buf + 8, SSRC);
    for (uint16_t i = 0; i < plen; i++) {
        buf[12 + i] = (uint8_t)(fill + i);
    }
    return (uint16_t)(12 + plen);
}

TEST(test_fec_structural_fields)
{
    uint8_t p0[64], p1[64];
    uint16_t l0 = build_rtp(p0, 1000, 0xAABBCCDDu, 96, 1, 0x10, 20);
    uint16_t l1 = build_rtp(p1, 1001, 0x11223344u, 96, 0, 0x40, 20);
    const uint8_t *pkts[2] = {p0, p1};
    uint16_t lens[2] = {l0, l1};

    uint8_t fec[128];
    size_t fl = 0;
    ASSERT_OK(fec_encode(pkts, lens, 2, fec, sizeof(fec), &fl));

    /* SN base = min seq; mask = two MSBs (offsets 0,1). */
    ASSERT_EQ(nanortc_read_u16be(fec + 2), (uint16_t)1000);
    ASSERT_EQ(nanortc_read_u16be(fec + 12), (uint16_t)0xC000);
    /* TS recovery = ts0 ^ ts1. */
    ASSERT_EQ(nanortc_read_u32be(fec + 4), 0xAABBCCDDu ^ 0x11223344u);
    /* length recovery = len16_0 ^ len16_1 (both 20 → 0). */
    ASSERT_EQ(nanortc_read_u16be(fec + 8), (uint16_t)(20u ^ 20u));
    /* M/PT recovery = byte1_0 ^ byte1_1 = (0x80|96) ^ 96. */
    ASSERT_EQ(fec[1], (uint8_t)((0x80u | 96u) ^ 96u));
    /* P/X/CC recovery: both byte0 are 0x80 → &0x3F = 0 → XOR = 0. */
    ASSERT_EQ(fec[0], (uint8_t)0);
    /* protection length = payload region = 20. */
    ASSERT_EQ(nanortc_read_u16be(fec + 10), (uint16_t)20);
    ASSERT_EQ(fl, (size_t)(FEC_OVERHEAD + 20));
}

/* Encode `n` equal-length packets, then drop each in turn and recover byte-exact. */
static void roundtrip_n(uint8_t n, uint16_t plen)
{
    uint8_t pkts_store[FEC_MAX_GROUP][1300];
    const uint8_t *pkts[FEC_MAX_GROUP];
    uint16_t lens[FEC_MAX_GROUP];
    for (uint8_t i = 0; i < n; i++) {
        lens[i] = build_rtp(pkts_store[i], (uint16_t)(5000 + i), 0x1000u + i * 90u, 96,
                            (uint8_t)(i == n - 1), (uint8_t)(0x20 + i * 7), plen);
        pkts[i] = pkts_store[i];
    }

    uint8_t fec[1400];
    size_t fl = 0;
    ASSERT_OK(fec_encode(pkts, lens, n, fec, sizeof(fec), &fl));

    for (uint8_t drop = 0; drop < n; drop++) {
        const uint8_t *recv[FEC_MAX_GROUP];
        uint16_t rlens[FEC_MAX_GROUP];
        uint8_t nr = 0;
        for (uint8_t i = 0; i < n; i++) {
            if (i == drop) {
                continue;
            }
            recv[nr] = pkts[i];
            rlens[nr] = lens[i];
            nr++;
        }
        uint8_t out[1300];
        size_t ol = 0;
        uint16_t oseq = 0;
        ASSERT_EQ(fec_recover(fec, fl, SSRC, recv, rlens, nr, out, sizeof(out), &ol, &oseq),
                  NANORTC_OK);
        ASSERT_EQ(oseq, (uint16_t)(5000 + drop));
        ASSERT_EQ(ol, (size_t)lens[drop]);
        ASSERT_MEM_EQ(out, pkts[drop], lens[drop]); /* byte-exact recovery */
    }
}

TEST(test_fec_roundtrip_recovers_each)
{
    roundtrip_n(2, 100);
    roundtrip_n(4, 200);
    roundtrip_n(8, 400);
    roundtrip_n(1, 50); /* group of 1: the FEC is a copy → recovers it */
}

TEST(test_fec_roundtrip_variable_lengths)
{
    uint8_t p[4][600];
    const uint8_t *pkts[4];
    uint16_t lens[4];
    uint16_t plens[4] = {500, 40, 300, 120}; /* mixed → zero-padding in the XOR */
    for (uint8_t i = 0; i < 4; i++) {
        lens[i] = build_rtp(p[i], (uint16_t)(7000 + i), 0x2000u + i, 96, 0, (uint8_t)(0x30 + i),
                            plens[i]);
        pkts[i] = p[i];
    }
    uint8_t fec[700];
    size_t fl = 0;
    ASSERT_OK(fec_encode(pkts, lens, 4, fec, sizeof(fec), &fl));

    for (uint8_t drop = 0; drop < 4; drop++) {
        const uint8_t *recv[4];
        uint16_t rl[4];
        uint8_t nr = 0;
        for (uint8_t i = 0; i < 4; i++) {
            if (i != drop) {
                recv[nr] = pkts[i];
                rl[nr] = lens[i];
                nr++;
            }
        }
        uint8_t out[600];
        size_t ol = 0;
        uint16_t oseq = 0;
        ASSERT_EQ(fec_recover(fec, fl, SSRC, recv, rl, nr, out, sizeof(out), &ol, &oseq),
                  NANORTC_OK);
        ASSERT_EQ(ol, (size_t)lens[drop]); /* correct recovered length */
        ASSERT_MEM_EQ(out, pkts[drop], lens[drop]);
    }
}

TEST(test_fec_wraparound_seqs)
{
    uint8_t p[3][64];
    const uint8_t *pkts[3];
    uint16_t lens[3];
    uint16_t seqs[3] = {0xFFFE, 0xFFFF, 0x0000};
    for (uint8_t i = 0; i < 3; i++) {
        lens[i] = build_rtp(p[i], seqs[i], 0x3000u + i, 96, 0, (uint8_t)(0x50 + i), 30);
        pkts[i] = p[i];
    }
    uint8_t fec[128];
    size_t fl = 0;
    ASSERT_OK(fec_encode(pkts, lens, 3, fec, sizeof(fec), &fl));
    ASSERT_EQ(nanortc_read_u16be(fec + 2), (uint16_t)0xFFFE); /* base = earliest across wrap */

    /* drop the wrapped one (0x0000) and recover. */
    const uint8_t *recv[2] = {p[0], p[1]};
    uint16_t rl[2] = {lens[0], lens[1]};
    uint8_t out[64];
    size_t ol = 0;
    uint16_t oseq = 0;
    ASSERT_EQ(fec_recover(fec, fl, SSRC, recv, rl, 2, out, sizeof(out), &ol, &oseq), NANORTC_OK);
    ASSERT_EQ(oseq, (uint16_t)0x0000);
    ASSERT_MEM_EQ(out, p[2], lens[2]);
}

TEST(test_fec_unrecoverable_cases)
{
    uint8_t p[3][64];
    const uint8_t *pkts[3];
    uint16_t lens[3];
    for (uint8_t i = 0; i < 3; i++) {
        lens[i] = build_rtp(p[i], (uint16_t)(800 + i), i, 96, 0, (uint8_t)i, 20);
        pkts[i] = p[i];
    }
    uint8_t fec[128];
    size_t fl = 0;
    ASSERT_OK(fec_encode(pkts, lens, 3, fec, sizeof(fec), &fl));

    uint8_t out[64];
    size_t ol = 0;
    uint16_t oseq = 0;
    /* 0 missing → nothing to recover. */
    const uint8_t *all[3] = {p[0], p[1], p[2]};
    uint16_t al[3] = {lens[0], lens[1], lens[2]};
    ASSERT_EQ(fec_recover(fec, fl, SSRC, all, al, 3, out, sizeof(out), &ol, &oseq),
              NANORTC_ERR_NO_DATA);
    /* 2 missing → level-0 cannot recover. */
    const uint8_t *one[1] = {p[1]};
    uint16_t one_l[1] = {lens[1]};
    ASSERT_EQ(fec_recover(fec, fl, SSRC, one, one_l, 1, out, sizeof(out), &ol, &oseq),
              NANORTC_ERR_NO_DATA);
}

TEST(test_fec_encode_validates_members_before_reading)
{
    uint8_t valid[16] = {0x80, 96};
    uint8_t short_packet[1] = {0x80};
    const uint8_t *pkts[2] = {valid, NULL};
    uint16_t lens[2] = {sizeof(valid), FEC_RTP_HDR};
    uint8_t out[64];
    size_t len = 99;
    ASSERT_EQ(fec_encode(pkts, lens, 2, out, sizeof(out), &len), NANORTC_ERR_INVALID_PARAM);
    pkts[0] = NULL;
    ASSERT_EQ(fec_encode(pkts, lens, 1, out, sizeof(out), &len), NANORTC_ERR_INVALID_PARAM);
    pkts[0] = short_packet;
    lens[0] = sizeof(short_packet);
    ASSERT_EQ(fec_encode(pkts, lens, 1, out, sizeof(out), &len), NANORTC_ERR_INVALID_PARAM);
    ASSERT_EQ(len, 99u);
}

TEST(test_fec_recover_validates_arrays_and_ignores_duplicate_members)
{
    uint8_t p[3][32];
    const uint8_t *pkts[3];
    uint16_t lens[3];
    for (uint8_t i = 0; i < 3; i++) {
        lens[i] = build_rtp(p[i], (uint16_t)(100 + i), 200, 96, 0, i, 12);
        pkts[i] = p[i];
    }
    uint8_t fec[64], out[64];
    size_t fl, ol;
    uint16_t seq;
    ASSERT_OK(fec_encode(pkts, lens, 3, fec, sizeof(fec), &fl));
    ASSERT_EQ(fec_recover(fec, fl, SSRC, NULL, lens, 1, out, sizeof(out), &ol, &seq),
              NANORTC_ERR_INVALID_PARAM);
    ASSERT_EQ(fec_recover(fec, fl, SSRC, pkts, NULL, 1, out, sizeof(out), &ol, &seq),
              NANORTC_ERR_INVALID_PARAM);
    const uint8_t *recv[3] = {p[0], p[0], p[2]};
    uint16_t recv_lens[3] = {lens[0], lens[0], lens[2]};
    ASSERT_OK(fec_recover(fec, fl, SSRC, recv, recv_lens, 3, out, sizeof(out), &ol, &seq));
    ASSERT_EQ(seq, 101);
    ASSERT_EQ(ol, lens[1]);
    ASSERT_MEM_EQ(out, p[1], ol);
}

TEST(test_fec_encode_rejects_mixed_ssrc_before_writing)
{
    uint8_t p[2][16], out[32], unchanged[32];
    const uint8_t *pkts[2] = {p[0], p[1]};
    uint16_t lens[2] = {build_rtp(p[0], 100, 200, 96, 0, 1, 2),
                        build_rtp(p[1], 101, 200, 96, 0, 3, 2)};
    nanortc_write_u32be(p[1] + 8, SSRC + 1u);
    memset(out, 0xA5, sizeof(out));
    memcpy(unchanged, out, sizeof(out));
    size_t len = 99;
    ASSERT_EQ(fec_encode(pkts, lens, 2, out, sizeof(out), &len), NANORTC_ERR_INVALID_PARAM);
    ASSERT_EQ(len, 99);
    ASSERT_MEM_EQ(out, unchanged, sizeof(out));
    nanortc_write_u32be(p[1] + 8, SSRC);
    ASSERT_OK(fec_encode(pkts, lens, 2, out, sizeof(out), &len));
    ASSERT_EQ(nanortc_read_u16be(out + 12), 0xC000);
}

TEST(test_fec_encode_rejects_duplicate_sequence_before_writing)
{
    uint8_t p[2][16], out[32], unchanged[32];
    const uint8_t *pkts[2] = {p[0], p[1]};
    uint16_t lens[2] = {build_rtp(p[0], 100, 200, 96, 0, 1, 2),
                        build_rtp(p[1], 100, 300, 96, 0, 3, 2)};
    memset(out, 0xA5, sizeof(out));
    memcpy(unchanged, out, sizeof(out));
    size_t len = 99;
    ASSERT_EQ(fec_encode(pkts, lens, 2, out, sizeof(out), &len), NANORTC_ERR_INVALID_PARAM);
    ASSERT_EQ(len, 99);
    ASSERT_MEM_EQ(out, unchanged, sizeof(out));
}

TEST(test_fec_recover_ignores_foreign_ssrc_with_matching_sequence)
{
    /* RFC 5109 §7.3/§7.4: two packets at SN 100/101, M/PT XOR=0x80,
     * timestamp XOR=0x10101010, length XOR=0, payload XOR=0x2266. */
    const uint8_t fec[] = {0, 0x80, 0, 100, 0x10, 0x10, 0x10, 0x10,
                           0, 0,    0, 2,   0xC0, 0,    0x22, 0x66};
    const uint8_t first[] = {0x80, 0x60, 0, 100, 1, 2, 3, 4, 0x12, 0x34, 0x56, 0x78, 0x11, 0x22};
    const uint8_t missing[] = {0x80, 0xE0, 0,    101,  0x11, 0x12, 0x13,
                               0x14, 0x12, 0x34, 0x56, 0x78, 0x33, 0x44};
    uint8_t foreign[sizeof(missing)], out[32];
    memcpy(foreign, missing, sizeof(foreign));
    nanortc_write_u32be(foreign + 8, SSRC + 1u);
    foreign[12] = foreign[13] = 0x99;
    const uint8_t *recv[2] = {foreign, first};
    const uint16_t lens[2] = {sizeof(foreign), sizeof(first)};
    size_t len;
    uint16_t sequence;
    /* A foreign SN 101 must not hide the genuinely missing member. */
    ASSERT_OK(
        fec_recover(fec, sizeof(fec), SSRC, recv, lens, 2, out, sizeof(out), &len, &sequence));
    ASSERT_EQ(sequence, 101);
    ASSERT_EQ(len, sizeof(missing));
    ASSERT_MEM_EQ(out, missing, sizeof(missing));
    /* A foreign SN 100 must not be XORed in place of the genuine member. */
    nanortc_write_u16be(foreign + 2, 100);
    ASSERT_OK(
        fec_recover(fec, sizeof(fec), SSRC, recv, lens, 2, out, sizeof(out), &len, &sequence));
    ASSERT_MEM_EQ(out, missing, sizeof(missing));
}

#endif /* NANORTC_FEATURE_VIDEO_FEC */

TEST_MAIN_BEGIN("nanortc ULPFEC codec tests")
#if NANORTC_FEATURE_VIDEO_FEC
RUN(test_fec_structural_fields);
RUN(test_fec_roundtrip_recovers_each);
RUN(test_fec_roundtrip_variable_lengths);
RUN(test_fec_wraparound_seqs);
RUN(test_fec_unrecoverable_cases);
RUN(test_fec_encode_validates_members_before_reading);
RUN(test_fec_recover_validates_arrays_and_ignores_duplicate_members);
RUN(test_fec_encode_rejects_mixed_ssrc_before_writing);
RUN(test_fec_encode_rejects_duplicate_sequence_before_writing);
RUN(test_fec_recover_ignores_foreign_ssrc_with_matching_sequence);
#endif
TEST_MAIN_END
