/* Public negotiation boundaries: state transitions and failure atomicity. */
#include "nanortc.h"
#include "nano_test.h"
#include "nano_test_config.h"
#include <string.h>

static void init_peer(nanortc_t *rtc)
{
    nanortc_config_t cfg = NANORTC_CONFIG_DEFAULT();
    cfg.crypto = nano_test_crypto();
    ASSERT_OK(nanortc_init(rtc, &cfg));
#if NANORTC_FEATURE_DATACHANNEL
    ASSERT_TRUE(nanortc_create_datachannel(rtc, "test", NULL) >= 0);
#endif
#if NANORTC_FEATURE_AUDIO
    ASSERT_TRUE(nanortc_add_audio_track(rtc, NANORTC_DIR_SENDRECV, NANORTC_CODEC_OPUS, 48000, 2) >=
                0);
#elif NANORTC_FEATURE_VIDEO
    ASSERT_TRUE(nanortc_add_video_track(rtc, NANORTC_DIR_SENDRECV, NANORTC_CODEC_H264) >= 0);
#endif
}

TEST(test_negotiation_pending_offer)
{
    nanortc_t rtc;
    init_peer(&rtc);
    ASSERT_EQ(nanortc_accept_answer(&rtc, "v=0\r\n", 5), NANORTC_ERR_STATE);
    char offer[8192];
    ASSERT_OK(nanortc_create_offer(&rtc, offer, sizeof(offer), NULL));
    nanortc_t before = rtc;
    ASSERT_EQ(nanortc_create_offer(&rtc, offer, sizeof(offer), NULL), NANORTC_ERR_STATE);
    ASSERT_MEM_EQ(&rtc, &before, sizeof(rtc));
    nanortc_destroy(&rtc);
}

TEST(test_negotiation_short_buffers_are_atomic)
{
    nanortc_t a, b;
    init_peer(&a);
    init_peer(&b);
    char offer[8192], answer[8192];
    nanortc_t before = a;
    ASSERT_EQ(nanortc_create_offer(&a, offer, 1, NULL), NANORTC_ERR_BUFFER_TOO_SMALL);
    ASSERT_MEM_EQ(&a, &before, sizeof(a));
    size_t len;
    ASSERT_OK(nanortc_create_offer(&a, offer, sizeof(offer), &len));
    before = b;
    ASSERT_EQ(nanortc_accept_offer(&b, offer, len, answer, 1, NULL), NANORTC_ERR_BUFFER_TOO_SMALL);
    ASSERT_MEM_EQ(&b, &before, sizeof(b));
    ASSERT_OK(nanortc_accept_offer(&b, offer, len, answer, sizeof(answer), &len));
    ASSERT_OK(nanortc_accept_answer(&a, answer, len));
    ASSERT_EQ(a.signaling_state, NANORTC_SIGNALING_NEGOTIATED);
    ASSERT_EQ(b.signaling_state, NANORTC_SIGNALING_NEGOTIATED);
    ASSERT_TRUE(a.dtls.peer_fingerprint_set);
    ASSERT_TRUE(b.dtls.peer_fingerprint_set);
    ASSERT_MEM_EQ(a.dtls.expected_peer_fingerprint, a.sdp.remote_fingerprint,
                  NANORTC_DTLS_FINGERPRINT_SIZE);
    nanortc_destroy(&a);
    nanortc_destroy(&b);
}

TEST(test_negotiation_rejects_missing_fingerprint_atomically)
{
    nanortc_t rtc;
    init_peer(&rtc);
    nanortc_t before = rtc;
    const char missing[] = "v=0\r\na=ice-ufrag:abcd\r\na=ice-pwd:abcdefghijklmnopqrstuv\r\n";
    char answer[8192];
    ASSERT_EQ(
        nanortc_accept_offer(&rtc, missing, sizeof(missing) - 1, answer, sizeof(answer), NULL),
        NANORTC_ERR_PARSE);
    ASSERT_MEM_EQ(&rtc, &before, sizeof(rtc));
    nanortc_destroy(&rtc);
}

TEST(test_negotiation_candidate_parser_and_atomic_failure)
{
    nanortc_t rtc;
    init_peer(&rtc);
    const char relay[] =
        "a=candidate:1 1 UDP 100 192.0.2.1 3478 typ relay raddr 192.0.2.2 rport 5000";
    ASSERT_OK(nanortc_add_remote_candidate(&rtc, relay, sizeof(relay) - 1));
    ASSERT_EQ(rtc.ice.remote_candidates[0].type, NANORTC_ICE_CAND_RELAY);
    ASSERT_EQ(rtc.ice.remote_priorities[0], 100);
    ASSERT_TRUE(rtc.remote_candidate_related[0].present);
    ASSERT_EQ(rtc.remote_candidate_related[0].family, 4);
    ASSERT_EQ(rtc.remote_candidate_related[0].port, 5000);
    const uint8_t expected_related[] = {192, 0, 2, 2};
    ASSERT_MEM_EQ(rtc.remote_candidate_related[0].addr, expected_related, sizeof(expected_related));
    const char duplicate[] =
        "candidate:1 1 UDP 999 192.0.2.1 3478 typ relay raddr 192.0.2.2 rport 6000";
    ASSERT_OK(nanortc_add_remote_candidate(&rtc, duplicate, sizeof(duplicate) - 1));
    ASSERT_EQ(rtc.ice.remote_candidate_count, 1);
    ASSERT_EQ(rtc.ice.remote_priorities[0], 999);
    ASSERT_EQ(rtc.remote_candidate_related[0].port, 6000);
    const char overflow[] = "candidate:1 1 UDP 100 192.0.2.2 65537 typ host";
    nanortc_t before = rtc;
    ASSERT_EQ(nanortc_add_remote_candidate(&rtc, overflow, sizeof(overflow) - 1),
              NANORTC_ERR_PARSE);
    ASSERT_MEM_EQ(&rtc, &before, sizeof(rtc));
    const char partial[] = "candidate:1 1 UDP 300 192.0.2.1 3478 typ relay raddr 192.0.2.3";
    ASSERT_EQ(nanortc_add_remote_candidate(&rtc, partial, sizeof(partial) - 1), NANORTC_ERR_PARSE);
    ASSERT_MEM_EQ(&rtc, &before, sizeof(rtc));
    const char host_duplicate[] = "candidate:1 1 UDP 200 192.0.2.1 3478 typ host";
    ASSERT_OK(nanortc_add_remote_candidate(&rtc, host_duplicate, sizeof(host_duplicate) - 1));
    ASSERT_EQ(rtc.ice.remote_candidate_count, 1);
    ASSERT_EQ(rtc.ice.remote_candidates[0].type, NANORTC_ICE_CAND_HOST);
    ASSERT_EQ(rtc.ice.remote_priorities[0], 200);
    ASSERT_TRUE(!rtc.remote_candidate_related[0].present);
    nanortc_destroy(&rtc);
}

TEST(test_negotiation_full_candidates_preserve_priority)
{
    nanortc_t a, b;
    init_peer(&a);
    init_peer(&b);
    ASSERT_OK(nanortc_add_local_candidate(&a, "192.0.2.1", 5000));
    char offer[8192], answer[8192];
    size_t len;
    ASSERT_OK(nanortc_create_offer(&a, offer, sizeof(offer), &len));
    const char srflx[] = "a=candidate:2 1 UDP 100 192.0.2.2 5001 typ srflx "
                         "raddr 192.0.2.3 rport 9000\r\n";
    memcpy(offer + len, srflx, sizeof(srflx) - 1);
    len += sizeof(srflx) - 1;
    ASSERT_OK(nanortc_accept_offer(&b, offer, len, answer, sizeof(answer), NULL));
    ASSERT_EQ(b.ice.remote_candidate_count, 2);
    ASSERT_EQ(b.ice.remote_priorities[0], ICE_HOST_PRIORITY(0));
    ASSERT_EQ(b.ice.remote_candidates[0].family, 4);
    ASSERT_TRUE(!b.remote_candidate_related[0].present);
    ASSERT_EQ(b.ice.remote_priorities[1], 100);
    ASSERT_TRUE(b.remote_candidate_related[1].present);
    ASSERT_EQ(b.remote_candidate_related[1].family, 4);
    ASSERT_EQ(b.remote_candidate_related[1].port, 9000);
    const uint8_t expected_related[] = {192, 0, 2, 3};
    ASSERT_MEM_EQ(b.remote_candidate_related[1].addr, expected_related, sizeof(expected_related));
    ASSERT_OK(nanortc_ice_restart(&b));
    ASSERT_TRUE(!b.remote_candidate_related[1].present);
    nanortc_destroy(&a);
    nanortc_destroy(&b);
}

#if NANORTC_FEATURE_IPV6
TEST(test_negotiation_trickle_preserves_related_ipv6)
{
    nanortc_t rtc;
    init_peer(&rtc);
    const char candidate[] = "candidate:2 1 UDP 100 2001:db8::1 5000 typ relay "
                             "raddr 2001:db8::2 rport 65535";
    ASSERT_OK(nanortc_add_remote_candidate(&rtc, candidate, sizeof(candidate) - 1));
    const uint8_t expected[] = {0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2};
    ASSERT_TRUE(rtc.remote_candidate_related[0].present);
    ASSERT_EQ(rtc.remote_candidate_related[0].family, 6);
    ASSERT_EQ(rtc.remote_candidate_related[0].port, 65535);
    ASSERT_MEM_EQ(rtc.remote_candidate_related[0].addr, expected, sizeof(expected));
    nanortc_destroy(&rtc);
}
#endif

TEST(test_negotiation_full_related_failure_is_atomic)
{
    nanortc_t a, b;
    init_peer(&a);
    init_peer(&b);
    char offer[8192], answer[8192];
    size_t len;
    ASSERT_OK(nanortc_create_offer(&a, offer, sizeof(offer), &len));
    const char candidates[] =
        "a=candidate:1 1 UDP 100 192.0.2.1 5000 typ host\r\n"
        "a=candidate:2 1 UDP 100 192.0.2.2 5001 typ srflx raddr 192.0.2.3 rport 65536\r\n";
    memcpy(offer + len, candidates, sizeof(candidates) - 1);
    len += sizeof(candidates) - 1;
    nanortc_t before = b;
    ASSERT_EQ(nanortc_accept_offer(&b, offer, len, answer, sizeof(answer), NULL),
              NANORTC_ERR_PARSE);
    ASSERT_MEM_EQ(&b, &before, sizeof(b));
    nanortc_destroy(&a);
    nanortc_destroy(&b);
}

static void complete_dtls(nanortc_t *a, nanortc_t *b)
{
    ASSERT_OK(dtls_start(&a->dtls));
    for (unsigned i = 0; i < 30; i++) {
        uint8_t packet[NANORTC_DTLS_BUF_SIZE];
        size_t len;
        while (dtls_poll_output(&a->dtls, packet, sizeof(packet), &len) == NANORTC_OK)
            ASSERT_OK(dtls_handle_data(&b->dtls, packet, len));
        while (dtls_poll_output(&b->dtls, packet, sizeof(packet), &len) == NANORTC_OK)
            ASSERT_OK(dtls_handle_data(&a->dtls, packet, len));
        if (a->dtls.state == NANORTC_DTLS_STATE_ESTABLISHED &&
            b->dtls.state == NANORTC_DTLS_STATE_ESTABLISHED)
            break;
    }
    ASSERT_EQ(a->dtls.state, NANORTC_DTLS_STATE_ESTABLISHED);
    ASSERT_EQ(b->dtls.state, NANORTC_DTLS_STATE_ESTABLISHED);
    a->state = b->state = NANORTC_STATE_CONNECTED;
}

TEST(test_negotiation_ice_restart_preserves_dtls)
{
    nanortc_t a, b;
    init_peer(&a);
    init_peer(&b);
    char offer[8192], answer[8192];
    size_t len;
    ASSERT_OK(nanortc_create_offer(&a, offer, sizeof(offer), &len));
    ASSERT_OK(nanortc_accept_offer(&b, offer, len, answer, sizeof(answer), &len));
    ASSERT_OK(nanortc_accept_answer(&a, answer, len));
    complete_dtls(&a, &b);
    nano_dtls_t previous_a = a.dtls, previous_b = b.dtls;

    /* The original DTLS server initiates ICE restart. Its actpass offer must
     * preserve the existing client role at the answerer (RFC 8842 §6). */
    ASSERT_OK(nanortc_ice_restart(&b));
    ASSERT_OK(nanortc_create_offer(&b, offer, sizeof(offer), &len));
    char original[sizeof(offer)];
    memcpy(original, offer, len);
    char *fingerprint = offer;
    while ((fingerprint = strstr(fingerprint, "a=fingerprint:sha-256 ")) != NULL) {
        fingerprint += sizeof("a=fingerprint:sha-256 ") - 1;
        *fingerprint = *fingerprint == 'A' ? 'B' : 'A';
    }
    nanortc_t unchanged = a;
    ASSERT_EQ(nanortc_accept_offer(&a, offer, len, answer, sizeof(answer), NULL),
              NANORTC_ERR_CRYPTO);
    ASSERT_MEM_EQ(&a, &unchanged, sizeof(a));
    memcpy(offer, original, len);
    ASSERT_OK(nanortc_accept_offer(&a, offer, len, answer, sizeof(answer), &len));
    ASSERT_OK(nanortc_accept_answer(&b, answer, len));
    ASSERT_TRUE(a.ice_restarting);
    ASSERT_TRUE(b.ice_restarting);
    ASSERT_MEM_EQ(&a.dtls, &previous_a, sizeof(previous_a));
    ASSERT_MEM_EQ(&b.dtls, &previous_b, sizeof(previous_b));
    ASSERT_EQ(a.sdp.local_setup, NANORTC_SDP_SETUP_ACTIVE);
    ASSERT_EQ(b.sdp.local_setup, NANORTC_SDP_SETUP_PASSIVE);
    nanortc_destroy(&a);
    nanortc_destroy(&b);
}

#if NANORTC_FEATURE_AUDIO
TEST(test_negotiation_rejects_codec_change)
{
    nanortc_t a, b;
    init_peer(&a);
    init_peer(&b);
    char offer[8192], answer[8192];
    size_t len;
    ASSERT_OK(nanortc_create_offer(&a, offer, sizeof(offer), &len));
    char *codec = strstr(offer, "opus/48000/2");
    ASSERT_TRUE(codec != NULL);
    memcpy(codec, "PCMA", 4);
    nanortc_t before = b;
    ASSERT_EQ(nanortc_accept_offer(&b, offer, len, answer, sizeof(answer), NULL),
              NANORTC_ERR_NOT_IMPLEMENTED);
    ASSERT_MEM_EQ(&b, &before, sizeof(b));
    nanortc_destroy(&a);
    nanortc_destroy(&b);
}
#endif

TEST_MAIN_BEGIN("test_negotiation")
RUN(test_negotiation_pending_offer);
RUN(test_negotiation_short_buffers_are_atomic);
RUN(test_negotiation_rejects_missing_fingerprint_atomically);
RUN(test_negotiation_candidate_parser_and_atomic_failure);
RUN(test_negotiation_full_candidates_preserve_priority);
#if NANORTC_FEATURE_IPV6
RUN(test_negotiation_trickle_preserves_related_ipv6);
#endif
RUN(test_negotiation_full_related_failure_is_atomic);
RUN(test_negotiation_ice_restart_preserves_dtls);
#if NANORTC_FEATURE_AUDIO
RUN(test_negotiation_rejects_codec_change);
#endif
TEST_MAIN_END
