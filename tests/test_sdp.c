/*
 * nanortc — SDP parser/generator tests
 *
 * SPDX-License-Identifier: MIT
 */

#include "nanortc.h"
#include "nano_rtc_internal.h"
#include "nano_sdp.h"
#include "nano_test.h"
#include "nano_test_config.h"
#include <string.h>

/* ================================================================
 * Parser tests
 * ================================================================ */

static const char *CHROME_OFFER =
    "v=0\r\n"
    "o=- 1234567890 2 IN IP4 127.0.0.1\r\n"
    "s=-\r\n"
    "t=0 0\r\n"
    "a=group:BUNDLE 0\r\n"
    "m=application 9 UDP/DTLS/SCTP webrtc-datachannel\r\n"
    "c=IN IP4 0.0.0.0\r\n"
    "a=mid:0\r\n"
    "a=ice-ufrag:abcd\r\n"
    "a=ice-pwd:secretpassword1234567890\r\n"
    "a=fingerprint:sha-256 AA:BB:CC:DD:EE:FF:00:11:22:33:44:55:66:77:88:99:"
    "AA:BB:CC:DD:EE:FF:00:11:22:33:44:55:66:77:88:99\r\n"
    "a=setup:actpass\r\n"
    "a=sctp-port:5000\r\n"
    "a=max-message-size:262144\r\n";

TEST(test_sdp_parse_chrome_offer)
{
    nano_sdp_t sdp;
    sdp_init(&sdp);

    size_t len = 0;
    while (CHROME_OFFER[len])
        len++;

    ASSERT_OK(sdp_parse(&sdp, CHROME_OFFER, len));
    ASSERT_TRUE(sdp.parsed);

    ASSERT_MEM_EQ(sdp.remote_ufrag, "abcd", 4);
    ASSERT_MEM_EQ(sdp.remote_pwd, "secretpassword1234567890", 24);
    ASSERT_EQ(sdp.remote_sctp_port, 5000);
    ASSERT_EQ(sdp.remote_setup, NANO_SDP_SETUP_ACTPASS);

    /* Fingerprint should start with "sha-256 AA:BB:" */
    ASSERT_TRUE(sdp.remote_fingerprint[0] != '\0');
    ASSERT_MEM_EQ(sdp.remote_fingerprint, "sha-256 AA:BB:", 14);
}

TEST(test_sdp_parse_missing_ufrag)
{
    nano_sdp_t sdp;
    sdp_init(&sdp);

    const char *bad_sdp = "v=0\r\na=ice-pwd:test\r\n";
    size_t len = 0;
    while (bad_sdp[len])
        len++;

    ASSERT_FAIL(sdp_parse(&sdp, bad_sdp, len));
}

TEST(test_sdp_parse_null)
{
    nano_sdp_t sdp;
    sdp_init(&sdp);
    ASSERT_FAIL(sdp_parse(&sdp, NULL, 0));
    ASSERT_FAIL(sdp_parse(NULL, "v=0\r\n", 5));
}

TEST(test_sdp_parse_setup_active)
{
    nano_sdp_t sdp;
    sdp_init(&sdp);

    const char *offer = "v=0\r\n"
                        "a=ice-ufrag:test\r\n"
                        "a=ice-pwd:testpassword\r\n"
                        "a=setup:active\r\n";
    size_t len = 0;
    while (offer[len])
        len++;

    ASSERT_OK(sdp_parse(&sdp, offer, len));
    ASSERT_EQ(sdp.remote_setup, NANO_SDP_SETUP_ACTIVE);
}

/* ================================================================
 * Generator tests
 * ================================================================ */

TEST(test_sdp_generate_answer)
{
    nano_sdp_t sdp;
    sdp_init(&sdp);

    memcpy(sdp.local_ufrag, "abcd1234", 8);
    memcpy(sdp.local_pwd, "password0123456789ab", 20);
    sdp.local_sctp_port = 5000;
    sdp.local_setup = NANO_SDP_SETUP_PASSIVE;

    char buf[1024];
    size_t out_len = 0;
    ASSERT_OK(sdp_generate_answer(&sdp, buf, sizeof(buf), &out_len));
    ASSERT_TRUE(out_len > 0);

    /* Verify key fields are present */
    ASSERT_TRUE(strstr(buf, "v=0") != NULL);
    ASSERT_TRUE(strstr(buf, "m=application 9 UDP/DTLS/SCTP webrtc-datachannel") != NULL);
    ASSERT_TRUE(strstr(buf, "a=ice-ufrag:abcd1234") != NULL);
    ASSERT_TRUE(strstr(buf, "a=ice-pwd:password0123456789ab") != NULL);
    ASSERT_TRUE(strstr(buf, "a=setup:passive") != NULL);
#if NANO_FEATURE_DATACHANNEL
    ASSERT_TRUE(strstr(buf, "a=sctp-port:5000") != NULL);
    ASSERT_TRUE(strstr(buf, "a=max-message-size:262144") != NULL);
#endif
}

TEST(test_sdp_generate_overflow)
{
    nano_sdp_t sdp;
    sdp_init(&sdp);
    memcpy(sdp.local_ufrag, "a", 1);
    memcpy(sdp.local_pwd, "b", 1);

    char buf[16]; /* Way too small */
    size_t out_len = 0;
    ASSERT_FAIL(sdp_generate_answer(&sdp, buf, sizeof(buf), &out_len));
}

TEST(test_sdp_roundtrip)
{
    nano_sdp_t sdp;
    sdp_init(&sdp);
    memcpy(sdp.local_ufrag, "myufrag", 7);
    memcpy(sdp.local_pwd, "mypassword123456", 16);
    sdp.local_sctp_port = 5000;
    sdp.local_setup = NANO_SDP_SETUP_ACTIVE;

    /* Generate */
    char buf[1024];
    size_t out_len = 0;
    ASSERT_OK(sdp_generate_answer(&sdp, buf, sizeof(buf), &out_len));

    /* Parse back */
    nano_sdp_t sdp2;
    sdp_init(&sdp2);
    ASSERT_OK(sdp_parse(&sdp2, buf, out_len));
    ASSERT_MEM_EQ(sdp2.remote_ufrag, "myufrag", 7);
    ASSERT_MEM_EQ(sdp2.remote_pwd, "mypassword123456", 16);
#if NANO_FEATURE_DATACHANNEL
    ASSERT_EQ(sdp2.remote_sctp_port, 5000);
#endif
    ASSERT_EQ(sdp2.remote_setup, NANO_SDP_SETUP_ACTIVE);
}

/* ================================================================
 * Multi-browser SDP compatibility tests
 * ================================================================ */

/* Firefox uses different ordering and formatting than Chrome */
static const char *FIREFOX_OFFER =
    "v=0\r\n"
    "o=mozilla...THIS_IS_SDPARTA 1234 0 IN IP4 0.0.0.0\r\n"
    "s=-\r\n"
    "t=0 0\r\n"
    "a=group:BUNDLE 0\r\n"
    "a=ice-options:trickle\r\n"
    "m=application 9 UDP/DTLS/SCTP webrtc-datachannel\r\n"
    "c=IN IP4 0.0.0.0\r\n"
    "a=sendrecv\r\n"
    "a=ice-ufrag:ffufrag1\r\n"
    "a=ice-pwd:firefoxpassword12345678\r\n"
    "a=fingerprint:sha-256 11:22:33:44:55:66:77:88:99:AA:BB:CC:DD:EE:FF:00:"
    "11:22:33:44:55:66:77:88:99:AA:BB:CC:DD:EE:FF:00\r\n"
    "a=setup:actpass\r\n"
    "a=sctp-port:5000\r\n"
    "a=max-message-size:1073741823\r\n"
    "a=mid:0\r\n";

TEST(test_sdp_parse_firefox_offer)
{
    nano_sdp_t sdp;
    sdp_init(&sdp);

    size_t len = 0;
    while (FIREFOX_OFFER[len])
        len++;

    ASSERT_OK(sdp_parse(&sdp, FIREFOX_OFFER, len));
    ASSERT_TRUE(sdp.parsed);

    ASSERT_MEM_EQ(sdp.remote_ufrag, "ffufrag1", 8);
    ASSERT_MEM_EQ(sdp.remote_pwd, "firefoxpassword12345678", 23);
    ASSERT_EQ(sdp.remote_sctp_port, 5000);
    ASSERT_EQ(sdp.remote_setup, NANO_SDP_SETUP_ACTPASS);
    ASSERT_TRUE(sdp.remote_fingerprint[0] != '\0');
}

/* Safari offer: uses setup:passive when it is the answerer,
 * and has slightly different attribute ordering */
static const char *SAFARI_OFFER =
    "v=0\r\n"
    "o=- 2890844526 2890842807 IN IP4 0.0.0.0\r\n"
    "s=-\r\n"
    "t=0 0\r\n"
    "a=group:BUNDLE 0\r\n"
    "m=application 9 UDP/DTLS/SCTP webrtc-datachannel\r\n"
    "c=IN IP4 0.0.0.0\r\n"
    "a=mid:0\r\n"
    "a=sctp-port:5000\r\n"
    "a=max-message-size:65536\r\n"
    "a=ice-ufrag:sfufrag\r\n"
    "a=ice-pwd:safaripassword1234567\r\n"
    "a=fingerprint:sha-256 AB:CD:EF:01:23:45:67:89:AB:CD:EF:01:23:45:67:89:"
    "AB:CD:EF:01:23:45:67:89:AB:CD:EF:01:23:45:67:89\r\n"
    "a=setup:actpass\r\n";

TEST(test_sdp_parse_safari_offer)
{
    nano_sdp_t sdp;
    sdp_init(&sdp);

    size_t len = 0;
    while (SAFARI_OFFER[len])
        len++;

    ASSERT_OK(sdp_parse(&sdp, SAFARI_OFFER, len));
    ASSERT_TRUE(sdp.parsed);

    ASSERT_MEM_EQ(sdp.remote_ufrag, "sfufrag", 7);
    ASSERT_MEM_EQ(sdp.remote_pwd, "safaripassword1234567", 21);
    ASSERT_EQ(sdp.remote_sctp_port, 5000);
    ASSERT_EQ(sdp.remote_setup, NANO_SDP_SETUP_ACTPASS);
}

/* Minimal offer with only required fields */
TEST(test_sdp_parse_minimal)
{
    nano_sdp_t sdp;
    sdp_init(&sdp);

    const char *offer = "v=0\r\n"
                        "a=ice-ufrag:min\r\n"
                        "a=ice-pwd:minimalpassword\r\n"
                        "a=setup:passive\r\n";
    size_t len = 0;
    while (offer[len])
        len++;

    ASSERT_OK(sdp_parse(&sdp, offer, len));
    ASSERT_MEM_EQ(sdp.remote_ufrag, "min", 3);
    ASSERT_EQ(sdp.remote_setup, NANO_SDP_SETUP_PASSIVE);
}

/* ================================================================
 * libdatachannel SDP compatibility test (TD-007)
 * ================================================================ */

/* Real SDP offer from libdatachannel v0.22.5 — includes embedded candidates,
 * a=sendrecv, a=ice-options:trickle, and setup:actpass */
static const char *LIBDATACHANNEL_OFFER =
    "v=0\r\n"
    "o=rtc 2890844526 0 IN IP4 127.0.0.1\r\n"
    "s=-\r\n"
    "t=0 0\r\n"
    "a=group:BUNDLE 0\r\n"
    "a=msid-semantic:WMS *\r\n"
    "a=ice-options:trickle\r\n"
    "m=application 9 UDP/DTLS/SCTP webrtc-datachannel\r\n"
    "c=IN IP4 0.0.0.0\r\n"
    "a=mid:0\r\n"
    "a=sendrecv\r\n"
    "a=ice-ufrag:ldcufrag1\r\n"
    "a=ice-pwd:ldcpassword123456789012\r\n"
    "a=fingerprint:sha-256 A1:B2:C3:D4:E5:F6:07:18:29:3A:4B:5C:6D:7E:8F:90:"
    "A1:B2:C3:D4:E5:F6:07:18:29:3A:4B:5C:6D:7E:8F:90\r\n"
    "a=setup:actpass\r\n"
    "a=sctp-port:5000\r\n"
    "a=max-message-size:262144\r\n"
    "a=candidate:1 1 UDP 2122252543 192.168.1.100 50000 typ host\r\n"
    "a=candidate:2 1 UDP 2122187007 10.0.0.5 50001 typ host\r\n"
    "a=end-of-candidates\r\n";

TEST(test_sdp_parse_libdatachannel_offer)
{
    nano_sdp_t sdp;
    sdp_init(&sdp);

    size_t len = 0;
    while (LIBDATACHANNEL_OFFER[len])
        len++;

    ASSERT_OK(sdp_parse(&sdp, LIBDATACHANNEL_OFFER, len));
    ASSERT_TRUE(sdp.parsed);

    /* Verify standard fields */
    ASSERT_MEM_EQ(sdp.remote_ufrag, "ldcufrag1", 9);
    ASSERT_MEM_EQ(sdp.remote_pwd, "ldcpassword123456789012", 23);
    ASSERT_EQ(sdp.remote_sctp_port, 5000);
    ASSERT_EQ(sdp.remote_setup, NANO_SDP_SETUP_ACTPASS);
    ASSERT_TRUE(sdp.remote_fingerprint[0] != '\0');
    ASSERT_MEM_EQ(sdp.remote_fingerprint, "sha-256 A1:B2:", 14);

    /* Verify embedded ICE candidates */
    ASSERT_EQ(sdp.candidate_count, 2);

    ASSERT_MEM_EQ(sdp.remote_candidates[0].addr, "192.168.1.100", 13);
    ASSERT_EQ(sdp.remote_candidates[0].port, 50000);

    ASSERT_MEM_EQ(sdp.remote_candidates[1].addr, "10.0.0.5", 8);
    ASSERT_EQ(sdp.remote_candidates[1].port, 50001);
}

/* Verify existing browser offers still parse correctly (no candidates) */
TEST(test_sdp_parse_no_candidates)
{
    nano_sdp_t sdp;
    sdp_init(&sdp);

    size_t len = 0;
    while (CHROME_OFFER[len])
        len++;

    ASSERT_OK(sdp_parse(&sdp, CHROME_OFFER, len));
    ASSERT_EQ(sdp.candidate_count, 0);
}

/* ================================================================
 * Accept offer integration test
 * ================================================================ */

TEST(test_accept_offer_generates_answer)
{
    nano_rtc_t rtc;
    nano_rtc_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.crypto = nano_test_crypto();
    cfg.role = NANO_ROLE_CONTROLLED;

    ASSERT_OK(nano_rtc_init(&rtc, &cfg));

    char answer[2048];
    size_t answer_len = 0;
    int rc = nano_accept_offer(&rtc, CHROME_OFFER, answer, sizeof(answer), &answer_len);
    ASSERT_OK(rc);
    ASSERT_TRUE(answer_len > 0);

    /* Verify answer contains key fields */
    ASSERT_TRUE(strstr(answer, "v=0") != NULL);
    ASSERT_TRUE(strstr(answer, "a=ice-ufrag:") != NULL);
    ASSERT_TRUE(strstr(answer, "a=ice-pwd:") != NULL);
#if NANO_FEATURE_DATACHANNEL
    ASSERT_TRUE(strstr(answer, "a=sctp-port:5000") != NULL);
#endif

    /* Verify ICE credentials were set */
    ASSERT_TRUE(rtc.ice.remote_ufrag[0] != '\0');
    ASSERT_TRUE(rtc.ice.remote_pwd[0] != '\0');
    ASSERT_TRUE(rtc.ice.local_ufrag[0] != '\0');
    ASSERT_TRUE(rtc.ice.local_pwd[0] != '\0');

    nano_rtc_destroy(&rtc);
}

/* ================================================================
 * Audio SDP tests (NANO_HAVE_MEDIA_TRANSPORT)
 * ================================================================ */

#if NANO_HAVE_MEDIA_TRANSPORT

/* Chrome audio+DC offer with Opus */
static const char *CHROME_AUDIO_OFFER =
    "v=0\r\n"
    "o=- 1234567890 2 IN IP4 127.0.0.1\r\n"
    "s=-\r\n"
    "t=0 0\r\n"
    "a=group:BUNDLE 0 1\r\n"
    "m=application 9 UDP/DTLS/SCTP webrtc-datachannel\r\n"
    "c=IN IP4 0.0.0.0\r\n"
    "a=mid:0\r\n"
    "a=ice-ufrag:audiofrag\r\n"
    "a=ice-pwd:audiopassword1234567890\r\n"
    "a=fingerprint:sha-256 AA:BB:CC:DD:EE:FF:00:11:22:33:44:55:66:77:88:99:"
    "AA:BB:CC:DD:EE:FF:00:11:22:33:44:55:66:77:88:99\r\n"
    "a=setup:actpass\r\n"
    "a=sctp-port:5000\r\n"
    "m=audio 9 UDP/TLS/RTP/SAVPF 111\r\n"
    "c=IN IP4 0.0.0.0\r\n"
    "a=mid:1\r\n"
    "a=sendrecv\r\n"
    "a=rtpmap:111 opus/48000/2\r\n"
    "a=fmtp:111 minptime=10;useinbandfec=1\r\n";

TEST(test_sdp_parse_audio_offer)
{
    nano_sdp_t sdp;
    sdp_init(&sdp);

    size_t len = 0;
    while (CHROME_AUDIO_OFFER[len])
        len++;

    ASSERT_OK(sdp_parse(&sdp, CHROME_AUDIO_OFFER, len));
    ASSERT_TRUE(sdp.parsed);

    /* ICE/DTLS fields still parsed */
    ASSERT_MEM_EQ(sdp.remote_ufrag, "audiofrag", 9);
    ASSERT_EQ(sdp.remote_sctp_port, 5000);

    /* Audio fields */
    ASSERT_TRUE(sdp.has_audio);
    ASSERT_EQ(sdp.audio_pt, 111);
    ASSERT_EQ(sdp.audio_sample_rate, 48000);
    ASSERT_EQ(sdp.audio_channels, 2);
}

/* Audio-only offer (no DataChannel) */
static const char *AUDIO_ONLY_OFFER =
    "v=0\r\n"
    "o=- 1 1 IN IP4 0.0.0.0\r\n"
    "s=-\r\n"
    "t=0 0\r\n"
    "a=group:BUNDLE 0\r\n"
    "m=audio 9 UDP/TLS/RTP/SAVPF 96\r\n"
    "c=IN IP4 0.0.0.0\r\n"
    "a=mid:0\r\n"
    "a=sendrecv\r\n"
    "a=ice-ufrag:audioonly\r\n"
    "a=ice-pwd:audiopassword1234\r\n"
    "a=fingerprint:sha-256 11:22:33:44:55:66:77:88:99:AA:BB:CC:DD:EE:FF:00:"
    "11:22:33:44:55:66:77:88:99:AA:BB:CC:DD:EE:FF:00\r\n"
    "a=setup:actpass\r\n"
    "a=rtpmap:96 opus/48000/2\r\n";

TEST(test_sdp_parse_audio_only_offer)
{
    nano_sdp_t sdp;
    sdp_init(&sdp);

    size_t len = 0;
    while (AUDIO_ONLY_OFFER[len])
        len++;

    ASSERT_OK(sdp_parse(&sdp, AUDIO_ONLY_OFFER, len));
    ASSERT_TRUE(sdp.has_audio);
    ASSERT_EQ(sdp.audio_pt, 96);
    ASSERT_EQ(sdp.audio_sample_rate, 48000);
    ASSERT_EQ(sdp.audio_channels, 2);
}

TEST(test_sdp_generate_audio_answer)
{
    nano_sdp_t sdp;
    sdp_init(&sdp);

    memcpy(sdp.local_ufrag, "myufrag", 7);
    memcpy(sdp.local_pwd, "mypassword123456", 16);
    sdp.local_sctp_port = 5000;
    sdp.local_setup = NANO_SDP_SETUP_PASSIVE;
    sdp.has_audio = true;
    sdp.audio_pt = 111;
    sdp.audio_sample_rate = 48000;
    sdp.audio_channels = 2;

    char buf[2048];
    size_t out_len = 0;
    ASSERT_OK(sdp_generate_answer(&sdp, buf, sizeof(buf), &out_len));
    ASSERT_TRUE(out_len > 0);

    /* Verify BUNDLE includes both MIDs */
    ASSERT_TRUE(strstr(buf, "a=group:BUNDLE 0 1") != NULL);

    /* Verify audio m-line */
    ASSERT_TRUE(strstr(buf, "m=audio 9 UDP/TLS/RTP/SAVPF 111") != NULL);
    ASSERT_TRUE(strstr(buf, "a=mid:1") != NULL);
    ASSERT_TRUE(strstr(buf, "a=rtpmap:111 opus/48000/2") != NULL);
}

TEST(test_sdp_audio_roundtrip)
{
    nano_sdp_t sdp;
    sdp_init(&sdp);
    memcpy(sdp.local_ufrag, "rtufrag", 7);
    memcpy(sdp.local_pwd, "rtpassword12345678", 18);
    sdp.local_setup = NANO_SDP_SETUP_ACTIVE;
    sdp.has_audio = true;
    sdp.audio_pt = 111;
    sdp.audio_sample_rate = 48000;
    sdp.audio_channels = 2;

    /* Generate */
    char buf[2048];
    size_t out_len = 0;
    ASSERT_OK(sdp_generate_answer(&sdp, buf, sizeof(buf), &out_len));

    /* Parse back */
    nano_sdp_t sdp2;
    sdp_init(&sdp2);
    ASSERT_OK(sdp_parse(&sdp2, buf, out_len));
    ASSERT_TRUE(sdp2.has_audio);
    ASSERT_EQ(sdp2.audio_pt, 111);
    ASSERT_EQ(sdp2.audio_sample_rate, 48000);
    ASSERT_EQ(sdp2.audio_channels, 2);
}

#endif /* NANO_HAVE_MEDIA_TRANSPORT */

/* ================================================================
 * Test runner
 * ================================================================ */

TEST_MAIN_BEGIN("test_sdp")
RUN(test_sdp_parse_chrome_offer);
RUN(test_sdp_parse_missing_ufrag);
RUN(test_sdp_parse_null);
RUN(test_sdp_parse_setup_active);
RUN(test_sdp_parse_firefox_offer);
RUN(test_sdp_parse_safari_offer);
RUN(test_sdp_parse_minimal);
RUN(test_sdp_generate_answer);
RUN(test_sdp_generate_overflow);
RUN(test_sdp_roundtrip);
RUN(test_sdp_parse_libdatachannel_offer);
RUN(test_sdp_parse_no_candidates);
RUN(test_accept_offer_generates_answer);
#if NANO_HAVE_MEDIA_TRANSPORT
RUN(test_sdp_parse_audio_offer);
RUN(test_sdp_parse_audio_only_offer);
RUN(test_sdp_generate_audio_answer);
RUN(test_sdp_audio_roundtrip);
#endif
TEST_MAIN_END
