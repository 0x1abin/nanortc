/* Drive the public API through live ICE/DTLS/SCTP/SRTP and both restart roles. */
#include "nanortc.h"
#include "nano_rtc_internal.h"
#include "nano_test.h"
#include "nano_test_config.h"
#include <string.h>

static nanortc_t peers[2];
static unsigned connected[2], opened[2], messages[2], audio[2], video[2];
static int audio_mid, video_mid, channel;
static const uint8_t opus[] = {0xf8, 0xff, 0xfe};
static const uint8_t idr[] = {0, 0, 0, 1, 0x65, 0x88, 0x84, 0x21};

static void relay(unsigned index, uint32_t now)
{
    nanortc_t *from = &peers[index], *to = &peers[index ^ 1u];
    nanortc_output_t out;
    while (nanortc_poll_output(from, &out) == NANORTC_OK) {
        if (out.type == NANORTC_OUTPUT_TRANSMIT) {
            /* Completion mutates from; copy the borrowed packet first. */
            uint8_t packet[NANORTC_TX_SLOT_SIZE];
            ASSERT_TRUE(out.transmit.len <= sizeof(packet));
            memcpy(packet, out.transmit.data, out.transmit.len);
            ASSERT_OK(
                nanortc_report_transmit(from, out.transmit.token, NANORTC_TRANSMIT_SENT, now));
            int rc = nanortc_handle_input(to, &(nanortc_input_t){.now_ms = now,
                                                                 .data = packet,
                                                                 .len = out.transmit.len,
                                                                 .src = out.transmit.src,
                                                                 .dst = out.transmit.dest});
            for (unsigned retry = 0; rc == NANORTC_ERR_WOULD_BLOCK && retry < 8; retry++) {
                relay(index ^ 1u, now);
                rc = nanortc_handle_input(to, &(nanortc_input_t){.now_ms = now,
                                                                 .data = packet,
                                                                 .len = out.transmit.len,
                                                                 .src = out.transmit.src,
                                                                 .dst = out.transmit.dest});
            }
            /* Crossed checks can cancel the old transaction before its reply. */
            if (rc == NANORTC_ERR_PROTOCOL && ice_is_stun(packet, out.transmit.len))
                continue;
            ASSERT_OK(rc);
        } else if (out.event.type == NANORTC_EV_CONNECTED) {
            connected[index]++;
        } else if (out.event.type == NANORTC_EV_DATACHANNEL_OPEN) {
            opened[index]++;
        } else if (out.event.type == NANORTC_EV_DATACHANNEL_DATA) {
            ASSERT_EQ(out.event.datachannel_data.len, 4);
            ASSERT_MEM_EQ(out.event.datachannel_data.data, "live", 4);
            messages[index]++;
        } else if (out.event.type == NANORTC_EV_MEDIA_DATA) {
            if (out.event.media_data.mid == audio_mid) {
                ASSERT_EQ(out.event.media_data.len, sizeof(opus));
                ASSERT_MEM_EQ(out.event.media_data.data, opus, sizeof(opus));
                audio[index]++;
            } else if (out.event.media_data.mid == video_mid) {
                ASSERT_EQ(out.event.media_data.len, sizeof(idr) - 4);
                ASSERT_MEM_EQ(out.event.media_data.data, idr + 4, sizeof(idr) - 4);
                video[index]++;
            }
        }
    }
}

static void pump(uint32_t now)
{
    for (unsigned i = 0; i < 12; i++) {
        ASSERT_OK(nanortc_handle_input(&peers[0], &(nanortc_input_t){.now_ms = now}));
        ASSERT_OK(nanortc_handle_input(&peers[1], &(nanortc_input_t){.now_ms = now}));
        relay(0, now);
        relay(1, now);
    }
}

static void exchange(unsigned offerer)
{
    char offer[NANORTC_SDP_BUF_SIZE], answer[NANORTC_SDP_BUF_SIZE];
    size_t len;
    ASSERT_OK(nanortc_create_offer(&peers[offerer], offer, sizeof(offer), &len));
    ASSERT_OK(nanortc_accept_offer(&peers[offerer ^ 1u], offer, len, answer, sizeof(answer), &len));
    ASSERT_OK(nanortc_accept_answer(&peers[offerer], answer, len));
}

static void send_live(uint32_t now, uint32_t timestamp)
{
#if NANORTC_FEATURE_DATACHANNEL
    unsigned before_dc = messages[1];
    ASSERT_OK(nanortc_datachannel_send_string(&peers[0], (uint16_t)channel, "live"));
#endif
#if NANORTC_FEATURE_AUDIO
    unsigned before_audio = audio[1];
    ASSERT_OK(nanortc_send_audio(&peers[0], (uint8_t)audio_mid, timestamp, opus, sizeof(opus)));
#endif
#if NANORTC_FEATURE_VIDEO
    unsigned before_video = video[1];
    ASSERT_OK(nanortc_send_video(&peers[0], (uint8_t)video_mid, timestamp, idr, sizeof(idr)));
#endif
    for (uint32_t delta = 0; delta < 200; delta += 10)
        pump(now + delta);
#if NANORTC_FEATURE_DATACHANNEL
    ASSERT_EQ(messages[1], before_dc + 1);
#endif
#if NANORTC_FEATURE_AUDIO
    ASSERT_EQ(audio[1], before_audio + 1);
#endif
#if NANORTC_FEATURE_VIDEO
    ASSERT_EQ(video[1], before_video + 1);
#endif
    (void)timestamp;
}

TEST(test_restart_both_roles_preserves_live_channels_and_media)
{
    memset(connected, 0, sizeof(connected));
    memset(opened, 0, sizeof(opened));
    memset(messages, 0, sizeof(messages));
    memset(audio, 0, sizeof(audio));
    memset(video, 0, sizeof(video));
    audio_mid = video_mid = channel = -1;
    for (unsigned i = 0; i < 2; i++) {
        nanortc_config_t cfg = NANORTC_CONFIG_DEFAULT();
        cfg.crypto = nano_test_crypto();
        cfg.role = i == 0 ? NANORTC_ROLE_CONTROLLING : NANORTC_ROLE_CONTROLLED;
#if NANORTC_FEATURE_AUDIO
        cfg.jitter_depth_ms = 0;
#endif
        ASSERT_OK(nanortc_init(&peers[i], &cfg));
        ASSERT_OK(nanortc_add_local_candidate(&peers[i], "127.0.0.1", (uint16_t)(5000 + i)));
    }
#if NANORTC_FEATURE_DATACHANNEL
    channel = nanortc_create_datachannel(&peers[0], "live", NULL);
    ASSERT_TRUE(channel >= 0);
#endif
#if NANORTC_FEATURE_AUDIO
    audio_mid =
        nanortc_add_audio_track(&peers[0], NANORTC_DIR_SENDRECV, NANORTC_CODEC_OPUS, 48000, 2);
    ASSERT_TRUE(audio_mid >= 0);
#endif
#if NANORTC_FEATURE_VIDEO
    video_mid = nanortc_add_video_track(&peers[0], NANORTC_DIR_SENDRECV, NANORTC_CODEC_H264);
    ASSERT_TRUE(video_mid >= 0);
#endif
    exchange(0);
    uint32_t now = 100;
    for (; now < 3000; now += 10) {
        pump(now);
        if (nanortc_is_connected(&peers[0]) && nanortc_is_connected(&peers[1])
#if NANORTC_FEATURE_DATACHANNEL
            && opened[0] && opened[1]
#endif
        )
            break;
    }
    ASSERT_TRUE(now < 3000);
    send_live(now, 4800);
    now += 200;
    for (unsigned role = 0; role < 2; role++) {
        unsigned previous_open[2] = {opened[0], opened[1]};
        void *identities[2] = {peers[0].dtls.crypto_ctx, peers[1].dtls.crypto_ctx};
#if NANORTC_HAVE_MEDIA_TRANSPORT
        nano_srtp_t keys[2] = {peers[0].srtp, peers[1].srtp};
        uint16_t sequences[NANORTC_MAX_MEDIA_TRACKS];
        for (uint8_t i = 0; i < peers[0].media_count; i++)
            sequences[i] = peers[0].media[i].rtp.seq;
#endif
#if NANORTC_FEATURE_DATACHANNEL
        uint32_t tsn = peers[0].sctp.next_tsn;
#endif
        ASSERT_OK(nanortc_ice_restart(&peers[role]));
        /* The old route can still carry data before new checks finish. */
        send_live(now, (role + 2u) * 4800u);
        now += 200;
        exchange(role);
        for (unsigned rounds = 0; rounds < 300; rounds++) {
            pump(now);
            now += 10;
            if (!peers[0].ice_restarting && !peers[1].ice_restarting)
                break;
        }
        ASSERT_FALSE(peers[0].ice_restarting);
        ASSERT_FALSE(peers[1].ice_restarting);
        for (unsigned i = 0; i < 2; i++) {
            ASSERT_EQ(connected[i], 1);
            ASSERT_EQ(opened[i], previous_open[i]);
            ASSERT_TRUE(peers[i].dtls.crypto_ctx == identities[i]);
#if NANORTC_HAVE_MEDIA_TRANSPORT
            ASSERT_MEM_EQ(peers[i].srtp.send_key, keys[i].send_key, sizeof(keys[i].send_key));
#endif
        }
#if NANORTC_HAVE_MEDIA_TRANSPORT
        for (uint8_t i = 0; i < peers[0].media_count; i++)
            ASSERT_TRUE((uint16_t)(peers[0].media[i].rtp.seq - sequences[i]) > 0);
#endif
#if NANORTC_FEATURE_DATACHANNEL
        ASSERT_TRUE((int32_t)(peers[0].sctp.next_tsn - tsn) > 0);
#endif
        send_live(now, (role + 4u) * 4800u);
        now += 200;
    }
    /* Receiving a request cannot renew consent to send in the other direction. */
    peers[0].ice.consent_expiry_ms = now + 100;
    peers[0].ice.consent_next_ms = now + 1000;
    peers[1].ice.consent_next_ms = 0;
    uint8_t request[256];
    size_t request_len;
    ASSERT_OK(ice_generate_consent(&peers[1].ice, now, nano_test_crypto(), request, sizeof(request),
                                   &request_len));
    nanortc_input_t input = {.now_ms = now,
                             .data = request,
                             .len = request_len,
                             .src = {.family = 4, .port = 5001, .addr = {127, 0, 0, 1}},
                             .dst = {.family = 4, .port = 5000, .addr = {127, 0, 0, 1}}};
    ASSERT_OK(nanortc_handle_input(&peers[0], &input));
    ASSERT_EQ(peers[0].ice.consent_expiry_ms, now + 100);
    input.now_ms = now + 100;
    ASSERT_OK(nanortc_handle_input(&peers[0], &input));
    ASSERT_FALSE(nanortc_is_alive(&peers[0]));
    ASSERT_EQ(peers[0].ice.state, NANORTC_ICE_STATE_DISCONNECTED);
    nanortc_destroy(&peers[0]);
    nanortc_destroy(&peers[1]);
}

TEST(test_restart_old_route_stops_at_consent_expiry)
{
    nanortc_config_t cfg = NANORTC_CONFIG_DEFAULT();
    cfg.crypto = nano_test_crypto();
    nanortc_t *rtc = &peers[0];
    ASSERT_OK(nanortc_init(rtc, &cfg));
    rtc->state = NANORTC_STATE_CONNECTED;
    rtc->dtls.state = NANORTC_DTLS_STATE_ESTABLISHED;
    rtc->now_ms = 100;
    rtc->ice.consent_expiry_ms = 200;
    rtc->remote_addr = (nanortc_addr_t){.family = 4, .port = 5000};
    ASSERT_OK(nanortc_ice_restart(rtc));
    const uint8_t packet[] = {23, 0xfe, 0xfd};
    ASSERT_OK(nano_rtc_enqueue_transmit(rtc, packet, sizeof(packet), &rtc->remote_addr, false));
    /* Skip input to isolate queue admission from protocol timer side effects. */
    rtc->now_ms = 200;
    ASSERT_EQ(nano_rtc_enqueue_transmit(rtc, packet, sizeof(packet), &rtc->remote_addr, false),
              NANORTC_ERR_WOULD_BLOCK);
    nanortc_output_t out;
    while (nanortc_poll_output(rtc, &out) == NANORTC_OK)
        ASSERT_EQ(out.type, NANORTC_OUTPUT_EVENT);
    uint32_t wait;
    ASSERT_OK(nanortc_next_timeout_ms(rtc, 200, &wait));
    ASSERT_TRUE(wait > 0);
    nanortc_stats_t stats;
    ASSERT_OK(nanortc_get_stats(rtc, &stats));
    ASSERT_EQ(stats.packets_sent, 0);
    ASSERT_EQ(stats.packets_dropped, 1);
    nanortc_destroy(rtc);
}

TEST_MAIN_BEGIN("live session restart")
RUN(test_restart_both_roles_preserves_live_channels_and_media);
RUN(test_restart_old_route_stops_at_consent_expiry);
TEST_MAIN_END
