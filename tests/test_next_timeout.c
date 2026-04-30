/*
 * nanortc — nanortc_next_timeout_ms() aggregator tests
 *
 * Validates that the public deadline-aggregator API picks up each
 * subsystem's armed timer and clamps as the contract promises:
 *
 *   - Idle / nothing armed         → returns the conservative idle cap.
 *   - DTLS handshake in progress   → result is capped to MIN_POLL.
 *   - ICE check pacing             → result tracks ice.next_check_ms.
 *   - ICE consent freshness        → result tracks consent_next_ms / expiry.
 *   - STUN srflx retry             → result tracks stun_retry_at_ms.
 *   - TURN refresh                 → result tracks turn.refresh_at_ms.
 *   - Multiple subsystems          → result is the min over all sources.
 *
 * SCTP and RTCP cadences are exercised indirectly: the SCTP path needs
 * a fully established association which is too heavy for a unit test.
 * The aggregator's contract for those is identical to the others (read
 * `*_next_timeout_ms()`, take the min) and is covered by the structural
 * tests below — adding plumbing-only test cases would be redundant.
 *
 * SPDX-License-Identifier: MIT
 */

#include "nanortc.h"
#include "nano_test.h"
#include <string.h>

#if NANORTC_FEATURE_TURN
#include "nano_stun.h" /* STUN_FAMILY_IPV4 */
#endif

/* Static instance — nanortc_t is large enough that some CI runners trip
 * stack-overflow on locals. The tests reset it via memset() up front. */
static nanortc_t g_rtc;

/* Default idle cap when nothing is armed (mirrors nano_rtc.c). */
#define IDLE_CAP_MS 1000u

TEST(test_next_timeout_idle_returns_cap)
{
    nanortc_t *rtc = &g_rtc;
    memset(rtc, 0, sizeof(*rtc));

    uint32_t out = 0xDEADBEEFu;
    ASSERT_OK(nanortc_next_timeout_ms(rtc, 1000, &out));
    ASSERT_EQ(out, IDLE_CAP_MS);
}

TEST(test_next_timeout_null_args_reject)
{
    nanortc_t *rtc = &g_rtc;
    memset(rtc, 0, sizeof(*rtc));
    uint32_t out = 0;
    ASSERT_FAIL(nanortc_next_timeout_ms(NULL, 0, &out));
    ASSERT_FAIL(nanortc_next_timeout_ms(rtc, 0, NULL));
}

TEST(test_next_timeout_dtls_handshake_caps_at_min_poll)
{
    nanortc_t *rtc = &g_rtc;
    memset(rtc, 0, sizeof(*rtc));
    rtc->state = NANORTC_STATE_DTLS_HANDSHAKING;

    uint32_t out = 0;
    ASSERT_OK(nanortc_next_timeout_ms(rtc, 0, &out));
    ASSERT_EQ(out, NANORTC_MIN_POLL_INTERVAL_MS);
}

TEST(test_next_timeout_ice_controlling_uses_next_check_ms)
{
    nanortc_t *rtc = &g_rtc;
    memset(rtc, 0, sizeof(*rtc));

    rtc->ice.is_controlling = true;
    rtc->ice.state = NANORTC_ICE_STATE_CHECKING;
    rtc->ice.next_check_ms = 1500;

    uint32_t out = 0;
    ASSERT_OK(nanortc_next_timeout_ms(rtc, 1000, &out));
    /* 1500 - 1000 = 500ms left until the next ICE check fires. */
    ASSERT_EQ(out, 500u);
}

TEST(test_next_timeout_ice_check_already_due_returns_zero)
{
    nanortc_t *rtc = &g_rtc;
    memset(rtc, 0, sizeof(*rtc));

    rtc->ice.is_controlling = true;
    rtc->ice.state = NANORTC_ICE_STATE_CHECKING;
    rtc->ice.next_check_ms = 1000;

    uint32_t out = 99;
    ASSERT_OK(nanortc_next_timeout_ms(rtc, 1500, &out));
    ASSERT_EQ(out, 0u);
}

TEST(test_next_timeout_ice_consent_picks_smaller_of_send_and_expiry)
{
    nanortc_t *rtc = &g_rtc;
    memset(rtc, 0, sizeof(*rtc));

    /* Connected: consent_next_ms (send keepalive) + consent_expiry_ms
     * (path-dead deadline). Both armed; aggregator must pick the
     * smaller distance. */
    rtc->ice.state = NANORTC_ICE_STATE_CONNECTED;
    rtc->ice.consent_next_ms = 5000;   /* send next consent in 1s */
    rtc->ice.consent_expiry_ms = 4500; /* but expiry is in 0.5s */

    uint32_t out = 0;
    ASSERT_OK(nanortc_next_timeout_ms(rtc, 4000, &out));
    ASSERT_EQ(out, 500u); /* expiry wins */
}

TEST(test_next_timeout_stun_srflx_retry)
{
    nanortc_t *rtc = &g_rtc;
    memset(rtc, 0, sizeof(*rtc));

    rtc->stun_server_configured = true;
    rtc->srflx_discovered = false;
    rtc->stun_retries = 1; /* < 3, still retrying */
    rtc->stun_retry_at_ms = 2200;

    uint32_t out = 0;
    ASSERT_OK(nanortc_next_timeout_ms(rtc, 2000, &out));
    ASSERT_EQ(out, 200u);
}

TEST(test_next_timeout_stun_srflx_first_attempt_immediate)
{
    nanortc_t *rtc = &g_rtc;
    memset(rtc, 0, sizeof(*rtc));

    /* stun_retry_at_ms == 0 + retries == 0 means "first send pending" —
     * fire on the next tick. */
    rtc->stun_server_configured = true;
    rtc->srflx_discovered = false;
    rtc->stun_retries = 0;
    rtc->stun_retry_at_ms = 0;

    uint32_t out = 99;
    ASSERT_OK(nanortc_next_timeout_ms(rtc, 1000, &out));
    ASSERT_EQ(out, 0u);
}

TEST(test_next_timeout_stun_srflx_done_no_retry)
{
    nanortc_t *rtc = &g_rtc;
    memset(rtc, 0, sizeof(*rtc));

    rtc->stun_server_configured = true;
    rtc->srflx_discovered = true; /* already done */
    rtc->stun_retry_at_ms = 0;

    uint32_t out = 0;
    ASSERT_OK(nanortc_next_timeout_ms(rtc, 1000, &out));
    /* No subsystem armed → idle cap. */
    ASSERT_EQ(out, IDLE_CAP_MS);
}

#if NANORTC_FEATURE_TURN

TEST(test_next_timeout_turn_idle_state_returns_zero)
{
    nanortc_t *rtc = &g_rtc;
    memset(rtc, 0, sizeof(*rtc));
    rtc->turn.configured = true;
    rtc->turn.state = NANORTC_TURN_IDLE;

    uint32_t out = 99;
    ASSERT_OK(nanortc_next_timeout_ms(rtc, 0, &out));
    ASSERT_EQ(out, 0u);
}

TEST(test_next_timeout_turn_allocated_picks_min_refresh)
{
    nanortc_t *rtc = &g_rtc;
    memset(rtc, 0, sizeof(*rtc));

    rtc->turn.configured = true;
    rtc->turn.state = NANORTC_TURN_ALLOCATED;
    rtc->turn.server_family = STUN_FAMILY_IPV4;
    rtc->turn.refresh_at_ms = 60000; /* 60s out */
    rtc->turn.permission_count = 1;
    rtc->turn.permission_at_ms = 30000; /* 30s out — wins */

    uint32_t out = 0;
    ASSERT_OK(nanortc_next_timeout_ms(rtc, 0, &out));
    ASSERT_EQ(out, 30000u);
}

TEST(test_next_timeout_turn_min_across_subsystems)
{
    nanortc_t *rtc = &g_rtc;
    memset(rtc, 0, sizeof(*rtc));

    /* TURN refresh in 50s, ICE consent expiry in 10s — ICE wins. */
    rtc->ice.state = NANORTC_ICE_STATE_CONNECTED;
    rtc->ice.consent_next_ms = 30000;
    rtc->ice.consent_expiry_ms = 10000;

    rtc->turn.configured = true;
    rtc->turn.state = NANORTC_TURN_ALLOCATED;
    rtc->turn.server_family = STUN_FAMILY_IPV4;
    rtc->turn.refresh_at_ms = 50000;

    uint32_t out = 0;
    ASSERT_OK(nanortc_next_timeout_ms(rtc, 0, &out));
    ASSERT_EQ(out, 10000u);
}

#endif /* NANORTC_FEATURE_TURN */

#if NANORTC_HAVE_MEDIA_TRANSPORT

TEST(test_next_timeout_rtcp_period_after_srtp_ready)
{
    nanortc_t *rtc = &g_rtc;
    memset(rtc, 0, sizeof(*rtc));

    rtc->srtp.ready = true;
    rtc->last_rtcp_send_ms = 0;

    /* now=2000 → since last SR=2000, RTCP period default 5000 →
     * 3000 ms left. */
    uint32_t out = 0;
    ASSERT_OK(nanortc_next_timeout_ms(rtc, 2000, &out));
    ASSERT_EQ(out, NANORTC_RTCP_INTERVAL_MS - 2000u);
}

TEST(test_next_timeout_rtcp_overdue_returns_zero)
{
    nanortc_t *rtc = &g_rtc;
    memset(rtc, 0, sizeof(*rtc));

    rtc->srtp.ready = true;
    rtc->last_rtcp_send_ms = 0;

    /* now=10000 → since=10000 > RTCP period → fire immediately. */
    uint32_t out = 99;
    ASSERT_OK(nanortc_next_timeout_ms(rtc, 10000, &out));
    ASSERT_EQ(out, 0u);
}

#endif /* NANORTC_HAVE_MEDIA_TRANSPORT */

TEST_MAIN_BEGIN("test_next_timeout")
RUN(test_next_timeout_idle_returns_cap);
RUN(test_next_timeout_null_args_reject);
RUN(test_next_timeout_dtls_handshake_caps_at_min_poll);
RUN(test_next_timeout_ice_controlling_uses_next_check_ms);
RUN(test_next_timeout_ice_check_already_due_returns_zero);
RUN(test_next_timeout_ice_consent_picks_smaller_of_send_and_expiry);
RUN(test_next_timeout_stun_srflx_retry);
RUN(test_next_timeout_stun_srflx_first_attempt_immediate);
RUN(test_next_timeout_stun_srflx_done_no_retry);
#if NANORTC_FEATURE_TURN
RUN(test_next_timeout_turn_idle_state_returns_zero);
RUN(test_next_timeout_turn_allocated_picks_min_refresh);
RUN(test_next_timeout_turn_min_across_subsystems);
#endif
#if NANORTC_HAVE_MEDIA_TRANSPORT
RUN(test_next_timeout_rtcp_period_after_srtp_ready);
RUN(test_next_timeout_rtcp_overdue_returns_zero);
#endif
TEST_MAIN_END
