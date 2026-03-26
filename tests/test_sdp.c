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
    while (CHROME_OFFER[len]) len++;

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
    while (bad_sdp[len]) len++;

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

    const char *offer =
        "v=0\r\n"
        "a=ice-ufrag:test\r\n"
        "a=ice-pwd:testpassword\r\n"
        "a=setup:active\r\n";
    size_t len = 0;
    while (offer[len]) len++;

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
    ASSERT_TRUE(strstr(buf, "a=sctp-port:5000") != NULL);
    ASSERT_TRUE(strstr(buf, "a=max-message-size:262144") != NULL);
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
    ASSERT_EQ(sdp2.remote_sctp_port, 5000);
    ASSERT_EQ(sdp2.remote_setup, NANO_SDP_SETUP_ACTIVE);
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
    int rc = nano_accept_offer(&rtc, CHROME_OFFER, answer, sizeof(answer));
    ASSERT_TRUE(rc > 0); /* Returns answer length */

    /* Verify answer contains key fields */
    ASSERT_TRUE(strstr(answer, "v=0") != NULL);
    ASSERT_TRUE(strstr(answer, "a=ice-ufrag:") != NULL);
    ASSERT_TRUE(strstr(answer, "a=ice-pwd:") != NULL);
    ASSERT_TRUE(strstr(answer, "a=sctp-port:5000") != NULL);

    /* Verify ICE credentials were set */
    ASSERT_TRUE(rtc.ice.remote_ufrag[0] != '\0');
    ASSERT_TRUE(rtc.ice.remote_pwd[0] != '\0');
    ASSERT_TRUE(rtc.ice.local_ufrag[0] != '\0');
    ASSERT_TRUE(rtc.ice.local_pwd[0] != '\0');

    nano_rtc_destroy(&rtc);
}

/* ================================================================
 * Test runner
 * ================================================================ */

TEST_MAIN_BEGIN("test_sdp")
    RUN(test_sdp_parse_chrome_offer);
    RUN(test_sdp_parse_missing_ufrag);
    RUN(test_sdp_parse_null);
    RUN(test_sdp_parse_setup_active);
    RUN(test_sdp_generate_answer);
    RUN(test_sdp_generate_overflow);
    RUN(test_sdp_roundtrip);
    RUN(test_accept_offer_generates_answer);
TEST_MAIN_END
