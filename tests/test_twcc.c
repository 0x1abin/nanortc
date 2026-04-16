/*
 * nanortc — Transport-Wide Congestion Control (TWCC) feedback parser tests
 *
 * Exercises the parser (src/nano_twcc.c) against:
 *   - Hand-built byte vectors for each chunk type (run-length, status
 *     vector 1-bit, status vector 2-bit).
 *   - A RFC-grade reference vector covering mixed chunks + delta sizes.
 *   - Malformed inputs (truncation, bad PT/FMT, count overflow).
 *
 * SPDX-License-Identifier: MIT
 */

#include "nano_twcc.h"
#include "nanortc.h"
#include "nano_test.h"
#include <string.h>

/* ================================================================
 * Helpers: TWCC feedback builder
 *
 * Produces packets with hand-chosen chunks and deltas so tests can
 * assert on exact parse output. Buffer is caller-provided.
 * ================================================================ */

struct twcc_builder {
    uint8_t *buf;
    size_t cap;
    size_t len; /* bytes currently written */
};

static void twcc_build_header(struct twcc_builder *b, uint32_t sender_ssrc, uint32_t media_ssrc,
                              uint16_t base_seq, uint16_t count, int32_t ref_time_tick,
                              uint8_t fb_pkt_count)
{
    /* Caller pre-zeroed the buffer; we write 20 bytes of header. */
    b->buf[0] = (2 << 6) | TWCC_FMT; /* V=2, P=0, FMT=15 */
    b->buf[1] = 205;                 /* PT=RTPFB */
    /* length filled in later via twcc_finalize() */
    nanortc_write_u32be(b->buf + 4, sender_ssrc);
    nanortc_write_u32be(b->buf + 8, media_ssrc);
    nanortc_write_u16be(b->buf + 12, base_seq);
    nanortc_write_u16be(b->buf + 14, count);
    /* 24-bit signed reference time */
    uint32_t ref24 = (uint32_t)ref_time_tick & 0x00FFFFFFu;
    b->buf[16] = (uint8_t)((ref24 >> 16) & 0xFF);
    b->buf[17] = (uint8_t)((ref24 >> 8) & 0xFF);
    b->buf[18] = (uint8_t)(ref24 & 0xFF);
    b->buf[19] = fb_pkt_count;
    b->len = 20;
}

static void twcc_append_u16(struct twcc_builder *b, uint16_t value)
{
    nanortc_write_u16be(b->buf + b->len, value);
    b->len += 2;
}

static void twcc_append_u8(struct twcc_builder *b, uint8_t value)
{
    b->buf[b->len++] = value;
}

/* Build a run-length chunk word. */
static uint16_t twcc_chunk_run(nano_twcc_status_t symbol, uint16_t run)
{
    return (uint16_t)(((uint16_t)symbol & 0x3) << 13) | (uint16_t)(run & 0x1FFF);
}

/* Build a 1-bit status-vector chunk word from up to 14 symbols. */
static uint16_t twcc_chunk_vec1(const uint8_t *bits, size_t n)
{
    uint16_t v = 0x8000; /* T=1, S=0 */
    for (size_t i = 0; i < n && i < 14; i++) {
        if (bits[i]) {
            v |= (uint16_t)(1u << (13 - i));
        }
    }
    return v;
}

/* Build a 2-bit status-vector chunk word from up to 7 symbols. */
static uint16_t twcc_chunk_vec2(const uint8_t *symbols, size_t n)
{
    uint16_t v = 0xC000; /* T=1, S=1 */
    for (size_t i = 0; i < n && i < 7; i++) {
        v |= (uint16_t)((symbols[i] & 0x3) << ((6 - i) * 2));
    }
    return v;
}

/* Finalize: pad to 4-byte boundary and write the RTCP length field. */
static void twcc_finalize(struct twcc_builder *b)
{
    while (b->len % 4 != 0) {
        b->buf[b->len++] = 0;
    }
    uint16_t words = (uint16_t)((b->len / 4) - 1);
    nanortc_write_u16be(b->buf + 2, words);
}

/* ================================================================
 * Callback capture — collects per-packet results for assertion.
 * ================================================================ */

typedef struct {
    uint16_t seq;
    nano_twcc_status_t status;
    int32_t delta_us;
} captured_pkt_t;

typedef struct {
    captured_pkt_t items[32];
    size_t count;
} capture_ctx_t;

static void capture_cb(uint16_t seq, nano_twcc_status_t status, int32_t delta_us, void *user)
{
    capture_ctx_t *ctx = (capture_ctx_t *)user;
    if (ctx->count < sizeof(ctx->items) / sizeof(ctx->items[0])) {
        ctx->items[ctx->count].seq = seq;
        ctx->items[ctx->count].status = status;
        ctx->items[ctx->count].delta_us = delta_us;
        ctx->count++;
    }
}

/* ================================================================
 * Header / boundary tests
 * ================================================================ */

TEST(test_twcc_parse_null_data)
{
    nano_twcc_summary_t sum;
    ASSERT_FAIL(twcc_parse_feedback(NULL, 24, &sum, NULL, NULL));
}

TEST(test_twcc_parse_null_summary)
{
    uint8_t buf[24] = {0};
    ASSERT_FAIL(twcc_parse_feedback(buf, sizeof(buf), NULL, NULL, NULL));
}

TEST(test_twcc_parse_too_short)
{
    uint8_t buf[10] = {0};
    nano_twcc_summary_t sum;
    ASSERT_FAIL(twcc_parse_feedback(buf, sizeof(buf), &sum, NULL, NULL));
}

TEST(test_twcc_parse_bad_version)
{
    uint8_t buf[32] = {0};
    struct twcc_builder b = {buf, sizeof(buf), 0};
    twcc_build_header(&b, 1, 2, 0, 0, 0, 0);
    twcc_finalize(&b);
    buf[0] = (1 << 6) | TWCC_FMT; /* V=1 is invalid */
    nano_twcc_summary_t sum;
    ASSERT_FAIL(twcc_parse_feedback(buf, b.len, &sum, NULL, NULL));
}

TEST(test_twcc_parse_bad_pt)
{
    uint8_t buf[32] = {0};
    struct twcc_builder b = {buf, sizeof(buf), 0};
    twcc_build_header(&b, 1, 2, 0, 0, 0, 0);
    twcc_finalize(&b);
    buf[1] = 200; /* SR, not RTPFB */
    nano_twcc_summary_t sum;
    ASSERT_FAIL(twcc_parse_feedback(buf, b.len, &sum, NULL, NULL));
}

TEST(test_twcc_parse_bad_fmt)
{
    uint8_t buf[32] = {0};
    struct twcc_builder b = {buf, sizeof(buf), 0};
    twcc_build_header(&b, 1, 2, 0, 0, 0, 0);
    twcc_finalize(&b);
    buf[0] = (2 << 6) | 1; /* FMT=1 (NACK), not 15 */
    nano_twcc_summary_t sum;
    ASSERT_FAIL(twcc_parse_feedback(buf, b.len, &sum, NULL, NULL));
}

TEST(test_twcc_parse_length_beyond_buffer)
{
    uint8_t buf[32] = {0};
    struct twcc_builder b = {buf, sizeof(buf), 0};
    twcc_build_header(&b, 1, 2, 0, 0, 0, 0);
    twcc_finalize(&b);
    /* Claim length that exceeds actual buffer. */
    nanortc_write_u16be(buf + 2, 100);
    nano_twcc_summary_t sum;
    ASSERT_FAIL(twcc_parse_feedback(buf, b.len, &sum, NULL, NULL));
}

TEST(test_twcc_parse_empty_feedback)
{
    uint8_t buf[32] = {0};
    struct twcc_builder b = {buf, sizeof(buf), 0};
    twcc_build_header(&b, 0xAABB, 0xCCDD, 100, /*count*/ 0, 1000, 7);
    twcc_finalize(&b);

    nano_twcc_summary_t sum;
    ASSERT_OK(twcc_parse_feedback(buf, b.len, &sum, NULL, NULL));
    ASSERT_EQ(sum.sender_ssrc, 0xAABBu);
    ASSERT_EQ(sum.media_ssrc, 0xCCDDu);
    ASSERT_EQ(sum.base_seq, 100);
    ASSERT_EQ(sum.packet_status_count, 0);
    ASSERT_EQ(sum.fb_pkt_count, 7);
    ASSERT_EQ(sum.received_count, 0);
    ASSERT_EQ(sum.reference_time_us, 1000LL * 64000LL);
}

TEST(test_twcc_parse_negative_reference_time)
{
    /* Reference time -1 (all 0xFF 24-bit): sign-extended to 0xFFFFFFFF → -1. */
    uint8_t buf[32] = {0};
    struct twcc_builder b = {buf, sizeof(buf), 0};
    twcc_build_header(&b, 1, 2, 0, 0, -1, 0);
    twcc_finalize(&b);

    nano_twcc_summary_t sum;
    ASSERT_OK(twcc_parse_feedback(buf, b.len, &sum, NULL, NULL));
    ASSERT_EQ(sum.reference_time_us, -64000LL);
}

/* ================================================================
 * Run-length chunk tests
 * ================================================================ */

TEST(test_twcc_run_length_all_received_small)
{
    /* 10 packets, all received with small 1-byte deltas of value 4 (1 ms). */
    uint8_t buf[64] = {0};
    struct twcc_builder b = {buf, sizeof(buf), 0};
    twcc_build_header(&b, 1, 2, 1000, 10, 0, 0);
    twcc_append_u16(&b, twcc_chunk_run(NANO_TWCC_STATUS_SMALL_DELTA, 10));
    for (int i = 0; i < 10; i++) {
        twcc_append_u8(&b, 4); /* delta = 4 * 250us = 1000us */
    }
    twcc_finalize(&b);

    capture_ctx_t ctx = {0};
    nano_twcc_summary_t sum;
    ASSERT_OK(twcc_parse_feedback(buf, b.len, &sum, capture_cb, &ctx));
    ASSERT_EQ(sum.packet_status_count, 10);
    ASSERT_EQ(sum.received_count, 10);
    ASSERT_EQ(ctx.count, 10u);
    for (size_t i = 0; i < 10; i++) {
        ASSERT_EQ(ctx.items[i].seq, (uint16_t)(1000 + i));
        ASSERT_EQ(ctx.items[i].status, NANO_TWCC_STATUS_SMALL_DELTA);
        ASSERT_EQ(ctx.items[i].delta_us, 1000);
    }
}

TEST(test_twcc_run_length_all_lost)
{
    uint8_t buf[32] = {0};
    struct twcc_builder b = {buf, sizeof(buf), 0};
    twcc_build_header(&b, 1, 2, 0, 5, 0, 0);
    twcc_append_u16(&b, twcc_chunk_run(NANO_TWCC_STATUS_NOT_RECEIVED, 5));
    twcc_finalize(&b);

    nano_twcc_summary_t sum;
    ASSERT_OK(twcc_parse_feedback(buf, b.len, &sum, NULL, NULL));
    ASSERT_EQ(sum.packet_status_count, 5);
    ASSERT_EQ(sum.received_count, 0);
}

TEST(test_twcc_run_length_zero_rejected)
{
    uint8_t buf[32] = {0};
    struct twcc_builder b = {buf, sizeof(buf), 0};
    twcc_build_header(&b, 1, 2, 0, 1, 0, 0);
    twcc_append_u16(&b, twcc_chunk_run(NANO_TWCC_STATUS_NOT_RECEIVED, 0));
    twcc_finalize(&b);

    nano_twcc_summary_t sum;
    ASSERT_FAIL(twcc_parse_feedback(buf, b.len, &sum, NULL, NULL));
}

TEST(test_twcc_run_length_large_delta)
{
    /* Run of 3 with 2-byte signed deltas, including a negative value. */
    uint8_t buf[64] = {0};
    struct twcc_builder b = {buf, sizeof(buf), 0};
    twcc_build_header(&b, 1, 2, 10, 3, 0, 0);
    twcc_append_u16(&b, twcc_chunk_run(NANO_TWCC_STATUS_LARGE_DELTA, 3));
    /* Large delta values (int16 * 250us): 1000 → 250000us, -200 → -50000us, 0 → 0. */
    twcc_append_u16(&b, 1000);
    twcc_append_u16(&b, (uint16_t)(int16_t)-200);
    twcc_append_u16(&b, 0);
    twcc_finalize(&b);

    capture_ctx_t ctx = {0};
    nano_twcc_summary_t sum;
    ASSERT_OK(twcc_parse_feedback(buf, b.len, &sum, capture_cb, &ctx));
    ASSERT_EQ(sum.packet_status_count, 3);
    ASSERT_EQ(sum.received_count, 3);
    ASSERT_EQ(ctx.count, 3u);
    ASSERT_EQ(ctx.items[0].delta_us, 250000);
    ASSERT_EQ(ctx.items[1].delta_us, -50000);
    ASSERT_EQ(ctx.items[2].delta_us, 0);
}

/* ================================================================
 * Status-vector chunk tests (1-bit and 2-bit)
 * ================================================================ */

TEST(test_twcc_vec1_mixed_received_lost)
{
    /* 14 bits: R L R L R L R L R L R L R L (alternating).
     * 7 received (small delta), 7 lost. */
    uint8_t bits[14] = {1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0};
    uint8_t buf[64] = {0};
    struct twcc_builder b = {buf, sizeof(buf), 0};
    twcc_build_header(&b, 1, 2, 0, 14, 0, 0);
    twcc_append_u16(&b, twcc_chunk_vec1(bits, 14));
    for (int i = 0; i < 7; i++) {
        twcc_append_u8(&b, 2); /* delta = 500us each */
    }
    twcc_finalize(&b);

    capture_ctx_t ctx = {0};
    nano_twcc_summary_t sum;
    ASSERT_OK(twcc_parse_feedback(buf, b.len, &sum, capture_cb, &ctx));
    ASSERT_EQ(sum.packet_status_count, 14);
    ASSERT_EQ(sum.received_count, 7);
    ASSERT_EQ(ctx.count, 14u);
    for (size_t i = 0; i < 14; i++) {
        nano_twcc_status_t expected =
            bits[i] ? NANO_TWCC_STATUS_SMALL_DELTA : NANO_TWCC_STATUS_NOT_RECEIVED;
        ASSERT_EQ(ctx.items[i].status, expected);
        ASSERT_EQ(ctx.items[i].delta_us, bits[i] ? 500 : 0);
    }
}

TEST(test_twcc_vec2_seven_symbols)
{
    /* 7 packets: 3 lost, 2 small delta, 2 large delta. */
    uint8_t syms[7] = {NANO_TWCC_STATUS_NOT_RECEIVED, NANO_TWCC_STATUS_SMALL_DELTA,
                       NANO_TWCC_STATUS_SMALL_DELTA,  NANO_TWCC_STATUS_LARGE_DELTA,
                       NANO_TWCC_STATUS_NOT_RECEIVED, NANO_TWCC_STATUS_LARGE_DELTA,
                       NANO_TWCC_STATUS_NOT_RECEIVED};
    uint8_t buf[64] = {0};
    struct twcc_builder b = {buf, sizeof(buf), 0};
    twcc_build_header(&b, 0x1111, 0x2222, 500, 7, 42, 3);
    twcc_append_u16(&b, twcc_chunk_vec2(syms, 7));
    /* Deltas: small=4 (1ms), small=8 (2ms), large=-4 (-1ms), large=12 (3ms). */
    twcc_append_u8(&b, 4);
    twcc_append_u8(&b, 8);
    twcc_append_u16(&b, (uint16_t)(int16_t)-4);
    twcc_append_u16(&b, 12);
    twcc_finalize(&b);

    capture_ctx_t ctx = {0};
    nano_twcc_summary_t sum;
    ASSERT_OK(twcc_parse_feedback(buf, b.len, &sum, capture_cb, &ctx));
    ASSERT_EQ(sum.sender_ssrc, 0x1111u);
    ASSERT_EQ(sum.media_ssrc, 0x2222u);
    ASSERT_EQ(sum.base_seq, 500);
    ASSERT_EQ(sum.packet_status_count, 7);
    ASSERT_EQ(sum.received_count, 4);
    ASSERT_EQ(sum.fb_pkt_count, 3);
    ASSERT_EQ(sum.reference_time_us, 42LL * 64000LL);
    ASSERT_EQ(ctx.count, 7u);
    ASSERT_EQ(ctx.items[0].status, NANO_TWCC_STATUS_NOT_RECEIVED);
    ASSERT_EQ(ctx.items[0].delta_us, 0);
    ASSERT_EQ(ctx.items[1].status, NANO_TWCC_STATUS_SMALL_DELTA);
    ASSERT_EQ(ctx.items[1].delta_us, 1000);
    ASSERT_EQ(ctx.items[2].status, NANO_TWCC_STATUS_SMALL_DELTA);
    ASSERT_EQ(ctx.items[2].delta_us, 2000);
    ASSERT_EQ(ctx.items[3].status, NANO_TWCC_STATUS_LARGE_DELTA);
    ASSERT_EQ(ctx.items[3].delta_us, -1000);
    ASSERT_EQ(ctx.items[4].status, NANO_TWCC_STATUS_NOT_RECEIVED);
    ASSERT_EQ(ctx.items[5].status, NANO_TWCC_STATUS_LARGE_DELTA);
    ASSERT_EQ(ctx.items[5].delta_us, 3000);
    ASSERT_EQ(ctx.items[6].status, NANO_TWCC_STATUS_NOT_RECEIVED);
}

/* ================================================================
 * Mixed chunk types
 * ================================================================ */

TEST(test_twcc_mixed_chunks)
{
    /* 8 run of received + 7 mixed via 2-bit vector = 15 packets. */
    uint8_t syms[7] = {NANO_TWCC_STATUS_NOT_RECEIVED, NANO_TWCC_STATUS_SMALL_DELTA,
                       NANO_TWCC_STATUS_NOT_RECEIVED, NANO_TWCC_STATUS_NOT_RECEIVED,
                       NANO_TWCC_STATUS_SMALL_DELTA,  NANO_TWCC_STATUS_NOT_RECEIVED,
                       NANO_TWCC_STATUS_NOT_RECEIVED};
    uint8_t buf[96] = {0};
    struct twcc_builder b = {buf, sizeof(buf), 0};
    twcc_build_header(&b, 1, 2, 0, 15, 0, 0);
    twcc_append_u16(&b, twcc_chunk_run(NANO_TWCC_STATUS_SMALL_DELTA, 8));
    twcc_append_u16(&b, twcc_chunk_vec2(syms, 7));
    for (int i = 0; i < 8; i++) {
        twcc_append_u8(&b, 1); /* small delta 250us */
    }
    twcc_append_u8(&b, 2); /* syms[1] */
    twcc_append_u8(&b, 3); /* syms[4] */
    twcc_finalize(&b);

    nano_twcc_summary_t sum;
    ASSERT_OK(twcc_parse_feedback(buf, b.len, &sum, NULL, NULL));
    ASSERT_EQ(sum.packet_status_count, 15);
    ASSERT_EQ(sum.received_count, 10);
}

TEST(test_twcc_run_truncates_at_count)
{
    /* Run-length claims 100 packets but count=5: parser must stop at 5. */
    uint8_t buf[32] = {0};
    struct twcc_builder b = {buf, sizeof(buf), 0};
    twcc_build_header(&b, 1, 2, 0, 5, 0, 0);
    twcc_append_u16(&b, twcc_chunk_run(NANO_TWCC_STATUS_NOT_RECEIVED, 100));
    twcc_finalize(&b);

    nano_twcc_summary_t sum;
    ASSERT_OK(twcc_parse_feedback(buf, b.len, &sum, NULL, NULL));
    ASSERT_EQ(sum.packet_status_count, 5);
    ASSERT_EQ(sum.received_count, 0);
}

/* ================================================================
 * Malformed: truncated deltas
 * ================================================================ */

TEST(test_twcc_truncated_small_delta)
{
    /* Chunk says 3 received small-delta but only 2 delta bytes included. */
    uint8_t buf[32] = {0};
    struct twcc_builder b = {buf, sizeof(buf), 0};
    twcc_build_header(&b, 1, 2, 0, 3, 0, 0);
    twcc_append_u16(&b, twcc_chunk_run(NANO_TWCC_STATUS_SMALL_DELTA, 3));
    twcc_append_u8(&b, 4);
    twcc_append_u8(&b, 4);
    /* Missing 3rd delta byte — but pad to 4-byte boundary anyway so length check passes. */
    /* Intentionally NOT appending the third delta. */
    /* Manually set RTCP length short to claim fewer bytes than needed. */
    uint16_t short_words = (uint16_t)((b.len / 4) - 1); /* missing last delta */
    /* Pad the buffer for transport but don't extend packet length. */
    while (b.len % 4 != 0) {
        b.buf[b.len++] = 0;
    }
    nanortc_write_u16be(b.buf + 2, short_words);
    /* Send only the claimed length. */
    size_t claimed = ((size_t)short_words + 1) * 4;

    nano_twcc_summary_t sum;
    ASSERT_FAIL(twcc_parse_feedback(buf, claimed, &sum, NULL, NULL));
}

TEST(test_twcc_truncated_chunks)
{
    /* Header claims 10 packets but only one chunk (8 symbols coverage). */
    uint8_t buf[32] = {0};
    struct twcc_builder b = {buf, sizeof(buf), 0};
    twcc_build_header(&b, 1, 2, 0, 10, 0, 0);
    twcc_append_u16(&b, twcc_chunk_run(NANO_TWCC_STATUS_NOT_RECEIVED, 5));
    twcc_finalize(&b);

    nano_twcc_summary_t sum;
    ASSERT_FAIL(twcc_parse_feedback(buf, b.len, &sum, NULL, NULL));
}

TEST(test_twcc_count_exceeds_cap)
{
    uint8_t buf[32] = {0};
    struct twcc_builder b = {buf, sizeof(buf), 0};
    twcc_build_header(&b, 1, 2, 0, NANORTC_TWCC_MAX_PACKETS_PER_FB + 1, 0, 0);
    twcc_append_u16(&b, twcc_chunk_run(NANO_TWCC_STATUS_NOT_RECEIVED, 100));
    twcc_finalize(&b);

    nano_twcc_summary_t sum;
    ASSERT_FAIL(twcc_parse_feedback(buf, b.len, &sum, NULL, NULL));
}

/* ================================================================
 * Hard-coded RFC-style byte vector
 *
 * Hand-computed: 5 packets starting at seq 0x0064 (100),
 * reference time 0x0003E8 (1000 ticks = 64 s),
 * fb_pkt_count = 2. Run-length chunk: 5 small-deltas.
 * Deltas: 1, 2, 3, 4, 5 (each * 250us).
 * ================================================================ */

TEST(test_twcc_hand_crafted_vector)
{
    uint8_t pkt[] = {
        0x8F, 0xCD, 0x00, 0x06, /* V=2, P=0, FMT=15, PT=205, length=6 (28 bytes) */
        0x12, 0x34, 0x56, 0x78, /* sender SSRC */
        0x87, 0x65, 0x43, 0x21, /* media SSRC */
        0x00, 0x64, 0x00, 0x05, /* base_seq=100, count=5 */
        0x00, 0x03, 0xE8, 0x02, /* ref_time=1000 (64 s), fb_pkt_count=2 */
        0x20, 0x05,             /* chunk: run-length symbol=01 (small), run=5 */
        0x01, 0x02,             /* delta[0]=1 (250us), delta[1]=2 (500us) */
        0x03, 0x04,             /* delta[2]=3 (750us), delta[3]=4 (1000us) */
        0x05, 0x00,             /* delta[4]=5 (1250us), pad */
    };

    capture_ctx_t ctx = {0};
    nano_twcc_summary_t sum;
    ASSERT_OK(twcc_parse_feedback(pkt, sizeof(pkt), &sum, capture_cb, &ctx));
    ASSERT_EQ(sum.sender_ssrc, 0x12345678u);
    ASSERT_EQ(sum.media_ssrc, 0x87654321u);
    ASSERT_EQ(sum.base_seq, 100);
    ASSERT_EQ(sum.packet_status_count, 5);
    ASSERT_EQ(sum.received_count, 5);
    ASSERT_EQ(sum.fb_pkt_count, 2);
    ASSERT_EQ(sum.reference_time_us, 1000LL * 64000LL);
    ASSERT_EQ(ctx.count, 5u);
    ASSERT_EQ(ctx.items[0].seq, 100);
    ASSERT_EQ(ctx.items[0].delta_us, 250);
    ASSERT_EQ(ctx.items[4].seq, 104);
    ASSERT_EQ(ctx.items[4].delta_us, 1250);
}

/* ================================================================
 * Callback-optional paths (no cb still validates deltas)
 * ================================================================ */

TEST(test_twcc_no_callback_still_validates)
{
    /* Valid packet parsed without callback: summary must be populated. */
    uint8_t buf[64] = {0};
    struct twcc_builder b = {buf, sizeof(buf), 0};
    twcc_build_header(&b, 0xDEAD, 0xBEEF, 2000, 3, 0, 99);
    twcc_append_u16(&b, twcc_chunk_run(NANO_TWCC_STATUS_SMALL_DELTA, 3));
    twcc_append_u8(&b, 1);
    twcc_append_u8(&b, 1);
    twcc_append_u8(&b, 1);
    twcc_finalize(&b);

    nano_twcc_summary_t sum;
    ASSERT_OK(twcc_parse_feedback(buf, b.len, &sum, NULL, NULL));
    ASSERT_EQ(sum.sender_ssrc, 0xDEADu);
    ASSERT_EQ(sum.media_ssrc, 0xBEEFu);
    ASSERT_EQ(sum.base_seq, 2000);
    ASSERT_EQ(sum.packet_status_count, 3);
    ASSERT_EQ(sum.received_count, 3);
    ASSERT_EQ(sum.fb_pkt_count, 99);
}

/* ================================================================
 * Runner
 * ================================================================ */

TEST_MAIN_BEGIN("TWCC feedback parser tests")
RUN(test_twcc_parse_null_data);
RUN(test_twcc_parse_null_summary);
RUN(test_twcc_parse_too_short);
RUN(test_twcc_parse_bad_version);
RUN(test_twcc_parse_bad_pt);
RUN(test_twcc_parse_bad_fmt);
RUN(test_twcc_parse_length_beyond_buffer);
RUN(test_twcc_parse_empty_feedback);
RUN(test_twcc_parse_negative_reference_time);
RUN(test_twcc_run_length_all_received_small);
RUN(test_twcc_run_length_all_lost);
RUN(test_twcc_run_length_zero_rejected);
RUN(test_twcc_run_length_large_delta);
RUN(test_twcc_vec1_mixed_received_lost);
RUN(test_twcc_vec2_seven_symbols);
RUN(test_twcc_mixed_chunks);
RUN(test_twcc_run_truncates_at_count);
RUN(test_twcc_truncated_small_delta);
RUN(test_twcc_truncated_chunks);
RUN(test_twcc_count_exceeds_cap);
RUN(test_twcc_hand_crafted_vector);
RUN(test_twcc_no_callback_still_validates);
TEST_MAIN_END
