/* A closed session cannot recreate transport work.
 * SPDX-License-Identifier: MIT */
#include "nanortc.h"
#include "nano_rtc_internal.h"
#include "nano_test.h"
#include "nano_test_config.h"
#include <string.h>

static nanortc_t rtc;

static void expect_disconnect_only(void)
{
    nanortc_output_t out;
    ASSERT_OK(nanortc_poll_output(&rtc, &out));
    ASSERT_EQ(out.type, NANORTC_OUTPUT_EVENT);
    ASSERT_EQ(out.event.type, NANORTC_EV_DISCONNECTED);
    ASSERT_EQ(out.event.disconnect_reason, NANORTC_DISCONNECT_LOCAL);
    ASSERT_EQ(nanortc_poll_output(&rtc, &out), NANORTC_ERR_NO_DATA);
}

TEST(test_closed_session_rejects_transport_admission)
{
    memset(&rtc, 0, sizeof(rtc));
    nanortc_disconnect(&rtc);
    const uint8_t packet[] = {1};
    const nanortc_addr_t destination = {.family = 4, .port = 5000};
    ASSERT_EQ(nano_rtc_enqueue_transmit(&rtc, packet, sizeof(packet), &destination, false),
              NANORTC_ERR_STATE);
    expect_disconnect_only();
}

TEST(test_close_invalidates_pending_transmit_and_drops_queued_work)
{
    memset(&rtc, 0, sizeof(rtc));
    const uint8_t packet[] = {1};
    const nanortc_addr_t destination = {.family = 4, .port = 5000};
    ASSERT_OK(nano_rtc_enqueue_transmit(&rtc, packet, sizeof(packet), &destination, false));
    nanortc_output_t out;
    ASSERT_OK(nanortc_poll_output(&rtc, &out));
    uint64_t token = out.transmit.token;
    ASSERT_TRUE(rtc.transmit_pending);
    ASSERT_OK(nano_rtc_enqueue_transmit(&rtc, packet, sizeof(packet), &destination, false));
    nanortc_disconnect(&rtc);
    ASSERT_FALSE(rtc.transmit_pending);
    ASSERT_EQ(nanortc_report_transmit(&rtc, token, NANORTC_TRANSMIT_SENT, 100),
              NANORTC_ERR_INVALID_PARAM);
    expect_disconnect_only();
}

#if NANORTC_FEATURE_DATACHANNEL
TEST(test_closed_session_cannot_create_datachannel)
{
    memset(&rtc, 0, sizeof(rtc));
    ASSERT_TRUE(nanortc_create_datachannel(&rtc, "before", NULL) >= 0);
    nanortc_disconnect(&rtc);
    uint8_t count = rtc.datachannel.channel_count;
    uint8_t mids = rtc.sdp.mid_count;
    ASSERT_EQ(nanortc_create_datachannel(&rtc, "after", NULL), NANORTC_ERR_STATE);
    ASSERT_EQ(rtc.datachannel.channel_count, count);
    ASSERT_EQ(rtc.sdp.mid_count, mids);
    expect_disconnect_only();
}
#endif

#if NANORTC_HAVE_MEDIA_TRANSPORT
static void init_media(void)
{
    memset(&rtc, 0, sizeof(rtc));
    rtc.config.crypto = nano_test_crypto();
    uint8_t keys[2 * (NANORTC_SRTP_KEY_SIZE + NANORTC_SRTP_SALT_SIZE)] = {0};
    ASSERT_OK(nano_srtp_init(&rtc.srtp, nano_test_crypto(), 1));
    ASSERT_OK(nano_srtp_derive_keys(&rtc.srtp, keys, sizeof(keys)));
}
#endif

#if NANORTC_FEATURE_AUDIO
TEST(test_closed_session_cannot_send_audio)
{
    init_media();
    int mid = nanortc_add_audio_track(&rtc, NANORTC_DIR_SENDONLY, NANORTC_CODEC_OPUS, 48000, 1);
    ASSERT_TRUE(mid >= 0);
    rtc.state = NANORTC_STATE_DTLS_CONNECTED;
    nanortc_disconnect(&rtc);
    const uint8_t packet[] = {0xF8, 0xFF, 0xFE};
    ASSERT_EQ(nanortc_send_audio(&rtc, (uint8_t)mid, 0, packet, sizeof(packet)), NANORTC_ERR_STATE);
    ASSERT_EQ(rtc.media[0].rtp.seq, 0);
    expect_disconnect_only();
}
#endif

#if NANORTC_FEATURE_VIDEO
TEST(test_closed_session_cannot_send_video_or_request_keyframe)
{
    init_media();
    int mid = nanortc_add_video_track(&rtc, NANORTC_DIR_SENDRECV, NANORTC_CODEC_H264);
    ASSERT_TRUE(mid >= 0);
    rtc.state = NANORTC_STATE_DTLS_CONNECTED;
    nanortc_disconnect(&rtc);
    const uint8_t packet[] = {0, 0, 1, 0x65, 0xAA};
    ASSERT_EQ(nanortc_send_video(&rtc, (uint8_t)mid, 0, packet, sizeof(packet)), NANORTC_ERR_STATE);
    ASSERT_EQ(nanortc_request_keyframe(&rtc, (uint8_t)mid), NANORTC_ERR_STATE);
    ASSERT_EQ(rtc.pkt_ring_tail, 0);
    expect_disconnect_only();
}
#endif

TEST_MAIN_BEGIN("nanortc closed-session boundaries")
RUN(test_closed_session_rejects_transport_admission);
RUN(test_close_invalidates_pending_transmit_and_drops_queued_work);
#if NANORTC_FEATURE_DATACHANNEL
RUN(test_closed_session_cannot_create_datachannel);
#endif
#if NANORTC_FEATURE_AUDIO
RUN(test_closed_session_cannot_send_audio);
#endif
#if NANORTC_FEATURE_VIDEO
RUN(test_closed_session_cannot_send_video_or_request_keyframe);
#endif
TEST_MAIN_END
