/*
 * nanortc — End-to-end tests
 *
 * Two nanortc_t instances wired together in memory.
 * No network, no sockets — pure Sans I/O loopback.
 *
 * As modules are implemented, this test grows to cover
 * the full connection lifecycle:
 *   ICE → DTLS → SCTP → DataChannel
 *
 * SPDX-License-Identifier: MIT
 */

#include "nanortc.h"
#include "nanortc_crypto.h"
#include "nano_test.h"
#include "nano_test_config.h"
#include <string.h>

/* ----------------------------------------------------------------
 * E2E helpers — Sans I/O data bridge
 * ---------------------------------------------------------------- */

/* Helper: create a default config */
static nanortc_config_t e2e_default_config(void)
{
    nanortc_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.crypto = nano_test_crypto();
    cfg.role = NANORTC_ROLE_CONTROLLED;
#if NANORTC_FEATURE_AUDIO
    cfg.jitter_depth_ms = 100;
    cfg.audio_codec = NANORTC_CODEC_OPUS;
    cfg.audio_sample_rate = 48000;
    cfg.audio_channels = 1;
    cfg.audio_direction = NANORTC_DIR_SENDRECV;
#endif
#if NANORTC_FEATURE_VIDEO
    cfg.video_codec = NANORTC_CODEC_H264;
    cfg.video_direction = NANORTC_DIR_SENDRECV;
#endif
    return cfg;
}

/*
 * Relay all TRANSMIT outputs from `from` into `to` as received data.
 * Returns number of packets relayed, or negative on error.
 */
static int e2e_relay(nanortc_t *from, nanortc_t *to, uint32_t now_ms)
{
    int relayed = 0;
    nanortc_output_t out;

    while (nanortc_poll_output(from, &out) == NANORTC_OK) {
        if (out.type == NANORTC_OUTPUT_TRANSMIT) {
            nanortc_addr_t src;
            memset(&src, 0, sizeof(src));
            src.family = 4;
            src.addr[0] = 192;
            src.addr[1] = 168;
            src.addr[2] = 1;
            src.addr[3] = 1;
            src.port = 9999;

            int rc = nanortc_handle_receive(to, now_ms, out.transmit.data, out.transmit.len, &src);
            (void)rc;
            relayed++;
        }
    }
    return relayed;
}

/*
 * Pump both instances: relay outputs from A→B and B→A
 * until neither produces new output or max_rounds reached.
 * Returns total packets relayed.
 */
static int e2e_pump(nanortc_t *a, nanortc_t *b, uint32_t now_ms, int max_rounds)
{
    int total = 0;
    for (int i = 0; i < max_rounds; i++) {
        int ra = e2e_relay(a, b, now_ms);
        int rb = e2e_relay(b, a, now_ms);
        if (ra <= 0 && rb <= 0) {
            break;
        }
        total += (ra > 0 ? ra : 0) + (rb > 0 ? rb : 0);
    }
    return total;
}

/* ----------------------------------------------------------------
 * Tests
 * ---------------------------------------------------------------- */

TEST(test_e2e_init_pair)
{
    /* Two instances must coexist independently (no global state) */
    nanortc_t server, client;
    nanortc_config_t cfg = e2e_default_config();

    ASSERT_OK(nanortc_init(&server, &cfg));
    ASSERT_OK(nanortc_init(&client, &cfg));

    /* Both should be in NEW state */
    ASSERT_EQ(server.state, NANORTC_STATE_NEW);
    ASSERT_EQ(client.state, NANORTC_STATE_NEW);

    /* Destroying one should not affect the other */
    nanortc_destroy(&server);
    ASSERT_EQ(server.state, NANORTC_STATE_CLOSED);
    ASSERT_EQ(client.state, NANORTC_STATE_NEW);

    nanortc_destroy(&client);
}

TEST(test_e2e_stubs_not_implemented)
{
    /* APIs that are still stubs should return NANORTC_ERR_NOT_IMPLEMENTED */
    nanortc_t rtc;
    nanortc_config_t cfg = e2e_default_config();
    ASSERT_OK(nanortc_init(&rtc, &cfg));

    char buf[2048];
    /* nanortc_accept_offer now works (parses SDP), but invalid SDP returns parse error */
    ASSERT_FAIL(nanortc_accept_offer(&rtc, "v=0\r\n", buf, sizeof(buf), NULL));
    /* nanortc_create_offer now works on NEW state */
    ASSERT_OK(nanortc_create_offer(&rtc, buf, sizeof(buf), NULL));
    /* After create_offer, state is still NEW (waiting for answer), accept_answer parses SDP */
    ASSERT_FAIL(nanortc_accept_answer(&rtc, "v=0\r\n"));
    ASSERT_OK(nanortc_add_local_candidate(&rtc, "192.168.1.1", 9999));
    /* nanortc_add_remote_candidate now parses SDP candidate; invalid format returns parse error */
    ASSERT_EQ(nanortc_add_remote_candidate(&rtc, "candidate:..."), NANORTC_ERR_PARSE);
    /* Valid candidate succeeds */
    ASSERT_OK(nanortc_add_remote_candidate(
        &rtc, "candidate:0 1 UDP 2122260223 192.168.1.100 50000 typ host"));
    ASSERT_OK(nanortc_add_remote_candidate(&rtc, "192.168.1.200 60000"));

    /* nanortc_handle_receive and nanortc_handle_timeout are now implemented */

    uint8_t data[] = {0x00, 0x01, 0x00, 0x00};

#if NANORTC_FEATURE_DATACHANNEL
    /* nanortc_send_datachannel now returns NANORTC_ERR_STATE (not connected) */
    ASSERT_EQ(nanortc_send_datachannel(&rtc, 0, data, sizeof(data)), NANORTC_ERR_STATE);
    ASSERT_EQ(nanortc_send_datachannel_string(&rtc, 0, "hello"), NANORTC_ERR_STATE);
#endif

#if NANORTC_FEATURE_AUDIO
    ASSERT_EQ(nanortc_send_audio(&rtc, 0, data, sizeof(data)), NANORTC_ERR_NOT_IMPLEMENTED);
#endif

#if NANORTC_FEATURE_VIDEO
    ASSERT_EQ(nanortc_send_video(&rtc, 0, data, sizeof(data), 1), NANORTC_ERR_NOT_IMPLEMENTED);
    ASSERT_EQ(nanortc_request_keyframe(&rtc), NANORTC_ERR_NOT_IMPLEMENTED);
#endif

    nanortc_destroy(&rtc);
}

TEST(test_e2e_loopback_skeleton)
{
    /*
     * Full loopback skeleton:
     *   server: init → accept_offer → poll_output
     *   client: init → create_offer → poll_output
     *   pump: relay outputs between them
     *
     * SDP is still a stub, so no data flows from SDP path.
     * ICE loopback tested separately below.
     */
    nanortc_t server, client;
    nanortc_config_t cfg = e2e_default_config();

    ASSERT_OK(nanortc_init(&server, &cfg));

    cfg.role = NANORTC_ROLE_CONTROLLING;
    ASSERT_OK(nanortc_init(&client, &cfg));

    /* Client creates offer (now implemented) */
    char offer[2048];
    int rc = nanortc_create_offer(&client, offer, sizeof(offer), NULL);
    ASSERT_OK(rc);

    /* No transmit output should be queued yet (no remote candidate) */
    nanortc_output_t out;
    ASSERT_EQ(nanortc_poll_output(&server, &out), NANORTC_ERR_NO_DATA);
    ASSERT_EQ(nanortc_poll_output(&client, &out), NANORTC_ERR_NO_DATA);

    /* Pump should relay 0 packets (nothing to relay) */
    uint32_t now_ms = 0;
    int relayed = e2e_pump(&server, &client, now_ms, 10);
    ASSERT_EQ(relayed, 0);

    nanortc_destroy(&server);
    nanortc_destroy(&client);
}

TEST(test_e2e_multiple_instances)
{
    /*
     * Verify 4 instances can coexist (MCU might host multiple connections).
     * No global state means no interference.
     */
    nanortc_t instances[4];
    nanortc_config_t cfg = e2e_default_config();

    for (int i = 0; i < 4; i++) {
        ASSERT_OK(nanortc_init(&instances[i], &cfg));
        ASSERT_EQ(instances[i].state, NANORTC_STATE_NEW);
    }

    /* Destroy in reverse order */
    for (int i = 3; i >= 0; i--) {
        nanortc_destroy(&instances[i]);
        ASSERT_EQ(instances[i].state, NANORTC_STATE_CLOSED);

        /* Earlier instances should still be NEW */
        for (int j = 0; j < i; j++) {
            ASSERT_EQ(instances[j].state, NANORTC_STATE_NEW);
        }
    }
}

TEST(test_e2e_demux_byte_ranges)
{
    /*
     * Verify RFC 7983 demux byte ranges are handled without crash.
     * STUN path now returns parse errors for malformed packets.
     * DTLS/SRTP still return NOT_IMPLEMENTED.
     */
    nanortc_t rtc;
    nanortc_config_t cfg = e2e_default_config();
    ASSERT_OK(nanortc_init(&rtc, &cfg));

    nanortc_addr_t addr;
    memset(&addr, 0, sizeof(addr));
    addr.family = 4;

    /* STUN range: 0x00-0x03 (malformed — no valid STUN, returns parse error) */
    uint8_t stun_pkt[20] = {0x00, 0x01, 0x00, 0x00};
    int rc = nanortc_handle_receive(&rtc, 0, stun_pkt, sizeof(stun_pkt), &addr);
    ASSERT_TRUE(rc < 0); /* parse error expected for malformed STUN */

    /* DTLS range: 0x14-0x40 — rejected before ICE connects */
    uint8_t dtls_pkt[20] = {0x14, 0xFE, 0xFD};
    ASSERT_EQ(nanortc_handle_receive(&rtc, 0, dtls_pkt, sizeof(dtls_pkt), &addr),
              NANORTC_ERR_STATE);

    /* SRTP range: 0x80-0xBF */
    uint8_t srtp_pkt[20] = {0x80, 0x60};
    ASSERT_EQ(nanortc_handle_receive(&rtc, 0, srtp_pkt, sizeof(srtp_pkt), &addr),
              NANORTC_ERR_NOT_IMPLEMENTED);

    /* Edge cases: null data returns INVALID_PARAM */
    ASSERT_EQ(nanortc_handle_receive(&rtc, 0, NULL, 0, &addr), NANORTC_ERR_INVALID_PARAM);

    /* Unknown byte range */
    uint8_t one = 0xFF;
    ASSERT_EQ(nanortc_handle_receive(&rtc, 0, &one, 1, &addr), NANORTC_ERR_PROTOCOL);

    nanortc_destroy(&rtc);
}

TEST(test_e2e_ice_loopback)
{
    /*
     * Full ICE loopback: controlling sends STUN check,
     * controlled responds, controlling receives response.
     * Both reach ICE_CONNECTED.
     */
    nanortc_t offerer, answerer;

    /* Offerer = controlling role */
    nanortc_config_t off_cfg = e2e_default_config();
    off_cfg.role = NANORTC_ROLE_CONTROLLING;
    ASSERT_OK(nanortc_init(&offerer, &off_cfg));

    /* Answerer = controlled role */
    nanortc_config_t ans_cfg = e2e_default_config();
    ans_cfg.role = NANORTC_ROLE_CONTROLLED;
    ASSERT_OK(nanortc_init(&answerer, &ans_cfg));

    /* Set matching ICE credentials */
    memcpy(offerer.ice.local_ufrag, "OFF", 4);
    offerer.ice.local_ufrag_len = 3;
    memcpy(offerer.ice.local_pwd, "offerer-password-1234", 22);
    offerer.ice.local_pwd_len = 21;
    memcpy(offerer.ice.remote_ufrag, "ANS", 4);
    offerer.ice.remote_ufrag_len = 3;
    memcpy(offerer.ice.remote_pwd, "answerer-password-5678", 23);
    offerer.ice.remote_pwd_len = 22;
    offerer.ice.tie_breaker = 0x1234567890ABCDEFull;

    memcpy(answerer.ice.local_ufrag, "ANS", 4);
    answerer.ice.local_ufrag_len = 3;
    memcpy(answerer.ice.local_pwd, "answerer-password-5678", 23);
    answerer.ice.local_pwd_len = 22;
    memcpy(answerer.ice.remote_ufrag, "OFF", 4);
    answerer.ice.remote_ufrag_len = 3;
    memcpy(answerer.ice.remote_pwd, "offerer-password-1234", 22);
    answerer.ice.remote_pwd_len = 21;

    /* Set remote candidate address on offerer (where to send checks) */
    offerer.ice.remote_candidates[0].family = 4;
    offerer.ice.remote_candidates[0].addr[0] = 192;
    offerer.ice.remote_candidates[0].addr[1] = 168;
    offerer.ice.remote_candidates[0].addr[2] = 1;
    offerer.ice.remote_candidates[0].addr[3] = 2;
    offerer.ice.remote_candidates[0].port = 5000;
    offerer.ice.remote_candidate_count = 1;

    /* Step 1: offerer generates STUN Binding Request via timeout */
    uint32_t now_ms = 100;
    ASSERT_OK(nanortc_handle_timeout(&offerer, now_ms));
    ASSERT_EQ(offerer.ice.state, NANORTC_ICE_STATE_CHECKING);

    /* Step 2: relay offerer's STUN request to answerer */
    nanortc_output_t out;
    ASSERT_OK(nanortc_poll_output(&offerer, &out));
    ASSERT_EQ(out.type, NANORTC_OUTPUT_TRANSMIT);
    ASSERT_TRUE(out.transmit.len > 0);

    /* Save request data (pointer references rtc->stun_buf, must copy) */
    uint8_t saved_req[256];
    size_t saved_req_len = out.transmit.len;
    memcpy(saved_req, out.transmit.data, saved_req_len);

    /* Drain the TIMEOUT output from handle_timeout */
    nanortc_output_t tout;
    ASSERT_OK(nanortc_poll_output(&offerer, &tout));
    ASSERT_EQ(tout.type, NANORTC_OUTPUT_TIMEOUT);

    /* Feed the STUN request into the answerer */
    nanortc_addr_t offerer_addr;
    memset(&offerer_addr, 0, sizeof(offerer_addr));
    offerer_addr.family = 4;
    offerer_addr.addr[0] = 192;
    offerer_addr.addr[1] = 168;
    offerer_addr.addr[2] = 1;
    offerer_addr.addr[3] = 1;
    offerer_addr.port = 9999;

    ASSERT_OK(nanortc_handle_receive(&answerer, now_ms, saved_req, saved_req_len, &offerer_addr));

    /* Answerer: ICE connected → DTLS handshaking (server waits for ClientHello) */
    ASSERT_EQ(answerer.ice.state, NANORTC_ICE_STATE_CONNECTED);
    ASSERT_EQ(answerer.state, NANORTC_STATE_DTLS_HANDSHAKING);

    /* Step 3: relay answerer's STUN response back to offerer */
    nanortc_output_t ans_out;
    ASSERT_OK(nanortc_poll_output(&answerer, &ans_out));
    ASSERT_EQ(ans_out.type, NANORTC_OUTPUT_TRANSMIT);

    /* Save response data (pointer references rtc->stun_buf) */
    uint8_t saved_resp[256];
    size_t saved_resp_len = ans_out.transmit.len;
    memcpy(saved_resp, ans_out.transmit.data, saved_resp_len);

    /* Check for ICE_CONNECTED event on answerer */
    nanortc_output_t evt;
    ASSERT_OK(nanortc_poll_output(&answerer, &evt));
    ASSERT_EQ(evt.type, NANORTC_OUTPUT_EVENT);
    ASSERT_EQ(evt.event.type, NANORTC_EVENT_ICE_CONNECTED);

    /* Feed response into offerer */
    nanortc_addr_t answerer_addr;
    memset(&answerer_addr, 0, sizeof(answerer_addr));
    answerer_addr.family = 4;
    answerer_addr.addr[0] = 192;
    answerer_addr.addr[1] = 168;
    answerer_addr.addr[2] = 1;
    answerer_addr.addr[3] = 2;
    answerer_addr.port = 5000;

    ASSERT_OK(nanortc_handle_receive(&offerer, now_ms, saved_resp, saved_resp_len, &answerer_addr));

    /* Offerer: ICE connected → DTLS handshaking (client sends ClientHello) */
    ASSERT_EQ(offerer.ice.state, NANORTC_ICE_STATE_CONNECTED);
    ASSERT_EQ(offerer.state, NANORTC_STATE_DTLS_HANDSHAKING);

    /* ICE_CONNECTED event should be queued for offerer */
    nanortc_output_t off_evt;
    ASSERT_OK(nanortc_poll_output(&offerer, &off_evt));
    ASSERT_EQ(off_evt.type, NANORTC_OUTPUT_EVENT);
    ASSERT_EQ(off_evt.event.type, NANORTC_EVENT_ICE_CONNECTED);

    /* Offerer (client role) should have a ClientHello TRANSMIT output */
    nanortc_output_t ch_out;
    ASSERT_OK(nanortc_poll_output(&offerer, &ch_out));
    ASSERT_EQ(ch_out.type, NANORTC_OUTPUT_TRANSMIT);
    ASSERT_TRUE(ch_out.transmit.len > 0);

    nanortc_destroy(&offerer);
    nanortc_destroy(&answerer);
}

/* ----------------------------------------------------------------
 * ICE → DTLS full handshake E2E test
 * ---------------------------------------------------------------- */

/*
 * Helper: set up ICE credentials for matched offerer/answerer pair.
 */
static void e2e_setup_ice_creds(nanortc_t *offerer, nanortc_t *answerer)
{
    memcpy(offerer->ice.local_ufrag, "OFF", 4);
    offerer->ice.local_ufrag_len = 3;
    memcpy(offerer->ice.local_pwd, "offerer-password-1234", 22);
    offerer->ice.local_pwd_len = 21;
    memcpy(offerer->ice.remote_ufrag, "ANS", 4);
    offerer->ice.remote_ufrag_len = 3;
    memcpy(offerer->ice.remote_pwd, "answerer-password-5678", 23);
    offerer->ice.remote_pwd_len = 22;
    offerer->ice.tie_breaker = 0x1234567890ABCDEFull;

    memcpy(answerer->ice.local_ufrag, "ANS", 4);
    answerer->ice.local_ufrag_len = 3;
    memcpy(answerer->ice.local_pwd, "answerer-password-5678", 23);
    answerer->ice.local_pwd_len = 22;
    memcpy(answerer->ice.remote_ufrag, "OFF", 4);
    answerer->ice.remote_ufrag_len = 3;
    memcpy(answerer->ice.remote_pwd, "offerer-password-1234", 22);
    answerer->ice.remote_pwd_len = 21;

    offerer->ice.remote_candidates[0].family = 4;
    offerer->ice.remote_candidates[0].addr[0] = 192;
    offerer->ice.remote_candidates[0].addr[1] = 168;
    offerer->ice.remote_candidates[0].addr[2] = 1;
    offerer->ice.remote_candidates[0].addr[3] = 2;
    offerer->ice.remote_candidates[0].port = 5000;
    offerer->ice.remote_candidate_count = 1;
}

TEST(test_e2e_ice_dtls_loopback)
{
    nanortc_t offerer, answerer;

    nanortc_config_t off_cfg = e2e_default_config();
    off_cfg.role = NANORTC_ROLE_CONTROLLING;
    ASSERT_OK(nanortc_init(&offerer, &off_cfg));

    nanortc_config_t ans_cfg = e2e_default_config();
    ans_cfg.role = NANORTC_ROLE_CONTROLLED;
    ASSERT_OK(nanortc_init(&answerer, &ans_cfg));

    e2e_setup_ice_creds(&offerer, &answerer);

    /* Step 1: ICE handshake */
    uint32_t now_ms = 100;
    ASSERT_OK(nanortc_handle_timeout(&offerer, now_ms));

    /* Pump ICE + DTLS: relay packets until both DTLS_CONNECTED */
    int connected = 0;
    for (int round = 0; round < 30; round++) {
        e2e_pump(&offerer, &answerer, now_ms, 5);

        /* Offerer transitions DTLS_CONNECTED → SCTP_CONNECTING automatically */
        if (offerer.state >= NANORTC_STATE_DTLS_CONNECTED &&
            answerer.state >= NANORTC_STATE_DTLS_CONNECTED) {
            connected = 1;
            break;
        }
    }

    ASSERT_TRUE(connected);
    /* Offerer may be at SCTP_CONNECTING (sent INIT after DTLS) */
    ASSERT_TRUE(offerer.state >= NANORTC_STATE_DTLS_CONNECTED);
    ASSERT_TRUE(answerer.state >= NANORTC_STATE_DTLS_CONNECTED);

    /* Verify fingerprints are available */
    ASSERT_TRUE(dtls_get_fingerprint(&offerer.dtls) != NULL);
    ASSERT_TRUE(dtls_get_fingerprint(&answerer.dtls) != NULL);

    /* Verify keying material was derived */
    ASSERT_TRUE(offerer.dtls.keying_material_ready);
    ASSERT_TRUE(answerer.dtls.keying_material_ready);

    nanortc_destroy(&offerer);
    nanortc_destroy(&answerer);
}

/* ---- Runner ---- */

TEST_MAIN_BEGIN("nanortc E2E tests")
RUN(test_e2e_init_pair);
RUN(test_e2e_stubs_not_implemented);
RUN(test_e2e_loopback_skeleton);
RUN(test_e2e_multiple_instances);
RUN(test_e2e_demux_byte_ranges);
RUN(test_e2e_ice_loopback);
RUN(test_e2e_ice_dtls_loopback);
TEST_MAIN_END
