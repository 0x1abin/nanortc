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

#endif /* NANORTC_FEATURE_VIDEO_FEC */

TEST_MAIN_BEGIN("nanortc ULPFEC codec tests")
#if NANORTC_FEATURE_VIDEO_FEC
RUN(test_fec_structural_fields);
RUN(test_fec_roundtrip_recovers_each);
RUN(test_fec_roundtrip_variable_lengths);
RUN(test_fec_wraparound_seqs);
RUN(test_fec_unrecoverable_cases);
#endif
TEST_MAIN_END
