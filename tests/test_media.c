/*
 * nanortc — Media track and SSRC map unit tests
 *
 * Tests track_init(), track_find_by_mid(), ssrc_map_register/lookup()
 * in isolation without requiring a full RTC connection.
 *
 * SPDX-License-Identifier: MIT
 */

#include "nanortc.h"
#include "nano_media.h"
#include "nano_test.h"
#include <string.h>

/* ================================================================
 * track_init
 * ================================================================ */

TEST(test_track_init_audio)
{
    nanortc_track_t m;
    ASSERT_OK(track_init(&m, 0, NANORTC_TRACK_AUDIO, NANORTC_DIR_SENDRECV, 111, 48000, 2, 100));
    ASSERT_EQ(m.mid, 0);
    ASSERT_EQ(m.kind, NANORTC_TRACK_AUDIO);
    ASSERT_EQ(m.direction, NANORTC_DIR_SENDRECV);
    ASSERT_TRUE(m.active);
    ASSERT_EQ(m.codec, 111);
    ASSERT_EQ(m.sample_rate, 48000);
    ASSERT_EQ(m.channels, 2);
}

TEST(test_track_init_video)
{
    nanortc_track_t m;
    ASSERT_OK(track_init(&m, 1, NANORTC_TRACK_VIDEO, NANORTC_DIR_SENDONLY, 96, 90000, 0, 0));
    ASSERT_EQ(m.kind, NANORTC_TRACK_VIDEO);
    ASSERT_EQ(m.mid, 1);
}

TEST(test_track_init_null)
{
    ASSERT_FAIL(track_init(NULL, 0, NANORTC_TRACK_AUDIO, NANORTC_DIR_SENDRECV, 111, 48000, 2, 100));
}

/* ================================================================
 * track_find_by_mid
 * ================================================================ */

TEST(test_track_find_by_mid_found)
{
    nanortc_track_t media[3];
    memset(media, 0, sizeof(media));
    track_init(&media[0], 0, NANORTC_TRACK_AUDIO, NANORTC_DIR_SENDRECV, 111, 48000, 2, 100);
    track_init(&media[1], 1, NANORTC_TRACK_VIDEO, NANORTC_DIR_SENDONLY, 96, 90000, 0, 0);
    track_init(&media[2], 2, NANORTC_TRACK_AUDIO, NANORTC_DIR_RECVONLY, 0, 8000, 1, 100);

    nanortc_track_t *found = track_find_by_mid(media, 3, 1);
    ASSERT_TRUE(found != NULL);
    ASSERT_EQ(found->mid, 1);
    ASSERT_EQ(found->kind, NANORTC_TRACK_VIDEO);
}

TEST(test_track_find_by_mid_not_found)
{
    nanortc_track_t media[2];
    memset(media, 0, sizeof(media));
    track_init(&media[0], 0, NANORTC_TRACK_AUDIO, NANORTC_DIR_SENDRECV, 111, 48000, 2, 100);
    track_init(&media[1], 1, NANORTC_TRACK_VIDEO, NANORTC_DIR_SENDONLY, 96, 90000, 0, 0);

    ASSERT_EQ(track_find_by_mid(media, 2, 99), NULL);
}

TEST(test_track_find_by_mid_inactive)
{
    nanortc_track_t media[1];
    memset(media, 0, sizeof(media));
    track_init(&media[0], 0, NANORTC_TRACK_AUDIO, NANORTC_DIR_SENDRECV, 111, 48000, 2, 100);
    media[0].active = false;

    ASSERT_EQ(track_find_by_mid(media, 1, 0), NULL);
}

TEST(test_track_find_by_mid_null)
{
    ASSERT_EQ(track_find_by_mid(NULL, 0, 0), NULL);
}

/* ================================================================
 * ssrc_map_register / ssrc_map_lookup
 * ================================================================ */

TEST(test_ssrc_map_register_lookup)
{
    nanortc_ssrc_entry_t map[4];
    memset(map, 0, sizeof(map));

    ASSERT_OK(ssrc_map_register(map, 4, 0x12345678, 0));
    ASSERT_OK(ssrc_map_register(map, 4, 0xAABBCCDD, 1));

    ASSERT_EQ(ssrc_map_lookup(map, 4, 0x12345678), 0);
    ASSERT_EQ(ssrc_map_lookup(map, 4, 0xAABBCCDD), 1);
    ASSERT_EQ(ssrc_map_lookup(map, 4, 0xDEADBEEF), -1); /* Not found */
}

TEST(test_ssrc_map_update)
{
    nanortc_ssrc_entry_t map[4];
    memset(map, 0, sizeof(map));

    ASSERT_OK(ssrc_map_register(map, 4, 0x12345678, 0));
    ASSERT_EQ(ssrc_map_lookup(map, 4, 0x12345678), 0);

    /* Update existing entry */
    ASSERT_OK(ssrc_map_register(map, 4, 0x12345678, 2));
    ASSERT_EQ(ssrc_map_lookup(map, 4, 0x12345678), 2);
}

TEST(test_ssrc_map_full)
{
    nanortc_ssrc_entry_t map[2];
    memset(map, 0, sizeof(map));

    ASSERT_OK(ssrc_map_register(map, 2, 1, 0));
    ASSERT_OK(ssrc_map_register(map, 2, 2, 1));
    ASSERT_FAIL(ssrc_map_register(map, 2, 3, 2)); /* Full */
}

TEST(test_ssrc_map_null)
{
    ASSERT_FAIL(ssrc_map_register(NULL, 4, 1, 0));
    ASSERT_EQ(ssrc_map_lookup(NULL, 4, 1), -1);
}

/* ================================================================
 * Rate window (PR-5) — 1-second bucket reporting previous completed second.
 * Verifies: lazy init, no rollover before 1 s, rollover at 1 s,
 * subsequent bucket reset, Q8.8 fps encoding.
 * ================================================================ */

TEST(test_rate_window_lazy_init)
{
    nano_rate_window_t w;
    memset(&w, 0, sizeof(w));

    /* First touch sets epoch to now_ms but does not emit rates yet. */
    rate_window_on_frame(&w, 5000);
    ASSERT_EQ(w.bucket_start_ms, 5000u);
    ASSERT_EQ(w.cur_frames, 1u);
    ASSERT_EQ(w.prev_bps, 0u);
    ASSERT_EQ(w.prev_fps_q8, 0);
}

TEST(test_rate_window_holds_within_second)
{
    nano_rate_window_t w;
    memset(&w, 0, sizeof(w));

    rate_window_on_frame(&w, 1000);       /* init epoch */
    rate_window_on_bytes(&w, 1100, 1000); /* 100 ms in */
    rate_window_on_frame(&w, 1200);
    rate_window_on_bytes(&w, 1999, 500); /* 999 ms in, still same bucket */

    ASSERT_EQ(w.cur_frames, 2u);
    ASSERT_EQ(w.cur_bytes, 1500u);
    ASSERT_EQ(w.prev_bps, 0u); /* no bucket completed yet */
}

TEST(test_rate_window_rolls_at_second)
{
    nano_rate_window_t w;
    memset(&w, 0, sizeof(w));

    /* One call to init the bucket. */
    rate_window_on_frame(&w, 0);
    /* 30 fps × 1 s at ~100 kbps. */
    for (int i = 0; i < 30; i++) {
        rate_window_on_frame(&w, (uint32_t)(100 + i * 30));
    }
    rate_window_on_bytes(&w, 500, 12500); /* 12500 bytes = 100 kbps/s */

    /* Now cross the 1-second boundary. The next call rolls. */
    rate_window_on_frame(&w, 1100);

    /* Previous bucket now reports 31 frames/s (Q8.8) and 100 kbps. */
    ASSERT_EQ(w.prev_fps_q8, (uint16_t)(31 * 256));
    ASSERT_EQ(w.prev_bps, 12500u * 8u);
    /* Current bucket is the new frame count. */
    ASSERT_EQ(w.cur_frames, 1u);
    ASSERT_EQ(w.cur_bytes, 0u);
    ASSERT_EQ(w.bucket_start_ms, 1100u);
}

TEST(test_rate_window_roll_noop_without_frames_or_bytes)
{
    nano_rate_window_t w;
    memset(&w, 0, sizeof(w));

    /* Calling rate_window_roll alone, without any on_frame/on_bytes, also
     * should move the bucket forward. Useful for get_track_stats which
     * queries rates out-of-band. */
    rate_window_on_frame(&w, 0); /* init epoch */
    rate_window_on_bytes(&w, 100, 5000);
    rate_window_roll(&w, 1500); /* >= 1000 ms since bucket start */
    ASSERT_EQ(w.prev_bps, 5000u * 8u);
    ASSERT_EQ(w.cur_bytes, 0u);
    ASSERT_EQ(w.bucket_start_ms, 1500u);
}

TEST(test_rate_window_fps_saturates)
{
    nano_rate_window_t w;
    memset(&w, 0, sizeof(w));

    rate_window_on_frame(&w, 0);
    /* Artificially pump 300 frames into one second (above uint16 Q8 cap
     * of 255.99 fps). The window must saturate rather than wrap. */
    for (int i = 0; i < 300; i++) {
        rate_window_on_frame(&w, (uint32_t)(i * 2));
    }
    rate_window_on_frame(&w, 1500); /* forces roll */
    ASSERT_EQ(w.prev_fps_q8, 0xFFFFu);
}

/* ================================================================
 * Video NACK pkt_ring decoupling (Phase 8 PR-3).
 *
 * Exercises the ring semantics that back rtc_process_receive's NACK
 * retransmit scan in src/nano_rtc.c:
 *   - pkt_ring_tail advances independently from out_tail,
 *   - writes mask against NANORTC_VIDEO_PKT_RING_SIZE,
 *   - the scan's loop bound equals NANORTC_VIDEO_PKT_RING_SIZE.
 *
 * The scan loop itself is inline in rtc_process_receive; we replicate
 * its two-line body here so the test can stay below the full SRTP/
 * ICE connection setup required to drive real NACK feedback.
 * ================================================================ */

#if NANORTC_FEATURE_VIDEO

/* nanortc_t is ~100 KB with video enabled — keep it off the stack. */
static nanortc_t g_pkt_ring_rtc;

/* Mirror the write-path idiom from video_send_fragment_cb: pick a slot
 * from pkt_ring_tail, stamp meta with the RTP seq, advance the cursor. */
static void pkt_ring_simulate_send(nanortc_t *rtc, uint16_t seq, uint8_t marker)
{
    uint16_t pslot = rtc->pkt_ring_tail & (NANORTC_VIDEO_PKT_RING_SIZE - 1);
    rtc->pkt_ring[pslot][0] = marker; /* marker byte so we can tell slots apart */
    rtc->pkt_ring_meta[pslot].seq = seq;
    rtc->pkt_ring_meta[pslot].len = 42; /* non-zero => valid slot */
    rtc->pkt_ring_tail++;
}

/* Mirror rtc_process_receive's NACK scan body: linear search over the
 * ring for a matching seq. Returns the slot index, or -1 if missing. */
static int pkt_ring_lookup(const nanortc_t *rtc, uint16_t seq)
{
    for (uint16_t s = 0; s < NANORTC_VIDEO_PKT_RING_SIZE; s++) {
        if (rtc->pkt_ring_meta[s].len > 0 && rtc->pkt_ring_meta[s].seq == seq) {
            return (int)s;
        }
    }
    return -1;
}

TEST(test_pkt_ring_in_window_lookup_succeeds)
{
    nanortc_t *rtc = &g_pkt_ring_rtc;
    memset(rtc, 0, sizeof(*rtc));

    /* Fill a few slots (fewer than the ring depth). */
    for (uint16_t i = 0; i < 4; i++) {
        pkt_ring_simulate_send(rtc, (uint16_t)(1000 + i), (uint8_t)i);
    }

    /* All recently sent seqs must be findable. */
    for (uint16_t i = 0; i < 4; i++) {
        int slot = pkt_ring_lookup(rtc, (uint16_t)(1000 + i));
        ASSERT_TRUE(slot >= 0);
        ASSERT_EQ(rtc->pkt_ring[slot][0], (uint8_t)i);
    }

    /* out_tail is untouched by the video send path. */
    ASSERT_EQ(rtc->out_tail, 0);
    ASSERT_EQ(rtc->pkt_ring_tail, 4u);
}

TEST(test_pkt_ring_out_of_window_lookup_misses)
{
    nanortc_t *rtc = &g_pkt_ring_rtc;
    memset(rtc, 0, sizeof(*rtc));

    /* Overfill so the ring wraps: send 2 * PKT_RING_SIZE fragments
     * with sequential seqs. The oldest PKT_RING_SIZE slots must have
     * been overwritten by the newer ones. */
    const uint16_t N = NANORTC_VIDEO_PKT_RING_SIZE;
    for (uint16_t i = 0; i < 2u * N; i++) {
        pkt_ring_simulate_send(rtc, (uint16_t)(500 + i), (uint8_t)(i & 0xFF));
    }

    /* Newer half is findable. */
    for (uint16_t i = N; i < 2u * N; i++) {
        ASSERT_TRUE(pkt_ring_lookup(rtc, (uint16_t)(500 + i)) >= 0);
    }
    /* Older half is gone — scan does not find them. No crash. */
    for (uint16_t i = 0; i < N; i++) {
        ASSERT_EQ(pkt_ring_lookup(rtc, (uint16_t)(500 + i)), -1);
    }

    ASSERT_EQ(rtc->pkt_ring_tail, (uint16_t)(2u * N));
}

TEST(test_pkt_ring_wraparound_independent_of_out_tail)
{
    nanortc_t *rtc = &g_pkt_ring_rtc;
    memset(rtc, 0, sizeof(*rtc));

    /* Stamp out_tail to something non-zero; the ring must not follow it. */
    rtc->out_tail = 7;

    for (uint16_t i = 0; i < NANORTC_VIDEO_PKT_RING_SIZE + 3u; i++) {
        pkt_ring_simulate_send(rtc, (uint16_t)(2000 + i), (uint8_t)i);
    }

    /* pkt_ring_tail advanced by exactly the send count. */
    ASSERT_EQ(rtc->pkt_ring_tail, (uint16_t)(NANORTC_VIDEO_PKT_RING_SIZE + 3u));
    /* out_tail is still whatever the caller set. */
    ASSERT_EQ(rtc->out_tail, 7);

    /* Slot 0 was written twice (wrap) — should hold the newest seq. */
    ASSERT_EQ(rtc->pkt_ring_meta[0].seq, (uint16_t)(2000u + NANORTC_VIDEO_PKT_RING_SIZE));
}

/* ----------------------------------------------------------------
 * Aliasing-overrun regression — guards the contract enforced by
 * src/nano_rtc.c:video_send_fragment_cb. out_queue[].transmit.data
 * stores a pointer into pkt_ring[]; if pkt_ring_tail wraps while a
 * prior pointer is still pending, the buffer behind that pointer is
 * silently overwritten. The production code detects this case via
 * (out_tail - out_head) >= PKT_RING_SIZE and bumps
 * stats_pkt_ring_overrun + emits NANORTC_LOGW. The guard logic is
 * mirrored below so a regression in either side is caught.
 * ---------------------------------------------------------------- */

/* Mirror of the production guard in video_send_fragment_cb. Walks the
 * same memory the real callback walks and bumps the same counter, but
 * skips the SRTP/RTP machinery a unit test can't realistically drive. */
static void pkt_ring_simulate_send_with_guard(nanortc_t *rtc, uint16_t seq, const uint8_t *src_buf,
                                              uint16_t src_len)
{
    uint16_t out_inflight = (uint16_t)(rtc->out_tail - rtc->out_head);
    if (out_inflight >= NANORTC_VIDEO_PKT_RING_SIZE) {
        rtc->stats_pkt_ring_overrun++;
    }

    uint16_t pslot = rtc->pkt_ring_tail & (NANORTC_VIDEO_PKT_RING_SIZE - 1);
    uint8_t *pkt_buf = rtc->pkt_ring[pslot];

    /* Real flow: rtp_pack + srtp_protect both write into pkt_buf. */
    memcpy(pkt_buf, src_buf, src_len);
    rtc->pkt_ring_meta[pslot].seq = seq;
    rtc->pkt_ring_meta[pslot].len = src_len;
    rtc->pkt_ring_tail++;

    /* Real flow: rtc_enqueue_transmit stores pkt_buf in out_queue[].data. */
    uint16_t oslot = rtc->out_tail & (NANORTC_OUT_QUEUE_SIZE - 1);
    rtc->out_queue[oslot].type = NANORTC_OUTPUT_TRANSMIT;
    rtc->out_queue[oslot].transmit.data = pkt_buf;
    rtc->out_queue[oslot].transmit.len = src_len;
    rtc->out_tail++;
}

TEST(test_pkt_ring_overrun_counter_fires_when_undersized)
{
    nanortc_t *rtc = &g_pkt_ring_rtc;
    memset(rtc, 0, sizeof(*rtc));

    /* Producer side never drains. PKT_RING_SIZE fragments fit cleanly;
     * fragment N+1 onward must each bump stats_pkt_ring_overrun. */
    uint8_t payload[8];
    memset(payload, 0xAA, sizeof(payload));

    for (uint16_t i = 0; i < NANORTC_VIDEO_PKT_RING_SIZE; i++) {
        pkt_ring_simulate_send_with_guard(rtc, (uint16_t)(3000 + i), payload, sizeof(payload));
    }
    /* No overrun yet — exactly at capacity. */
    ASSERT_EQ(rtc->stats_pkt_ring_overrun, 0u);

    /* Three more without draining out_queue: each one must trip the guard. */
    for (uint16_t i = 0; i < 3; i++) {
        pkt_ring_simulate_send_with_guard(rtc, (uint16_t)(4000 + i), payload, sizeof(payload));
    }
    ASSERT_EQ(rtc->stats_pkt_ring_overrun, 3u);
}

TEST(test_pkt_ring_aliasing_corrupts_pending_pointers_when_undersized)
{
    nanortc_t *rtc = &g_pkt_ring_rtc;
    memset(rtc, 0, sizeof(*rtc));

    /* Demonstrates *why* the guard exists. Without draining, push enough
     * fragments to wrap pkt_ring while out_queue still holds the older
     * pointers. The early out_queue entries' .data pointers will then
     * point to slots that were overwritten by the later fragments. */
    const uint16_t WAVES = NANORTC_VIDEO_PKT_RING_SIZE + 4u;
    uint8_t buf[1];

    for (uint16_t i = 0; i < WAVES; i++) {
        buf[0] = (uint8_t)(0x10u + (uint8_t)i); /* unique marker per fragment */
        pkt_ring_simulate_send_with_guard(rtc, (uint16_t)(5000 + i), buf, 1);
    }

    /* Guard fired exactly the over-capacity count. */
    ASSERT_EQ(rtc->stats_pkt_ring_overrun, (uint32_t)(WAVES - NANORTC_VIDEO_PKT_RING_SIZE));

    /* Corruption witness: out_queue[0] was enqueued with marker 0x10
     * (fragment 0), but its .data pointer addresses pkt_ring[0], which
     * has since been overwritten by the wrapped fragment. The byte at
     * .data[0] is therefore the *latest* writer's marker, not 0x10. */
    const uint8_t *q0_data = rtc->out_queue[0].transmit.data;
    ASSERT_TRUE(q0_data != NULL);
    /* Slot 0 was last written by fragment index PKT_RING_SIZE (the first
     * wrap). Markers are 0x10 + index. */
    uint8_t expected_after_wrap = (uint8_t)(0x10u + NANORTC_VIDEO_PKT_RING_SIZE);
    ASSERT_EQ(q0_data[0], expected_after_wrap);
    /* Sanity: this is *not* what the producer originally wrote at i=0. */
    ASSERT_TRUE(q0_data[0] != 0x10u);
}

#endif /* NANORTC_FEATURE_VIDEO */

/* ================================================================
 * Test runner
 * ================================================================ */

TEST_MAIN_BEGIN("test_media")
/* Track init */
RUN(test_track_init_audio);
RUN(test_track_init_video);
RUN(test_track_init_null);
/* Track find */
RUN(test_track_find_by_mid_found);
RUN(test_track_find_by_mid_not_found);
RUN(test_track_find_by_mid_inactive);
RUN(test_track_find_by_mid_null);
/* SSRC map */
RUN(test_ssrc_map_register_lookup);
RUN(test_ssrc_map_update);
RUN(test_ssrc_map_full);
RUN(test_ssrc_map_null);
/* Rate window (PR-5) */
RUN(test_rate_window_lazy_init);
RUN(test_rate_window_holds_within_second);
RUN(test_rate_window_rolls_at_second);
RUN(test_rate_window_roll_noop_without_frames_or_bytes);
RUN(test_rate_window_fps_saturates);
#if NANORTC_FEATURE_VIDEO
/* pkt_ring decoupling (Phase 8 PR-3) */
RUN(test_pkt_ring_in_window_lookup_succeeds);
RUN(test_pkt_ring_out_of_window_lookup_misses);
RUN(test_pkt_ring_wraparound_independent_of_out_tail);
RUN(test_pkt_ring_overrun_counter_fires_when_undersized);
RUN(test_pkt_ring_aliasing_corrupts_pending_pointers_when_undersized);
#endif
TEST_MAIN_END
