/* Actual egress accounting and transport-wide feedback regressions.
 * SPDX-License-Identifier: MIT */
#include "nanortc.h"
#include "nano_rtc_internal.h"
#include "nano_test.h"
#include "nano_test_config.h"
#include <string.h>

#if NANORTC_FEATURE_VIDEO
#define TEST_FEEDBACK_BYTES 24
static nanortc_t rtc;
static nano_srtp_t peer;

static void init_sender(void)
{
    memset(&rtc, 0, sizeof(rtc));
    rtc.config.crypto = nano_test_crypto();
    rtc.remote_addr.family = 4;
    rtc.remote_addr.port = 5000;
    uint8_t keys[2 * (NANORTC_SRTP_KEY_SIZE + NANORTC_SRTP_SALT_SIZE)] = {0};
    ASSERT_OK(nano_srtp_init(&rtc.srtp, nano_test_crypto(), 1));
    ASSERT_OK(nano_srtp_derive_keys(&rtc.srtp, keys, sizeof(keys)));
    ASSERT_OK(nano_srtp_init(&peer, nano_test_crypto(), 0));
    ASSERT_OK(nano_srtp_derive_keys(&peer, keys, sizeof(keys)));
    ASSERT_OK(bwe_init(&rtc.bwe));
#if NANORTC_FEATURE_VIDEO_PACING
    rtc.pacer.budget_bytes = NANORTC_PACING_MAX_BURST_BYTES;
#endif
}

static void send_frame(uint8_t mid, nanortc_output_t *out)
{
    const uint8_t frame[] = {0, 0, 1, 0x65, 0xAA};
    ASSERT_OK(nanortc_send_video(&rtc, mid, rtc.now_ms, frame, sizeof(frame)));
    ASSERT_OK(nanortc_poll_output(&rtc, out));
    ASSERT_EQ(out->type, NANORTC_OUTPUT_TRANSMIT);
}

/* draft-holmer-rmcat-transport-wide-cc-extensions §3.1: RTPFB FMT=15,
 * one run-length chunk containing a received packet with a small delta.
 * Reference time is in 64 ms units; receive deltas are in 250 us units. */
static void receive_feedback(uint16_t sequence, uint32_t arrival_ms, uint32_t now_ms)
{
    uint8_t feedback[TEST_FEEDBACK_BYTES] = {
        0x8F, 205, 0, 5, 0, 0, 0, 9, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0x20, 1, 0, 0,
    };
    nanortc_write_u16be(feedback + 12, sequence);
    uint32_t reference = arrival_ms / 64;
    feedback[16] = (uint8_t)(reference >> 16);
    feedback[17] = (uint8_t)(reference >> 8);
    feedback[18] = (uint8_t)reference;
    feedback[22] = (uint8_t)((arrival_ms % 64) * 4);
    ASSERT_OK(bwe_on_twcc_feedback(&rtc.bwe, feedback, sizeof(feedback), now_ms));
}

#if NANORTC_MAX_MEDIA_TRACKS >= 2
TEST(test_tracks_count_only_reported_sends_and_share_wrapping_twcc)
{
    init_sender();
#if NANORTC_FEATURE_DATACHANNEL
    /* An application m-line occupies a MID, but no media array slot. */
    rtc.sdp.dc_mid = 0;
    rtc.sdp.mid_count = 1;
#endif
    int first = nanortc_add_video_track(&rtc, NANORTC_DIR_SENDONLY, NANORTC_CODEC_H264);
    int second = nanortc_add_video_track(&rtc, NANORTC_DIR_SENDONLY, NANORTC_CODEC_H264);
    ASSERT_TRUE(first >= 0 && second >= 0);
#if NANORTC_FEATURE_DATACHANNEL
    ASSERT_EQ(first, 1);
#endif
    rtc.media[0].rtp.twcc_ext_id = rtc.media[1].rtp.twcc_ext_id = 3;
    rtc.media[0].rtp.ssrc = 1;
    rtc.media[1].rtp.ssrc = 2;
    rtc.twcc_sequence = UINT16_MAX;
    rtc.state = NANORTC_STATE_DTLS_CONNECTED;
    rtc.now_ms = UINT32_MAX - 20;
    nanortc_output_t out;
    send_frame((uint8_t)first, &out);
    ASSERT_EQ(nanortc_read_u16be(out.transmit.data + 17), UINT16_MAX);
    ASSERT_EQ(rtc.media[0].rtcp.packets_sent, 0);
    nanortc_track_stats_t admission;
    ASSERT_OK(nanortc_get_track_stats(&rtc, (uint8_t)first, &admission));
    ASSERT_EQ(admission.packets_accepted, 1);
    ASSERT_EQ(admission.frames_accepted, 1);
    ASSERT_EQ(admission.packets_sent, 0);
    ASSERT_EQ(rtc.stats_packets_sent, 0);
    ASSERT_EQ(rtc.bwe.sent[NANORTC_BWE_SEND_HISTORY_SIZE - 1].bytes, 0);
    size_t first_len = out.transmit.len;
    ASSERT_OK(
        nanortc_report_transmit(&rtc, out.transmit.token, NANORTC_TRANSMIT_SENT, UINT32_MAX - 10));
    ASSERT_EQ(rtc.media[0].rtcp.packets_sent, 1);
    ASSERT_EQ(rtc.media[0].rtcp.octets_sent, 2);
    ASSERT_EQ(rtc.media[1].rtcp.packets_sent, 0);
    ASSERT_EQ(rtc.bwe.sent[NANORTC_BWE_SEND_HISTORY_SIZE - 1].sequence, UINT16_MAX);
    ASSERT_EQ(rtc.bwe.sent[NANORTC_BWE_SEND_HISTORY_SIZE - 1].sent_ms, UINT32_MAX - 10);

    rtc.now_ms = 0;
    send_frame((uint8_t)second, &out);
    ASSERT_EQ(nanortc_read_u16be(out.transmit.data + 17), 0);
    size_t second_len = out.transmit.len;
    ASSERT_OK(nanortc_report_transmit(&rtc, out.transmit.token, NANORTC_TRANSMIT_SENT, 5));
    ASSERT_EQ(rtc.media[1].rtcp.packets_sent, 1);
    ASSERT_EQ(rtc.media[1].rtcp.octets_sent, 2);
    ASSERT_EQ(rtc.bwe.sent[0].sequence, UINT32_C(65536));
    ASSERT_EQ(rtc.bwe.sent[0].sent_ms, 5);

    rtc.now_ms = 10;
    send_frame((uint8_t)first, &out);
    ASSERT_OK(nanortc_report_transmit(&rtc, out.transmit.token, NANORTC_TRANSMIT_DROPPED, 10));
    ASSERT_EQ(rtc.stats_packets_sent, 2);
    ASSERT_EQ(rtc.stats_bytes_sent, first_len + second_len);
    ASSERT_EQ(rtc.stats_packets_dropped, 1);
    ASSERT_EQ(rtc.media[0].rtcp.packets_sent, 1);
    ASSERT_EQ(rtc.bwe.latest_sequence, UINT32_C(65536));
    ASSERT_OK(nanortc_get_track_stats(&rtc, (uint8_t)first, &admission));
    ASSERT_EQ(admission.packets_accepted, 2);
    ASSERT_EQ(admission.frames_accepted, 2);
    ASSERT_EQ(admission.packets_sent, 1);
    ASSERT_EQ(rtc.bwe.sent[1].bytes, 0);
    receive_feedback(UINT16_MAX, 100, 20);
    receive_feedback(0, 116, 21);
    ASSERT_EQ(rtc.bwe.history_missed, 0);
    ASSERT_EQ(rtc.bwe.delay_samples, 2);
}
#endif

TEST(test_nack_repeats_ciphertext_and_excludes_ambiguous_delay)
{
    init_sender();
    int mid = nanortc_add_video_track(&rtc, NANORTC_DIR_SENDONLY, NANORTC_CODEC_H264);
    ASSERT_TRUE(mid >= 0);
    rtc.media[0].rtp.twcc_ext_id = 3;
    rtc.media[0].rtp.ssrc = 1;
    rtc.state = NANORTC_STATE_DTLS_CONNECTED;
    nanortc_output_t out;
    send_frame((uint8_t)mid, &out);
    uint8_t original[NANORTC_MEDIA_BUF_SIZE];
    size_t original_len = out.transmit.len;
    memcpy(original, out.transmit.data, original_len);
    ASSERT_OK(nanortc_report_transmit(&rtc, out.transmit.token, NANORTC_TRANSMIT_SENT, 100));

    /* RFC 4585 §6.2.1 Generic NACK for the original RTP sequence. */
    uint8_t nack[NANORTC_MEDIA_BUF_SIZE] = {0x81, 205, 0, 3, 0, 0, 0, 9, 0, 0, 0, 1, 0, 0, 0, 0};
    memcpy(nack + 12, original + 2, 2);
    size_t protected_len;
    ASSERT_OK(nano_srtp_protect_rtcp(&peer, nack, 16, &protected_len));
    ASSERT_OK(nano_rtc_media_handle_rtp_or_rtcp(&rtc, nack, protected_len));
    ASSERT_OK(nanortc_poll_output(&rtc, &out));
    ASSERT_EQ(out.type, NANORTC_OUTPUT_TRANSMIT);
    ASSERT_EQ(out.transmit.len, original_len);
    ASSERT_MEM_EQ(out.transmit.data, original, original_len);
    ASSERT_OK(nanortc_report_transmit(&rtc, out.transmit.token, NANORTC_TRANSMIT_SENT, 200));
    ASSERT_EQ(rtc.twcc_sequence, 1);
    ASSERT_EQ(rtc.media[0].rtcp.packets_sent, 2);
    ASSERT_EQ(rtc.bwe.sent[0].sent_ms, 100);
    receive_feedback(0, 210, 220);
    ASSERT_EQ(rtc.bwe.delay_samples, 0);
    ASSERT_FALSE(rtc.bwe.delay_valid);

    send_frame((uint8_t)mid, &out);
    ASSERT_OK(nanortc_report_transmit(&rtc, out.transmit.token, NANORTC_TRANSMIT_SENT, 230));
    receive_feedback(1, 240, 250);
    ASSERT_EQ(rtc.bwe.delay_samples, 1);
    ASSERT_EQ(rtc.bwe.delay_first_send_ms, 230);
}

TEST(test_older_feedback_does_not_move_delay_window_backward)
{
    init_sender();
    bwe_record_transmit(&rtc.bwe, 0, 1000, 100, true, false);
    bwe_record_transmit(&rtc.bwe, 1, 1000, 200, true, false);
    receive_feedback(1, 201, 300);
    ASSERT_EQ(rtc.bwe.delay_first_send_ms, 200);
    receive_feedback(0, 101, 301);
    ASSERT_EQ(rtc.bwe.delay_first_send_ms, 200);
    ASSERT_EQ(rtc.bwe.delay_last_arrival_us, 201000);
    ASSERT_EQ(rtc.bwe.delay_samples, 1);
    ASSERT_FALSE(rtc.bwe.delay_valid);
}

TEST(test_feedback_matches_packets_that_depart_out_of_sequence)
{
    init_sender();
    /* A paced video packet can leave after a newer audio packet. Both
     * transport sequence numbers must remain matchable in send history. */
    bwe_record_transmit(&rtc.bwe, 1, 1000, 100, true, false);
    bwe_record_transmit(&rtc.bwe, 0, 1000, 200, true, false);
    receive_feedback(1, 110, 300);
    ASSERT_EQ(rtc.bwe.history_missed, 0);
    ASSERT_EQ(rtc.bwe.delay_samples, 1);
    ASSERT_EQ(rtc.bwe.delay_first_send_ms, 100);
    receive_feedback(0, 210, 301);
    ASSERT_EQ(rtc.bwe.history_missed, 0);
    ASSERT_EQ(rtc.bwe.delay_samples, 2);
}

TEST(test_overwritten_history_is_unknown_not_loss_or_capacity)
{
    init_sender();
    bwe_record_transmit(&rtc.bwe, 0, 1000, 100, true, false);
    bwe_record_transmit(&rtc.bwe, NANORTC_BWE_SEND_HISTORY_SIZE, 1000, 200, true, false);
    uint32_t target = bwe_get_bitrate(&rtc.bwe);
    receive_feedback(0, 101, 300);
    ASSERT_EQ(rtc.bwe.history_missed, 1);
    ASSERT_EQ(bwe_get_bitrate(&rtc.bwe), target);
    ASSERT_EQ(bwe_get_loss_q8(&rtc.bwe), 0);
    ASSERT_EQ(rtc.bwe.delay_samples, 0);
    ASSERT_FALSE(rtc.bwe.delay_valid);
}

TEST(test_rtt_uses_reported_sr_departure_and_matching_receiver_report)
{
    init_sender();
#if NANORTC_FEATURE_DATACHANNEL
    rtc.sdp.dc_mid = 0;
    rtc.sdp.mid_count = 1;
#endif
    int mid = nanortc_add_video_track(&rtc, NANORTC_DIR_SENDONLY, NANORTC_CODEC_H264);
    ASSERT_TRUE(mid >= 0);
    rtc.media[0].rtp.ssrc = rtc.media[0].rtcp.ssrc = 1;
    rtc.state = NANORTC_STATE_DTLS_CONNECTED;
    nanortc_output_t out;
    send_frame((uint8_t)mid, &out);
    ASSERT_OK(nanortc_report_transmit(&rtc, out.transmit.token, NANORTC_TRANSMIT_SENT, 100));
    ASSERT_OK(nano_rtc_media_emit_rtcp_sr_cadence(&rtc, 1000));
    ASSERT_OK(nanortc_poll_output(&rtc, &out));
    ASSERT_EQ(out.type, NANORTC_OUTPUT_TRANSMIT);
    ASSERT_FALSE(rtc.media[0].rtcp.last_tx_sr_valid);
    ASSERT_OK(nanortc_report_transmit(&rtc, out.transmit.token, NANORTC_TRANSMIT_SENT, 1200));
    ASSERT_TRUE(rtc.media[0].rtcp.last_tx_sr_valid);
    ASSERT_EQ(rtc.media[0].rtcp.last_tx_sr_ntp, 0x00010000u);
    ASSERT_EQ(rtc.media[0].rtcp.last_tx_sr_ms, 1200);

    /* RFC 3550 §6.4.2: one report block for SSRC=1, fraction lost=32,
     * LSR=1.0 s, DLSR=0.25 s (16384 / 65536). 1600-1200-250=150 ms. */
    const uint8_t rr[] = {
        0x81, 201, 0, 7, 0, 0, 0, 9, 0, 0, 0, 1, 32, 0, 0,    0,
        0,    0,   0, 1, 0, 0, 0, 0, 0, 1, 0, 0, 0,  0, 0x40, 0,
    };
    uint8_t encrypted[NANORTC_MEDIA_BUF_SIZE];
    memcpy(encrypted, rr, sizeof(rr));
    size_t protected_len;
    ASSERT_OK(nano_srtp_protect_rtcp(&peer, encrypted, sizeof(rr), &protected_len));
    rtc.now_ms = 1600;
    ASSERT_OK(nano_rtc_media_handle_rtp_or_rtcp(&rtc, encrypted, protected_len));
    ASSERT_EQ(rtc.media[0].fraction_lost, 32);
    ASSERT_TRUE(rtc.media[0].rtcp.rtt_valid);
    ASSERT_EQ(rtc.media[0].rtcp.rtt_ms, 150);

    /* A stale LSR cannot overwrite the last matching RTT sample. */
    memcpy(encrypted, rr, sizeof(rr));
    encrypted[25] = 2;
    ASSERT_OK(nano_srtp_protect_rtcp(&peer, encrypted, sizeof(rr), &protected_len));
    rtc.now_ms = 1800;
    ASSERT_OK(nano_rtc_media_handle_rtp_or_rtcp(&rtc, encrypted, protected_len));
    ASSERT_EQ(rtc.media[0].rtcp.rtt_ms, 150);
}

TEST(test_report_timestamp_ahead_of_input_does_not_rewind_rate_window)
{
    init_sender();
    int mid = nanortc_add_video_track(&rtc, NANORTC_DIR_SENDONLY, NANORTC_CODEC_H264);
    ASSERT_TRUE(mid >= 0);
    rtc.state = NANORTC_STATE_DTLS_CONNECTED;
    nanortc_output_t out;
    send_frame((uint8_t)mid, &out);
    size_t bytes = out.transmit.len;
    rtc.now_ms = 1000;
    ASSERT_OK(nanortc_report_transmit(&rtc, out.transmit.token, NANORTC_TRANSMIT_SENT, 1001));
    ASSERT_EQ(rtc.now_ms, 1000);
    ASSERT_EQ(rtc.media[0].rate_window.bucket_start_ms, 1001);
    ASSERT_EQ(rtc.media[0].rate_window.cur_bytes, bytes);

    nanortc_track_stats_t stats;
    ASSERT_OK(nanortc_get_track_stats(&rtc, (uint8_t)mid, &stats));
    /* The completed [0,1000] ms bucket contained one accepted frame, but no
     * socket-accepted bytes. The actual send belongs to the current bucket. */
    ASSERT_EQ(stats.send_bitrate_bps, 0);
    ASSERT_EQ(stats.send_fps_q8, 256);
    send_frame((uint8_t)mid, &out);
    ASSERT_EQ(rtc.media[0].rate_window.bucket_start_ms, 1001);
    ASSERT_EQ(rtc.media[0].rate_window.cur_bytes, bytes);
    ASSERT_OK(nanortc_report_transmit(&rtc, out.transmit.token, NANORTC_TRANSMIT_SENT, 1002));
}
#endif

TEST_MAIN_BEGIN("nanortc media transmit reports")
#if NANORTC_FEATURE_VIDEO
#if NANORTC_MAX_MEDIA_TRACKS >= 2
RUN(test_tracks_count_only_reported_sends_and_share_wrapping_twcc);
#endif
RUN(test_nack_repeats_ciphertext_and_excludes_ambiguous_delay);
RUN(test_older_feedback_does_not_move_delay_window_backward);
RUN(test_feedback_matches_packets_that_depart_out_of_sequence);
RUN(test_overwritten_history_is_unknown_not_loss_or_capacity);
RUN(test_rtt_uses_reported_sr_departure_and_matching_receiver_report);
RUN(test_report_timestamp_ahead_of_input_does_not_rewind_rate_window);
#endif
TEST_MAIN_END
