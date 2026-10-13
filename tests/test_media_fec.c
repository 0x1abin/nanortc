/* Long-running receive integration without a DTLS handshake.
 * SPDX-License-Identifier: MIT */
#include "nanortc.h"
#include "nano_rtc_internal.h"
#include "nano_fec.h"
#include "nano_test.h"
#include "nano_test_config.h"
#include <string.h>

#if NANORTC_FEATURE_VIDEO && NANORTC_FEATURE_VIDEO_FEC
#define TEST_SSRC         0x12345678u
#define TEST_PACKET_BYTES 32
static nanortc_t receiver;
static nano_srtp_t sender;
static nanortc_t transmitter;

static void feed_packet(const uint8_t *plain, size_t len)
{
    uint8_t encrypted[NANORTC_MEDIA_BUF_SIZE];
    memcpy(encrypted, plain, len);
    size_t protected_len;
    ASSERT_OK(nano_srtp_protect(&sender, encrypted, len, &protected_len));
    receiver.now_ms++;
    ASSERT_OK(nano_rtc_media_handle_rtp_or_rtcp(&receiver, encrypted, protected_len));
    nanortc_output_t out;
    while (nano_test_poll_output(&receiver, &out) == NANORTC_OK) {
    }
}

TEST(test_fec_receive_ring_survives_more_than_1024_packets)
{
    memset(&receiver, 0, sizeof(receiver));
    receiver.config.crypto = nano_test_crypto();
    receiver.state = NANORTC_STATE_DTLS_CONNECTED;
    receiver.remote_addr.family = 4;
    receiver.remote_addr.port = 5000;
    uint8_t keys[2 * (NANORTC_SRTP_KEY_SIZE + NANORTC_SRTP_SALT_SIZE)] = {0};
    ASSERT_OK(nano_srtp_init(&sender, nano_test_crypto(), 1));
    ASSERT_OK(nano_srtp_derive_keys(&sender, keys, sizeof(keys)));
    ASSERT_OK(nano_srtp_init(&receiver.srtp, nano_test_crypto(), 0));
    ASSERT_OK(nano_srtp_derive_keys(&receiver.srtp, keys, sizeof(keys)));
    ASSERT_OK(track_init(&receiver.media[0], 0, NANORTC_TRACK_VIDEO, NANORTC_DIR_RECVONLY,
                         NANORTC_CODEC_H264, 90000, 0, 0));
    receiver.media_count = 1;
    receiver.media[0].rtcp.remote_ssrc = TEST_SSRC;
    ASSERT_OK(ssrc_map_register(receiver.ssrc_map, NANORTC_MAX_SSRC_MAP, TEST_SSRC, 0));

    uint16_t seq = 100;
    for (unsigned group = 0; group < 1024; group++) {
        uint8_t media[NANORTC_FEC_GROUP_SIZE][TEST_PACKET_BYTES];
        const uint8_t *members[NANORTC_FEC_GROUP_SIZE];
        uint16_t lens[NANORTC_FEC_GROUP_SIZE];
        for (unsigned i = 0; i < NANORTC_FEC_GROUP_SIZE; i++) {
            memset(media[i], 0x55, sizeof(media[i]));
            media[i][0] = 0x80;
            media[i][1] = 0x80 | 96;
            nanortc_write_u16be(media[i] + 2, seq++);
            nanortc_write_u32be(media[i] + 4, seq * 3000u);
            nanortc_write_u32be(media[i] + 8, TEST_SSRC);
            media[i][12] = 0x41;
            members[i] = media[i];
            lens[i] = sizeof(media[i]);
            if (i + 1 < NANORTC_FEC_GROUP_SIZE) {
                feed_packet(media[i], sizeof(media[i]));
            }
        }
        uint8_t fec[NANORTC_MEDIA_BUF_SIZE] = {0x80, NANORTC_VIDEO_FEC_PT};
        nanortc_write_u16be(fec + 2, (uint16_t)group);
        nanortc_write_u32be(fec + 8, TEST_SSRC + 1);
        size_t fec_len;
        ASSERT_OK(fec_encode(members, lens, NANORTC_FEC_GROUP_SIZE, fec + FEC_RTP_HDR,
                             sizeof(fec) - FEC_RTP_HDR, &fec_len));
        feed_packet(fec, FEC_RTP_HDR + fec_len);
        ASSERT_EQ(receiver.stats_fec_recovered, group + 1u);
    }
}

#if NANORTC_MAX_MEDIA_TRACKS >= 2
TEST(test_fec_sender_never_combines_video_tracks)
{
    memset(&transmitter, 0, sizeof(transmitter));
    transmitter.config.crypto = nano_test_crypto();
    transmitter.remote_addr.family = 4;
    uint8_t keys[2 * (NANORTC_SRTP_KEY_SIZE + NANORTC_SRTP_SALT_SIZE)] = {0};
    ASSERT_OK(nano_srtp_init(&transmitter.srtp, nano_test_crypto(), 1));
    ASSERT_OK(nano_srtp_derive_keys(&transmitter.srtp, keys, sizeof(keys)));
    int first = nanortc_add_video_track(&transmitter, NANORTC_DIR_SENDONLY, NANORTC_CODEC_H264);
    int second = nanortc_add_video_track(&transmitter, NANORTC_DIR_SENDONLY, NANORTC_CODEC_H264);
    ASSERT_TRUE(first >= 0 && second >= 0);
    transmitter.media[0].rtp.ssrc = 10;
    transmitter.media[1].rtp.ssrc = 20;
    transmitter.media[0].rtp.seq = transmitter.media[1].rtp.seq = 0;
    transmitter.bwe.smoothed_loss_q8 = NANORTC_FEC_LOSS_OFF_Q8;
    transmitter.state = NANORTC_STATE_DTLS_CONNECTED;
#if NANORTC_FEATURE_VIDEO_PACING
    transmitter.pacer.budget_bytes = NANORTC_PACING_MAX_BURST_BYTES;
#endif
    const uint8_t frame[] = {0, 0, 0, 1, 0x65, 0x88};
    ASSERT_OK(nanortc_send_video(&transmitter, (uint8_t)first, 0, frame, sizeof(frame)));
    ASSERT_EQ(transmitter.fec_tx_n, 1);
    nanortc_output_t out;
    while (nano_test_poll_output(&transmitter, &out) == NANORTC_OK) {
    }
    ASSERT_OK(nanortc_send_video(&transmitter, (uint8_t)second, 10, frame, sizeof(frame)));
    ASSERT_EQ(transmitter.fec_tx_n, 1);
    ASSERT_EQ(nanortc_read_u32be(transmitter.fec_tx_grp[0] + 8), 20);
    while (nano_test_poll_output(&transmitter, &out) == NANORTC_OK) {
    }
    for (uint8_t i = 1; i < NANORTC_FEC_GROUP_SIZE; i++) {
        ASSERT_OK(nanortc_send_video(&transmitter, (uint8_t)second, i * 10u, frame, sizeof(frame)));
        while (nano_test_poll_output(&transmitter, &out) == NANORTC_OK) {
        }
    }
    ASSERT_EQ(transmitter.stats_fec_sent, 1);
    ASSERT_EQ(transmitter.fec_tx_n, 0);
}
#endif
#endif

TEST_MAIN_BEGIN("nanortc media FEC receive lifetime")
#if NANORTC_FEATURE_VIDEO && NANORTC_FEATURE_VIDEO_FEC
RUN(test_fec_receive_ring_survives_more_than_1024_packets);
#if NANORTC_MAX_MEDIA_TRACKS >= 2
RUN(test_fec_sender_never_combines_video_tracks);
#endif
#endif
TEST_MAIN_END
