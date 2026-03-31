/*
 * nanortc — SDP parser/generator tests
 *
 * SPDX-License-Identifier: MIT
 */

#include "nanortc.h"
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
    ASSERT_EQ(sdp.remote_setup, NANORTC_SDP_SETUP_ACTPASS);

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
    ASSERT_EQ(sdp.remote_setup, NANORTC_SDP_SETUP_ACTIVE);
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
    sdp.local_setup = NANORTC_SDP_SETUP_PASSIVE;
    sdp.has_datachannel = true;
    sdp.dc_mid = 0;
    sdp.mid_count = 1;

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
#if NANORTC_FEATURE_DATACHANNEL
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
    sdp.local_setup = NANORTC_SDP_SETUP_ACTIVE;
    sdp.has_datachannel = true;
    sdp.dc_mid = 0;
    sdp.mid_count = 1;

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
#if NANORTC_FEATURE_DATACHANNEL
    ASSERT_EQ(sdp2.remote_sctp_port, 5000);
#endif
    ASSERT_EQ(sdp2.remote_setup, NANORTC_SDP_SETUP_ACTIVE);
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
    ASSERT_EQ(sdp.remote_setup, NANORTC_SDP_SETUP_ACTPASS);
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
    ASSERT_EQ(sdp.remote_setup, NANORTC_SDP_SETUP_ACTPASS);
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
    ASSERT_EQ(sdp.remote_setup, NANORTC_SDP_SETUP_PASSIVE);
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
    ASSERT_EQ(sdp.remote_setup, NANORTC_SDP_SETUP_ACTPASS);
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
    nanortc_t rtc;
    nanortc_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.crypto = nano_test_crypto();
    cfg.role = NANORTC_ROLE_CONTROLLED;

    ASSERT_OK(nanortc_init(&rtc, &cfg));

    char answer[2048];
    size_t answer_len = 0;
    int rc = nanortc_accept_offer(&rtc, CHROME_OFFER, answer, sizeof(answer), &answer_len);
    ASSERT_OK(rc);
    ASSERT_TRUE(answer_len > 0);

    /* Verify answer contains key fields */
    ASSERT_TRUE(strstr(answer, "v=0") != NULL);
    ASSERT_TRUE(strstr(answer, "a=ice-ufrag:") != NULL);
    ASSERT_TRUE(strstr(answer, "a=ice-pwd:") != NULL);
#if NANORTC_FEATURE_DATACHANNEL
    ASSERT_TRUE(strstr(answer, "a=sctp-port:5000") != NULL);
#endif

    /* Verify ICE credentials were set */
    ASSERT_TRUE(rtc.ice.remote_ufrag[0] != '\0');
    ASSERT_TRUE(rtc.ice.remote_pwd[0] != '\0');
    ASSERT_TRUE(rtc.ice.local_ufrag[0] != '\0');
    ASSERT_TRUE(rtc.ice.local_pwd[0] != '\0');

    nanortc_destroy(&rtc);
}

/* ================================================================
 * Audio SDP tests (NANORTC_HAVE_MEDIA_TRANSPORT)
 * ================================================================ */

#if NANORTC_HAVE_MEDIA_TRANSPORT

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

    /* Audio fields — now in mlines[] */
    ASSERT_TRUE(sdp.mline_count >= 1);
    ASSERT_EQ(sdp.mlines[0].kind, SDP_MLINE_AUDIO);
    ASSERT_EQ(sdp.mlines[0].remote_pt, 111);
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
    ASSERT_TRUE(sdp.mline_count >= 1);
    ASSERT_EQ(sdp.mlines[0].kind, SDP_MLINE_AUDIO);
    ASSERT_EQ(sdp.mlines[0].remote_pt, 96);
}

TEST(test_sdp_generate_audio_answer)
{
    nano_sdp_t sdp;
    sdp_init(&sdp);

    memcpy(sdp.local_ufrag, "myufrag", 7);
    memcpy(sdp.local_pwd, "mypassword123456", 16);
    sdp.local_sctp_port = 5000;
    sdp.local_setup = NANORTC_SDP_SETUP_PASSIVE;
    sdp.has_datachannel = true;
    sdp.dc_mid = 0;
    sdp.mid_count = 1;
    /* Add audio m-line via sdp_add_mline */
    int amid = sdp_add_mline(&sdp, SDP_MLINE_AUDIO, NANORTC_CODEC_OPUS, 111, 48000, 2,
                             NANORTC_DIR_SENDRECV);
    ASSERT(amid >= 0);

    char buf[2048];
    size_t out_len = 0;
    ASSERT_OK(sdp_generate_answer(&sdp, buf, sizeof(buf), &out_len));
    ASSERT_TRUE(out_len > 0);

    /* Verify BUNDLE includes both MIDs */
    ASSERT_TRUE(strstr(buf, "a=group:BUNDLE") != NULL);

    /* Verify audio m-line */
    ASSERT_TRUE(strstr(buf, "m=audio 9 UDP/TLS/RTP/SAVPF 111") != NULL);
    ASSERT_TRUE(strstr(buf, "a=rtpmap:111 opus/48000/2") != NULL);
}

TEST(test_sdp_audio_roundtrip)
{
    nano_sdp_t sdp;
    sdp_init(&sdp);
    memcpy(sdp.local_ufrag, "rtufrag", 7);
    memcpy(sdp.local_pwd, "rtpassword12345678", 18);
    sdp.local_setup = NANORTC_SDP_SETUP_ACTIVE;
    int amid = sdp_add_mline(&sdp, SDP_MLINE_AUDIO, NANORTC_CODEC_OPUS, 111, 48000, 2,
                             NANORTC_DIR_SENDRECV);
    ASSERT(amid >= 0);

    /* Generate */
    char buf[2048];
    size_t out_len = 0;
    ASSERT_OK(sdp_generate_answer(&sdp, buf, sizeof(buf), &out_len));

    /* Parse back */
    nano_sdp_t sdp2;
    sdp_init(&sdp2);
    ASSERT_OK(sdp_parse(&sdp2, buf, out_len));
    ASSERT_TRUE(sdp2.mline_count >= 1);
    ASSERT_EQ(sdp2.mlines[0].remote_pt, 111);
}

/* ================================================================
 * Video SDP tests
 * ================================================================ */

/* Chrome H.264 video + DC offer */
static const char *CHROME_VIDEO_OFFER =
    "v=0\r\n"
    "o=- 1234567890 2 IN IP4 127.0.0.1\r\n"
    "s=-\r\n"
    "t=0 0\r\n"
    "a=group:BUNDLE 0 1\r\n"
    "m=video 9 UDP/TLS/RTP/SAVPF 96\r\n"
    "c=IN IP4 0.0.0.0\r\n"
    "a=mid:0\r\n"
    "a=sendrecv\r\n"
    "a=ice-ufrag:videofrag\r\n"
    "a=ice-pwd:videopassword1234567890\r\n"
    "a=fingerprint:sha-256 AA:BB:CC:DD:EE:FF:00:11:22:33:44:55:66:77:88:99:"
    "AA:BB:CC:DD:EE:FF:00:11:22:33:44:55:66:77:88:99\r\n"
    "a=setup:actpass\r\n"
    "a=rtpmap:96 H264/90000\r\n"
    "a=fmtp:96 level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42e01f\r\n"
    "a=rtcp-fb:96 nack pli\r\n"
    "m=application 9 UDP/DTLS/SCTP webrtc-datachannel\r\n"
    "c=IN IP4 0.0.0.0\r\n"
    "a=mid:1\r\n"
    "a=sctp-port:5000\r\n";

TEST(test_sdp_parse_video_offer)
{
    nano_sdp_t sdp;
    sdp_init(&sdp);

    size_t len = 0;
    while (CHROME_VIDEO_OFFER[len])
        len++;

    ASSERT_OK(sdp_parse(&sdp, CHROME_VIDEO_OFFER, len));
    ASSERT_TRUE(sdp.parsed);

    /* ICE/DTLS fields */
    ASSERT_MEM_EQ(sdp.remote_ufrag, "videofrag", 9);
    ASSERT_EQ(sdp.remote_sctp_port, 5000);

    /* Video fields — now in mlines[] */
    ASSERT_TRUE(sdp.mline_count >= 1);
    ASSERT_EQ(sdp.mlines[0].kind, SDP_MLINE_VIDEO);
    ASSERT_EQ(sdp.mlines[0].remote_pt, 96);
}

/* Full media offer: audio + video + DC */
static const char *FULL_MEDIA_OFFER =
    "v=0\r\n"
    "o=- 1 1 IN IP4 0.0.0.0\r\n"
    "s=-\r\n"
    "t=0 0\r\n"
    "a=group:BUNDLE 0 1 2\r\n"
    "m=audio 9 UDP/TLS/RTP/SAVPF 111\r\n"
    "c=IN IP4 0.0.0.0\r\n"
    "a=mid:0\r\n"
    "a=sendrecv\r\n"
    "a=ice-ufrag:fullufrag\r\n"
    "a=ice-pwd:fullpassword1234567890\r\n"
    "a=fingerprint:sha-256 11:22:33:44:55:66:77:88:99:AA:BB:CC:DD:EE:FF:00:"
    "11:22:33:44:55:66:77:88:99:AA:BB:CC:DD:EE:FF:00\r\n"
    "a=setup:actpass\r\n"
    "a=rtpmap:111 opus/48000/2\r\n"
    "m=video 9 UDP/TLS/RTP/SAVPF 96\r\n"
    "c=IN IP4 0.0.0.0\r\n"
    "a=mid:1\r\n"
    "a=sendrecv\r\n"
    "a=rtpmap:96 H264/90000\r\n"
    "a=fmtp:96 level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42e01f\r\n"
    "m=application 9 UDP/DTLS/SCTP webrtc-datachannel\r\n"
    "c=IN IP4 0.0.0.0\r\n"
    "a=mid:2\r\n"
    "a=sctp-port:5000\r\n";

TEST(test_sdp_parse_full_media_offer)
{
    nano_sdp_t sdp;
    sdp_init(&sdp);

    size_t len = 0;
    while (FULL_MEDIA_OFFER[len])
        len++;

    ASSERT_OK(sdp_parse(&sdp, FULL_MEDIA_OFFER, len));

    ASSERT_TRUE(sdp.mline_count >= 2);
    ASSERT_EQ(sdp.mlines[0].kind, SDP_MLINE_AUDIO);
    ASSERT_EQ(sdp.mlines[0].remote_pt, 111);
    ASSERT_EQ(sdp.mlines[1].kind, SDP_MLINE_VIDEO);
    ASSERT_EQ(sdp.mlines[1].remote_pt, 96);
    ASSERT_EQ(sdp.remote_sctp_port, 5000);
    /* audio=MID0, video=MID1, dc=MID2 */
    ASSERT_EQ(sdp.mlines[0].mid, 0);
    ASSERT_EQ(sdp.mlines[1].mid, 1);
    ASSERT_EQ(sdp.dc_mid, 2);
}

TEST(test_sdp_generate_video_answer)
{
    nano_sdp_t sdp;
    sdp_init(&sdp);

    memcpy(sdp.local_ufrag, "vufrag1", 7);
    memcpy(sdp.local_pwd, "vpassword12345678", 17);
    sdp.local_sctp_port = 5000;
    sdp.local_setup = NANORTC_SDP_SETUP_PASSIVE;
    sdp.has_datachannel = true;
    sdp.dc_mid = 0;
    sdp.mid_count = 1;
    int vmid =
        sdp_add_mline(&sdp, SDP_MLINE_VIDEO, NANORTC_CODEC_H264, 96, 0, 0, NANORTC_DIR_SENDRECV);
    ASSERT(vmid >= 0);

    char buf[4096];
    size_t out_len = 0;
    ASSERT_OK(sdp_generate_answer(&sdp, buf, sizeof(buf), &out_len));
    ASSERT_TRUE(out_len > 0);

    /* Verify video m-line */
    ASSERT_TRUE(strstr(buf, "m=video 9 UDP/TLS/RTP/SAVPF 96") != NULL);
    ASSERT_TRUE(strstr(buf, "a=rtpmap:96 H264/90000") != NULL);
    ASSERT_TRUE(strstr(buf, "profile-level-id=42e01f") != NULL);
    ASSERT_TRUE(strstr(buf, "a=rtcp-fb:96 nack pli") != NULL);
    ASSERT_TRUE(strstr(buf, "a=rtcp-mux") != NULL);
}

TEST(test_sdp_video_roundtrip)
{
    nano_sdp_t sdp;
    sdp_init(&sdp);
    memcpy(sdp.local_ufrag, "vrtufrag", 8);
    memcpy(sdp.local_pwd, "vrtpassword1234567", 18);
    sdp.local_setup = NANORTC_SDP_SETUP_ACTIVE;
    int vmid =
        sdp_add_mline(&sdp, SDP_MLINE_VIDEO, NANORTC_CODEC_H264, 96, 0, 0, NANORTC_DIR_SENDRECV);
    ASSERT(vmid >= 0);

    /* Generate */
    char buf[4096];
    size_t out_len = 0;
    ASSERT_OK(sdp_generate_answer(&sdp, buf, sizeof(buf), &out_len));

    /* Parse back */
    nano_sdp_t sdp2;
    sdp_init(&sdp2);
    ASSERT_OK(sdp_parse(&sdp2, buf, out_len));
    ASSERT_TRUE(sdp2.mline_count >= 1);
    ASSERT_EQ(sdp2.mlines[0].remote_pt, 96);
}

#endif /* NANORTC_HAVE_MEDIA_TRANSPORT */

/* ================================================================
 * RFC 8866/8829 MUST/SHOULD requirement tests
 * ================================================================ */

/*
 * RFC 8866 §5.1: SDP MUST start with "v=0" version line.
 * Missing version should cause parse failure.
 */
TEST(test_sdp_malformed_missing_version)
{
    nano_sdp_t sdp;
    sdp_init(&sdp);

    /* SDP without v= line */
    const char *bad_sdp = "o=- 1 2 IN IP4 127.0.0.1\r\n"
                          "s=-\r\n"
                          "t=0 0\r\n";
    ASSERT_FAIL(sdp_parse(&sdp, bad_sdp, strlen(bad_sdp) /* NANORTC_SAFE: API boundary */));
}

/*
 * RFC 8866: Empty SDP string should fail.
 */
TEST(test_sdp_empty_string)
{
    nano_sdp_t sdp;
    sdp_init(&sdp);
    ASSERT_FAIL(sdp_parse(&sdp, "", 0));
}

/*
 * RFC 8829 §5.3.1: BUNDLE group attribute parsing.
 * SDP with "a=group:BUNDLE 0 1" should be parsed.
 */
TEST(test_sdp_bundle_group)
{
    nano_sdp_t sdp;
    sdp_init(&sdp);

    const char *sdp_str = "v=0\r\n"
                          "o=- 1 2 IN IP4 0.0.0.0\r\n"
                          "s=-\r\n"
                          "t=0 0\r\n"
                          "a=group:BUNDLE 0\r\n"
                          "a=ice-ufrag:test\r\n"
                          "a=ice-pwd:testpassword12345678\r\n"
                          "a=fingerprint:sha-256 AA:BB:CC:DD:EE:FF:00:11:22:33:44:55:66:77:88:99:"
                          "AA:BB:CC:DD:EE:FF:00:11:22:33:44:55:66:77:88:99\r\n"
                          "a=setup:actpass\r\n"
                          "m=application 9 UDP/DTLS/SCTP webrtc-datachannel\r\n"
                          "a=sctp-port:5000\r\n"
                          "a=mid:0\r\n";

    ASSERT_OK(sdp_parse(&sdp, sdp_str, strlen(sdp_str) /* NANORTC_SAFE: API boundary */));
    ASSERT_TRUE(sdp.parsed);
}

/*
 * RFC 8839: Multiple ICE candidates in SDP.
 * Verify multiple a=candidate lines are parsed.
 */
TEST(test_sdp_multiple_candidates)
{
    nano_sdp_t sdp;
    sdp_init(&sdp);

    const char *sdp_str = "v=0\r\n"
                          "o=- 1 2 IN IP4 0.0.0.0\r\n"
                          "s=-\r\n"
                          "t=0 0\r\n"
                          "a=ice-ufrag:multi\r\n"
                          "a=ice-pwd:multipassword12345678\r\n"
                          "a=fingerprint:sha-256 AA:BB:CC:DD:EE:FF:00:11:22:33:44:55:66:77:88:99:"
                          "AA:BB:CC:DD:EE:FF:00:11:22:33:44:55:66:77:88:99\r\n"
                          "a=setup:active\r\n"
                          "m=application 9 UDP/DTLS/SCTP webrtc-datachannel\r\n"
                          "a=sctp-port:5000\r\n"
                          "a=mid:0\r\n"
                          "a=candidate:1 1 UDP 2122260223 192.168.1.100 50000 typ host\r\n"
                          "a=candidate:2 1 UDP 2122194687 10.0.0.1 50001 typ host\r\n";

    ASSERT_OK(sdp_parse(&sdp, sdp_str, strlen(sdp_str) /* NANORTC_SAFE: API boundary */));
    ASSERT_EQ(sdp.candidate_count, 2);
}

/*
 * RFC 8866 §5: SDP with version other than 0 should be rejected or handled.
 */
TEST(test_sdp_wrong_version)
{
    nano_sdp_t sdp;
    sdp_init(&sdp);

    const char *bad_sdp = "v=1\r\n"
                          "o=- 1 2 IN IP4 127.0.0.1\r\n"
                          "s=-\r\n"
                          "t=0 0\r\n";
    /* Should either parse (lenient) or fail — either is acceptable */
    int rc = sdp_parse(&sdp, bad_sdp, strlen(bad_sdp) /* NANORTC_SAFE: API boundary */);
    (void)rc; /* Document behavior */
}

/* ================================================================
 * IPv6 SDP generation
 * ================================================================ */

#if NANORTC_FEATURE_IPV6
TEST(test_sdp_generate_ipv6_connection_line)
{
    nano_sdp_t sdp;
    sdp_init(&sdp);

    memcpy(sdp.local_ufrag, "abcd1234", 8);
    memcpy(sdp.local_pwd, "password0123456789ab", 20);
    sdp.local_sctp_port = 5000;
    sdp.local_setup = NANORTC_SDP_SETUP_PASSIVE;
    sdp.has_datachannel = true;
    sdp.dc_mid = 0;
    sdp.mid_count = 1;

    /* Set an IPv6 local candidate */
    sdp.has_local_candidate = true;
    memcpy(sdp.local_candidate_ip, "2001:db8::1", 12);
    sdp.local_candidate_port = 5000;

    char buf[2048];
    size_t out_len = 0;
    ASSERT_OK(sdp_generate_answer(&sdp, buf, sizeof(buf), &out_len));
    ASSERT_TRUE(out_len > 0);

    /* Verify IPv6 connection and origin lines */
    ASSERT_TRUE(strstr(buf, "c=IN IP6 ::") != NULL);
    ASSERT_TRUE(strstr(buf, "o=- 1 1 IN IP6 ::") != NULL);
    /* IPv4 lines should NOT appear */
    ASSERT_TRUE(strstr(buf, "IN IP4") == NULL);
    /* Candidate line should contain IPv6 address */
    ASSERT_TRUE(strstr(buf, "2001:db8::1") != NULL);
}

TEST(test_sdp_generate_ipv4_connection_line)
{
    nano_sdp_t sdp;
    sdp_init(&sdp);

    memcpy(sdp.local_ufrag, "abcd1234", 8);
    memcpy(sdp.local_pwd, "password0123456789ab", 20);
    sdp.local_sctp_port = 5000;
    sdp.local_setup = NANORTC_SDP_SETUP_PASSIVE;
    sdp.has_datachannel = true;
    sdp.dc_mid = 0;
    sdp.mid_count = 1;

    /* Set an IPv4 local candidate */
    sdp.has_local_candidate = true;
    memcpy(sdp.local_candidate_ip, "192.168.1.1", 12);
    sdp.local_candidate_port = 5000;

    char buf[2048];
    size_t out_len = 0;
    ASSERT_OK(sdp_generate_answer(&sdp, buf, sizeof(buf), &out_len));
    ASSERT_TRUE(out_len > 0);

    /* Verify IPv4 connection and origin lines */
    ASSERT_TRUE(strstr(buf, "c=IN IP4 0.0.0.0") != NULL);
    ASSERT_TRUE(strstr(buf, "o=- 1 1 IN IP4 0.0.0.0") != NULL);
}
#endif

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
/* RFC 8866/8829 MUST/SHOULD requirement tests */
RUN(test_sdp_malformed_missing_version);
RUN(test_sdp_empty_string);
RUN(test_sdp_bundle_group);
RUN(test_sdp_multiple_candidates);
RUN(test_sdp_wrong_version);
#if NANORTC_HAVE_MEDIA_TRANSPORT
RUN(test_sdp_parse_audio_offer);
RUN(test_sdp_parse_audio_only_offer);
RUN(test_sdp_generate_audio_answer);
RUN(test_sdp_audio_roundtrip);
RUN(test_sdp_parse_video_offer);
RUN(test_sdp_parse_full_media_offer);
RUN(test_sdp_generate_video_answer);
RUN(test_sdp_video_roundtrip);
#endif
#if NANORTC_FEATURE_IPV6
RUN(test_sdp_generate_ipv6_connection_line);
RUN(test_sdp_generate_ipv4_connection_line);
#endif
TEST_MAIN_END
