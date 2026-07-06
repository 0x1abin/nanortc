/*
 * nanortc — Adaptive media rate controller tests (NANORTC_FEATURE_VIDEO_RATE_CONTROL)
 *
 * Validates the pure-compute spec selector + asymmetric hysteresis without any
 * I/O: rung selection from the estimate (with the safety discount), immediate
 * multi-rung down-steps, headroom- and hold-time-gated single-rung up-steps,
 * loss-forced down-steps, no-flap on a stable estimate, and edge cases
 * (cold start, no feedback, single-rung ladder, clamps, NULL/invalid params).
 *
 * SPDX-License-Identifier: MIT
 */

#include "nanortc.h"
#include "nano_rate_control.h"
#include "nano_rtc_internal.h" /* nano_rtc_media_rate_control_tick */
#include "nano_test.h"
#include "nano_test_config.h" /* nano_test_crypto */

#include <stdint.h>
#include <string.h>

#if NANORTC_FEATURE_VIDEO_RATE_CONTROL

/* Three-rung capability ladder, ascending by bitrate (rung 0 = lowest spec). */
static const nanortc_spec_rung_t LADDER3[] = {
    {640, 360, 15, 400000},
    {1280, 720, 30, 1500000},
    {1920, 1080, 30, 4000000},
};
#define LADDER3_N ((uint8_t)(sizeof(LADDER3) / sizeof(LADDER3[0])))

TEST(test_rate_control_null_and_invalid)
{
    nano_rate_control_t rc;
    ASSERT_OK(rate_control_init(&rc));
    ASSERT_FAIL(rate_control_init(NULL));
    ASSERT_FAIL(rate_control_update(NULL, 1000000, 0, LADDER3, LADDER3_N, 0));
    ASSERT_FAIL(rate_control_update(&rc, 1000000, 0, NULL, LADDER3_N, 0));
    ASSERT_FAIL(rate_control_update(&rc, 1000000, 0, LADDER3, 0, 0));
    ASSERT_EQ(rate_control_get_rung(NULL), 0);
}

TEST(test_rate_control_no_feedback_holds)
{
    nano_rate_control_t rc;
    ASSERT_OK(rate_control_init(&rc));
    /* No estimate yet: provisional floor, not latched. */
    ASSERT_EQ(rate_control_update(&rc, 0, 0, LADDER3, LADDER3_N, 0), 0);
    ASSERT_EQ(rate_control_get_rung(&rc), 0);
    /* First real estimate still latches correctly afterwards. */
    ASSERT_EQ(rate_control_update(&rc, 6000000, 0, LADDER3, LADDER3_N, 100), 2);
    /* Estimate drops back to 0: hold the latched rung. */
    ASSERT_EQ(rate_control_update(&rc, 0, 0, LADDER3, LADDER3_N, 200), 2);
}

TEST(test_rate_control_cold_start_selects_affordable)
{
    nano_rate_control_t rc;
    /* Fresh controller each time: the cold-start latch returns the target
     * directly, so this exercises pure rung selection. */
    rate_control_init(&rc);
    ASSERT_EQ(rate_control_update(&rc, 450000, 0, LADDER3, LADDER3_N, 0), 0); /* floor */
    rate_control_init(&rc);
    ASSERT_EQ(rate_control_update(&rc, 2000000, 0, LADDER3, LADDER3_N, 0), 1);
    rate_control_init(&rc);
    ASSERT_EQ(rate_control_update(&rc, 6000000, 0, LADDER3, LADDER3_N, 0), 2);
}

TEST(test_rate_control_clamp_extremes)
{
    nano_rate_control_t rc;
    rate_control_init(&rc);
    ASSERT_EQ(rate_control_update(&rc, UINT32_MAX, 0, LADDER3, LADDER3_N, 0), 2); /* top */
    rate_control_init(&rc);
    ASSERT_EQ(rate_control_update(&rc, 1, 0, LADDER3, LADDER3_N, 0), 0); /* floor */
}

TEST(test_rate_control_down_immediate_skips_rungs)
{
    nano_rate_control_t rc;
    ASSERT_OK(rate_control_init(&rc));
    ASSERT_EQ(rate_control_update(&rc, 6000000, 0, LADDER3, LADDER3_N, 0), 2); /* latch top */
    /* Estimate collapses to rung-0 territory: drop straight 2 -> 0 in one call. */
    ASSERT_EQ(rate_control_update(&rc, 450000, 0, LADDER3, LADDER3_N, 100), 0);
}

TEST(test_rate_control_up_needs_headroom)
{
    nano_rate_control_t rc;
    ASSERT_OK(rate_control_init(&rc));
    ASSERT_EQ(rate_control_update(&rc, 500000, 0, LADDER3, LADDER3_N, 1000), 0); /* latch floor */

    /* An estimate that makes rung 1 the target (budget >= rung-1 bitrate) but
     * does NOT clear the up-headroom margin. Derived from the macros so the
     * precondition holds regardless of the configured percentages. */
    uint32_t budget_for_rung1 =
        (uint32_t)(((uint64_t)LADDER3[1].bitrate_bps * 100u) / NANORTC_RATE_CONTROL_SAFETY_PCT) +
        5000u;
    uint32_t need = (uint32_t)(((uint64_t)LADDER3[1].bitrate_bps *
                                (100u + NANORTC_RATE_CONTROL_UP_HEADROOM_PCT)) /
                               100u);
    ASSERT_TRUE(budget_for_rung1 < need); /* precondition: target up, headroom short */

    /* No up-step even after a long wait, because headroom is never met. */
    ASSERT_EQ(rate_control_update(&rc, budget_for_rung1, 0, LADDER3, LADDER3_N, 2000), 0);
    ASSERT_EQ(rate_control_update(&rc, budget_for_rung1, 0, LADDER3, LADDER3_N,
                                  2000 + NANORTC_RATE_CONTROL_MIN_HOLD_MS * 4u),
              0);
}

TEST(test_rate_control_up_needs_hold_time)
{
    nano_rate_control_t rc;
    ASSERT_OK(rate_control_init(&rc));
    ASSERT_EQ(rate_control_update(&rc, 500000, 0, LADDER3, LADDER3_N, 0), 0); /* latch floor */

    uint32_t est = 2500000; /* ample for rung 1, clears headroom */
    uint32_t hold = NANORTC_RATE_CONTROL_MIN_HOLD_MS;
    /* Before the hold elapses: armed but no up-step. */
    ASSERT_EQ(rate_control_update(&rc, est, 0, LADDER3, LADDER3_N, 1000), 0);
    ASSERT_EQ(rate_control_update(&rc, est, 0, LADDER3, LADDER3_N, 1000 + hold - 1), 0);
    /* At/after the hold: step up exactly one rung. */
    ASSERT_EQ(rate_control_update(&rc, est, 0, LADDER3, LADDER3_N, 1000 + hold), 1);
}

TEST(test_rate_control_up_one_rung_per_hold)
{
    nano_rate_control_t rc;
    ASSERT_OK(rate_control_init(&rc));
    ASSERT_EQ(rate_control_update(&rc, 500000, 0, LADDER3, LADDER3_N, 0), 0); /* latch floor */

    uint32_t est = 10000000; /* budget covers the top rung */
    uint32_t hold = NANORTC_RATE_CONTROL_MIN_HOLD_MS;
    /* 0 -> 1 after one hold. */
    ASSERT_EQ(rate_control_update(&rc, est, 0, LADDER3, LADDER3_N, 1000), 0);
    ASSERT_EQ(rate_control_update(&rc, est, 0, LADDER3, LADDER3_N, 1000 + hold), 1);
    /* 1 -> 2 only after another full hold (timer re-armed for the next rung). */
    ASSERT_EQ(rate_control_update(&rc, est, 0, LADDER3, LADDER3_N, 1000 + hold), 1);
    ASSERT_EQ(rate_control_update(&rc, est, 0, LADDER3, LADDER3_N, 1000 + 2u * hold), 2);
}

TEST(test_rate_control_loss_forces_down)
{
    nano_rate_control_t rc;
    ASSERT_OK(rate_control_init(&rc));
    ASSERT_EQ(rate_control_update(&rc, 6000000, 0, LADDER3, LADDER3_N, 0), 2); /* latch top */
    /* Ample estimate (would hold rung 2) but high loss forces one rung down. */
    ASSERT_EQ(rate_control_update(&rc, 6000000, NANORTC_RATE_CONTROL_LOSS_DOWN_Q8, LADDER3,
                                  LADDER3_N, 100),
              1);
    /* Sustained loss walks it down again. */
    ASSERT_EQ(rate_control_update(&rc, 6000000, NANORTC_RATE_CONTROL_LOSS_DOWN_Q8, LADDER3,
                                  LADDER3_N, 200),
              0);
}

TEST(test_rate_control_stable_no_flap)
{
    nano_rate_control_t rc;
    ASSERT_OK(rate_control_init(&rc));
    ASSERT_EQ(rate_control_update(&rc, 2000000, 0, LADDER3, LADDER3_N, 0), 1); /* latch rung 1 */
    for (uint32_t t = 100; t < 100000; t += 1000) {
        ASSERT_EQ(rate_control_update(&rc, 2000000, 0, LADDER3, LADDER3_N, t), 1);
    }
}

TEST(test_rate_control_single_rung_ladder)
{
    static const nanortc_spec_rung_t one[] = {{320, 240, 15, 200000}};
    nano_rate_control_t rc;
    ASSERT_OK(rate_control_init(&rc));
    /* Any estimate maps to the only rung; the up-step path must not read past
     * the ladder end (ladder[cur+1]) — there is no higher rung to target. */
    ASSERT_EQ(rate_control_update(&rc, 50000000, 0, one, 1, 0), 0);
    ASSERT_EQ(rate_control_update(&rc, 50000000, 0, one, 1, 100000), 0);
    ASSERT_EQ(rate_control_update(&rc, 0, 0, one, 1, 200000), 0);
}

/* ---- Public API + event wiring (integration with nanortc_t) ---- */

TEST(test_rate_control_set_capability_ladder_api)
{
    nanortc_t rtc;
    nanortc_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.crypto = nano_test_crypto();
    cfg.role = NANORTC_ROLE_CONTROLLED;
    ASSERT_OK(nanortc_init(&rtc, &cfg));

    ASSERT_FAIL(nanortc_set_capability_ladder(NULL, LADDER3, LADDER3_N));

    /* Valid ascending ladder is accepted and stored by reference (zero-copy). */
    ASSERT_OK(nanortc_set_capability_ladder(&rtc, LADDER3, LADDER3_N));
    ASSERT_TRUE(rtc.rc_ladder == LADDER3);
    ASSERT_EQ(rtc.rc_ladder_n, LADDER3_N);

    /* A ladder not ascending by bitrate is rejected (boundary validation) and
     * the previously installed ladder is left untouched. */
    static const nanortc_spec_rung_t bad[] = {
        {1280, 720, 30, 1500000},
        {640, 360, 15, 400000},
    };
    ASSERT_FAIL(nanortc_set_capability_ladder(&rtc, bad, 2));
    ASSERT_TRUE(rtc.rc_ladder == LADDER3);

    /* NULL / count 0 clears the ladder (controller idle). */
    ASSERT_OK(nanortc_set_capability_ladder(&rtc, NULL, 0));
    ASSERT_TRUE(rtc.rc_ladder == NULL);
    ASSERT_EQ(rtc.rc_ladder_n, 0);

    nanortc_destroy(&rtc);
}

/* Helper: drain outputs, return 1 if a SPEC_RECOMMENDATION was seen; if @p reco
 * is non-NULL, store the payload of the last one seen. */
static int drain_spec_reco_full(nanortc_t *rtc, nanortc_ev_spec_recommendation_t *reco)
{
    nanortc_output_t out;
    int seen = 0;
    while (nanortc_poll_output(rtc, &out) == NANORTC_OK) {
        if (out.type == NANORTC_OUTPUT_EVENT && out.event.type == NANORTC_EV_SPEC_RECOMMENDATION) {
            seen = 1;
            if (reco) {
                *reco = out.event.spec_recommendation;
            }
        }
    }
    return seen;
}

/* Helper: drain outputs, return 1 if a SPEC_RECOMMENDATION was seen; if @p rung
 * is non-NULL, store the recommended rung of the last one seen. */
static int drain_spec_reco(nanortc_t *rtc, uint8_t *rung)
{
    nanortc_ev_spec_recommendation_t reco;
    int seen = drain_spec_reco_full(rtc, &reco);
    if (seen && rung) {
        *rung = reco.rung;
    }
    return seen;
}

TEST(test_rate_control_tick_emits_event)
{
    nanortc_t rtc;
    nanortc_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.crypto = nano_test_crypto();
    cfg.role = NANORTC_ROLE_CONTROLLED;
    ASSERT_OK(nanortc_init(&rtc, &cfg));

    /* No ladder installed → tick is a no-op (controller idle, no event). */
    rtc.bwe.estimated_bitrate = 6000000;
    nano_rtc_media_rate_control_tick(&rtc);
    ASSERT_EQ(drain_spec_reco(&rtc, NULL), 0);

    /* Install ladder + an ample estimate → first selection emits the top rung. */
    ASSERT_OK(nanortc_set_capability_ladder(&rtc, LADDER3, LADDER3_N));
    rtc.bwe.estimated_bitrate = 6000000; /* budget ~5.1M → rung 2 */
    rtc.now_ms = 1000;
    nano_rtc_media_rate_control_tick(&rtc);

    nanortc_output_t out;
    int got = 0;
    while (nanortc_poll_output(&rtc, &out) == NANORTC_OK) {
        if (out.type == NANORTC_OUTPUT_EVENT && out.event.type == NANORTC_EV_SPEC_RECOMMENDATION) {
            got = 1;
            ASSERT_EQ(out.event.spec_recommendation.rung, 2);
            ASSERT_EQ(out.event.spec_recommendation.width, LADDER3[2].width);
            ASSERT_EQ(out.event.spec_recommendation.height, LADDER3[2].height);
            ASSERT_EQ(out.event.spec_recommendation.fps, LADDER3[2].fps);
            ASSERT_EQ(out.event.spec_recommendation.bitrate_bps, LADDER3[2].bitrate_bps);
        }
    }
    ASSERT_EQ(got, 1);

    /* Same estimate again → rung unchanged → no new recommendation. */
    rtc.now_ms = 2000;
    nano_rtc_media_rate_control_tick(&rtc);
    ASSERT_EQ(drain_spec_reco(&rtc, NULL), 0);

    /* Estimate collapses → immediate down-step emits a new recommendation. */
    rtc.bwe.estimated_bitrate = 450000; /* budget < rung 1 → rung 0 */
    rtc.now_ms = 3000;
    nano_rtc_media_rate_control_tick(&rtc);
    uint8_t rung = 0xFF;
    ASSERT_EQ(drain_spec_reco(&rtc, &rung), 1);
    ASSERT_EQ(rung, 0);

    nanortc_destroy(&rtc);
}

TEST(test_rate_control_ladder_reinstall_reemits)
{
    static const nanortc_spec_rung_t ladder_b[] = {
        {320, 180, 10, 400000},
        {960, 540, 24, 1500000},
        {1280, 720, 30, 4000000},
    };

    nanortc_t rtc;
    nanortc_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.crypto = nano_test_crypto();
    cfg.role = NANORTC_ROLE_CONTROLLED;
    ASSERT_OK(nanortc_init(&rtc, &cfg));

    rtc.bwe.estimated_bitrate = 6000000; /* budget ~5.1M → rung 2 for both ladders */
    rtc.now_ms = 1000;

    ASSERT_OK(nanortc_set_capability_ladder(&rtc, LADDER3, LADDER3_N));
    nano_rtc_media_rate_control_tick(&rtc);
    nanortc_ev_spec_recommendation_t reco;
    ASSERT_EQ(drain_spec_reco_full(&rtc, &reco), 1);
    ASSERT_EQ(reco.rung, 2);
    ASSERT_EQ(reco.width, LADDER3[2].width);

    /* Stable rung on the same ladder does not re-emit. */
    rtc.now_ms = 2000;
    nano_rtc_media_rate_control_tick(&rtc);
    ASSERT_EQ(drain_spec_reco_full(&rtc, NULL), 0);

    /* Clearing + reinstalling a new ladder resets controller state, so the
     * first selection must re-emit even when the numeric rung index is the same. */
    ASSERT_OK(nanortc_set_capability_ladder(&rtc, NULL, 0));
    ASSERT_OK(nanortc_set_capability_ladder(&rtc, ladder_b,
                                            (uint8_t)(sizeof(ladder_b) / sizeof(ladder_b[0]))));
    rtc.now_ms = 3000;
    nano_rtc_media_rate_control_tick(&rtc);
    ASSERT_EQ(drain_spec_reco_full(&rtc, &reco), 1);
    ASSERT_EQ(reco.rung, 2);
    ASSERT_EQ(reco.width, ladder_b[2].width);
    ASSERT_EQ(reco.height, ladder_b[2].height);
    ASSERT_EQ(reco.fps, ladder_b[2].fps);

    nanortc_destroy(&rtc);
}

#endif /* NANORTC_FEATURE_VIDEO_RATE_CONTROL */

TEST_MAIN_BEGIN("nanortc adaptive rate-control tests")
#if NANORTC_FEATURE_VIDEO_RATE_CONTROL
RUN(test_rate_control_null_and_invalid);
RUN(test_rate_control_no_feedback_holds);
RUN(test_rate_control_cold_start_selects_affordable);
RUN(test_rate_control_clamp_extremes);
RUN(test_rate_control_down_immediate_skips_rungs);
RUN(test_rate_control_up_needs_headroom);
RUN(test_rate_control_up_needs_hold_time);
RUN(test_rate_control_up_one_rung_per_hold);
RUN(test_rate_control_loss_forces_down);
RUN(test_rate_control_stable_no_flap);
RUN(test_rate_control_single_rung_ladder);
RUN(test_rate_control_set_capability_ladder_api);
RUN(test_rate_control_tick_emits_event);
RUN(test_rate_control_ladder_reinstall_reemits);
#endif
TEST_MAIN_END
