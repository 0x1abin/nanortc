/*
 * nanortc — End-to-end tests
 *
 * Two nano_rtc_t instances wired together in memory.
 * No network, no sockets — pure Sans I/O loopback.
 *
 * As modules are implemented, this test grows to cover
 * the full connection lifecycle:
 *   ICE → DTLS → SCTP → DataChannel
 *
 * SPDX-License-Identifier: MIT
 */

#include "nanortc.h"
#include "nano_rtc_internal.h"
#include "nano_crypto.h"
#include "nano_test.h"
#include "nano_test_config.h"
#include <string.h>

/* ----------------------------------------------------------------
 * E2E helpers — Sans I/O data bridge
 * ---------------------------------------------------------------- */

/* Helper: create a default config */
static nano_rtc_config_t e2e_default_config(void)
{
    nano_rtc_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.crypto = nano_test_crypto();
    cfg.role = NANO_ROLE_CONTROLLED;
#if NANORTC_PROFILE >= NANO_PROFILE_AUDIO
    cfg.jitter_depth_ms = 100;
    cfg.audio_codec = NANO_CODEC_OPUS;
    cfg.audio_sample_rate = 48000;
    cfg.audio_channels = 1;
    cfg.audio_direction = NANO_DIR_SENDRECV;
#endif
#if NANORTC_PROFILE >= NANO_PROFILE_MEDIA
    cfg.video_codec = NANO_CODEC_H264;
    cfg.video_direction = NANO_DIR_SENDRECV;
#endif
    return cfg;
}

/*
 * Relay all TRANSMIT outputs from `from` into `to` as received data.
 * Returns number of packets relayed, or negative on error.
 */
static int e2e_relay(nano_rtc_t *from, nano_rtc_t *to, uint32_t now_ms)
{
    int relayed = 0;
    nano_output_t out;

    while (nano_poll_output(from, &out) == NANO_OK) {
        if (out.type == NANO_OUTPUT_TRANSMIT) {
            nano_addr_t src;
            memset(&src, 0, sizeof(src));
            src.family = 4;
            src.addr[0] = 192;
            src.addr[1] = 168;
            src.addr[2] = 1;
            src.addr[3] = 1;
            src.port = 9999;

            int rc = nano_handle_receive(to, now_ms, out.transmit.data,
                                         out.transmit.len, &src);
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
static int e2e_pump(nano_rtc_t *a, nano_rtc_t *b, uint32_t now_ms, int max_rounds)
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
    nano_rtc_t server, client;
    nano_rtc_config_t cfg = e2e_default_config();

    ASSERT_OK(nano_rtc_init(&server, &cfg));
    ASSERT_OK(nano_rtc_init(&client, &cfg));

    /* Both should be in NEW state */
    ASSERT_EQ(server.state, NANO_STATE_NEW);
    ASSERT_EQ(client.state, NANO_STATE_NEW);

    /* Destroying one should not affect the other */
    nano_rtc_destroy(&server);
    ASSERT_EQ(server.state, NANO_STATE_CLOSED);
    ASSERT_EQ(client.state, NANO_STATE_NEW);

    nano_rtc_destroy(&client);
}

TEST(test_e2e_stubs_not_implemented)
{
    /* APIs that are still stubs should return NANO_ERR_NOT_IMPLEMENTED */
    nano_rtc_t rtc;
    nano_rtc_config_t cfg = e2e_default_config();
    ASSERT_OK(nano_rtc_init(&rtc, &cfg));

    char buf[256];
    ASSERT_EQ(nano_accept_offer(&rtc, "v=0\r\n", buf, sizeof(buf)),
              NANO_ERR_NOT_IMPLEMENTED);
    ASSERT_EQ(nano_create_offer(&rtc, buf, sizeof(buf)),
              NANO_ERR_NOT_IMPLEMENTED);
    ASSERT_EQ(nano_accept_answer(&rtc, "v=0\r\n"),
              NANO_ERR_NOT_IMPLEMENTED);
    ASSERT_EQ(nano_add_local_candidate(&rtc, "192.168.1.1", 9999),
              NANO_ERR_NOT_IMPLEMENTED);
    ASSERT_EQ(nano_add_remote_candidate(&rtc, "candidate:..."),
              NANO_ERR_NOT_IMPLEMENTED);

    /* nano_handle_receive and nano_handle_timeout are now implemented
     * (no longer stubs) — tested separately in demux and ICE tests */

    uint8_t data[] = {0x00, 0x01, 0x00, 0x00};
    ASSERT_EQ(nano_send_datachannel(&rtc, 0, data, sizeof(data)),
              NANO_ERR_NOT_IMPLEMENTED);
    ASSERT_EQ(nano_send_datachannel_string(&rtc, 0, "hello"),
              NANO_ERR_NOT_IMPLEMENTED);

#if NANORTC_PROFILE >= NANO_PROFILE_AUDIO
    ASSERT_EQ(nano_send_audio(&rtc, 0, data, sizeof(data)),
              NANO_ERR_NOT_IMPLEMENTED);
#endif

#if NANORTC_PROFILE >= NANO_PROFILE_MEDIA
    ASSERT_EQ(nano_send_video(&rtc, 0, data, sizeof(data), 1),
              NANO_ERR_NOT_IMPLEMENTED);
    ASSERT_EQ(nano_request_keyframe(&rtc),
              NANO_ERR_NOT_IMPLEMENTED);
#endif

    nano_rtc_destroy(&rtc);
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
    nano_rtc_t server, client;
    nano_rtc_config_t cfg = e2e_default_config();

    ASSERT_OK(nano_rtc_init(&server, &cfg));
    ASSERT_OK(nano_rtc_init(&client, &cfg));

    /* Client creates offer (stub: returns NOT_IMPLEMENTED) */
    char offer[2048];
    int rc = nano_create_offer(&client, offer, sizeof(offer));
    ASSERT_EQ(rc, NANO_ERR_NOT_IMPLEMENTED);

    /* No output should be queued */
    nano_output_t out;
    ASSERT_EQ(nano_poll_output(&server, &out), NANO_ERR_NO_DATA);
    ASSERT_EQ(nano_poll_output(&client, &out), NANO_ERR_NO_DATA);

    /* Pump should relay 0 packets (nothing to relay) */
    uint32_t now_ms = 0;
    int relayed = e2e_pump(&server, &client, now_ms, 10);
    ASSERT_EQ(relayed, 0);

    nano_rtc_destroy(&server);
    nano_rtc_destroy(&client);
}

TEST(test_e2e_multiple_instances)
{
    /*
     * Verify 4 instances can coexist (MCU might host multiple connections).
     * No global state means no interference.
     */
    nano_rtc_t instances[4];
    nano_rtc_config_t cfg = e2e_default_config();

    for (int i = 0; i < 4; i++) {
        ASSERT_OK(nano_rtc_init(&instances[i], &cfg));
        ASSERT_EQ(instances[i].state, NANO_STATE_NEW);
    }

    /* Destroy in reverse order */
    for (int i = 3; i >= 0; i--) {
        nano_rtc_destroy(&instances[i]);
        ASSERT_EQ(instances[i].state, NANO_STATE_CLOSED);

        /* Earlier instances should still be NEW */
        for (int j = 0; j < i; j++) {
            ASSERT_EQ(instances[j].state, NANO_STATE_NEW);
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
    nano_rtc_t rtc;
    nano_rtc_config_t cfg = e2e_default_config();
    ASSERT_OK(nano_rtc_init(&rtc, &cfg));

    nano_addr_t addr;
    memset(&addr, 0, sizeof(addr));
    addr.family = 4;

    /* STUN range: 0x00-0x03 (malformed — no valid STUN, returns parse error) */
    uint8_t stun_pkt[20] = {0x00, 0x01, 0x00, 0x00};
    int rc = nano_handle_receive(&rtc, 0, stun_pkt, sizeof(stun_pkt), &addr);
    ASSERT_TRUE(rc < 0); /* parse error expected for malformed STUN */

    /* DTLS range: 0x14-0x40 */
    uint8_t dtls_pkt[20] = {0x14, 0xFE, 0xFD};
    ASSERT_EQ(nano_handle_receive(&rtc, 0, dtls_pkt, sizeof(dtls_pkt), &addr),
              NANO_ERR_NOT_IMPLEMENTED);

    /* SRTP range: 0x80-0xBF */
    uint8_t srtp_pkt[20] = {0x80, 0x60};
    ASSERT_EQ(nano_handle_receive(&rtc, 0, srtp_pkt, sizeof(srtp_pkt), &addr),
              NANO_ERR_NOT_IMPLEMENTED);

    /* Edge cases: null data returns INVALID_PARAM */
    ASSERT_EQ(nano_handle_receive(&rtc, 0, NULL, 0, &addr),
              NANO_ERR_INVALID_PARAM);

    /* Unknown byte range */
    uint8_t one = 0xFF;
    ASSERT_EQ(nano_handle_receive(&rtc, 0, &one, 1, &addr),
              NANO_ERR_PROTOCOL);

    nano_rtc_destroy(&rtc);
}

TEST(test_e2e_ice_loopback)
{
    /*
     * Full ICE loopback: controlling sends STUN check,
     * controlled responds, controlling receives response.
     * Both reach ICE_CONNECTED.
     */
    nano_rtc_t offerer, answerer;

    /* Offerer = controlling role */
    nano_rtc_config_t off_cfg = e2e_default_config();
    off_cfg.role = NANO_ROLE_CONTROLLING;
    ASSERT_OK(nano_rtc_init(&offerer, &off_cfg));

    /* Answerer = controlled role */
    nano_rtc_config_t ans_cfg = e2e_default_config();
    ans_cfg.role = NANO_ROLE_CONTROLLED;
    ASSERT_OK(nano_rtc_init(&answerer, &ans_cfg));

    /* Set matching ICE credentials */
    memcpy(offerer.ice.local_ufrag, "OFF", 4);
    memcpy(offerer.ice.local_pwd, "offerer-password-1234", 22);
    memcpy(offerer.ice.remote_ufrag, "ANS", 4);
    memcpy(offerer.ice.remote_pwd, "answerer-password-5678", 23);
    offerer.ice.tie_breaker = 0x1234567890ABCDEFull;

    memcpy(answerer.ice.local_ufrag, "ANS", 4);
    memcpy(answerer.ice.local_pwd, "answerer-password-5678", 23);
    memcpy(answerer.ice.remote_ufrag, "OFF", 4);
    memcpy(answerer.ice.remote_pwd, "offerer-password-1234", 22);

    /* Set remote candidate address on offerer (where to send checks) */
    offerer.ice.remote_family = 4;
    offerer.ice.remote_addr[0] = 192;
    offerer.ice.remote_addr[1] = 168;
    offerer.ice.remote_addr[2] = 1;
    offerer.ice.remote_addr[3] = 2;
    offerer.ice.remote_port = 5000;

    /* Step 1: offerer generates STUN Binding Request via timeout */
    uint32_t now_ms = 100;
    ASSERT_OK(nano_handle_timeout(&offerer, now_ms));
    ASSERT_EQ(offerer.ice.state, NANO_ICE_STATE_CHECKING);

    /* Step 2: relay offerer's STUN request to answerer */
    nano_output_t out;
    ASSERT_OK(nano_poll_output(&offerer, &out));
    ASSERT_EQ(out.type, NANO_OUTPUT_TRANSMIT);
    ASSERT_TRUE(out.transmit.len > 0);

    /* Save request data (pointer references rtc->stun_buf, must copy) */
    uint8_t saved_req[256];
    size_t saved_req_len = out.transmit.len;
    memcpy(saved_req, out.transmit.data, saved_req_len);

    /* Drain the TIMEOUT output from handle_timeout */
    nano_output_t tout;
    ASSERT_OK(nano_poll_output(&offerer, &tout));
    ASSERT_EQ(tout.type, NANO_OUTPUT_TIMEOUT);

    /* Feed the STUN request into the answerer */
    nano_addr_t offerer_addr;
    memset(&offerer_addr, 0, sizeof(offerer_addr));
    offerer_addr.family = 4;
    offerer_addr.addr[0] = 192;
    offerer_addr.addr[1] = 168;
    offerer_addr.addr[2] = 1;
    offerer_addr.addr[3] = 1;
    offerer_addr.port = 9999;

    ASSERT_OK(nano_handle_receive(&answerer, now_ms,
                                   saved_req, saved_req_len,
                                   &offerer_addr));

    /* Answerer should now be CONNECTED (USE-CANDIDATE was in the request) */
    ASSERT_EQ(answerer.ice.state, NANO_ICE_STATE_CONNECTED);
    ASSERT_EQ(answerer.state, NANO_STATE_ICE_CONNECTED);

    /* Step 3: relay answerer's STUN response back to offerer */
    nano_output_t ans_out;
    ASSERT_OK(nano_poll_output(&answerer, &ans_out));
    ASSERT_EQ(ans_out.type, NANO_OUTPUT_TRANSMIT);

    /* Save response data (pointer references rtc->stun_buf) */
    uint8_t saved_resp[256];
    size_t saved_resp_len = ans_out.transmit.len;
    memcpy(saved_resp, ans_out.transmit.data, saved_resp_len);

    /* Check for ICE_CONNECTED event on answerer */
    nano_output_t evt;
    ASSERT_OK(nano_poll_output(&answerer, &evt));
    ASSERT_EQ(evt.type, NANO_OUTPUT_EVENT);
    ASSERT_EQ(evt.event.type, NANO_EVENT_ICE_CONNECTED);

    /* Feed response into offerer */
    nano_addr_t answerer_addr;
    memset(&answerer_addr, 0, sizeof(answerer_addr));
    answerer_addr.family = 4;
    answerer_addr.addr[0] = 192;
    answerer_addr.addr[1] = 168;
    answerer_addr.addr[2] = 1;
    answerer_addr.addr[3] = 2;
    answerer_addr.port = 5000;

    ASSERT_OK(nano_handle_receive(&offerer, now_ms,
                                   saved_resp, saved_resp_len,
                                   &answerer_addr));

    /* Offerer should now be CONNECTED */
    ASSERT_EQ(offerer.ice.state, NANO_ICE_STATE_CONNECTED);
    ASSERT_EQ(offerer.state, NANO_STATE_ICE_CONNECTED);

    /* ICE_CONNECTED event should be queued for offerer */
    nano_output_t off_evt;
    ASSERT_OK(nano_poll_output(&offerer, &off_evt));
    ASSERT_EQ(off_evt.type, NANO_OUTPUT_EVENT);
    ASSERT_EQ(off_evt.event.type, NANO_EVENT_ICE_CONNECTED);

    nano_rtc_destroy(&offerer);
    nano_rtc_destroy(&answerer);
}

/* ---- Runner ---- */

TEST_MAIN_BEGIN("nanortc E2E tests")
    RUN(test_e2e_init_pair);
    RUN(test_e2e_stubs_not_implemented);
    RUN(test_e2e_loopback_skeleton);
    RUN(test_e2e_multiple_instances);
    RUN(test_e2e_demux_byte_ranges);
    RUN(test_e2e_ice_loopback);
TEST_MAIN_END
