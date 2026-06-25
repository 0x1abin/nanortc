/*
 * nanortc — Send-side video RTP pacer tests (NANORTC_FEATURE_VIDEO_PACING)
 *
 * Drives the pacer mechanics (src/nano_rtc_media.c: nano_rtc_pacer_enqueue /
 * nano_rtc_pacer_pump / nano_rtc_pacer_next_deadline_ms) in isolation — a
 * zeroed nanortc_t with the pacer + pkt_ring fields seeded by hand, no DTLS
 * handshake — so the leaky-token-bucket arithmetic is asserted deterministically:
 *
 *   - initial burst budget releases, then metering holds until time advances;
 *   - a frame whose size fits the budget within the latency cap is delivered
 *     fully and purely by metering (no catch-up), spread across several pumps;
 *   - a backlog older than NANORTC_PACING_MAX_QUEUE_MS is flushed in one pump
 *     (catch-up) so the pacer never adds unbounded latency;
 *   - nanortc_next_timeout_ms()'s pacer branch reports the next-release delay
 *     and UINT32_MAX when the FIFO is empty.
 *
 * The end-to-end integration (real send path → pace FIFO → poll_output pump,
 * plus the admission/aliasing interaction) is covered by
 * test_e2e_video_send_admission in tests/test_e2e.c.
 *
 * SPDX-License-Identifier: MIT
 */

#include "nanortc.h"
#include "nano_rtc_internal.h"
#include "nano_test.h"

#include <stdint.h>
#include <string.h>

#if NANORTC_FEATURE_VIDEO_PACING

/* sizeof(nanortc_t) is large (tens of KB for a media build) — keep it off the
 * stack. One suite, single-threaded, so a file-scope instance is fine. */
static nanortc_t g_rtc;

/* Stage @p nfrags fragments of @p len bytes into the pace FIFO at time @p now,
 * exactly as the real send path does: write pkt_ring_meta, advance the ring
 * write cursor, then hand the slot to the pacer. */
static void seed_frame(nanortc_t *rtc, uint16_t nfrags, uint16_t len, uint32_t now)
{
    rtc->now_ms = now;
    for (uint16_t i = 0; i < nfrags; i++) {
        uint16_t slot = (uint16_t)(rtc->pkt_ring_tail & (NANORTC_VIDEO_PKT_RING_SIZE - 1));
        rtc->pkt_ring_meta[slot].seq = (uint16_t)(1000 + i);
        rtc->pkt_ring_meta[slot].len = len;
        rtc->pkt_ring_tail++;
        nano_rtc_pacer_enqueue(rtc, slot);
    }
}

static uint16_t out_depth(const nanortc_t *rtc)
{
    return (uint16_t)(rtc->out_tail - rtc->out_head);
}

static uint16_t pace_depth(const nanortc_t *rtc)
{
    return (uint16_t)(rtc->pacer.tail - rtc->pacer.head);
}

/* Simulate the application consuming every queued output. */
static void drain_out(nanortc_t *rtc)
{
    rtc->out_head = rtc->out_tail;
}

static void reset_rtc(uint32_t est_bps)
{
    memset(&g_rtc, 0, sizeof(g_rtc));
    g_rtc.bwe.estimated_bitrate = est_bps;
    /* nanortc_init() seeds the bucket full; replicate that here. */
    g_rtc.pacer.budget_bytes = NANORTC_PACING_MAX_BURST_BYTES;
}

/* The initial (full) burst budget releases on the first pump; with no time
 * advance the bucket cannot refill, so the rest of the frame is held. */
TEST(test_pacing_initial_burst_then_holds)
{
    reset_rtc(1000000u); /* 1 Mbps */
    const uint16_t L = 1000, N = 20;
    uint32_t now = 100000u;
    seed_frame(&g_rtc, N, L, now);
    ASSERT_EQ(pace_depth(&g_rtc), N);

    /* Burst budget = NANORTC_PACING_MAX_BURST_BYTES (3000) / 1000 = 3 packets. */
    nano_rtc_pacer_pump(&g_rtc);
    uint16_t first = out_depth(&g_rtc);
    ASSERT_EQ(first, (uint16_t)(NANORTC_PACING_MAX_BURST_BYTES / L));
    ASSERT_TRUE(first < N); /* not the whole frame — metering engaged */
    drain_out(&g_rtc);

    /* No clock advance → no refill → nothing more releases. */
    nano_rtc_pacer_pump(&g_rtc);
    ASSERT_EQ(out_depth(&g_rtc), 0);
    ASSERT_EQ(pace_depth(&g_rtc), (uint16_t)(N - first));

}

/* A frame that drains within the latency cap is delivered fully and purely by
 * the token bucket — every fragment exactly once, spread over several pumps,
 * and the catch-up valve never fires. */
TEST(test_pacing_meters_full_frame_no_catchup)
{
    reset_rtc(2000000u); /* 2 Mbps → 3 Mbps paced = 375 B/ms */
    const uint16_t L = 1000, N = 6;
    uint32_t now = 100000u;
    seed_frame(&g_rtc, N, L, now);

    uint32_t total = 0;
    int pumps_with_release = 0;
    for (int step = 0; step < 200 && pace_depth(&g_rtc) > 0; step++) {
        nano_rtc_pacer_pump(&g_rtc);
        uint16_t r = out_depth(&g_rtc);
        if (r > 0) {
            pumps_with_release++;
        }
        total += r;
        drain_out(&g_rtc);
        now += 5u; /* 5 ms/step; 6 KB at 375 B/ms ≈ 16 ms < catch-up cap */
        g_rtc.now_ms = now;
    }

    ASSERT_EQ(total, (uint32_t)N);       /* every fragment delivered exactly once */
    ASSERT_TRUE(pumps_with_release >= 2); /* spread over time, not one burst */
    ASSERT_EQ(g_rtc.stats_pace_catchup, 0u); /* pure metering, no catch-up */
    ASSERT_EQ(g_rtc.stats_paced_packets, (uint32_t)N);

}

/* A backlog the link cannot drain in time is flushed in one pump once it ages
 * past NANORTC_PACING_MAX_QUEUE_MS — the hard latency cap. */
TEST(test_pacing_catchup_caps_latency)
{
    reset_rtc(100000u);          /* 100 kbps → 150 kbps paced = ~18.75 B/ms */
    g_rtc.pacer.budget_bytes = 0; /* start with an empty bucket */
    const uint16_t L = 1000, N = 20;
    uint32_t now = 50000u;
    seed_frame(&g_rtc, N, L, now);

    /* At t0 with no credit and nothing aged, nothing releases. */
    nano_rtc_pacer_pump(&g_rtc);
    ASSERT_EQ(out_depth(&g_rtc), 0);

    /* Jump past the cap: every fragment is now aged → the whole backlog drains
     * in a single pump (N=20 ≤ NANORTC_OUT_QUEUE_SIZE). */
    now += (uint32_t)NANORTC_PACING_MAX_QUEUE_MS + 5u;
    g_rtc.now_ms = now;
    nano_rtc_pacer_pump(&g_rtc);
    ASSERT_EQ(out_depth(&g_rtc), N);
    ASSERT_EQ(pace_depth(&g_rtc), 0);          /* FIFO emptied */
    ASSERT_TRUE(g_rtc.stats_pace_catchup > 0); /* catch-up valve fired */

}

/* The deadline reported to nanortc_next_timeout_ms() is positive, bounded by
 * the catch-up cap, and honored: advancing the clock by it makes the head
 * fragment releasable. An empty FIFO arms no deadline (UINT32_MAX). */
TEST(test_pacing_next_deadline)
{
    reset_rtc(1000000u);
    g_rtc.pacer.budget_bytes = 0;
    const uint16_t L = 1000, N = 5;
    uint32_t now = 200000u;
    seed_frame(&g_rtc, N, L, now);

    nano_rtc_pacer_pump(&g_rtc); /* no credit, not aged → releases nothing */
    ASSERT_EQ(out_depth(&g_rtc), 0);

    uint32_t d = nano_rtc_pacer_next_deadline_ms(&g_rtc, now);
    ASSERT_TRUE(d >= 1u && d <= (uint32_t)NANORTC_PACING_MAX_QUEUE_MS);

    /* Advancing by the reported delay must make at least one fragment due. */
    now += d;
    g_rtc.now_ms = now;
    nano_rtc_pacer_pump(&g_rtc);
    ASSERT_TRUE(out_depth(&g_rtc) >= 1);

    /* Empty FIFO → no deadline armed. */
    g_rtc.pacer.head = g_rtc.pacer.tail;
    ASSERT_EQ(nano_rtc_pacer_next_deadline_ms(&g_rtc, now), UINT32_MAX);

}

/* A degenerate (zero) BWE estimate must not stall egress or divide by zero:
 * the rate floor keeps fragments flowing as the bucket accrues. */
TEST(test_pacing_zero_estimate_uses_floor)
{
    reset_rtc(0u);               /* floor = NANORTC_PACING_MIN_RATE_BPS applies */
    g_rtc.pacer.budget_bytes = 0;
    const uint16_t L = 200, N = 4; /* small frags so the floor rate can clear them */
    uint32_t now = 10000u;
    seed_frame(&g_rtc, N, L, now);

    /* Pump once to arm the cached deadline, then check it is finite/bounded
     * (the floor rate guarantees a computable, non-infinite wait). */
    nano_rtc_pacer_pump(&g_rtc);
    uint32_t deadline = nano_rtc_pacer_next_deadline_ms(&g_rtc, now);
    ASSERT_TRUE(deadline <= (uint32_t)NANORTC_PACING_MAX_QUEUE_MS);

    /* Advance within the catch-up cap; the floor rate alone must release at
     * least one fragment via the token bucket (not via catch-up). */
    now += (uint32_t)NANORTC_PACING_MAX_QUEUE_MS - 1u;
    g_rtc.now_ms = now;
    nano_rtc_pacer_pump(&g_rtc);
    ASSERT_TRUE(out_depth(&g_rtc) >= 1);
    ASSERT_EQ(g_rtc.stats_pace_catchup, 0u);

}

#endif /* NANORTC_FEATURE_VIDEO_PACING */

TEST_MAIN_BEGIN("nanortc video pacing tests")
#if NANORTC_FEATURE_VIDEO_PACING
RUN(test_pacing_initial_burst_then_holds);
RUN(test_pacing_meters_full_frame_no_catchup);
RUN(test_pacing_catchup_caps_latency);
RUN(test_pacing_next_deadline);
RUN(test_pacing_zero_estimate_uses_floor);
#endif
TEST_MAIN_END
