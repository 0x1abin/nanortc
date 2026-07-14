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
 * Pending DTLS/SCTP/DCEP output is also covered: retained protocol work wakes
 * immediately once it can be pumped, while a pre-association DCEP OPEN does
 * not spin the event loop before SCTP is established.
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

#if NANORTC_FEATURE_TURN
static void setup_turn_permission_candidate(nanortc_t *rtc)
{
    memset(rtc, 0, sizeof(*rtc));
    rtc->turn.configured = true;
    rtc->turn.state = NANORTC_TURN_ALLOCATED;
    rtc->turn.relay_family = STUN_FAMILY_IPV4;
    rtc->ice.remote_candidate_count = 1;
    rtc->ice.remote_candidates[0].family = 4;
    rtc->ice.remote_candidates[0].addr[0] = 192;
    rtc->ice.remote_candidates[0].addr[2] = 2;
    rtc->ice.remote_candidates[0].addr[3] = 10;
    rtc->ice.remote_candidates[0].port = 40000;
}

static void add_matching_permission(nanortc_t *rtc)
{
    rtc->turn.permission_count = 1;
    rtc->turn.permissions[0].family = rtc->ice.remote_candidates[0].family;
    memcpy(rtc->turn.permissions[0].addr, rtc->ice.remote_candidates[0].addr, NANORTC_ADDR_SIZE);
    rtc->turn.permissions[0].port = rtc->ice.remote_candidates[0].port;
}
#endif

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

#if NANORTC_FEATURE_DATACHANNEL
TEST(test_next_timeout_pending_dtls_and_sctp_output_are_immediate)
{
    nanortc_t *rtc = &g_rtc;
    memset(rtc, 0, sizeof(*rtc));
    rtc->ice.state = NANORTC_ICE_STATE_FAILED;

    rtc->dtls.out_len = 1u;
    uint32_t out = 99u;
    ASSERT_OK(nanortc_next_timeout_ms(rtc, 1000u, &out));
    ASSERT_EQ(out, 0u);

    rtc->dtls.out_len = 0u;
    rtc->sctp.state = NANORTC_SCTP_STATE_COOKIE_WAIT;
    rtc->sctp.out_tail = 1u;
    out = 99u;
    ASSERT_OK(nanortc_next_timeout_ms(rtc, 1000u, &out));
    ASSERT_EQ(out, 0u);
}

TEST(test_next_timeout_dcep_waits_for_sctp_established)
{
    nanortc_t *rtc = &g_rtc;
    memset(rtc, 0, sizeof(*rtc));
    rtc->ice.state = NANORTC_ICE_STATE_FAILED;
    rtc->datachannel.has_output = true;
    rtc->datachannel.out_len = 1u;

    uint32_t out = 0u;
    ASSERT_OK(nanortc_next_timeout_ms(rtc, 1000u, &out));
    ASSERT_EQ(out, IDLE_CAP_MS);

    rtc->sctp.state = NANORTC_SCTP_STATE_ESTABLISHED;
    out = 99u;
    ASSERT_OK(nanortc_next_timeout_ms(rtc, 1000u, &out));
    ASSERT_EQ(out, 0u);
}
#endif

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

TEST(test_next_timeout_ice_check_future_across_u32_wrap)
{
    nanortc_t *rtc = &g_rtc;
    memset(rtc, 0, sizeof(*rtc));

    rtc->ice.is_controlling = true;
    rtc->ice.state = NANORTC_ICE_STATE_CHECKING;
    rtc->ice.next_check_ms = 50;

    /* From UINT32_MAX-49 to 50 is exactly 100 ms modulo 2^32. */
    uint32_t out = 0;
    ASSERT_OK(nanortc_next_timeout_ms(rtc, UINT32_MAX - 49u, &out));
    ASSERT_EQ(out, 100u);
}

TEST(test_next_timeout_ice_check_overdue_across_u32_wrap)
{
    nanortc_t *rtc = &g_rtc;
    memset(rtc, 0, sizeof(*rtc));

    rtc->ice.is_controlling = true;
    rtc->ice.state = NANORTC_ICE_STATE_CHECKING;
    rtc->ice.next_check_ms = UINT32_MAX - 20u;

    uint32_t out = 99;
    ASSERT_OK(nanortc_next_timeout_ms(rtc, 10, &out));
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

TEST(test_next_timeout_ice_consent_future_across_u32_wrap)
{
    nanortc_t *rtc = &g_rtc;
    memset(rtc, 0, sizeof(*rtc));

    rtc->ice.state = NANORTC_ICE_STATE_CONNECTED;
    rtc->ice.consent_next_ms = 50;    /* 150 ms after now, across wrap */
    rtc->ice.consent_expiry_ms = 100; /* 200 ms after now */

    uint32_t out = 0;
    ASSERT_OK(nanortc_next_timeout_ms(rtc, UINT32_MAX - 99u, &out));
    ASSERT_EQ(out, 150u);
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

TEST(test_next_timeout_stun_srflx_future_across_u32_wrap)
{
    nanortc_t *rtc = &g_rtc;
    memset(rtc, 0, sizeof(*rtc));

    rtc->stun_server_configured = true;
    rtc->srflx_discovered = false;
    rtc->stun_retries = 1;
    rtc->stun_retry_at_ms = 25;

    uint32_t out = 0;
    ASSERT_OK(nanortc_next_timeout_ms(rtc, UINT32_MAX - 24u, &out));
    ASSERT_EQ(out, 50u);
}

TEST(test_next_timeout_stun_srflx_overdue_across_u32_wrap)
{
    nanortc_t *rtc = &g_rtc;
    memset(rtc, 0, sizeof(*rtc));

    rtc->stun_server_configured = true;
    rtc->srflx_discovered = false;
    rtc->stun_retries = 1;
    rtc->stun_retry_at_ms = UINT32_MAX - 24u;

    uint32_t out = 99;
    ASSERT_OK(nanortc_next_timeout_ms(rtc, 25, &out));
    ASSERT_EQ(out, 0u);
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
    rtc->turn.permissions[0].active = true;
    rtc->turn.permissions[0].deadline_ms = 30000; /* 30s out — wins */

    uint32_t out = 0;
    ASSERT_OK(nanortc_next_timeout_ms(rtc, 0, &out));
    ASSERT_EQ(out, 30000u);
}

TEST(test_next_timeout_turn_new_permission_is_immediate)
{
    nanortc_t *rtc = &g_rtc;
    setup_turn_permission_candidate(rtc);

    uint32_t out = 99u;
    ASSERT_OK(nanortc_next_timeout_ms(rtc, 1000u, &out));
    ASSERT_EQ(out, 0u);
}

TEST(test_next_timeout_turn_permission_retry_waits_until_due)
{
    nanortc_t *rtc = &g_rtc;
    setup_turn_permission_candidate(rtc);
    add_matching_permission(rtc);
    rtc->turn.permissions[0].deadline_ms = 1200u;

    uint32_t out = 0u;
    ASSERT_OK(nanortc_next_timeout_ms(rtc, 1000u, &out));
    ASSERT_EQ(out, 200u);

    ASSERT_OK(nanortc_next_timeout_ms(rtc, 1200u, &out));
    ASSERT_EQ(out, 0u);
}

TEST(test_next_timeout_turn_ineligible_permissions_do_not_spin)
{
    nanortc_t *rtc = &g_rtc;
    uint32_t out = 0u;

    setup_turn_permission_candidate(rtc);
    rtc->ice.remote_candidates[0].family = 6;
    ASSERT_OK(nanortc_next_timeout_ms(rtc, 1000u, &out));
    ASSERT_EQ(out, IDLE_CAP_MS);

    setup_turn_permission_candidate(rtc);
    add_matching_permission(rtc);
    rtc->turn.permissions[0].terminal = true;
    ASSERT_OK(nanortc_next_timeout_ms(rtc, 1000u, &out));
    ASSERT_EQ(out, IDLE_CAP_MS);

    setup_turn_permission_candidate(rtc);
    add_matching_permission(rtc);
    rtc->turn.permissions[0].pending = true;
    rtc->turn.permissions[0].deadline_ms = 1500u;
    ASSERT_OK(nanortc_next_timeout_ms(rtc, 1000u, &out));
    ASSERT_EQ(out, 500u);

    setup_turn_permission_candidate(rtc);
    rtc->turn.permission_count = NANORTC_TURN_MAX_PERMISSIONS;
    for (uint16_t i = 0; i < NANORTC_TURN_MAX_PERMISSIONS; i++) {
        rtc->turn.permissions[i].family = 4;
        rtc->turn.permissions[i].addr[0] = 198;
        rtc->turn.permissions[i].addr[1] = 51;
        rtc->turn.permissions[i].addr[2] = 100;
        rtc->turn.permissions[i].addr[3] = (uint8_t)(i + 1u);
        rtc->turn.permissions[i].active = true;
        rtc->turn.permissions[i].deadline_ms = 5000u;
    }
    ASSERT_OK(nanortc_next_timeout_ms(rtc, 1000u, &out));
    ASSERT_EQ(out, 4000u);
}

TEST(test_next_timeout_turn_refresh_future_across_u32_wrap)
{
    nanortc_t *rtc = &g_rtc;
    memset(rtc, 0, sizeof(*rtc));

    rtc->turn.configured = true;
    rtc->turn.state = NANORTC_TURN_ALLOCATED;
    rtc->turn.refresh_at_ms = 20;

    uint32_t out = 0;
    ASSERT_OK(nanortc_next_timeout_ms(rtc, UINT32_MAX - 9u, &out));
    ASSERT_EQ(out, 30u);
}

TEST(test_next_timeout_turn_refresh_overdue_across_u32_wrap)
{
    nanortc_t *rtc = &g_rtc;
    memset(rtc, 0, sizeof(*rtc));

    rtc->turn.configured = true;
    rtc->turn.state = NANORTC_TURN_ALLOCATED;
    rtc->turn.refresh_at_ms = UINT32_MAX - 9u;

    uint32_t out = 99;
    ASSERT_OK(nanortc_next_timeout_ms(rtc, 20, &out));
    ASSERT_EQ(out, 0u);
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
    rtc->last_rtcp_send_valid = true;

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
    rtc->last_rtcp_send_valid = true;

    /* now=10000 → since=10000 > RTCP period → fire immediately. */
    uint32_t out = 99;
    ASSERT_OK(nanortc_next_timeout_ms(rtc, 10000, &out));
    ASSERT_EQ(out, 0u);
}

TEST(test_next_timeout_rtcp_first_sr_is_immediate_when_timestamp_invalid)
{
    nanortc_t *rtc = &g_rtc;
    memset(rtc, 0, sizeof(*rtc));

    rtc->srtp.ready = true;
    rtc->last_rtcp_send_ms = 1234; /* stale storage must be ignored */
    rtc->last_rtcp_send_valid = false;

    uint32_t out = 99;
    ASSERT_OK(nanortc_next_timeout_ms(rtc, 2000, &out));
    ASSERT_EQ(out, 0u);
}

TEST(test_next_timeout_rtcp_period_across_u32_wrap)
{
    nanortc_t *rtc = &g_rtc;
    memset(rtc, 0, sizeof(*rtc));

    rtc->srtp.ready = true;
    rtc->last_rtcp_send_ms = UINT32_MAX - 1999u;
    rtc->last_rtcp_send_valid = true;

    /* 3000 ms elapsed modulo 2^32, leaving the rest of the SR period. */
    uint32_t out = 0;
    ASSERT_OK(nanortc_next_timeout_ms(rtc, 1000, &out));
    ASSERT_EQ(out, NANORTC_RTCP_INTERVAL_MS - 3000u);
}

#if NANORTC_FEATURE_VIDEO && NANORTC_FEATURE_VIDEO_PACING

TEST(test_next_timeout_pacer_future_across_u32_wrap)
{
    nanortc_t *rtc = &g_rtc;
    memset(rtc, 0, sizeof(*rtc));

    rtc->pacer.head = 0;
    rtc->pacer.tail = 1;
    rtc->pacer.next_release_ms = 25;

    uint32_t out = 0;
    ASSERT_OK(nanortc_next_timeout_ms(rtc, UINT32_MAX - 24u, &out));
    ASSERT_EQ(out, 50u);
}

TEST(test_next_timeout_pacer_overdue_across_u32_wrap)
{
    nanortc_t *rtc = &g_rtc;
    memset(rtc, 0, sizeof(*rtc));

    rtc->pacer.head = 0;
    rtc->pacer.tail = 1;
    rtc->pacer.next_release_ms = UINT32_MAX - 24u;

    uint32_t out = 99;
    ASSERT_OK(nanortc_next_timeout_ms(rtc, 25, &out));
    ASSERT_EQ(out, 0u);
}

#endif /* NANORTC_FEATURE_VIDEO && NANORTC_FEATURE_VIDEO_PACING */

#endif /* NANORTC_HAVE_MEDIA_TRANSPORT */

TEST_MAIN_BEGIN("test_next_timeout")
RUN(test_next_timeout_idle_returns_cap);
RUN(test_next_timeout_null_args_reject);
RUN(test_next_timeout_dtls_handshake_caps_at_min_poll);
#if NANORTC_FEATURE_DATACHANNEL
RUN(test_next_timeout_pending_dtls_and_sctp_output_are_immediate);
RUN(test_next_timeout_dcep_waits_for_sctp_established);
#endif
RUN(test_next_timeout_ice_controlling_uses_next_check_ms);
RUN(test_next_timeout_ice_check_already_due_returns_zero);
RUN(test_next_timeout_ice_check_future_across_u32_wrap);
RUN(test_next_timeout_ice_check_overdue_across_u32_wrap);
RUN(test_next_timeout_ice_consent_picks_smaller_of_send_and_expiry);
RUN(test_next_timeout_ice_consent_future_across_u32_wrap);
RUN(test_next_timeout_stun_srflx_retry);
RUN(test_next_timeout_stun_srflx_future_across_u32_wrap);
RUN(test_next_timeout_stun_srflx_overdue_across_u32_wrap);
RUN(test_next_timeout_stun_srflx_first_attempt_immediate);
RUN(test_next_timeout_stun_srflx_done_no_retry);
#if NANORTC_FEATURE_TURN
RUN(test_next_timeout_turn_idle_state_returns_zero);
RUN(test_next_timeout_turn_allocated_picks_min_refresh);
RUN(test_next_timeout_turn_new_permission_is_immediate);
RUN(test_next_timeout_turn_permission_retry_waits_until_due);
RUN(test_next_timeout_turn_ineligible_permissions_do_not_spin);
RUN(test_next_timeout_turn_refresh_future_across_u32_wrap);
RUN(test_next_timeout_turn_refresh_overdue_across_u32_wrap);
RUN(test_next_timeout_turn_min_across_subsystems);
#endif
#if NANORTC_HAVE_MEDIA_TRANSPORT
RUN(test_next_timeout_rtcp_period_after_srtp_ready);
RUN(test_next_timeout_rtcp_overdue_returns_zero);
RUN(test_next_timeout_rtcp_first_sr_is_immediate_when_timestamp_invalid);
RUN(test_next_timeout_rtcp_period_across_u32_wrap);
#if NANORTC_FEATURE_VIDEO && NANORTC_FEATURE_VIDEO_PACING
RUN(test_next_timeout_pacer_future_across_u32_wrap);
RUN(test_next_timeout_pacer_overdue_across_u32_wrap);
#endif
#endif
TEST_MAIN_END
