/* Session/output contracts; the test platform reports actual send outcomes. */
#include "nanortc.h"
#include "nanortc_crypto.h"
#include "nano_rtc_internal.h"
#include "nano_test.h"
#include "nano_test_config.h"
#include <string.h>

static nanortc_t rtc;
static void init_session(void)
{
    nanortc_config_t cfg = {0};
    cfg.crypto = nano_test_crypto();
    ASSERT_OK(nanortc_init(&rtc, &cfg));
}

TEST(test_transmit_completion_is_single_and_counted_once)
{
    init_session();
    const uint8_t packet[] = {1, 2, 3};
    const nanortc_addr_t peer = {.family = 4, .port = 5000};
    ASSERT_OK(nano_rtc_enqueue_transmit(&rtc, packet, sizeof(packet), &peer, false));
    ASSERT_OK(nano_rtc_enqueue_transmit(&rtc, packet, sizeof(packet), &peer, false));
    nanortc_output_t out;
    ASSERT_OK(nanortc_poll_output(&rtc, &out));
    uint64_t first = out.transmit.token;
    ASSERT_EQ(nanortc_poll_output(&rtc, &out), NANORTC_ERR_WOULD_BLOCK);
    nanortc_stats_t stats;
    ASSERT_OK(nanortc_get_stats(&rtc, &stats));
    ASSERT_EQ(stats.packets_sent, 0);
    ASSERT_TRUE(stats.transmit_pending);
    uint32_t wait = 0;
    ASSERT_OK(nanortc_next_timeout_ms(&rtc, 100, &wait));
    ASSERT_TRUE(wait > 0);
    ASSERT_OK(nanortc_handle_input(&rtc, &(nanortc_input_t){.now_ms = 100}));
    ASSERT_OK(nanortc_report_transmit(&rtc, first, NANORTC_TRANSMIT_SENT, 101));
    ASSERT_EQ(rtc.now_ms, 100);
    ASSERT_EQ(nanortc_report_transmit(&rtc, first, NANORTC_TRANSMIT_SENT, 101), NANORTC_ERR_INVALID_PARAM);
    ASSERT_OK(nanortc_poll_output(&rtc, &out));
    ASSERT_NEQ(out.transmit.token, first);
    ASSERT_OK(nanortc_report_transmit(&rtc, out.transmit.token, NANORTC_TRANSMIT_DROPPED, 102));
    ASSERT_OK(nanortc_get_stats(&rtc, &stats));
    ASSERT_EQ(stats.packets_sent, 1);
    ASSERT_EQ(stats.bytes_sent, sizeof(packet));
    ASSERT_EQ(stats.packets_dropped, 1);
    ASSERT_FALSE(stats.transmit_pending);
    nanortc_destroy(&rtc);
}

TEST(test_close_fresh_and_pending_is_terminal_and_idempotent)
{
    init_session();
    const uint8_t packet[] = {1};
    const nanortc_addr_t peer = {.family = 4};
    ASSERT_OK(nano_rtc_enqueue_transmit(&rtc, packet, sizeof(packet), &peer, false));
    nanortc_output_t out;
    ASSERT_OK(nanortc_poll_output(&rtc, &out));
    uint64_t token = out.transmit.token;
    nanortc_disconnect(&rtc);
    nanortc_disconnect(&rtc);
    ASSERT_FALSE(nanortc_is_alive(&rtc));
    ASSERT_EQ(nanortc_report_transmit(&rtc, token, NANORTC_TRANSMIT_SENT, 0), NANORTC_ERR_INVALID_PARAM);
    ASSERT_OK(nanortc_poll_output(&rtc, &out));
    ASSERT_EQ(out.type, NANORTC_OUTPUT_EVENT);
    ASSERT_EQ(out.event.type, NANORTC_EV_DISCONNECTED);
    ASSERT_EQ(out.event.disconnect_reason, NANORTC_DISCONNECT_LOCAL);
    ASSERT_EQ(nanortc_poll_output(&rtc, &out), NANORTC_ERR_NO_DATA);
    ASSERT_EQ(nanortc_handle_input(&rtc, &(nanortc_input_t){.now_ms = 999}), NANORTC_ERR_STATE);
    ASSERT_EQ(nanortc_ice_restart(&rtc), NANORTC_ERR_STATE);
    nanortc_destroy(&rtc);
    nanortc_destroy(&rtc);
}

TEST(test_queries_do_not_mutate_state)
{
    init_session();
    static nanortc_t before;
    before = rtc;
    nanortc_stats_t stats;
    uint32_t wait;
    ASSERT_OK(nanortc_get_stats(&rtc, &stats));
    ASSERT_OK(nanortc_next_timeout_ms(&rtc, 100, &wait));
    ASSERT_MEM_EQ(&before, &rtc, sizeof(rtc));
    nanortc_destroy(&rtc);
}

TEST_MAIN_BEGIN("Session contracts")
RUN(test_transmit_completion_is_single_and_counted_once);
RUN(test_close_fresh_and_pending_is_terminal_and_idempotent);
RUN(test_queries_do_not_mutate_state);
TEST_MAIN_END
