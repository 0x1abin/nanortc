/* RTP send-path vectors independent of the packetizer iterator.
 * SPDX-License-Identifier: MIT */
#include "nanortc.h"
#include "nano_test.h"
#include "nano_test_config.h"
#include <string.h>

#if NANORTC_FEATURE_VIDEO
static nanortc_t rtc;
static nano_srtp_t receiver;

static int init_video(nanortc_codec_t codec)
{
    memset(&rtc, 0, sizeof(rtc));
    rtc.config.crypto = nano_test_crypto();
    rtc.remote_addr.family = 4;
    rtc.remote_addr.port = 5000;
    uint8_t keys[2 * (NANORTC_SRTP_KEY_SIZE + NANORTC_SRTP_SALT_SIZE)] = {0};
    if (nano_srtp_init(&rtc.srtp, nano_test_crypto(), 1) != NANORTC_OK ||
        nano_srtp_derive_keys(&rtc.srtp, keys, sizeof(keys)) != NANORTC_OK ||
        nano_srtp_init(&receiver, nano_test_crypto(), 0) != NANORTC_OK ||
        nano_srtp_derive_keys(&receiver, keys, sizeof(keys)) != NANORTC_OK)
        return NANORTC_ERR_STATE;
    return nanortc_add_video_track(&rtc, NANORTC_DIR_SENDONLY, codec);
}

TEST(test_h264_lookahead_preserves_both_nals_and_last_marker)
{
    int mid = init_video(NANORTC_CODEC_H264);
    ASSERT_TRUE(mid >= 0);
    rtc.state = NANORTC_STATE_DTLS_CONNECTED;
    const uint8_t frame[] = {0, 0, 1, 0x67, 0xAA, 0, 0, 0, 1, 0x65, 0xBB, 0};
    ASSERT_OK(nanortc_send_video(&rtc, (uint8_t)mid, 33, frame, sizeof(frame)));
    rtc.now_ms += NANORTC_PACING_MAX_QUEUE_MS + 1;
    for (unsigned i = 0; i < 2; i++) {
        nanortc_output_t out;
        ASSERT_OK(nano_test_poll_output(&rtc, &out));
        ASSERT_EQ(out.type, NANORTC_OUTPUT_TRANSMIT);
        uint8_t packet[NANORTC_MEDIA_BUF_SIZE];
        memcpy(packet, out.transmit.data, out.transmit.len);
        size_t len;
        ASSERT_OK(nano_srtp_unprotect(&receiver, packet, out.transmit.len, &len));
        ASSERT_EQ(len, RTP_HEADER_SIZE + 2u);
        ASSERT_EQ(packet[1] >> 7, i);
        ASSERT_EQ(packet[RTP_HEADER_SIZE], i ? 0x65 : 0x67);
        ASSERT_EQ(packet[RTP_HEADER_SIZE + 1], i ? 0xBB : 0xAA);
    }
}

#if NANORTC_MAX_MEDIA_TRACKS >= 2
TEST(test_video_tracks_share_twcc_sequence_space)
{
    int first = init_video(NANORTC_CODEC_H264);
    int second = nanortc_add_video_track(&rtc, NANORTC_DIR_SENDONLY, NANORTC_CODEC_H264);
    ASSERT_TRUE(first >= 0 && second >= 0);
    rtc.media[0].rtp.twcc_ext_id = rtc.media[1].rtp.twcc_ext_id = 3;
    rtc.media[0].rtp.ssrc = 1;
    rtc.media[1].rtp.ssrc = 2;
    rtc.state = NANORTC_STATE_DTLS_CONNECTED;
    const uint8_t frame[] = {0, 0, 1, 0x65, 0xAA};
    ASSERT_OK(nanortc_send_video(&rtc, (uint8_t)first, 0, frame, sizeof(frame)));
    ASSERT_OK(nanortc_send_video(&rtc, (uint8_t)second, 0, frame, sizeof(frame)));
    rtc.now_ms += NANORTC_PACING_MAX_QUEUE_MS + 1;
    for (unsigned i = 0; i < 2; i++) {
        nanortc_output_t out;
        ASSERT_OK(nano_test_poll_output(&rtc, &out));
        ASSERT_EQ(nanortc_read_u16be(out.transmit.data + 17), i);
    }
}
#endif

#if NANORTC_FEATURE_H265
TEST(test_h265_excess_nals_fail_without_partial_frame)
{
    int mid = init_video(NANORTC_CODEC_H265);
    ASSERT_TRUE(mid >= 0);
    rtc.state = NANORTC_STATE_DTLS_CONNECTED;
    uint8_t frame[(NANORTC_MAX_NALS_PER_AU + 1) * 6];
    const uint8_t nal[] = {0, 0, 1, 0x26, 1, 0xAA};
    for (unsigned i = 0; i <= NANORTC_MAX_NALS_PER_AU; i++)
        memcpy(frame + i * sizeof(nal), nal, sizeof(nal));
    ASSERT_EQ(nanortc_send_video(&rtc, (uint8_t)mid, 0, frame, sizeof(frame)),
              NANORTC_ERR_BUFFER_TOO_SMALL);
    ASSERT_EQ(rtc.pkt_ring_tail, 0);
    ASSERT_EQ(rtc.out_tail, rtc.out_head);
}
#endif
#endif

TEST_MAIN_BEGIN("nanortc direct media packetization")
#if NANORTC_FEATURE_VIDEO
RUN(test_h264_lookahead_preserves_both_nals_and_last_marker);
#if NANORTC_MAX_MEDIA_TRACKS >= 2
RUN(test_video_tracks_share_twcc_sequence_space);
#endif
#if NANORTC_FEATURE_H265
RUN(test_h265_excess_nals_fail_without_partial_frame);
#endif
#endif
TEST_MAIN_END
