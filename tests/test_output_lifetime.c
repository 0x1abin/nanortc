/*
 * nanortc — Output payload lifetime contract tests
 *
 * Verifies the lifetime contract documented on `nanortc_output_t` in
 * include/nanortc.h: a `transmit.data` pointer is valid only until the
 * next nanortc_poll_output() / nanortc_handle_input() / nanortc_destroy()
 * call. This is what makes the zero-copy output path safe; a regression
 * would silently corrupt media or relayed payloads in production.
 *
 * Coverage (driven through the public nanortc_poll_output() API, not a
 * synthetic helper):
 *   - Video pkt_ring path: if the caller drains before pkt_ring wraps,
 *     the returned data still matches the producer-side bytes.
 *   - Video pkt_ring overrun: if the caller fails to drain before wrap,
 *     stats_pkt_ring_overrun fires and the early entries' `transmit.data`
 *     aliases the latest writer's slot — documenting why the contract
 *     forbids retention past the next library entry.
 *   - TURN lazy wrap: poll_output() rewrites rtc->turn_buf in place. Two
 *     consecutive polls return the same pointer (turn_buf) but with
 *     distinct contents — caller MUST consume each before the next poll.
 *   - TURN wrap drop: if a payload exceeds turn_buf, the entry is silently
 *     dropped and stats_wrap_dropped fires.
 *
 * SPDX-License-Identifier: MIT
 */

#include "nanortc.h"
#include "nano_test.h"
#include <string.h>

#if NANORTC_FEATURE_TURN
#include "nano_turn.h"
#include "nano_stun.h" /* for STUN_FAMILY_IPV4 */
#endif

/* The library uses a 64 KB+ struct on full media; keep one static instance
 * to avoid stack-overflow on small CI runners. */
static nanortc_t g_rtc;

/* ================================================================
 * Video pkt_ring lifetime (NANORTC_FEATURE_VIDEO)
 * ================================================================ */

#if NANORTC_FEATURE_VIDEO

/* Mirror of the production guard in src/nano_rtc.c:video_send_fragment_cb.
 * Drives the same pkt_ring + out_queue layout the real callback drives,
 * but skips the SRTP/RTP machinery a unit test cannot realistically run.
 * The point of these tests is the lifetime contract surfaced through
 * nanortc_poll_output(), not the encoder itself. */
static void enqueue_pkt_ring_fragment(nanortc_t *rtc, uint16_t seq, uint8_t marker)
{
    uint16_t out_inflight = (uint16_t)(rtc->out_tail - rtc->out_head);
    if (out_inflight >= NANORTC_VIDEO_PKT_RING_SIZE) {
        rtc->stats_pkt_ring_overrun++;
    }

    uint16_t pslot = rtc->pkt_ring_tail & (NANORTC_VIDEO_PKT_RING_SIZE - 1);
    uint8_t *pkt_buf = rtc->pkt_ring[pslot];
    pkt_buf[0] = marker;
    rtc->pkt_ring_meta[pslot].seq = seq;
    rtc->pkt_ring_meta[pslot].len = 1;
    rtc->pkt_ring_tail++;

    uint16_t oslot = rtc->out_tail & (NANORTC_OUT_QUEUE_SIZE - 1);
    rtc->out_queue[oslot].type = NANORTC_OUTPUT_TRANSMIT;
    rtc->out_queue[oslot].transmit.data = pkt_buf;
    rtc->out_queue[oslot].transmit.len = 1;
    /* dest left zeroed — no TURN wrap on this path */
    rtc->out_tail++;
}

TEST(test_pkt_ring_drain_before_wrap_keeps_pointers_valid)
{
    nanortc_t *rtc = &g_rtc;
    memset(rtc, 0, sizeof(*rtc));

    /* Push exactly NANORTC_VIDEO_PKT_RING_SIZE fragments — no wrap. */
    for (uint16_t i = 0; i < NANORTC_VIDEO_PKT_RING_SIZE; i++) {
        enqueue_pkt_ring_fragment(rtc, (uint16_t)(7000 + i), (uint8_t)(0x40 + i));
    }
    ASSERT_EQ(rtc->stats_pkt_ring_overrun, 0u);

    /* Drain one-by-one: each poll's transmit.data must still hold the
     * producer's marker. This is the canonical "drain before wrap" path
     * the contract supports. */
    for (uint16_t i = 0; i < NANORTC_VIDEO_PKT_RING_SIZE; i++) {
        nanortc_output_t out;
        ASSERT_OK(nanortc_poll_output(rtc, &out));
        ASSERT_EQ(out.type, NANORTC_OUTPUT_TRANSMIT);
        ASSERT_EQ(out.transmit.len, 1u);
        ASSERT_TRUE(out.transmit.data != NULL);
        ASSERT_EQ(out.transmit.data[0], (uint8_t)(0x40 + i));
    }

    /* Queue is now empty. */
    nanortc_output_t out;
    ASSERT_FAIL(nanortc_poll_output(rtc, &out));
}

TEST(test_pkt_ring_overrun_aliases_pre_drain_pointers)
{
    nanortc_t *rtc = &g_rtc;
    memset(rtc, 0, sizeof(*rtc));

    /* Push PKT_RING_SIZE + 1 without polling. The +1 fragment wraps and
     * overwrites slot 0 in pkt_ring; the still-pending out_queue[0].data
     * pointer therefore aliases the wrapped writer's bytes. This is the
     * failure mode the contract warns about. */
    const uint16_t N = (uint16_t)(NANORTC_VIDEO_PKT_RING_SIZE + 1u);
    for (uint16_t i = 0; i < N; i++) {
        enqueue_pkt_ring_fragment(rtc, (uint16_t)(8000 + i), (uint8_t)(0x80 + i));
    }
    ASSERT_EQ(rtc->stats_pkt_ring_overrun, 1u);

    /* Now poll: the first returned entry's data byte is NOT 0x80 (the
     * original) but 0x80 + PKT_RING_SIZE (the wrap writer). */
    nanortc_output_t out;
    ASSERT_OK(nanortc_poll_output(rtc, &out));
    ASSERT_EQ(out.type, NANORTC_OUTPUT_TRANSMIT);
    ASSERT_TRUE(out.transmit.data != NULL);
    ASSERT_EQ(out.transmit.data[0], (uint8_t)(0x80u + NANORTC_VIDEO_PKT_RING_SIZE));
    ASSERT_NEQ(out.transmit.data[0], (uint8_t)0x80u);
}

#endif /* NANORTC_FEATURE_VIDEO */

/* ================================================================
 * TURN lazy-wrap lifetime (NANORTC_FEATURE_TURN)
 * ================================================================ */

#if NANORTC_FEATURE_TURN

/* Pre-stage `rtc` so nanortc_poll_output() takes the lazy-wrap branch:
 * TURN configured + ALLOCATED + ICE selected pair is RELAY. */
static void prime_turn_state(nanortc_t *rtc)
{
    memset(rtc, 0, sizeof(*rtc));
    rtc->turn.configured = true;
    rtc->turn.state = NANORTC_TURN_ALLOCATED;
    rtc->turn.server_family = STUN_FAMILY_IPV4;
    rtc->turn.server_addr[0] = 192;
    rtc->turn.server_addr[1] = 0;
    rtc->turn.server_addr[2] = 2;
    rtc->turn.server_addr[3] = 100;
    rtc->turn.server_port = 3478;
    /* selected_type is read with __atomic_load_n; init via plain store
     * is fine — the test is single-threaded. */
    rtc->ice.selected_type = NANORTC_ICE_CAND_RELAY;
}

/* Enqueue a transmit slot pre-flagged for TURN wrap. Skips the static
 * rtc_enqueue_transmit() helper (not exposed) but uses the same fields
 * the helper writes. */
static void enqueue_turn_wrapped(nanortc_t *rtc, const uint8_t *payload, size_t len,
                                 const uint8_t peer_addr_v4[4], uint16_t peer_port)
{
    uint16_t slot = rtc->out_tail & (NANORTC_OUT_QUEUE_SIZE - 1);
    rtc->out_queue[slot].type = NANORTC_OUTPUT_TRANSMIT;
    rtc->out_queue[slot].transmit.data = payload;
    rtc->out_queue[slot].transmit.len = len;
    /* dest is overwritten by poll_output to point at the TURN server */
    rtc->out_wrap_meta[slot].via_turn = true;
    rtc->out_wrap_meta[slot].peer_dest.family = 4;
    memcpy(rtc->out_wrap_meta[slot].peer_dest.addr, peer_addr_v4, 4);
    rtc->out_wrap_meta[slot].peer_dest.port = peer_port;
    rtc->out_tail++;
}

TEST(test_turn_lazy_wrap_rewrites_turn_buf_between_polls)
{
    nanortc_t *rtc = &g_rtc;
    prime_turn_state(rtc);

    static const uint8_t peer_a[4] = {198, 51, 100, 1};
    static const uint8_t peer_b[4] = {198, 51, 100, 2};
    static const uint8_t payload_a[] = {0xAA, 0xAA, 0xAA, 0xAA};
    static const uint8_t payload_b[] = {0xBB, 0xBB, 0xBB, 0xBB, 0xBB, 0xBB};

    enqueue_turn_wrapped(rtc, payload_a, sizeof(payload_a), peer_a, 5001);
    enqueue_turn_wrapped(rtc, payload_b, sizeof(payload_b), peer_b, 5002);

    nanortc_output_t out1;
    ASSERT_OK(nanortc_poll_output(rtc, &out1));
    ASSERT_EQ(out1.type, NANORTC_OUTPUT_TRANSMIT);
    /* Lifetime contract: lazy wrap returns rtc->turn_buf as the payload
     * pointer. The wrapped envelope contains payload_a in its trailer. */
    ASSERT_TRUE(out1.transmit.data == rtc->turn_buf);
    ASSERT_TRUE(out1.transmit.len > sizeof(payload_a));
    /* Snapshot the first few wrapped bytes so we can prove the buffer
     * actually changes on the next poll. */
    uint8_t snapshot1[16] = {0};
    size_t snap_len = out1.transmit.len < sizeof(snapshot1) ? out1.transmit.len : sizeof(snapshot1);
    memcpy(snapshot1, out1.transmit.data, snap_len);

    nanortc_output_t out2;
    ASSERT_OK(nanortc_poll_output(rtc, &out2));
    ASSERT_EQ(out2.type, NANORTC_OUTPUT_TRANSMIT);
    /* Same pointer, distinct contents — this is the contract: callers
     * must consume out1 before invoking poll a second time, otherwise
     * what they hold has been silently rewritten. */
    ASSERT_TRUE(out2.transmit.data == rtc->turn_buf);
    ASSERT_TRUE(out2.transmit.len > sizeof(payload_b));
    bool differ = false;
    size_t cmp_len = out2.transmit.len < sizeof(snapshot1) ? out2.transmit.len : sizeof(snapshot1);
    for (size_t i = 0; i < cmp_len; i++) {
        if (snapshot1[i] != out2.transmit.data[i]) {
            differ = true;
            break;
        }
    }
    ASSERT_TRUE(differ);

    /* Both wraps succeeded; no drops. */
    ASSERT_EQ(rtc->stats_wrap_dropped, 0u);
}

TEST(test_turn_lazy_wrap_oversized_payload_drops_silently)
{
    nanortc_t *rtc = &g_rtc;
    prime_turn_state(rtc);

    /* Stage one oversized + one normal entry. The oversized one must
     * trip stats_wrap_dropped and not be returned to the caller; the
     * normal one must come back on the very next poll, so the caller
     * never observes the dropped slot. */
    static uint8_t oversized[NANORTC_TURN_BUF_SIZE + 64];
    memset(oversized, 0xCC, sizeof(oversized));

    static const uint8_t peer[4] = {203, 0, 113, 7};
    static const uint8_t payload_ok[] = {0x11, 0x22, 0x33};

    enqueue_turn_wrapped(rtc, oversized, sizeof(oversized), peer, 5003);
    enqueue_turn_wrapped(rtc, payload_ok, sizeof(payload_ok), peer, 5003);

    nanortc_output_t out;
    ASSERT_OK(nanortc_poll_output(rtc, &out));
    ASSERT_EQ(out.type, NANORTC_OUTPUT_TRANSMIT);
    /* The oversized slot was skipped, so what came back is the small
     * one. We can't introspect the wrapped envelope's payload trailer
     * cheaply here, but the wrap-dropped counter and the queue head
     * advance prove the dispatch path. */
    ASSERT_EQ(rtc->stats_wrap_dropped, 1u);
    ASSERT_TRUE(out.transmit.data == rtc->turn_buf);

    /* Queue is now drained. */
    nanortc_output_t empty;
    ASSERT_FAIL(nanortc_poll_output(rtc, &empty));
}

#endif /* NANORTC_FEATURE_TURN */

/* ================================================================
 * Test runner
 * ================================================================ */

TEST_MAIN_BEGIN("test_output_lifetime")
#if NANORTC_FEATURE_VIDEO
RUN(test_pkt_ring_drain_before_wrap_keeps_pointers_valid);
RUN(test_pkt_ring_overrun_aliases_pre_drain_pointers);
#endif
#if NANORTC_FEATURE_TURN
RUN(test_turn_lazy_wrap_rewrites_turn_buf_between_polls);
RUN(test_turn_lazy_wrap_oversized_payload_drops_silently);
#endif
TEST_MAIN_END
