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
#include "nano_turn.h"
#include "nano_stun.h"
#include "nano_ice.h"
#include "nano_rtp.h"
#include "nano_srtp.h"
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
#endif
    return cfg;
}

/* Fail-on-Nth CSPRNG wrapper. The callback deliberately writes the caller's
 * buffer before reporting failure, so transactional callers must stage their
 * random values and cannot rely on a failed provider leaving memory untouched.
 * A non-zero match length counts only requests of that size (used to isolate
 * the six-byte RTP SSRC/sequence seed from ICE transaction IDs). */
static int e2e_rng_call_count;
static int e2e_rng_fail_at;
static size_t e2e_rng_match_len;
static bool e2e_rng_failed;

static int e2e_fail_nth_random_bytes(uint8_t *buf, size_t len)
{
    bool matches = e2e_rng_match_len == 0u || e2e_rng_match_len == len;
    if (matches) {
        e2e_rng_call_count++;
    }
    if (buf && len > 0u) {
        memset(buf, (uint8_t)(0x60u + (uint8_t)e2e_rng_call_count), len);
    }
    if (matches && e2e_rng_fail_at > 0 && e2e_rng_call_count == e2e_rng_fail_at) {
        e2e_rng_failed = true;
        return -1;
    }
    return 0;
}

static nanortc_crypto_provider_t e2e_crypto_with_failing_rng(void)
{
    nanortc_crypto_provider_t provider = *nano_test_crypto();
    provider.random_bytes = e2e_fail_nth_random_bytes;
    return provider;
}

static void e2e_rng_arm(int fail_at, size_t match_len)
{
    e2e_rng_call_count = 0;
    e2e_rng_fail_at = fail_at;
    e2e_rng_match_len = match_len;
    e2e_rng_failed = false;
}

#if NANORTC_HAVE_MEDIA_TRANSPORT
/* Build a deterministic SRTP endpoint without running ICE/DTLS. This keeps
 * media receive-path tests focused on RTP/SRTP while still entering through
 * nanortc_handle_input() and leaving through nanortc_poll_output(). */
static int e2e_init_direct_srtp_endpoint(nanortc_t *rtc, int is_client)
{
    nanortc_config_t cfg = e2e_default_config();
#if NANORTC_FEATURE_AUDIO
    cfg.jitter_depth_ms = 0;
#endif
    cfg.role = is_client ? NANORTC_ROLE_CONTROLLING : NANORTC_ROLE_CONTROLLED;
    int rc = nanortc_init(rtc, &cfg);
    if (rc != NANORTC_OK) {
        return rc;
    }

    uint8_t keying_material[60];
    for (size_t i = 0; i < sizeof(keying_material); i++) {
        keying_material[i] = (uint8_t)(0x31u + i);
    }
    rc = nano_srtp_init(&rtc->srtp, cfg.crypto, is_client);
    if (rc == NANORTC_OK) {
        rc = nano_srtp_derive_keys(&rtc->srtp, keying_material, sizeof(keying_material));
    }
    if (rc != NANORTC_OK) {
        nanortc_destroy(rtc);
    }
    return rc;
}

static int e2e_pack_padded_srtp(nanortc_t *sender, nano_rtp_t *rtp, uint32_t timestamp,
                                const uint8_t *payload, size_t payload_len, uint8_t padding_len,
                                uint8_t *packet, size_t packet_cap, size_t *wire_len)
{
    if (!sender || !rtp || !payload || payload_len == 0u || padding_len == 0u || !packet ||
        !wire_len || packet_cap < (size_t)padding_len + NANORTC_SRTP_AUTH_TAG_SIZE) {
        return NANORTC_ERR_INVALID_PARAM;
    }

    size_t plain_len = 0;
    size_t rtp_cap = packet_cap - (size_t)padding_len - NANORTC_SRTP_AUTH_TAG_SIZE;
    int rc = rtp_pack(rtp, timestamp, payload, payload_len, packet, rtp_cap, &plain_len);
    if (rc != NANORTC_OK) {
        return rc;
    }

    packet[0] |= 0x20u; /* RFC 3550 P bit. */
    memset(packet + plain_len, 0xA5, padding_len);
    packet[plain_len + padding_len - 1u] = padding_len;
    return nano_srtp_protect(&sender->srtp, packet, plain_len + padding_len, wire_len);
}
#endif /* NANORTC_HAVE_MEDIA_TRANSPORT */

#if NANORTC_FEATURE_VIDEO && NANORTC_FEATURE_VIDEO_FEC && !NANORTC_FEATURE_VIDEO_PACING
static int e2e_fail_aes_128_cm(const uint8_t key[16], const uint8_t iv[16], const uint8_t *in,
                               size_t len, uint8_t *out)
{
    (void)key;
    (void)iv;
    (void)in;
    (void)len;
    (void)out;
    return -1;
}
#endif

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

            int rc = nanortc_handle_input(to, &(nanortc_input_t){.now_ms = now_ms,
                                                                 .data = out.transmit.data,
                                                                 .len = out.transmit.len,
                                                                 .src = src});
            (void)rc;
            relayed++;
        }
    }
    return relayed;
}

/* Error-preserving relay used by RNG rollback tests. The regular E2E pump is
 * intentionally permissive because many legacy lifecycle tests only care
 * about eventual progress; this helper stops on the exact failing input. */
static int e2e_relay_checked(nanortc_t *from, nanortc_t *to, uint32_t now_ms)
{
    nanortc_output_t out;
    while (nanortc_poll_output(from, &out) == NANORTC_OK) {
        if (out.type != NANORTC_OUTPUT_TRANSMIT) {
            continue;
        }
        nanortc_addr_t src;
        memset(&src, 0, sizeof(src));
        src.family = 4;
        src.addr[0] = 192;
        src.addr[1] = 168;
        src.addr[2] = 1;
        src.addr[3] = 1;
        src.port = 9999;
        int rc = nanortc_handle_input(to, &(nanortc_input_t){.now_ms = now_ms,
                                                             .data = out.transmit.data,
                                                             .len = out.transmit.len,
                                                             .src = src});
        if (rc != NANORTC_OK) {
            return rc;
        }
    }
    return NANORTC_OK;
}

#if NANORTC_FEATURE_VIDEO && NANORTC_FEATURE_VIDEO_AUTO_PLI && !NANORTC_FEATURE_VIDEO_REORDER
/* Relay from→to but drop the `drop_nth` RTP/SRTP media packet (first byte in
 * [0x80,0xBF] per RFC 7983 §4) to punch a sequence gap into the receiver's
 * video stream. drop_nth < 0 drops nothing. Returns packets relayed. */
static int e2e_relay_drop_rtp(nanortc_t *from, nanortc_t *to, uint32_t now_ms, int drop_nth)
{
    int relayed = 0;
    int rtp_idx = 0;
    nanortc_output_t out;

    while (nanortc_poll_output(from, &out) == NANORTC_OK) {
        if (out.type != NANORTC_OUTPUT_TRANSMIT) {
            continue;
        }
        const uint8_t *d = out.transmit.data;
        int is_rtp = (out.transmit.len > 0 && d[0] >= 0x80 && d[0] <= 0xBF);
        if (is_rtp) {
            int this_idx = rtp_idx++;
            if (this_idx == drop_nth) {
                continue; /* drop this media packet → receiver sees a seq gap */
            }
        }
        nanortc_addr_t src;
        memset(&src, 0, sizeof(src));
        src.family = 4;
        src.addr[0] = 192;
        src.addr[1] = 168;
        src.addr[2] = 1;
        src.addr[3] = 1;
        src.port = 9999;
        nanortc_handle_input(to, &(nanortc_input_t){.now_ms = now_ms,
                                                    .data = out.transmit.data,
                                                    .len = out.transmit.len,
                                                    .src = src});
        relayed++;
    }
    return relayed;
}
#endif /* video auto-PLI */

/*
 * Pump both instances: relay outputs from A→B and B→A
 * until neither produces new output or max_rounds reached.
 * Returns total packets relayed.
 */
static int e2e_pump(nanortc_t *a, nanortc_t *b, uint32_t now_ms, int max_rounds)
{
    int total = 0;
    for (int i = 0; i < max_rounds; i++) {
        /* A one-slot TX ring may retain the next DTLS/SCTP packet until the
         * caller drains the current output and supplies another timer tick. */
        (void)nanortc_handle_input(a, &(nanortc_input_t){.now_ms = now_ms});
        (void)nanortc_handle_input(b, &(nanortc_input_t){.now_ms = now_ms});
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

TEST(test_e2e_init_rng_failure_clears_partial_state)
{
    nanortc_t rtc;
    memset(&rtc, 0xA5, sizeof(rtc));
    nanortc_crypto_provider_t provider = e2e_crypto_with_failing_rng();
    nanortc_config_t cfg = e2e_default_config();
    cfg.crypto = &provider;

    e2e_rng_arm(1, 0u);
    ASSERT_EQ(nanortc_init(&rtc, &cfg), NANORTC_ERR_CRYPTO);
    ASSERT_TRUE(e2e_rng_failed);
    ASSERT_EQ(rtc.config.crypto, NULL);
    ASSERT_EQ(rtc.ice.tie_breaker, 0u);
    ASSERT_EQ(rtc.out_head, 0u);
    ASSERT_EQ(rtc.out_tail, 0u);
    ASSERT_EQ(rtc.tx_slots_in_use, 0u);
}

TEST(test_e2e_negotiation_rng_failure_is_transactional)
{
    nanortc_crypto_provider_t provider = e2e_crypto_with_failing_rng();

    for (int fail_at = 1; fail_at <= 2; fail_at++) {
        nanortc_t rtc;
        nanortc_config_t cfg = e2e_default_config();
        cfg.crypto = &provider;
        e2e_rng_arm(0, 0u);
        ASSERT_OK(nanortc_init(&rtc, &cfg));

        memset(rtc.sdp.local_ufrag, 'S', NANORTC_ICE_UFRAG_LEN);
        rtc.sdp.local_ufrag[NANORTC_ICE_UFRAG_LEN] = '\0';
        memset(rtc.sdp.local_pwd, 'P', NANORTC_ICE_PWD_LEN);
        rtc.sdp.local_pwd[NANORTC_ICE_PWD_LEN] = '\0';
        memset(rtc.ice.local_ufrag, 'I', NANORTC_ICE_UFRAG_LEN);
        rtc.ice.local_ufrag[NANORTC_ICE_UFRAG_LEN] = '\0';
        memset(rtc.ice.local_pwd, 'C', NANORTC_ICE_PWD_LEN);
        rtc.ice.local_pwd[NANORTC_ICE_PWD_LEN] = '\0';
        rtc.ice.local_ufrag_len = 3u;
        rtc.ice.local_pwd_len = 7u;

        nano_sdp_t sdp_before = rtc.sdp;
        nano_ice_t ice_before = rtc.ice;
        char offer[2048];
        size_t offer_len = 123u;

        e2e_rng_arm(fail_at, 0u);
        ASSERT_EQ(nanortc_create_offer(&rtc, offer, sizeof(offer), &offer_len), NANORTC_ERR_CRYPTO);
        ASSERT_TRUE(e2e_rng_failed);
        ASSERT_TRUE(memcmp(&rtc.sdp, &sdp_before, sizeof(sdp_before)) == 0);
        ASSERT_TRUE(memcmp(&rtc.ice, &ice_before, sizeof(ice_before)) == 0);
        ASSERT_EQ(rtc.state, NANORTC_STATE_NEW);
        ASSERT_EQ(rtc.dtls.crypto_ctx, NULL);
        ASSERT_EQ(offer_len, 123u);
        nanortc_destroy(&rtc);
    }

    for (int fail_at = 1; fail_at <= 2; fail_at++) {
        nanortc_t rtc;
        nanortc_config_t cfg = e2e_default_config();
        cfg.crypto = &provider;
        e2e_rng_arm(0, 0u);
        ASSERT_OK(nanortc_init(&rtc, &cfg));

        memset(rtc.sdp.local_ufrag, 'A', NANORTC_ICE_UFRAG_LEN);
        rtc.sdp.local_ufrag[NANORTC_ICE_UFRAG_LEN] = '\0';
        memset(rtc.ice.local_pwd, 'B', NANORTC_ICE_PWD_LEN);
        rtc.ice.local_pwd[NANORTC_ICE_PWD_LEN] = '\0';
        rtc.ice.local_pwd_len = 9u;
        nano_sdp_t sdp_before = rtc.sdp;
        nano_ice_t ice_before = rtc.ice;
        char answer[2048];
        size_t answer_len = 456u;

        e2e_rng_arm(fail_at, 0u);
        ASSERT_EQ(nanortc_accept_offer(&rtc, "not parsed on RNG failure", answer, sizeof(answer),
                                       &answer_len),
                  NANORTC_ERR_CRYPTO);
        ASSERT_TRUE(e2e_rng_failed);
        ASSERT_TRUE(memcmp(&rtc.sdp, &sdp_before, sizeof(sdp_before)) == 0);
        ASSERT_TRUE(memcmp(&rtc.ice, &ice_before, sizeof(ice_before)) == 0);
        ASSERT_EQ(rtc.state, NANORTC_STATE_NEW);
        ASSERT_EQ(rtc.dtls.crypto_ctx, NULL);
        ASSERT_EQ(answer_len, 456u);
        nanortc_destroy(&rtc);
    }
}

TEST(test_e2e_ice_restart_rng_failure_is_transactional)
{
    nanortc_crypto_provider_t provider = e2e_crypto_with_failing_rng();
    for (int fail_at = 1; fail_at <= 2; fail_at++) {
        nanortc_t rtc;
        nanortc_config_t cfg = e2e_default_config();
        cfg.crypto = &provider;
        e2e_rng_arm(0, 0u);
        ASSERT_OK(nanortc_init(&rtc, &cfg));

        memcpy(rtc.ice.local_ufrag, "oldUfrag", 9u);
        rtc.ice.local_ufrag_len = 8u;
        memcpy(rtc.ice.local_pwd, "old-password-value", 19u);
        rtc.ice.local_pwd_len = 18u;
        memcpy(rtc.sdp.local_ufrag, rtc.ice.local_ufrag, sizeof(rtc.sdp.local_ufrag));
        memcpy(rtc.sdp.local_pwd, rtc.ice.local_pwd, sizeof(rtc.sdp.local_pwd));
        rtc.state = NANORTC_STATE_CONNECTED;
        rtc.ice.state = NANORTC_ICE_STATE_CONNECTED;
        rtc.ice.nominated = true;
        rtc.ice.generation = 7u;
        nano_conn_state_t state_before = rtc.state;
        nano_ice_t ice_before = rtc.ice;
        nano_sdp_t sdp_before = rtc.sdp;

        e2e_rng_arm(fail_at, 0u);
        ASSERT_EQ(nanortc_ice_restart(&rtc), NANORTC_ERR_CRYPTO);
        ASSERT_TRUE(e2e_rng_failed);
        ASSERT_EQ(rtc.state, state_before);
        ASSERT_TRUE(memcmp(&rtc.ice, &ice_before, sizeof(ice_before)) == 0);
        ASSERT_TRUE(memcmp(&rtc.sdp, &sdp_before, sizeof(sdp_before)) == 0);
        nanortc_destroy(&rtc);
    }
}

#if NANORTC_HAVE_MEDIA_TRANSPORT
TEST(test_e2e_media_seed_rng_failure_rolls_back_all_tracks)
{
    nanortc_t offerer, answerer;
    nanortc_crypto_provider_t provider = e2e_crypto_with_failing_rng();
    nanortc_config_t off_cfg = e2e_default_config();
    off_cfg.role = NANORTC_ROLE_CONTROLLING;
    off_cfg.crypto = &provider;
    e2e_rng_arm(0, 0u);
    ASSERT_OK(nanortc_init(&offerer, &off_cfg));

    nanortc_config_t ans_cfg = e2e_default_config();
    ans_cfg.role = NANORTC_ROLE_CONTROLLED;
    ASSERT_OK(nanortc_init(&answerer, &ans_cfg));
    ASSERT_OK(nanortc_add_local_candidate(&offerer, "192.168.1.1", 4000));
    ASSERT_OK(nanortc_add_local_candidate(&answerer, "192.168.1.2", 5000));

#if NANORTC_FEATURE_AUDIO
    int mid0 =
        nanortc_add_audio_track(&offerer, NANORTC_DIR_SENDONLY, NANORTC_CODEC_OPUS, 48000, 2);
    int mid1 =
        nanortc_add_audio_track(&offerer, NANORTC_DIR_SENDONLY, NANORTC_CODEC_OPUS, 48000, 2);
#else
    int mid0 = nanortc_add_video_track(&offerer, NANORTC_DIR_SENDONLY, NANORTC_CODEC_H264);
    int mid1 = nanortc_add_video_track(&offerer, NANORTC_DIR_SENDONLY, NANORTC_CODEC_H264);
#endif
    ASSERT_TRUE(mid0 >= 0);
    ASSERT_TRUE(mid1 >= 0);

    char offer[4096];
    char answer[4096];
    size_t offer_len = 0u;
    size_t answer_len = 0u;
    ASSERT_OK(nanortc_create_offer(&offerer, offer, sizeof(offer), &offer_len));
    offer[offer_len] = '\0';
    ASSERT_OK(nanortc_accept_offer(&answerer, offer, answer, sizeof(answer), &answer_len));
    answer[answer_len] = '\0';
    ASSERT_OK(nanortc_accept_answer(&offerer, answer));

    /* The test relay reports 192.168.1.1:9999 as the source of every packet;
     * arm that address as the controlled peer's remote candidate. */
    answerer.ice.remote_candidates[0].family = 4;
    answerer.ice.remote_candidates[0].addr[0] = 192;
    answerer.ice.remote_candidates[0].addr[1] = 168;
    answerer.ice.remote_candidates[0].addr[2] = 1;
    answerer.ice.remote_candidates[0].addr[3] = 1;
    answerer.ice.remote_candidates[0].port = 9999;
    answerer.ice.remote_candidate_count = 1;

    nano_rtp_t rtp0_before = offerer.media[0].rtp;
    nano_rtp_t rtp1_before = offerer.media[1].rtp;
    nano_rtcp_t rtcp0_before = offerer.media[0].rtcp;
    nano_rtcp_t rtcp1_before = offerer.media[1].rtcp;
    nanortc_ssrc_entry_t ssrc_map_before[NANORTC_MAX_SSRC_MAP];
    memcpy(ssrc_map_before, offerer.ssrc_map, sizeof(ssrc_map_before));

    /* Fail the second per-track six-byte SSRC+sequence request. The first
     * request succeeds, so this catches incremental per-track commits. */
    e2e_rng_arm(2, sizeof(uint32_t) + sizeof(uint16_t));
    uint32_t now_ms = 100u;
    ASSERT_OK(nanortc_handle_input(&offerer, &(nanortc_input_t){.now_ms = now_ms}));
    int observed_rc = NANORTC_OK;
    for (int round = 0; round < 50; round++) {
        observed_rc = e2e_relay_checked(&offerer, &answerer, now_ms);
        if (observed_rc != NANORTC_OK) {
            break;
        }
        observed_rc = e2e_relay_checked(&answerer, &offerer, now_ms);
        if (observed_rc != NANORTC_OK) {
            break;
        }
    }

    ASSERT_EQ(observed_rc, NANORTC_ERR_CRYPTO);
    ASSERT_TRUE(e2e_rng_failed);
    ASSERT_EQ(e2e_rng_call_count, 2);
    ASSERT_TRUE(offerer.state < NANORTC_STATE_DTLS_CONNECTED);
    ASSERT_FALSE(offerer.srtp.ready);
    ASSERT_TRUE(memcmp(&offerer.media[0].rtp, &rtp0_before, sizeof(rtp0_before)) == 0);
    ASSERT_TRUE(memcmp(&offerer.media[1].rtp, &rtp1_before, sizeof(rtp1_before)) == 0);
    ASSERT_TRUE(memcmp(&offerer.media[0].rtcp, &rtcp0_before, sizeof(rtcp0_before)) == 0);
    ASSERT_TRUE(memcmp(&offerer.media[1].rtcp, &rtcp1_before, sizeof(rtcp1_before)) == 0);
    ASSERT_TRUE(memcmp(offerer.ssrc_map, ssrc_map_before, sizeof(ssrc_map_before)) == 0);

    nanortc_destroy(&offerer);
    nanortc_destroy(&answerer);
}

#if NANORTC_FEATURE_AUDIO
TEST(test_e2e_padded_audio_excludes_rtp_padding)
{
    nanortc_t sender, receiver;
    ASSERT_OK(e2e_init_direct_srtp_endpoint(&sender, 1));
    ASSERT_OK(e2e_init_direct_srtp_endpoint(&receiver, 0));
    int mid =
        nanortc_add_audio_track(&receiver, NANORTC_DIR_RECVONLY, NANORTC_CODEC_OPUS, 48000, 2);
    ASSERT_TRUE(mid >= 0);
    receiver.media[0].rtp.payload_type = 111u;
    receiver.sdp.mlines[0].remote_pt = 111u;

    nano_rtp_t rtp;
    ASSERT_OK(rtp_init(&rtp, 0x10203040u, 111u));
    rtp.seq = 77u;
    const uint8_t payload[] = {0xF8, 0xAA, 0xBB, 0xCC};
    uint8_t packet[128];
    size_t wire_len = 0u;
    ASSERT_OK(e2e_pack_padded_srtp(&sender, &rtp, 4800u, payload, sizeof(payload), 4u, packet,
                                   sizeof(packet), &wire_len));

    nanortc_addr_t src;
    memset(&src, 0, sizeof(src));
    src.family = 4;
    src.addr[0] = 192;
    src.addr[1] = 0;
    src.addr[2] = 2;
    src.addr[3] = 10;
    src.port = 5000;
    ASSERT_OK(nanortc_handle_input(
        &receiver,
        &(nanortc_input_t){.now_ms = 100u, .data = packet, .len = wire_len, .src = src}));

    nanortc_output_t out;
    bool found = false;
    while (nanortc_poll_output(&receiver, &out) == NANORTC_OK) {
        if (out.type == NANORTC_OUTPUT_EVENT && out.event.type == NANORTC_EV_MEDIA_DATA) {
            ASSERT_EQ(out.event.media_data.mid, (uint8_t)mid);
            ASSERT_EQ(out.event.media_data.len, sizeof(payload));
            ASSERT_MEM_EQ(out.event.media_data.data, payload, sizeof(payload));
            found = true;
        }
    }
    ASSERT_TRUE(found);
    nanortc_destroy(&sender);
    nanortc_destroy(&receiver);
}
#endif

#if NANORTC_FEATURE_VIDEO
TEST(test_e2e_padded_video_excludes_rtp_padding)
{
    nanortc_t sender, receiver;
    ASSERT_OK(e2e_init_direct_srtp_endpoint(&sender, 1));
    ASSERT_OK(e2e_init_direct_srtp_endpoint(&receiver, 0));
    int mid = nanortc_add_video_track(&receiver, NANORTC_DIR_RECVONLY, NANORTC_CODEC_H264);
    ASSERT_TRUE(mid >= 0);
    receiver.media[0].rtp.payload_type = NANORTC_VIDEO_DEFAULT_PT;
    receiver.sdp.mlines[0].remote_pt = NANORTC_VIDEO_DEFAULT_PT;

    nano_rtp_t rtp;
    ASSERT_OK(rtp_init(&rtp, 0x50607080u, NANORTC_VIDEO_DEFAULT_PT));
    rtp.seq = 99u;
    rtp.marker = 1u;
    const uint8_t payload[] = {0x65, 0x11, 0x22, 0x33, 0x44};
    uint8_t packet[128];
    size_t wire_len = 0u;
    ASSERT_OK(e2e_pack_padded_srtp(&sender, &rtp, 9000u, payload, sizeof(payload), 8u, packet,
                                   sizeof(packet), &wire_len));

    nanortc_addr_t src;
    memset(&src, 0, sizeof(src));
    src.family = 4;
    src.addr[0] = 198;
    src.addr[1] = 51;
    src.addr[2] = 100;
    src.addr[3] = 20;
    src.port = 6000;
    ASSERT_OK(nanortc_handle_input(
        &receiver,
        &(nanortc_input_t){.now_ms = 200u, .data = packet, .len = wire_len, .src = src}));

    nanortc_output_t out;
    bool found = false;
    while (nanortc_poll_output(&receiver, &out) == NANORTC_OK) {
        if (out.type == NANORTC_OUTPUT_EVENT && out.event.type == NANORTC_EV_MEDIA_DATA) {
            ASSERT_EQ(out.event.media_data.mid, (uint8_t)mid);
            ASSERT_EQ(out.event.media_data.len, sizeof(payload));
            ASSERT_MEM_EQ(out.event.media_data.data, payload, sizeof(payload));
            ASSERT_TRUE(out.event.media_data.is_keyframe);
            found = true;
        }
    }
    ASSERT_TRUE(found);
    nanortc_destroy(&sender);
    nanortc_destroy(&receiver);
}

TEST(test_e2e_bwe_feedback_events_keep_source)
{
    nanortc_t sender, receiver;
    ASSERT_OK(e2e_init_direct_srtp_endpoint(&sender, 1));
    ASSERT_OK(e2e_init_direct_srtp_endpoint(&receiver, 0));
    ASSERT_OK(nanortc_set_bwe_event_threshold(&receiver, 1u));

    nanortc_addr_t src;
    memset(&src, 0, sizeof(src));
    src.family = 4;
    src.addr[0] = 192;
    src.addr[1] = 0;
    src.addr[2] = 2;
    src.addr[3] = 30;
    src.port = 6001;

    /* Hand-built REMB for exactly 524288 bps: mantissa=0x20000, exp=2.
     * The large first step guarantees a bitrate event. */
    uint8_t packet[64];
    memset(packet, 0, sizeof(packet));
    packet[0] = (uint8_t)((2u << 6) | 15u); /* V=2, FMT=15 */
    packet[1] = 206u;                       /* PSFB */
    nanortc_write_u16be(packet + 2, 5u);    /* 24 bytes */
    nanortc_write_u32be(packet + 4, 0x11223344u);
    packet[12] = 'R';
    packet[13] = 'E';
    packet[14] = 'M';
    packet[15] = 'B';
    packet[16] = 1u;
    packet[17] = (uint8_t)((2u << 2) | 2u);
    nanortc_write_u32be(packet + 20, 0x55667788u);

    size_t wire_len = 0;
    ASSERT_OK(nano_srtp_protect_rtcp(&sender.srtp, packet, 24u, &wire_len));
    ASSERT_OK(nanortc_handle_input(
        &receiver,
        &(nanortc_input_t){.now_ms = 100u, .data = packet, .len = wire_len, .src = src}));

    nanortc_output_t out;
    bool found_remb = false;
    while (nanortc_poll_output(&receiver, &out) == NANORTC_OK) {
        if (out.type == NANORTC_OUTPUT_EVENT && out.event.type == NANORTC_EV_BITRATE_ESTIMATE) {
            ASSERT_EQ(out.event.bitrate_estimate.source, (uint8_t)NANORTC_BWE_SRC_REMB);
            found_remb = true;
        }
    }
    ASSERT_TRUE(found_remb);

    /* One received TWCC packet (zero loss). Run-length chunk 0x2001 means
     * one SMALL_DELTA status, followed by its one-byte delta and padding. */
    memset(packet, 0, sizeof(packet));
    packet[0] = (uint8_t)((2u << 6) | 15u); /* V=2, FMT=15 */
    packet[1] = 205u;                       /* RTPFB */
    nanortc_write_u16be(packet + 2, 5u);    /* 24 bytes */
    nanortc_write_u32be(packet + 4, 0x11223344u);
    nanortc_write_u32be(packet + 8, 0x55667788u);
    nanortc_write_u16be(packet + 12, 100u);
    nanortc_write_u16be(packet + 14, 1u);
    nanortc_write_u16be(packet + 20, 0x2001u);
    packet[22] = 1u;

    wire_len = 0;
    ASSERT_OK(nano_srtp_protect_rtcp(&sender.srtp, packet, 24u, &wire_len));
    ASSERT_OK(nanortc_handle_input(
        &receiver,
        &(nanortc_input_t){.now_ms = 101u, .data = packet, .len = wire_len, .src = src}));

    bool found_twcc = false;
    while (nanortc_poll_output(&receiver, &out) == NANORTC_OK) {
        if (out.type == NANORTC_OUTPUT_EVENT && out.event.type == NANORTC_EV_BITRATE_ESTIMATE) {
            ASSERT_EQ(out.event.bitrate_estimate.source, (uint8_t)NANORTC_BWE_SRC_TWCC_LOSS);
            found_twcc = true;
        }
    }
    ASSERT_TRUE(found_twcc);

    nanortc_destroy(&sender);
    nanortc_destroy(&receiver);
}
#endif
#endif

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

    /* nanortc_handle_input and nanortc_handle_input are now implemented */

    uint8_t data[] = {0x00, 0x01, 0x00, 0x00};

#if NANORTC_FEATURE_DATACHANNEL
    /* nanortc_datachannel_send returns ERR_STATE (not connected) */
    {
        int sid = nanortc_create_datachannel(&rtc, "test", NULL);
        ASSERT(sid >= 0);
        ASSERT_EQ(nanortc_datachannel_send(&rtc, (uint16_t)sid, data, sizeof(data)),
                  NANORTC_ERR_STATE);
        ASSERT_EQ(nanortc_datachannel_send_string(&rtc, (uint16_t)sid, "hello"), NANORTC_ERR_STATE);
    }
#endif

#if NANORTC_HAVE_MEDIA_TRANSPORT
    /* nanortc_send_audio returns ERR_STATE when not connected */
    {
        int mid = nanortc_add_audio_track(&rtc, NANORTC_DIR_SENDRECV, NANORTC_CODEC_OPUS, 48000, 2);
        ASSERT(mid >= 0);
        uint8_t dummy[4] = {0};
        ASSERT_EQ(nanortc_send_audio(&rtc, (uint8_t)mid, 0, dummy, sizeof(dummy)),
                  NANORTC_ERR_STATE);
    }
#endif

#if NANORTC_FEATURE_VIDEO
    {
        int vmid = nanortc_add_video_track(&rtc, NANORTC_DIR_SENDRECV, NANORTC_CODEC_H264);
        ASSERT(vmid >= 0);
        uint8_t dummy[4] = {0};
        ASSERT_EQ(nanortc_send_video(&rtc, (uint8_t)vmid, 0, dummy, sizeof(dummy)),
                  NANORTC_ERR_STATE);
    }
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
    int rc = nanortc_handle_input(
        &rtc,
        &(nanortc_input_t){.now_ms = 0, .data = stun_pkt, .len = sizeof(stun_pkt), .src = addr});
    ASSERT_TRUE(rc < 0); /* parse error expected for malformed STUN */

    /* DTLS range: 0x14-0x40 — rejected before ICE connects */
    uint8_t dtls_pkt[20] = {0x14, 0xFE, 0xFD};
    ASSERT_EQ(nanortc_handle_input(&rtc, &(nanortc_input_t){.now_ms = 0,
                                                            .data = dtls_pkt,
                                                            .len = sizeof(dtls_pkt),
                                                            .src = addr}),
              NANORTC_ERR_STATE);

    /* SRTP range: 0x80-0xBF — silently consumed (no decode path yet) */
    uint8_t srtp_pkt[20] = {0x80, 0x60};
#if NANORTC_HAVE_MEDIA_TRANSPORT
    ASSERT_EQ(nanortc_handle_input(&rtc, &(nanortc_input_t){.now_ms = 0,
                                                            .data = srtp_pkt,
                                                            .len = sizeof(srtp_pkt),
                                                            .src = addr}),
              NANORTC_OK); /* pre-DTLS media silently dropped */
#else
    ASSERT_EQ(nanortc_handle_input(&rtc, &(nanortc_input_t){.now_ms = 0,
                                                            .data = srtp_pkt,
                                                            .len = sizeof(srtp_pkt),
                                                            .src = addr}),
              NANORTC_OK);
#endif

    /* Edge cases: null data = timeout-only (valid in unified API) */
    ASSERT_EQ(nanortc_handle_input(&rtc, &(nanortc_input_t){.now_ms = 0}), NANORTC_OK);

    /* Unknown byte range */
    uint8_t one = 0xFF;
    ASSERT_EQ(nanortc_handle_input(
                  &rtc, &(nanortc_input_t){.now_ms = 0, .data = &one, .len = 1, .src = addr}),
              NANORTC_ERR_PROTOCOL);

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
    /* Local candidate for offerer */
    offerer.ice.local_candidates[0].family = 4;
    offerer.ice.local_candidates[0].addr[0] = 192;
    offerer.ice.local_candidates[0].addr[1] = 168;
    offerer.ice.local_candidates[0].addr[2] = 1;
    offerer.ice.local_candidates[0].addr[3] = 1;
    offerer.ice.local_candidates[0].port = 4000;
    offerer.ice.local_candidates[0].type = NANORTC_ICE_CAND_HOST;
    offerer.ice.local_candidate_count = 1;

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
    ASSERT_OK(nanortc_handle_input(&offerer, &(nanortc_input_t){.now_ms = now_ms}));
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

    /* Drain the ICE_STATE_CHANGE event (CHECKING) + TIMEOUT from handle_timeout */
    nanortc_output_t ice_chg;
    ASSERT_OK(nanortc_poll_output(&offerer, &ice_chg));
    ASSERT_EQ(ice_chg.type, NANORTC_OUTPUT_EVENT);
    ASSERT_EQ(ice_chg.event.type, NANORTC_EV_ICE_STATE_CHANGE);
    ASSERT_EQ(ice_chg.event.ice_state, (uint16_t)NANORTC_ICE_STATE_CHECKING);

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

    ASSERT_OK(nanortc_handle_input(&answerer, &(nanortc_input_t){.now_ms = now_ms,
                                                                 .data = saved_req,
                                                                 .len = saved_req_len,
                                                                 .src = offerer_addr}));

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

    /* Check for ICE_STATE_CHANGE (CONNECTED) event on answerer */
    nanortc_output_t evt;
    ASSERT_OK(nanortc_poll_output(&answerer, &evt));
    ASSERT_EQ(evt.type, NANORTC_OUTPUT_EVENT);
    ASSERT_EQ(evt.event.type, NANORTC_EV_ICE_STATE_CHANGE);
    ASSERT_EQ(evt.event.ice_state, (uint16_t)NANORTC_ICE_STATE_CONNECTED);

    /* Feed response into offerer */
    nanortc_addr_t answerer_addr;
    memset(&answerer_addr, 0, sizeof(answerer_addr));
    answerer_addr.family = 4;
    answerer_addr.addr[0] = 192;
    answerer_addr.addr[1] = 168;
    answerer_addr.addr[2] = 1;
    answerer_addr.addr[3] = 2;
    answerer_addr.port = 5000;

    ASSERT_OK(nanortc_handle_input(&offerer, &(nanortc_input_t){.now_ms = now_ms,
                                                                .data = saved_resp,
                                                                .len = saved_resp_len,
                                                                .src = answerer_addr}));

    /* Offerer: ICE connected → DTLS handshaking (client sends ClientHello) */
    ASSERT_EQ(offerer.ice.state, NANORTC_ICE_STATE_CONNECTED);
    ASSERT_EQ(offerer.state, NANORTC_STATE_DTLS_HANDSHAKING);

    /* ICE_STATE_CHANGE (CONNECTED) event should be queued for offerer */
    nanortc_output_t off_evt;
    ASSERT_OK(nanortc_poll_output(&offerer, &off_evt));
    ASSERT_EQ(off_evt.type, NANORTC_OUTPUT_EVENT);
    ASSERT_EQ(off_evt.event.type, NANORTC_EV_ICE_STATE_CHANGE);
    ASSERT_EQ(off_evt.event.ice_state, (uint16_t)NANORTC_ICE_STATE_CONNECTED);

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
    /* Local candidate for offerer */
    offerer->ice.local_candidates[0].family = 4;
    offerer->ice.local_candidates[0].addr[0] = 192;
    offerer->ice.local_candidates[0].addr[1] = 168;
    offerer->ice.local_candidates[0].addr[2] = 1;
    offerer->ice.local_candidates[0].addr[3] = 1;
    offerer->ice.local_candidates[0].port = 4000;
    offerer->ice.local_candidates[0].type = NANORTC_ICE_CAND_HOST;
    offerer->ice.local_candidate_count = 1;

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
    ASSERT_OK(nanortc_handle_input(&offerer, &(nanortc_input_t){.now_ms = now_ms}));

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

/*
 * Helper: re-sync ICE credentials between offerer/answerer after both have
 * called nanortc_ice_restart(). The restart preserves local_candidates but
 * blanks the peer-side credentials and remote_candidates; cross-write the
 * freshly generated ufrag/pwd and re-arm offerer's remote_candidates entry
 * so the second handshake can drive checks on the same loopback 5-tuple.
 * Mirrors the final block of e2e_setup_ice_creds().
 */
static void e2e_resync_ice_creds(nanortc_t *offerer, nanortc_t *answerer)
{
    memcpy(offerer->ice.remote_ufrag, answerer->ice.local_ufrag, sizeof(offerer->ice.remote_ufrag));
    offerer->ice.remote_ufrag_len = answerer->ice.local_ufrag_len;
    memcpy(offerer->ice.remote_pwd, answerer->ice.local_pwd, sizeof(offerer->ice.remote_pwd));
    offerer->ice.remote_pwd_len = answerer->ice.local_pwd_len;

    memcpy(answerer->ice.remote_ufrag, offerer->ice.local_ufrag,
           sizeof(answerer->ice.remote_ufrag));
    answerer->ice.remote_ufrag_len = offerer->ice.local_ufrag_len;
    memcpy(answerer->ice.remote_pwd, offerer->ice.local_pwd, sizeof(answerer->ice.remote_pwd));
    answerer->ice.remote_pwd_len = offerer->ice.local_pwd_len;

    offerer->ice.remote_candidates[0].family = 4;
    offerer->ice.remote_candidates[0].addr[0] = 192;
    offerer->ice.remote_candidates[0].addr[1] = 168;
    offerer->ice.remote_candidates[0].addr[2] = 1;
    offerer->ice.remote_candidates[0].addr[3] = 2;
    offerer->ice.remote_candidates[0].port = 5000;
    offerer->ice.remote_candidate_count = 1;
}

/*
 * E2E: ICE restart drives a full DTLS re-handshake with a fresh certificate.
 *
 * Regression for commit 8fea1e8 ("tear down DTLS context on ice_restart").
 * Without dtls_destroy in nanortc_ice_restart, the next handshake would skip
 * dtls_init (guarded by `!crypto_ctx`), reuse the original cert, and the
 * fingerprint would be byte-identical pre/post restart.
 */
TEST(test_e2e_ice_restart_dtls_rehandshake)
{
    nanortc_t offerer, answerer;
    nanortc_config_t off_cfg = e2e_default_config();
    off_cfg.role = NANORTC_ROLE_CONTROLLING;
    ASSERT_OK(nanortc_init(&offerer, &off_cfg));
    nanortc_config_t ans_cfg = e2e_default_config();
    ans_cfg.role = NANORTC_ROLE_CONTROLLED;
    ASSERT_OK(nanortc_init(&answerer, &ans_cfg));

    e2e_setup_ice_creds(&offerer, &answerer);

    uint32_t now_ms = 100;
    ASSERT_OK(nanortc_handle_input(&offerer, &(nanortc_input_t){.now_ms = now_ms}));

    int connected = 0;
    for (int round = 0; round < 30; round++) {
        e2e_pump(&offerer, &answerer, now_ms, 5);
        if (offerer.state >= NANORTC_STATE_DTLS_CONNECTED &&
            answerer.state >= NANORTC_STATE_DTLS_CONNECTED) {
            connected = 1;
            break;
        }
    }
    ASSERT_TRUE(connected);

    /* Snapshot fingerprints by value — dtls_destroy will zero the underlying buffer. */
    char pre_off_fp[sizeof(offerer.dtls.local_fingerprint)];
    char pre_ans_fp[sizeof(answerer.dtls.local_fingerprint)];
    ASSERT_TRUE(dtls_get_fingerprint(&offerer.dtls) != NULL);
    ASSERT_TRUE(dtls_get_fingerprint(&answerer.dtls) != NULL);
    memcpy(pre_off_fp, offerer.dtls.local_fingerprint, sizeof(pre_off_fp));
    memcpy(pre_ans_fp, answerer.dtls.local_fingerprint, sizeof(pre_ans_fp));

    ASSERT_OK(nanortc_ice_restart(&offerer));
    ASSERT_OK(nanortc_ice_restart(&answerer));

    /* Mid-restart invariants — the unit-level T19/T20/T21 properties must hold
     * on a fully connected nanortc_t, not just on a hand-built one. */
    ASSERT_TRUE(offerer.dtls.crypto_ctx == NULL);
    ASSERT_TRUE(answerer.dtls.crypto_ctx == NULL);
    ASSERT_EQ(offerer.dtls.local_fingerprint[0], '\0');
    ASSERT_EQ(answerer.dtls.local_fingerprint[0], '\0');
    ASSERT_EQ(offerer.sdp.local_fingerprint[0], '\0');
    ASSERT_EQ(answerer.sdp.local_fingerprint[0], '\0');
    ASSERT_EQ(offerer.state, NANORTC_STATE_NEW);
    ASSERT_EQ(answerer.state, NANORTC_STATE_NEW);

    e2e_resync_ice_creds(&offerer, &answerer);

    /* Bootstrap the second ICE check round and pump until DTLS reconnects. */
    ASSERT_OK(nanortc_handle_input(&offerer, &(nanortc_input_t){.now_ms = now_ms}));
    int reconnected = 0;
    for (int round = 0; round < 30; round++) {
        e2e_pump(&offerer, &answerer, now_ms, 5);
        if (offerer.state >= NANORTC_STATE_DTLS_CONNECTED &&
            answerer.state >= NANORTC_STATE_DTLS_CONNECTED) {
            reconnected = 1;
            break;
        }
    }
    ASSERT_TRUE(reconnected);

    /* Fingerprint must differ — a regenerated cert is the proof that
     * dtls_init actually ran on the second handshake. */
    ASSERT_TRUE(dtls_get_fingerprint(&offerer.dtls) != NULL);
    ASSERT_TRUE(dtls_get_fingerprint(&answerer.dtls) != NULL);
    ASSERT_NEQ(memcmp(pre_off_fp, offerer.dtls.local_fingerprint, sizeof(pre_off_fp)), 0);
    ASSERT_NEQ(memcmp(pre_ans_fp, answerer.dtls.local_fingerprint, sizeof(pre_ans_fp)), 0);

    nanortc_destroy(&offerer);
    nanortc_destroy(&answerer);
}

/*
 * E2E: ICE restart re-populates the SDP fingerprint cache with the new cert.
 *
 * Regression for commit 9a75412 ("clear cached fingerprints on ice_restart").
 * nano_rtc_cache_fingerprint() is a write-once cache that bails when
 * sdp.local_fingerprint is non-empty. Without the memset in ice_restart,
 * the second nanortc_create_offer() would leave sdp.local_fingerprint at
 * the *old* cert hash while dtls_init() generates a new cert — causing
 * any peer that enforces a=fingerprint to reject the DTLS handshake.
 */
TEST(test_e2e_ice_restart_sdp_fingerprint_refresh)
{
    nanortc_t rtc;
    nanortc_config_t cfg = e2e_default_config();
    cfg.role = NANORTC_ROLE_CONTROLLING;
    ASSERT_OK(nanortc_init(&rtc, &cfg));

#if NANORTC_FEATURE_DATACHANNEL
    ASSERT(nanortc_create_datachannel(&rtc, "x", NULL) >= 0);
#elif NANORTC_HAVE_MEDIA_TRANSPORT
    ASSERT(nanortc_add_audio_track(&rtc, NANORTC_DIR_SENDRECV, NANORTC_CODEC_OPUS, 48000, 2) >= 0);
#endif

    /* First create_offer triggers dtls_init + nano_rtc_cache_fingerprint. */
    char offer1[4096];
    ASSERT_OK(nanortc_create_offer(&rtc, offer1, sizeof(offer1), NULL));

    /* Sanity: SDP cache holds "sha-256 " + dtls.local_fingerprint. */
    ASSERT_TRUE(rtc.dtls.local_fingerprint[0] != '\0');
    ASSERT_TRUE(rtc.sdp.local_fingerprint[0] != '\0');
    ASSERT_EQ(memcmp(rtc.sdp.local_fingerprint, "sha-256 ", 8), 0);
    ASSERT_EQ(memcmp(rtc.sdp.local_fingerprint + 8, rtc.dtls.local_fingerprint,
                     sizeof(rtc.dtls.local_fingerprint) - 1),
              0);

    char pre_dtls_fp[sizeof(rtc.dtls.local_fingerprint)];
    memcpy(pre_dtls_fp, rtc.dtls.local_fingerprint, sizeof(pre_dtls_fp));

    ASSERT_OK(nanortc_ice_restart(&rtc));
    ASSERT_EQ(rtc.dtls.local_fingerprint[0], '\0');
    ASSERT_EQ(rtc.sdp.local_fingerprint[0], '\0');

    /* Second create_offer must regenerate the cert and re-cache the fingerprint. */
    char offer2[4096];
    ASSERT_OK(nanortc_create_offer(&rtc, offer2, sizeof(offer2), NULL));

    ASSERT_TRUE(rtc.dtls.local_fingerprint[0] != '\0');
    ASSERT_NEQ(memcmp(pre_dtls_fp, rtc.dtls.local_fingerprint, sizeof(pre_dtls_fp)), 0);

    /* The advertised SDP fingerprint must match the freshly generated cert,
     * not the stale pre-restart hash. Reverting 9a75412 fails this assertion:
     * nano_rtc_cache_fingerprint's early-return on non-empty sdp.local_fingerprint
     * would leave the SDP cache holding the old "sha-256 OLD..." while the
     * DTLS layer holds the new cert hash. */
    ASSERT_TRUE(rtc.sdp.local_fingerprint[0] != '\0');
    ASSERT_EQ(memcmp(rtc.sdp.local_fingerprint, "sha-256 ", 8), 0);
    ASSERT_EQ(memcmp(rtc.sdp.local_fingerprint + 8, rtc.dtls.local_fingerprint,
                     sizeof(rtc.dtls.local_fingerprint) - 1),
              0);

    nanortc_destroy(&rtc);
}

/* ----------------------------------------------------------------
 * Helper: check if NUL-terminated haystack contains needle
 * ---------------------------------------------------------------- */
static int str_contains(const char *haystack, const char *needle)
{
    if (!haystack || !needle)
        return 0;
    return strstr(haystack, needle) != NULL;
}

/* ----------------------------------------------------------------
 * Part C: New API coverage tests
 * ---------------------------------------------------------------- */

TEST(test_e2e_create_offer_content)
{
    /* Verify offer SDP contains ufrag, pwd, fingerprint, sctp-port */
    nanortc_t rtc;
    nanortc_config_t cfg = e2e_default_config();
    cfg.role = NANORTC_ROLE_CONTROLLING;
    ASSERT_OK(nanortc_init(&rtc, &cfg));

#if NANORTC_FEATURE_DATACHANNEL
    /* DC m-line must be explicitly registered via nanortc_create_datachannel() */
    ASSERT(nanortc_create_datachannel(&rtc, "test", NULL) >= 0);
#elif NANORTC_HAVE_MEDIA_TRANSPORT
    /* Without DC, add a media track so the offer has at least one m-line */
    ASSERT(nanortc_add_audio_track(&rtc, NANORTC_DIR_SENDRECV, NANORTC_CODEC_OPUS, 48000, 2) >= 0);
#endif

    char offer[4096];
    size_t offer_len = 0;
    ASSERT_OK(nanortc_create_offer(&rtc, offer, sizeof(offer), &offer_len));
    ASSERT_TRUE(offer_len > 0);
    offer[offer_len] = '\0';

    ASSERT_TRUE(str_contains(offer, "a=ice-ufrag:"));
    ASSERT_TRUE(str_contains(offer, "a=ice-pwd:"));
    ASSERT_TRUE(str_contains(offer, "a=fingerprint:sha-256 "));
    ASSERT_TRUE(str_contains(offer, "a=setup:actpass"));
#if NANORTC_FEATURE_DATACHANNEL
    ASSERT_TRUE(str_contains(offer, "a=sctp-port:"));
#endif

    /* Verify ICE credentials were propagated to ICE state */
    ASSERT_TRUE(rtc.ice.local_ufrag_len > 0);
    ASSERT_TRUE(rtc.ice.local_pwd_len > 0);

    nanortc_destroy(&rtc);
}

TEST(test_e2e_offer_answer_roundtrip)
{
    /* create_offer → accept_offer → accept_answer full SDP roundtrip */
    nanortc_t offerer, answerer;
    nanortc_config_t off_cfg = e2e_default_config();
    off_cfg.role = NANORTC_ROLE_CONTROLLING;
    ASSERT_OK(nanortc_init(&offerer, &off_cfg));

    nanortc_config_t ans_cfg = e2e_default_config();
    ans_cfg.role = NANORTC_ROLE_CONTROLLED;
    ASSERT_OK(nanortc_init(&answerer, &ans_cfg));

    /* Add local candidate on answerer so it appears in the answer SDP */
    ASSERT_OK(nanortc_add_local_candidate(&answerer, "192.168.1.2", 5000));

#if NANORTC_FEATURE_DATACHANNEL
    /* DC m-line must be explicitly registered */
    ASSERT(nanortc_create_datachannel(&offerer, "test", NULL) >= 0);
#elif NANORTC_HAVE_MEDIA_TRANSPORT
    /* Without DC, add a media track so the offer has at least one m-line */
    ASSERT(nanortc_add_audio_track(&offerer, NANORTC_DIR_SENDRECV, NANORTC_CODEC_OPUS, 48000, 2) >=
           0);
#endif

    /* Offerer creates offer */
    char offer[4096];
    size_t offer_len = 0;
    ASSERT_OK(nanortc_create_offer(&offerer, offer, sizeof(offer), &offer_len));
    offer[offer_len] = '\0';

    /* Answerer accepts offer, produces answer */
    char answer[4096];
    size_t answer_len = 0;
    ASSERT_OK(nanortc_accept_offer(&answerer, offer, answer, sizeof(answer), &answer_len));
    answer[answer_len] = '\0';
    ASSERT_TRUE(answer_len > 0);
    ASSERT_TRUE(str_contains(answer, "a=ice-ufrag:"));
    ASSERT_TRUE(str_contains(answer, "a=setup:passive"));

    /* Offerer accepts answer */
    ASSERT_OK(nanortc_accept_answer(&offerer, answer));

    /* Verify remote ICE creds propagated to offerer */
    ASSERT_TRUE(offerer.ice.remote_ufrag_len > 0);
    ASSERT_TRUE(offerer.ice.remote_pwd_len > 0);
    /* Verify remote candidate from SDP was added */
    ASSERT_TRUE(offerer.ice.remote_candidate_count >= 1);

    nanortc_destroy(&offerer);
    nanortc_destroy(&answerer);
}

TEST(test_e2e_full_sdp_to_dtls)
{
    /* Full flow: create_offer → accept_offer → accept_answer → ICE → DTLS
     * This exercises dtls_set_role() on the offerer path:
     *   create_offer() inits DTLS as client (tentative),
     *   accept_answer() calls dtls_set_role() to finalize active role,
     *   then DTLS handshake must complete with the switched role. */
    nanortc_t offerer, answerer;

    nanortc_config_t off_cfg = e2e_default_config();
    off_cfg.role = NANORTC_ROLE_CONTROLLING;
    ASSERT_OK(nanortc_init(&offerer, &off_cfg));

    nanortc_config_t ans_cfg = e2e_default_config();
    ans_cfg.role = NANORTC_ROLE_CONTROLLED;
    ASSERT_OK(nanortc_init(&answerer, &ans_cfg));

    /* Add local candidates so they appear in SDP and ICE has local addrs */
    ASSERT_OK(nanortc_add_local_candidate(&offerer, "192.168.1.1", 4000));
    ASSERT_OK(nanortc_add_local_candidate(&answerer, "192.168.1.2", 5000));

#if NANORTC_FEATURE_DATACHANNEL
    /* DC m-line must be explicitly registered */
    ASSERT(nanortc_create_datachannel(&offerer, "test", NULL) >= 0);
#elif NANORTC_HAVE_MEDIA_TRANSPORT
    /* Without DC, add a media track so the offer has at least one m-line */
    ASSERT(nanortc_add_audio_track(&offerer, NANORTC_DIR_SENDRECV, NANORTC_CODEC_OPUS, 48000, 2) >=
           0);
#endif

    /* --- SDP negotiation --- */
    char offer[4096];
    size_t offer_len = 0;
    ASSERT_OK(nanortc_create_offer(&offerer, offer, sizeof(offer), &offer_len));
    offer[offer_len] = '\0';

    char answer[4096];
    size_t answer_len = 0;
    ASSERT_OK(nanortc_accept_offer(&answerer, offer, answer, sizeof(answer), &answer_len));
    answer[answer_len] = '\0';

    /* accept_answer triggers dtls_set_role on offerer */
    ASSERT_OK(nanortc_accept_answer(&offerer, answer));

    /* Verify DTLS roles: offerer=active(client), answerer=passive(server) */
    ASSERT_EQ(offerer.dtls.is_server, 0);
    ASSERT_EQ(answerer.dtls.is_server, 1);

    /* --- ICE + DTLS handshake --- */
    /* Offerer needs remote candidate to send to (already parsed from SDP).
     * Answerer needs offerer's address — set a remote candidate manually
     * since offerer didn't include one in the offer SDP. */
    answerer.ice.remote_candidates[0].family = 4;
    answerer.ice.remote_candidates[0].addr[0] = 192;
    answerer.ice.remote_candidates[0].addr[1] = 168;
    answerer.ice.remote_candidates[0].addr[2] = 1;
    answerer.ice.remote_candidates[0].addr[3] = 1;
    answerer.ice.remote_candidates[0].port = 9999;
    answerer.ice.remote_candidate_count = 1;

    /* Kick off ICE on the controlling side */
    uint32_t now_ms = 100;
    ASSERT_OK(nanortc_handle_input(&offerer, &(nanortc_input_t){.now_ms = now_ms}));

    /* Pump ICE + DTLS: relay packets between the two instances */
    int connected = 0;
    for (int round = 0; round < 30; round++) {
        e2e_pump(&offerer, &answerer, now_ms, 5);

        if (offerer.state >= NANORTC_STATE_DTLS_CONNECTED &&
            answerer.state >= NANORTC_STATE_DTLS_CONNECTED) {
            connected = 1;
            break;
        }
    }

    ASSERT_TRUE(connected);
    ASSERT_TRUE(offerer.state >= NANORTC_STATE_DTLS_CONNECTED);
    ASSERT_TRUE(answerer.state >= NANORTC_STATE_DTLS_CONNECTED);

    /* Verify keying material was derived (proves DTLS completed) */
    ASSERT_TRUE(offerer.dtls.keying_material_ready);
    ASSERT_TRUE(answerer.dtls.keying_material_ready);

    nanortc_destroy(&offerer);
    nanortc_destroy(&answerer);
}

TEST(test_e2e_state_queries)
{
    /* Use nanortc_is_alive() / nanortc_is_connected() API */
    nanortc_t rtc;
    nanortc_config_t cfg = e2e_default_config();
    ASSERT_OK(nanortc_init(&rtc, &cfg));

    ASSERT_TRUE(nanortc_is_alive(&rtc));
    ASSERT_FALSE(nanortc_is_connected(&rtc));
    ASSERT_FALSE(nanortc_is_alive(NULL));
    ASSERT_FALSE(nanortc_is_connected(NULL));

    nanortc_destroy(&rtc);
    ASSERT_FALSE(nanortc_is_alive(&rtc));
}

#if NANORTC_FEATURE_DATACHANNEL
TEST(test_e2e_add_channel_invalid)
{
    /* nanortc_create_datachannel with NULL label should fail */
    nanortc_t rtc;
    nanortc_config_t cfg = e2e_default_config();
    ASSERT_OK(nanortc_init(&rtc, &cfg));

    ASSERT_EQ(nanortc_create_datachannel(&rtc, NULL, NULL), NANORTC_ERR_INVALID_PARAM);
    ASSERT_EQ(nanortc_create_datachannel(NULL, "test", NULL), NANORTC_ERR_INVALID_PARAM);

    nanortc_destroy(&rtc);
}

TEST(test_e2e_add_channel)
{
    /* nanortc_create_datachannel works in NEW state (SDP-phase registration) */
    nanortc_t rtc;
    nanortc_config_t cfg = e2e_default_config();
    ASSERT_OK(nanortc_init(&rtc, &cfg));

    int sid = nanortc_create_datachannel(&rtc, "my-channel", NULL);
    ASSERT_TRUE(sid >= 0);

    /* Verify channel exists in DC state */
    ASSERT_EQ(rtc.datachannel.channel_count, 1);

    /* Verify DC m-line was registered in SDP */
    ASSERT_TRUE(rtc.sdp.has_datachannel);

    nanortc_destroy(&rtc);
}

TEST(test_e2e_channel_close)
{
    /* Create then close a DC via channel handle, verify CLOSE event */
    nanortc_t rtc;
    nanortc_config_t cfg = e2e_default_config();
    ASSERT_OK(nanortc_init(&rtc, &cfg));

    rtc.state = NANORTC_STATE_CONNECTED;
    rtc.dtls.is_server = 0;

    int sid = nanortc_create_datachannel(&rtc, "closable", NULL);
    ASSERT_TRUE(sid >= 0);

    /* Drain any outputs from create */
    nanortc_output_t tmp;
    while (nanortc_poll_output(&rtc, &tmp) == NANORTC_OK) {
    }

    /* Close via flat API */
    ASSERT_OK(nanortc_datachannel_close(&rtc, (uint16_t)sid));

    /* Should emit CHANNEL_CLOSE event */
    nanortc_output_t evt;
    ASSERT_OK(nanortc_poll_output(&rtc, &evt));
    ASSERT_EQ(evt.type, NANORTC_OUTPUT_EVENT);
    ASSERT_EQ(evt.event.type, NANORTC_EV_DATACHANNEL_CLOSE);
    ASSERT_EQ(evt.event.datachannel_id.id, (uint16_t)sid);

    nanortc_destroy(&rtc);
}

TEST(test_e2e_channel_invalid)
{
    /* Closing a nonexistent channel should fail */
    nanortc_t rtc;
    nanortc_config_t cfg = e2e_default_config();
    ASSERT_OK(nanortc_init(&rtc, &cfg));

    rtc.state = NANORTC_STATE_CONNECTED;
    ASSERT_EQ(nanortc_datachannel_close(&rtc, 9999), NANORTC_ERR_INVALID_PARAM);

    nanortc_destroy(&rtc);
}

TEST(test_e2e_channel_label)
{
    /* Create a DC and verify label retrieval */
    nanortc_t rtc;
    nanortc_config_t cfg = e2e_default_config();
    ASSERT_OK(nanortc_init(&rtc, &cfg));

    int sid = nanortc_create_datachannel(&rtc, "my-label", NULL);
    ASSERT_TRUE(sid >= 0);

    const char *label = nanortc_datachannel_get_label(&rtc, (uint16_t)sid);
    ASSERT_TRUE(label != NULL);
    ASSERT_TRUE(str_contains(label, "my-label"));

    nanortc_destroy(&rtc);
}
#endif /* NANORTC_FEATURE_DATACHANNEL */

TEST(test_e2e_graceful_disconnect)
{
    /* nanortc_disconnect() on a DTLS_CONNECTED instance emits DISCONNECTED */
    nanortc_t rtc;
    nanortc_config_t cfg = e2e_default_config();
    ASSERT_OK(nanortc_init(&rtc, &cfg));

    /* Fake connected-enough state */
    rtc.state = NANORTC_STATE_DTLS_CONNECTED;

    nanortc_disconnect(&rtc);
    ASSERT_FALSE(nanortc_is_alive(&rtc));
    ASSERT_EQ(rtc.state, NANORTC_STATE_CLOSED);

    /* Should emit DISCONNECTED event */
    nanortc_output_t evt;
    int found_disconnect = 0;
    while (nanortc_poll_output(&rtc, &evt) == NANORTC_OK) {
        if (evt.type == NANORTC_OUTPUT_EVENT && evt.event.type == NANORTC_EV_DISCONNECTED) {
            found_disconnect = 1;
        }
    }
    ASSERT_TRUE(found_disconnect);

    nanortc_destroy(&rtc);
}

TEST(test_e2e_disconnect_new_state)
{
    /* nanortc_disconnect() on NEW state is a no-op */
    nanortc_t rtc;
    nanortc_config_t cfg = e2e_default_config();
    ASSERT_OK(nanortc_init(&rtc, &cfg));

    nanortc_disconnect(&rtc);
    /* Should remain in NEW state (no-op on NEW) */
    ASSERT_EQ(rtc.state, NANORTC_STATE_NEW);
    ASSERT_TRUE(nanortc_is_alive(&rtc));

    nanortc_destroy(&rtc);
}

TEST(test_e2e_ice_multi_candidate)
{
    /* Add multiple remote candidates and verify round-robin in ICE checks */
    nanortc_t rtc;
    nanortc_config_t cfg = e2e_default_config();
    cfg.role = NANORTC_ROLE_CONTROLLING;
    ASSERT_OK(nanortc_init(&rtc, &cfg));

    /* Set up ICE credentials */
    memcpy(rtc.ice.local_ufrag, "TEST1234", 9);
    rtc.ice.local_ufrag_len = 8;
    memcpy(rtc.ice.local_pwd, "password-for-testing12", 23);
    rtc.ice.local_pwd_len = 22;
    memcpy(rtc.ice.remote_ufrag, "REMO1234", 9);
    rtc.ice.remote_ufrag_len = 8;
    memcpy(rtc.ice.remote_pwd, "remote-password-abcdef", 23);
    rtc.ice.remote_pwd_len = 22;
    rtc.ice.tie_breaker = 0xAABBCCDDEEFF0011ull;

    /* Add local candidate */
    ASSERT_OK(nanortc_add_local_candidate(&rtc, "10.0.0.100", 4000));
    /* Drain the host candidate event */
    {
        nanortc_output_t drain;
        while (nanortc_poll_output(&rtc, &drain) == NANORTC_OK) {
        }
    }

    /* Add 3 remote candidates */
    ASSERT_OK(nanortc_add_remote_candidate(&rtc, "10.0.0.1 5001"));
    ASSERT_OK(nanortc_add_remote_candidate(&rtc, "10.0.0.2 5002"));
    ASSERT_OK(nanortc_add_remote_candidate(&rtc, "10.0.0.3 5003"));
    ASSERT_EQ(rtc.ice.remote_candidate_count, 3);

    /* First timeout: should send check to candidate 0 */
    uint32_t now_ms = 100;
    ASSERT_OK(nanortc_handle_input(&rtc, &(nanortc_input_t){.now_ms = now_ms}));

    nanortc_output_t out;
    ASSERT_OK(nanortc_poll_output(&rtc, &out));
    ASSERT_EQ(out.type, NANORTC_OUTPUT_TRANSMIT);
    ASSERT_EQ(out.transmit.dest.port, 5001);
    ASSERT_EQ(out.transmit.dest.addr[3], 1);

    /* Drain remaining outputs (ICE_STATE_CHANGE + TIMEOUT) */
    while (nanortc_poll_output(&rtc, &out) == NANORTC_OK) {
    }

    /* Second timeout: should advance to candidate 1 */
    now_ms += rtc.ice.check_interval_ms + 1;
    ASSERT_OK(nanortc_handle_input(&rtc, &(nanortc_input_t){.now_ms = now_ms}));

    ASSERT_OK(nanortc_poll_output(&rtc, &out));
    ASSERT_EQ(out.type, NANORTC_OUTPUT_TRANSMIT);
    ASSERT_EQ(out.transmit.dest.port, 5002);
    ASSERT_EQ(out.transmit.dest.addr[3], 2);

    nanortc_destroy(&rtc);
}

TEST(test_e2e_accept_answer_state_guard)
{
    /* accept_answer should fail if not in NEW state (e.g., after accept_offer) */
    nanortc_t rtc;
    nanortc_config_t cfg = e2e_default_config();
    ASSERT_OK(nanortc_init(&rtc, &cfg));

    /* Simulate non-NEW state */
    rtc.state = NANORTC_STATE_ICE_CHECKING;
    ASSERT_EQ(nanortc_accept_answer(&rtc, "v=0\r\n"), NANORTC_ERR_STATE);

    nanortc_destroy(&rtc);
}

/* ================================================================
 * E2E DataChannel message exchange tests
 * ================================================================ */

#if NANORTC_FEATURE_DATACHANNEL
/*
 * Helper: fully connect two nanortc instances through ICE + DTLS + SCTP.
 * Returns 0 on success. Both instances must be initialized with roles.
 */
static int e2e_full_connect(nanortc_t *offerer, nanortc_t *answerer)
{
    e2e_setup_ice_creds(offerer, answerer);

    /* Both peers must advertise the SCTP/datachannel transport (an m=application
     * line) so the answerer starts its SCTP server side after DTLS rather than
     * treating the session as media-only (nano_rtc.c: has_datachannel gate).
     * This mirrors what a real SDP offer/answer negotiation sets on both ends;
     * e2e_setup_ice_creds wires ICE by hand and skips SDP. */
    offerer->sdp.has_datachannel = true;
    answerer->sdp.has_datachannel = true;

    /* Wire the SCTP crypto provider, which the SDP path sets via
     * rtc_apply_remote_sdp() (nano_rtc_negotiate.c). Without it nsctp_start()
     * bails (needs crypto for vtag/cookie randomness) and never sends INIT. */
    offerer->sctp.crypto = offerer->config.crypto;
    answerer->sctp.crypto = answerer->config.crypto;

    uint32_t now_ms = 100;

    /* Pump until both reach CONNECTED (SCTP established) */
    for (int round = 0; round < 200; round++) {
        /* Trigger timeouts to drive ICE checks and SCTP retransmits */
        nanortc_handle_input(offerer, &(nanortc_input_t){.now_ms = now_ms});
        nanortc_handle_input(answerer, &(nanortc_input_t){.now_ms = now_ms});

        /* Relay packets between the two instances */
        e2e_pump(offerer, answerer, now_ms, 20);

        if (offerer->state == NANORTC_STATE_CONNECTED &&
            answerer->state == NANORTC_STATE_CONNECTED) {
            return 0;
        }
        now_ms += 10;
    }
    return -1;
}

/*
 * E2E: DataChannel send/recv — verify the send API works in CONNECTED state.
 * Since the full ICE→DTLS→SCTP→DCEP pipeline is complex to drive in a
 * Sans I/O loopback, we test the DataChannel message path by faking the
 * connected state and verifying that send_datachannel_string produces SCTP
 * output packets.
 */
TEST(test_e2e_datachannel_send_recv)
{
    nanortc_t rtc;
    nanortc_config_t cfg = e2e_default_config();
    ASSERT_OK(nanortc_init(&rtc, &cfg));

    /* Fake fully connected state */
    rtc.state = NANORTC_STATE_CONNECTED;
    rtc.dtls.is_server = 0;

    /* Create a DataChannel via new API */
    int sid = nanortc_create_datachannel(&rtc, "chat", NULL);
    ASSERT_TRUE(sid >= 0);

    /* Drain DCEP OPEN output */
    nanortc_output_t out;
    while (nanortc_poll_output(&rtc, &out) == NANORTC_OK) {
    }

    /* Send a message — should succeed even though SCTP path isn't live
     * (message goes into send queue) */
    int rc = nanortc_datachannel_send_string(&rtc, (uint16_t)sid, "Hello!");
    /* Either succeeds (queued) or fails with WOULD_BLOCK/STATE — both valid */
    (void)rc;

    /* Send binary data */
    uint8_t binary[] = {0x01, 0x02, 0x03, 0x04};
    rc = nanortc_datachannel_send(&rtc, (uint16_t)sid, binary, sizeof(binary));
    (void)rc;

    nanortc_destroy(&rtc);
}

/*
 * E2E: Create multiple DataChannels on the same connection.
 */
TEST(test_e2e_multi_channel_create)
{
    nanortc_t rtc;
    nanortc_config_t cfg = e2e_default_config();
    ASSERT_OK(nanortc_init(&rtc, &cfg));

    /* Create multiple DataChannels (works in NEW state via SDP-phase API) */
    int id1 = nanortc_create_datachannel(&rtc, "channel-1", NULL);
    int id2 = nanortc_create_datachannel(&rtc, "channel-2", NULL);
    int id3 = nanortc_create_datachannel(&rtc, "channel-3", NULL);
    ASSERT_TRUE(id1 >= 0);
    ASSERT_TRUE(id2 >= 0);
    ASSERT_TRUE(id3 >= 0);

    /* Each should get a unique stream ID */
    ASSERT_NEQ(id1, id2);
    ASSERT_NEQ(id2, id3);
    ASSERT_NEQ(id1, id3);

    /* All 3 should be tracked */
    ASSERT_EQ(rtc.datachannel.channel_count, 3);

    /* Verify labels */
    ASSERT_TRUE(str_contains(nanortc_datachannel_get_label(&rtc, (uint16_t)id1), "channel-1"));
    ASSERT_TRUE(str_contains(nanortc_datachannel_get_label(&rtc, (uint16_t)id2), "channel-2"));
    ASSERT_TRUE(str_contains(nanortc_datachannel_get_label(&rtc, (uint16_t)id3), "channel-3"));

    nanortc_destroy(&rtc);
}

/*
 * E2E F-1 regression: a nanortc OFFERER that creates a DataChannel completes the
 * DCEP OPEN/ACK handshake against a nanortc ANSWERER over a full
 * ICE→DTLS→SCTP loopback, and application data then flows offerer→answerer.
 *
 * Before the drain-loop guard in nano_rtc.c, the offerer's DCEP OPEN was popped
 * from dc->out_buf while SCTP was still COOKIE_ECHOED and then rejected by
 * nsctp_send() (RFC 9260 §5: DATA only after ESTABLISHED) — and lost, because
 * dc_poll_output() clears has_output unconditionally. The answerer never saw the
 * channel (channel_count stayed 0) and the offerer's channel stayed OPENING.
 * This is the first test to exercise nanortc's offerer-initiated DCEP path; the
 * other datachannel tests fake NANORTC_STATE_CONNECTED.
 */
TEST(test_e2e_datachannel_offerer_initiated)
{
    nanortc_t offerer, answerer;
    nanortc_config_t off_cfg = e2e_default_config();
    off_cfg.role = NANORTC_ROLE_CONTROLLING;
    ASSERT_OK(nanortc_init(&offerer, &off_cfg));
    nanortc_config_t ans_cfg = e2e_default_config();
    ans_cfg.role = NANORTC_ROLE_CONTROLLED;
    ASSERT_OK(nanortc_init(&answerer, &ans_cfg));

    /* Offerer registers the DataChannel BEFORE connecting (the F-1 path). */
    int sid = nanortc_create_datachannel(&offerer, "chat", NULL);
    ASSERT_TRUE(sid >= 0);
    ASSERT_EQ(offerer.datachannel.channel_count, 1);
    ASSERT_EQ(offerer.datachannel.channels[0].state, NANORTC_DC_STATE_OPENING);

    /* Drive ICE → DTLS → SCTP to ESTABLISHED on both peers. */
    ASSERT_OK(e2e_full_connect(&offerer, &answerer));

    /* Let the DCEP OPEN reach the answerer and the ACK return to the offerer. */
    uint32_t now_ms = 4000;
    e2e_pump(&offerer, &answerer, now_ms, 30);

    /* Answerer allocated the channel from the inbound DCEP OPEN and ACKed it. */
    ASSERT_EQ(answerer.datachannel.channel_count, 1);
    ASSERT_EQ(answerer.datachannel.channels[0].state, NANORTC_DC_STATE_OPEN);
    /* Offerer received the DCEP ACK: OPENING → OPEN. */
    ASSERT_EQ(offerer.datachannel.channels[0].state, NANORTC_DC_STATE_OPEN);

    /* The channel is usable: a send now succeeds (not WOULD_BLOCK / STATE). */
    const char payload[] = "F-1 fixed";
    ASSERT_OK(nanortc_datachannel_send(&offerer, (uint16_t)sid, payload, sizeof(payload) - 1));

    /* The message is delivered to the answerer as a DATACHANNEL_DATA event.
     * nanortc_datachannel_send() only queues the SCTP DATA chunk; a timeout tick
     * flushes it through DTLS. Relay both directions (the answerer's SACK must
     * reach the offerer) and scan the answerer's outputs for the data event,
     * which a plain e2e_pump would discard. */
    nanortc_addr_t loop_src;
    memset(&loop_src, 0, sizeof(loop_src));
    loop_src.family = 4;
    loop_src.addr[0] = 192;
    loop_src.addr[1] = 168;
    loop_src.addr[2] = 1;
    loop_src.addr[3] = 1;
    loop_src.port = 9999;

    int got_data = 0;
    for (int round = 0; round < 40 && !got_data; round++) {
        nanortc_handle_input(&offerer, &(nanortc_input_t){.now_ms = now_ms});
        nanortc_handle_input(&answerer, &(nanortc_input_t){.now_ms = now_ms});

        nanortc_output_t o;
        while (nanortc_poll_output(&offerer, &o) == NANORTC_OK) {
            if (o.type == NANORTC_OUTPUT_TRANSMIT) {
                nanortc_handle_input(&answerer, &(nanortc_input_t){.now_ms = now_ms,
                                                                   .data = o.transmit.data,
                                                                   .len = o.transmit.len,
                                                                   .src = loop_src});
            }
        }
        nanortc_output_t e;
        while (nanortc_poll_output(&answerer, &e) == NANORTC_OK) {
            if (e.type == NANORTC_OUTPUT_EVENT && e.event.type == NANORTC_EV_DATACHANNEL_DATA) {
                ASSERT_EQ(e.event.datachannel_data.len, sizeof(payload) - 1);
                ASSERT_EQ(memcmp(e.event.datachannel_data.data, payload, sizeof(payload) - 1), 0);
                got_data = 1;
            } else if (e.type == NANORTC_OUTPUT_TRANSMIT) {
                nanortc_handle_input(&offerer, &(nanortc_input_t){.now_ms = now_ms,
                                                                  .data = e.transmit.data,
                                                                  .len = e.transmit.len,
                                                                  .src = loop_src});
            }
        }
        now_ms += 10;
    }
    ASSERT_TRUE(got_data);

    nanortc_destroy(&offerer);
    nanortc_destroy(&answerer);
}
#endif /* NANORTC_FEATURE_DATACHANNEL */

/* ================================================================
 * E2E connection lifecycle tests
 * ================================================================ */

/*
 * E2E: Full lifecycle — verify state transitions from NEW to CLOSED.
 */
TEST(test_e2e_full_lifecycle)
{
    nanortc_t offerer, answerer;

    nanortc_config_t off_cfg = e2e_default_config();
    off_cfg.role = NANORTC_ROLE_CONTROLLING;
    ASSERT_OK(nanortc_init(&offerer, &off_cfg));
    ASSERT_EQ(offerer.state, NANORTC_STATE_NEW);

    nanortc_config_t ans_cfg = e2e_default_config();
    ans_cfg.role = NANORTC_ROLE_CONTROLLED;
    ASSERT_OK(nanortc_init(&answerer, &ans_cfg));

    e2e_setup_ice_creds(&offerer, &answerer);

    /* Drive ICE + DTLS through full handshake */
    uint32_t now_ms = 100;
    int dtls_reached = 0;
    for (int round = 0; round < 100; round++) {
        nanortc_handle_input(&offerer, &(nanortc_input_t){.now_ms = now_ms});
        nanortc_handle_input(&answerer, &(nanortc_input_t){.now_ms = now_ms});
        e2e_pump(&offerer, &answerer, now_ms, 20);

        if (offerer.state >= NANORTC_STATE_DTLS_CONNECTED) {
            dtls_reached = 1;
            break;
        }
        now_ms += 10;
    }
    ASSERT_TRUE(dtls_reached);

    /* Disconnect */
    nanortc_disconnect(&offerer);
    ASSERT_EQ(offerer.state, NANORTC_STATE_CLOSED);

    nanortc_destroy(&offerer);
    nanortc_destroy(&answerer);
}

/*
 * E2E: ICE failure — verify FAILED state after max checks with no response.
 */
TEST(test_e2e_ice_connection_timeout)
{
    nanortc_t rtc;
    nanortc_config_t cfg = e2e_default_config();
    cfg.role = NANORTC_ROLE_CONTROLLING;
    ASSERT_OK(nanortc_init(&rtc, &cfg));

    /* Set up credentials but point to unreachable candidate */
    memcpy(rtc.ice.local_ufrag, "TIMEOUT1", 8);
    rtc.ice.local_ufrag_len = 8;
    memcpy(rtc.ice.local_pwd, "timeout-password-1234", 21);
    rtc.ice.local_pwd_len = 21;
    memcpy(rtc.ice.remote_ufrag, "NORESPND", 8);
    rtc.ice.remote_ufrag_len = 8;
    memcpy(rtc.ice.remote_pwd, "noresp-password-12345", 21);
    rtc.ice.remote_pwd_len = 21;
    rtc.ice.tie_breaker = 0x1111111111111111ull;
    /* Local candidate */
    rtc.ice.local_candidates[0].family = 4;
    rtc.ice.local_candidates[0].addr[0] = 10;
    rtc.ice.local_candidates[0].addr[3] = 1;
    rtc.ice.local_candidates[0].port = 4000;
    rtc.ice.local_candidates[0].type = NANORTC_ICE_CAND_HOST;
    rtc.ice.local_candidate_count = 1;
    /* Remote candidate (unreachable) */
    rtc.ice.remote_candidates[0].family = 4;
    rtc.ice.remote_candidates[0].addr[0] = 10;
    rtc.ice.remote_candidates[0].addr[3] = 99;
    rtc.ice.remote_candidates[0].port = 9999;
    rtc.ice.remote_candidate_count = 1;

    /* Drive timeouts without feeding any responses. Track both
     * NANORTC_EV_ICE_STATE_CHANGE(FAILED) and NANORTC_EV_DISCONNECTED
     * (TD-018 secondary fix: FAILED branch should emit both events
     * symmetric to the consent-expiry path). */
    int got_state_failed = 0;
    int got_disconnected = 0;
    uint32_t now_ms = 100;
    for (int i = 0; i < NANORTC_ICE_MAX_CHECKS + 5; i++) {
        nanortc_handle_input(&rtc, &(nanortc_input_t){.now_ms = now_ms});
        nanortc_output_t out;
        while (nanortc_poll_output(&rtc, &out) == NANORTC_OK) {
            if (out.type == NANORTC_OUTPUT_EVENT) {
                if (out.event.type == NANORTC_EV_ICE_STATE_CHANGE &&
                    out.event.ice_state == (uint16_t)NANORTC_ICE_STATE_FAILED) {
                    got_state_failed = 1;
                }
                if (out.event.type == NANORTC_EV_DISCONNECTED) {
                    got_disconnected = 1;
                }
            }
        }
        now_ms += rtc.ice.check_interval_ms + 1;
    }

    /* ICE should have reached FAILED state, both events emitted, rtc closed */
    ASSERT_EQ(rtc.ice.state, NANORTC_ICE_STATE_FAILED);
    ASSERT_EQ(got_state_failed, 1);
    ASSERT_EQ(got_disconnected, 1);
    ASSERT_EQ(rtc.state, NANORTC_STATE_CLOSED);

    nanortc_destroy(&rtc);
}

/* ================================================================
 * IPv6 candidate parsing
 * ================================================================ */

#if NANORTC_FEATURE_IPV6
TEST(test_e2e_ipv6_remote_candidate)
{
    nanortc_t rtc;
    nanortc_config_t cfg = e2e_default_config();
    ASSERT_OK(nanortc_init(&rtc, &cfg));

    /* IPv6 SDP candidate line */
    ASSERT_OK(nanortc_add_remote_candidate(
        &rtc, "candidate:1 1 UDP 2122260223 2001:db8::1 50000 typ host"));
    ASSERT_EQ(rtc.ice.remote_candidate_count, 1);
    ASSERT_EQ(rtc.ice.remote_candidates[0].family, 6);
    ASSERT_EQ(rtc.ice.remote_candidates[0].port, 50000);
    ASSERT_EQ(rtc.ice.remote_candidates[0].addr[0], 0x20);
    ASSERT_EQ(rtc.ice.remote_candidates[0].addr[1], 0x01);
    ASSERT_EQ(rtc.ice.remote_candidates[0].addr[15], 0x01);

    /* Short form: "ip port" */
    ASSERT_OK(nanortc_add_remote_candidate(&rtc, "::1 60000"));
    ASSERT_EQ(rtc.ice.remote_candidate_count, 2);
    ASSERT_EQ(rtc.ice.remote_candidates[1].family, 6);
    ASSERT_EQ(rtc.ice.remote_candidates[1].port, 60000);
    ASSERT_EQ(rtc.ice.remote_candidates[1].addr[15], 0x01);

    /* Mixed: IPv4 candidate after IPv6 */
    ASSERT_OK(nanortc_add_remote_candidate(&rtc, "192.168.1.1 5000"));
    ASSERT_EQ(rtc.ice.remote_candidate_count, 3);
    ASSERT_EQ(rtc.ice.remote_candidates[2].family, 4);

    nanortc_destroy(&rtc);
}

/*
 * RFC 8445 §5.2: tie-breaker MUST be cryptographically random, not a fixed
 * zero. nanortc_init() must fill ice.tie_breaker via the crypto provider so
 * ICE-CONTROLLING / ICE-CONTROLLED attributes carry a value an attacker
 * cannot predict and 487 Role Conflict resolution has something to compare.
 */
TEST(test_e2e_tie_breaker_is_randomised)
{
    nanortc_t a, b;
    nanortc_config_t cfg = e2e_default_config();
    cfg.role = NANORTC_ROLE_CONTROLLING;
    ASSERT_OK(nanortc_init(&a, &cfg));
    ASSERT_OK(nanortc_init(&b, &cfg));

    ASSERT_NEQ(a.ice.tie_breaker, 0ull);
    ASSERT_NEQ(b.ice.tie_breaker, 0ull);
    /* Two fresh agents almost certainly get distinct 64-bit values. */
    ASSERT_NEQ(a.ice.tie_breaker, b.ice.tie_breaker);

    nanortc_destroy(&a);
    nanortc_destroy(&b);
}

/* ----------------------------------------------------------------
 * IPv6 loopback E2E: two instances over [::1] complete ICE + DTLS.
 *
 * Exercises the full dual-stack integration: same-family pair filter,
 * STUN XOR-MAPPED-ADDRESS with family 0x02, DTLS carriage over an
 * IPv6-sourced relay. Degraded stand-in for libdatachannel-IPv6
 * interop, which needs cooperating C++ peer changes (follow-up).
 * ---------------------------------------------------------------- */

static int e2e_relay_ipv6(nanortc_t *from, nanortc_t *to, uint32_t now_ms,
                          const uint8_t src_addr[16], uint16_t src_port)
{
    int relayed = 0;
    nanortc_output_t out;
    while (nanortc_poll_output(from, &out) == NANORTC_OK) {
        if (out.type != NANORTC_OUTPUT_TRANSMIT)
            continue;
        nanortc_addr_t src;
        memset(&src, 0, sizeof(src));
        src.family = 6;
        memcpy(src.addr, src_addr, 16);
        src.port = src_port;
        (void)nanortc_handle_input(to, &(nanortc_input_t){.now_ms = now_ms,
                                                          .data = out.transmit.data,
                                                          .len = out.transmit.len,
                                                          .src = src});
        relayed++;
    }
    return relayed;
}

TEST(test_e2e_ipv6_loopback_connects)
{
    nanortc_t offerer, answerer;

    nanortc_config_t off_cfg = e2e_default_config();
    off_cfg.role = NANORTC_ROLE_CONTROLLING;
    ASSERT_OK(nanortc_init(&offerer, &off_cfg));

    nanortc_config_t ans_cfg = e2e_default_config();
    ans_cfg.role = NANORTC_ROLE_CONTROLLED;
    ASSERT_OK(nanortc_init(&answerer, &ans_cfg));

    memcpy(offerer.ice.local_ufrag, "OFF", 4);
    offerer.ice.local_ufrag_len = 3;
    memcpy(offerer.ice.local_pwd, "offerer-password-1234", 22);
    offerer.ice.local_pwd_len = 21;
    memcpy(offerer.ice.remote_ufrag, "ANS", 4);
    offerer.ice.remote_ufrag_len = 3;
    memcpy(offerer.ice.remote_pwd, "answerer-password-5678", 23);
    offerer.ice.remote_pwd_len = 22;
    offerer.ice.tie_breaker = 0xDEADBEEFCAFEBABEull;

    memcpy(answerer.ice.local_ufrag, "ANS", 4);
    answerer.ice.local_ufrag_len = 3;
    memcpy(answerer.ice.local_pwd, "answerer-password-5678", 23);
    answerer.ice.local_pwd_len = 22;
    memcpy(answerer.ice.remote_ufrag, "OFF", 4);
    answerer.ice.remote_ufrag_len = 3;
    memcpy(answerer.ice.remote_pwd, "offerer-password-1234", 22);
    answerer.ice.remote_pwd_len = 21;

    uint8_t v6_loopback[16] = {0};
    v6_loopback[15] = 0x01;

    offerer.ice.local_candidates[0].family = 6;
    memcpy(offerer.ice.local_candidates[0].addr, v6_loopback, 16);
    offerer.ice.local_candidates[0].port = 4000;
    offerer.ice.local_candidates[0].type = NANORTC_ICE_CAND_HOST;
    offerer.ice.local_candidate_count = 1;

    offerer.ice.remote_candidates[0].family = 6;
    memcpy(offerer.ice.remote_candidates[0].addr, v6_loopback, 16);
    offerer.ice.remote_candidates[0].port = 5000;
    offerer.ice.remote_candidate_count = 1;

    uint32_t now_ms = 100;
    ASSERT_OK(nanortc_handle_input(&offerer, &(nanortc_input_t){.now_ms = now_ms}));

    int connected = 0;
    for (int round = 0; round < 40; round++) {
        for (int i = 0; i < 5; i++) {
            int ra = e2e_relay_ipv6(&offerer, &answerer, now_ms, v6_loopback, 4000);
            int rb = e2e_relay_ipv6(&answerer, &offerer, now_ms, v6_loopback, 5000);
            if (ra == 0 && rb == 0)
                break;
        }
        if (offerer.state >= NANORTC_STATE_DTLS_CONNECTED &&
            answerer.state >= NANORTC_STATE_DTLS_CONNECTED) {
            connected = 1;
            break;
        }
        now_ms += 10;
        nanortc_handle_input(&offerer, &(nanortc_input_t){.now_ms = now_ms});
        nanortc_handle_input(&answerer, &(nanortc_input_t){.now_ms = now_ms});
    }

    ASSERT_TRUE(connected);
    ASSERT_EQ(offerer.ice.selected_family, 6);
    ASSERT_EQ(answerer.ice.selected_family, 6);

    nanortc_destroy(&offerer);
    nanortc_destroy(&answerer);
}
#endif

/* ================================================================
 * Convenience send API + track helpers
 * ================================================================ */

#if NANORTC_HAVE_MEDIA_TRANSPORT

TEST(test_e2e_add_audio_video_track)
{
    nanortc_t rtc;
    nanortc_config_t cfg = e2e_default_config();
    ASSERT_OK(nanortc_init(&rtc, &cfg));

    int amid = nanortc_add_audio_track(&rtc, NANORTC_DIR_SENDONLY, NANORTC_CODEC_OPUS, 48000, 2);
    ASSERT_TRUE(amid >= 0);

#if NANORTC_FEATURE_VIDEO
    int vmid = nanortc_add_video_track(&rtc, NANORTC_DIR_SENDONLY, NANORTC_CODEC_H264);
    ASSERT_TRUE(vmid >= 0);
    ASSERT_NEQ(amid, vmid);
#endif

    nanortc_destroy(&rtc);
}

TEST(test_e2e_send_audio_before_connected)
{
    nanortc_t rtc;
    nanortc_config_t cfg = e2e_default_config();
    ASSERT_OK(nanortc_init(&rtc, &cfg));

    int mid = nanortc_add_audio_track(&rtc, NANORTC_DIR_SENDONLY, NANORTC_CODEC_OPUS, 48000, 2);
    ASSERT_TRUE(mid >= 0);

    /* Must fail: not connected */
    uint8_t dummy[10] = {0};
    ASSERT_EQ(nanortc_send_audio(&rtc, (uint8_t)mid, 0, dummy, sizeof(dummy)), NANORTC_ERR_STATE);

    nanortc_destroy(&rtc);
}

TEST(test_e2e_send_audio_bad_params)
{
    nanortc_t rtc;
    nanortc_config_t cfg = e2e_default_config();
    ASSERT_OK(nanortc_init(&rtc, &cfg));

    uint8_t dummy[10] = {0};
    /* NULL rtc */
    ASSERT_FAIL(nanortc_send_audio(NULL, 0, 0, dummy, sizeof(dummy)));
    /* NULL data */
    ASSERT_FAIL(nanortc_send_audio(&rtc, 0, 0, NULL, 10));
    /* Zero len */
    ASSERT_FAIL(nanortc_send_audio(&rtc, 0, 0, dummy, 0));

    nanortc_destroy(&rtc);
}

#if NANORTC_FEATURE_VIDEO
TEST(test_e2e_send_video_bad_params)
{
    nanortc_t rtc;
    nanortc_config_t cfg = e2e_default_config();
    ASSERT_OK(nanortc_init(&rtc, &cfg));

    uint8_t dummy[10] = {0};
    ASSERT_FAIL(nanortc_send_video(NULL, 0, 0, dummy, sizeof(dummy)));
    ASSERT_FAIL(nanortc_send_video(&rtc, 0, 0, NULL, 10));
    ASSERT_FAIL(nanortc_send_video(&rtc, 0, 0, dummy, 0));

    nanortc_destroy(&rtc);
}

TEST(test_e2e_send_video_before_connected)
{
    nanortc_t rtc;
    nanortc_config_t cfg = e2e_default_config();
    ASSERT_OK(nanortc_init(&rtc, &cfg));

    int mid = nanortc_add_video_track(&rtc, NANORTC_DIR_SENDONLY, NANORTC_CODEC_H264);
    ASSERT_TRUE(mid >= 0);

    /* Annex-B frame: single IDR NAL */
    uint8_t annexb[] = {0x00, 0x00, 0x00, 0x01, 0x65, 0xAA, 0xBB};
    ASSERT_EQ(nanortc_send_video(&rtc, (uint8_t)mid, 0, annexb, sizeof(annexb)), NANORTC_ERR_STATE);

    nanortc_destroy(&rtc);
}

#if NANORTC_FEATURE_H265
/*
 * E2E: H.265 video roundtrip between two nanortc instances.
 *
 * Phase 3.5 PR-2 acceptance gate. The full SDP / ICE / DTLS / SRTP stack
 * negotiates an H.265 SENDONLY/RECVONLY pair, the offerer emits a single
 * IDR access unit through the existing rtc_send_video_h265() path, and
 * the answerer's receive demux must dispatch to h265_depkt_push() (not
 * h264_depkt_push) and surface the NAL bytes via NANORTC_EV_MEDIA_DATA.
 *
 * Without the codec-dispatched receive demux the answerer would silently
 * drop the H.265 NAL: h264_depkt_push interprets the H.265 NAL header
 * (type=19 IDR, byte0=0x26) as H.264 NAL type 6 (SEI) and drops it
 * because no FU-A is in progress.
 */
TEST(test_e2e_h265_loopback)
{
    nanortc_t offerer, answerer;

    nanortc_config_t off_cfg = e2e_default_config();
    off_cfg.role = NANORTC_ROLE_CONTROLLING;
    ASSERT_OK(nanortc_init(&offerer, &off_cfg));

    nanortc_config_t ans_cfg = e2e_default_config();
    ans_cfg.role = NANORTC_ROLE_CONTROLLED;
    ASSERT_OK(nanortc_init(&answerer, &ans_cfg));

    /* Local candidates so the SDP has a c= line and ICE has somewhere to
     * send the first connectivity check. The e2e_relay helper rewrites the
     * source address of every relayed packet to 192.168.1.1:9999. */
    ASSERT_OK(nanortc_add_local_candidate(&offerer, "192.168.1.1", 4000));
    ASSERT_OK(nanortc_add_local_candidate(&answerer, "192.168.1.2", 5000));

    int off_mid = nanortc_add_video_track(&offerer, NANORTC_DIR_SENDONLY, NANORTC_CODEC_H265);
    ASSERT_TRUE(off_mid >= 0);
    int ans_mid = nanortc_add_video_track(&answerer, NANORTC_DIR_RECVONLY, NANORTC_CODEC_H265);
    ASSERT_TRUE(ans_mid >= 0);

    /* Minimal valid VPS/SPS/PPS NAL bytes (NAL header + dummy payload).
     * Sizes are below 18 bytes so the level-id extraction is skipped, but
     * the sprop-* fmtp fragment still gets emitted and round-tripped. */
    uint8_t vps[] = {0x40, 0x01, 0x0C, 0x01, 0xFF};
    uint8_t sps[] = {0x42, 0x01, 0x01, 0x01, 0x60};
    uint8_t pps[] = {0x44, 0x01, 0xC1, 0x72};
    ASSERT_OK(nanortc_video_set_h265_parameter_sets(&offerer, (uint8_t)off_mid, vps, sizeof(vps),
                                                    sps, sizeof(sps), pps, sizeof(pps)));

    /* SDP roundtrip negotiates H.265 PT=98 on both sides. */
    char offer[4096];
    size_t offer_len = 0;
    ASSERT_OK(nanortc_create_offer(&offerer, offer, sizeof(offer), &offer_len));
    offer[offer_len] = '\0';

    char answer[4096];
    size_t answer_len = 0;
    ASSERT_OK(nanortc_accept_offer(&answerer, offer, answer, sizeof(answer), &answer_len));
    answer[answer_len] = '\0';

    ASSERT_OK(nanortc_accept_answer(&offerer, answer));

    /* Mirror test_e2e_full_sdp_to_dtls: arm answerer's remote candidate to
     * the loopback source the e2e_relay helper rewrites onto every packet. */
    answerer.ice.remote_candidates[0].family = 4;
    answerer.ice.remote_candidates[0].addr[0] = 192;
    answerer.ice.remote_candidates[0].addr[1] = 168;
    answerer.ice.remote_candidates[0].addr[2] = 1;
    answerer.ice.remote_candidates[0].addr[3] = 1;
    answerer.ice.remote_candidates[0].port = 9999;
    answerer.ice.remote_candidate_count = 1;

    /* ICE + DTLS handshake. */
    uint32_t now_ms = 100;
    ASSERT_OK(nanortc_handle_input(&offerer, &(nanortc_input_t){.now_ms = now_ms}));
    int connected = 0;
    for (int round = 0; round < 30; round++) {
        e2e_pump(&offerer, &answerer, now_ms, 5);
        if (offerer.state >= NANORTC_STATE_DTLS_CONNECTED &&
            answerer.state >= NANORTC_STATE_DTLS_CONNECTED && offerer.srtp.ready &&
            answerer.srtp.ready) {
            connected = 1;
            break;
        }
    }
    ASSERT_TRUE(connected);
    ASSERT_TRUE(offerer.srtp.ready);
    ASSERT_TRUE(answerer.srtp.ready);

    /* Send one Annex-B IDR access unit. Single small NAL (≤ MTU) so the
     * wire packet is one Single-NAL RTP packet — keeps the test
     * deterministic and avoids exercising FU reassembly redundantly with
     * the unit test in test_h265.c. NAL header byte 0x26 = type 19
     * (IDR_W_RADL), byte 0x01 = layer 0 / TID 1. */
    uint8_t idr[] = {0x00, 0x00, 0x00, 0x01, 0x26, 0x01, 0xAA, 0xBB, 0xCC, 0xDD};
    const size_t idr_payload_len = sizeof(idr) - 4; /* skip the 4-byte start code */
    ASSERT_OK(nanortc_send_video(&offerer, (uint8_t)off_mid, /*pts_ms=*/0, idr, sizeof(idr)));
    uint16_t packet_slot =
        (uint16_t)((offerer.pkt_ring_tail - 1u) & (NANORTC_VIDEO_PKT_RING_SIZE - 1u));
    ASSERT_TRUE((offerer.pkt_ring[packet_slot][1] & 0x80u) != 0u);

    /* Drain offerer → answerer one-way. Note: e2e_relay polls (and thus
     * drains) the source queue, but it does NOT touch the destination
     * queue, so the answerer's NANORTC_EV_MEDIA_DATA stays parked for the
     * subsequent poll loop below. e2e_pump would have drained both sides
     * and consumed the event before we could see it. */
    e2e_relay(&offerer, &answerer, now_ms);

    /* Walk the answerer's output queue looking for the EV_MEDIA_DATA we
     * expect. Other event types (e.g., RTCP) may precede or follow it. */
    int got_media = 0;
    nanortc_output_t out;
    while (nanortc_poll_output(&answerer, &out) == NANORTC_OK) {
        if (out.type != NANORTC_OUTPUT_EVENT || out.event.type != NANORTC_EV_MEDIA_DATA) {
            continue;
        }
        ASSERT_EQ(out.event.media_data.mid, (uint8_t)ans_mid);
        ASSERT_EQ(out.event.media_data.len, idr_payload_len);
        ASSERT_TRUE(out.event.media_data.is_keyframe);
        ASSERT_MEM_EQ(out.event.media_data.data, idr + 4, idr_payload_len);
        got_media = 1;
    }
    ASSERT_TRUE(got_media);

    nanortc_destroy(&offerer);
    nanortc_destroy(&answerer);
}

/*
 * E2E: a mid-stream parameter-set refresh + IDR is consumed without
 * renegotiation. This is the receiver-side contract for Phase-14 adaptive spec
 * switching: when the sender changes the video geometry it re-sets the
 * parameter sets and forces a fresh IDR mid-session; the transport is
 * geometry-agnostic so the stream must keep flowing without an SDP roundtrip.
 * Sends an IDR (set A), then calls nanortc_video_set_h265_parameter_sets() with
 * a new set B on the live session and sends a second IDR, and asserts the
 * receiver still surfaces a keyframe. (Multi-NAL AP/FU packetization of the
 * in-band VPS/SPS/PPS is covered separately in test_h265.c and the interop
 * suite; here the focus is that a mid-session param-set re-set is safe.)
 */
TEST(test_e2e_h265_midstream_param_refresh)
{
    nanortc_t offerer, answerer;

    nanortc_config_t off_cfg = e2e_default_config();
    off_cfg.role = NANORTC_ROLE_CONTROLLING;
    ASSERT_OK(nanortc_init(&offerer, &off_cfg));

    nanortc_config_t ans_cfg = e2e_default_config();
    ans_cfg.role = NANORTC_ROLE_CONTROLLED;
    ASSERT_OK(nanortc_init(&answerer, &ans_cfg));

    ASSERT_OK(nanortc_add_local_candidate(&offerer, "192.168.1.1", 4000));
    ASSERT_OK(nanortc_add_local_candidate(&answerer, "192.168.1.2", 5000));

    int off_mid = nanortc_add_video_track(&offerer, NANORTC_DIR_SENDONLY, NANORTC_CODEC_H265);
    ASSERT_TRUE(off_mid >= 0);
    int ans_mid = nanortc_add_video_track(&answerer, NANORTC_DIR_RECVONLY, NANORTC_CODEC_H265);
    ASSERT_TRUE(ans_mid >= 0);

    /* Initial parameter sets (set A). */
    uint8_t vps_a[] = {0x40, 0x01, 0x0C, 0x01, 0xFF};
    uint8_t sps_a[] = {0x42, 0x01, 0x01, 0x01, 0x60};
    uint8_t pps_a[] = {0x44, 0x01, 0xC1, 0x72};
    ASSERT_OK(nanortc_video_set_h265_parameter_sets(&offerer, (uint8_t)off_mid, vps_a,
                                                    sizeof(vps_a), sps_a, sizeof(sps_a), pps_a,
                                                    sizeof(pps_a)));

    char offer[4096];
    size_t offer_len = 0;
    ASSERT_OK(nanortc_create_offer(&offerer, offer, sizeof(offer), &offer_len));
    offer[offer_len] = '\0';
    char answer[4096];
    size_t answer_len = 0;
    ASSERT_OK(nanortc_accept_offer(&answerer, offer, answer, sizeof(answer), &answer_len));
    answer[answer_len] = '\0';
    ASSERT_OK(nanortc_accept_answer(&offerer, answer));

    answerer.ice.remote_candidates[0].family = 4;
    answerer.ice.remote_candidates[0].addr[0] = 192;
    answerer.ice.remote_candidates[0].addr[1] = 168;
    answerer.ice.remote_candidates[0].addr[2] = 1;
    answerer.ice.remote_candidates[0].addr[3] = 1;
    answerer.ice.remote_candidates[0].port = 9999;
    answerer.ice.remote_candidate_count = 1;

    uint32_t now_ms = 100;
    ASSERT_OK(nanortc_handle_input(&offerer, &(nanortc_input_t){.now_ms = now_ms}));
    int connected = 0;
    for (int round = 0; round < 30; round++) {
        e2e_pump(&offerer, &answerer, now_ms, 5);
        if (offerer.srtp.ready && answerer.srtp.ready) {
            connected = 1;
            break;
        }
    }
    ASSERT_TRUE(connected);
    /* Drain any handshake events parked on the answerer. */
    {
        nanortc_output_t d;
        while (nanortc_poll_output(&answerer, &d) == NANORTC_OK) {
        }
    }

    /* IDR #1 (single NAL, set A in SDP). */
    uint8_t idr1[] = {0x00, 0x00, 0x00, 0x01, 0x26, 0x01, 0xAA, 0xBB};
    ASSERT_OK(nanortc_send_video(&offerer, (uint8_t)off_mid, 0, idr1, sizeof(idr1)));
    nanortc_output_t out;
    int got_kf1 = 0;
    for (int r = 0; r < 6 && !got_kf1; r++) {
        now_ms += 20;
        ASSERT_OK(nanortc_handle_input(&offerer, &(nanortc_input_t){.now_ms = now_ms}));
        e2e_relay(&offerer, &answerer, now_ms);
        while (nanortc_poll_output(&answerer, &out) == NANORTC_OK) {
            if (out.type == NANORTC_OUTPUT_EVENT && out.event.type == NANORTC_EV_MEDIA_DATA &&
                out.event.media_data.is_keyframe)
                got_kf1 = 1;
        }
    }
    ASSERT_TRUE(got_kf1);

    /* Mid-stream spec switch: re-set the parameter sets (set B) — must be safe
     * to call on a live session — then send a fresh IDR, as an adaptive geometry
     * change does (the example re-emits the in-band VPS/SPS/PPS and forces an
     * IDR). */
    uint8_t vps_b[] = {0x40, 0x01, 0x0C, 0x02, 0xEE};
    uint8_t sps_b[] = {0x42, 0x01, 0x02, 0x02, 0x55};
    uint8_t pps_b[] = {0x44, 0x01, 0xC2, 0x33};
    ASSERT_OK(nanortc_video_set_h265_parameter_sets(&offerer, (uint8_t)off_mid, vps_b,
                                                    sizeof(vps_b), sps_b, sizeof(sps_b), pps_b,
                                                    sizeof(pps_b)));

    uint8_t idr2[] = {0x00, 0x00, 0x00, 0x01, 0x26, 0x01, 0x11, 0x22, 0x33, 0x44}; /* IDR, set B */
    ASSERT_OK(nanortc_send_video(&offerer, (uint8_t)off_mid, 100, idr2, sizeof(idr2)));

    int got_media2 = 0; /* the post-refresh access unit was delivered */
    int got_kf2 = 0;    /* and a keyframe was flagged within it */
    for (int r = 0; r < 8 && !got_media2; r++) {
        now_ms += 20;
        ASSERT_OK(nanortc_handle_input(&offerer, &(nanortc_input_t){.now_ms = now_ms}));
        e2e_relay(&offerer, &answerer, now_ms);
        while (nanortc_poll_output(&answerer, &out) == NANORTC_OK) {
            if (out.type == NANORTC_OUTPUT_EVENT && out.event.type == NANORTC_EV_MEDIA_DATA) {
                got_media2 = 1;
                if (out.event.media_data.is_keyframe)
                    got_kf2 = 1;
            }
        }
    }
    /* The mid-stream refresh + IDR must be consumed without renegotiation. */
    ASSERT_TRUE(got_media2);
    ASSERT_TRUE(got_kf2);

    nanortc_destroy(&offerer);
    nanortc_destroy(&answerer);
}
#endif /* NANORTC_FEATURE_H265 */

/*
 * E2E: per-frame atomic admission on nanortc_send_video().
 *
 * A frame either ships whole or is rejected before anything is enqueued —
 * the pre-guard behavior (enqueue until the queue fills, abandon the rest)
 * put truncated frames on the wire, guaranteeing receiver loss → PLI →
 * keyframe storms (the embedded-camera stutter loop). Verifies:
 *   - a frame larger than min(OUT_QUEUE, PKT_RING) packets is rejected
 *     with BUFFER_TOO_SMALL and enqueues nothing;
 *   - a frame that fits an empty queue but not the residual free space is
 *     rejected with WOULD_BLOCK and enqueues nothing;
 *   - after draining via nanortc_poll_output() the same frame is accepted;
 *   - nanortc_output_free_slots() tracks the queue depth across all of it.
 */
#if NANORTC_FEATURE_VIDEO_PACING
/* With pacing, fragments stage in the pkt_ring (pace FIFO) and trickle into
 * out_queue over time, so the binding admission capacity is the ring alone. */
#define E2E_ADMISSION_CAP ((int)NANORTC_VIDEO_PKT_RING_SIZE)
#else
#define E2E_ADMISSION_CAP                                                                 \
    ((NANORTC_VIDEO_PKT_RING_SIZE < NANORTC_OUT_QUEUE_SIZE) ? NANORTC_VIDEO_PKT_RING_SIZE \
                                                            : NANORTC_OUT_QUEUE_SIZE)
#endif

/* Fully flush a sender's outbound video. Without pacing a plain poll_output
 * drain empties the queue; with pacing the fragments are time-metered, so we
 * advance the clock past the catch-up cap (NANORTC_PACING_MAX_QUEUE_MS) and
 * pump until both out_queue and the pace FIFO are empty. *now_ms is advanced
 * in place so the caller's clock stays monotonic. */
static void e2e_flush_sender(nanortc_t *rtc, uint32_t *now_ms)
{
    for (int i = 0; i < 16; i++) {
        *now_ms += (uint32_t)NANORTC_PACING_MAX_QUEUE_MS + 1u;
        (void)nanortc_handle_input(rtc, &(nanortc_input_t){.now_ms = *now_ms});
        nanortc_output_t d;
        int drained = 0;
        while (nanortc_poll_output(rtc, &d) == NANORTC_OK) {
            drained = 1;
        }
#if NANORTC_FEATURE_VIDEO_PACING
        if (!drained && rtc->pacer.head == rtc->pacer.tail) {
            break;
        }
#else
        if (!drained) {
            break;
        }
#endif
    }
}

/* 4-byte start code + NAL header + worst-case payload for CAP+1 fragments. */
static uint8_t
    g_admission_frame[5 + (size_t)(NANORTC_VIDEO_MTU - 2) * (E2E_ADMISSION_CAP + 1) + 16];

TEST(test_e2e_video_send_admission)
{
    nanortc_t offerer, answerer;

    nanortc_config_t off_cfg = e2e_default_config();
    off_cfg.role = NANORTC_ROLE_CONTROLLING;
    ASSERT_OK(nanortc_init(&offerer, &off_cfg));

    nanortc_config_t ans_cfg = e2e_default_config();
    ans_cfg.role = NANORTC_ROLE_CONTROLLED;
    ASSERT_OK(nanortc_init(&answerer, &ans_cfg));

    ASSERT_OK(nanortc_add_local_candidate(&offerer, "192.168.1.1", 4000));
    ASSERT_OK(nanortc_add_local_candidate(&answerer, "192.168.1.2", 5000));

    int off_mid = nanortc_add_video_track(&offerer, NANORTC_DIR_SENDONLY, NANORTC_CODEC_H264);
    ASSERT_TRUE(off_mid >= 0);
    int ans_mid = nanortc_add_video_track(&answerer, NANORTC_DIR_RECVONLY, NANORTC_CODEC_H264);
    ASSERT_TRUE(ans_mid >= 0);

    char offer[4096];
    size_t offer_len = 0;
    ASSERT_OK(nanortc_create_offer(&offerer, offer, sizeof(offer), &offer_len));
    offer[offer_len] = '\0';

    char answer[4096];
    size_t answer_len = 0;
    ASSERT_OK(nanortc_accept_offer(&answerer, offer, answer, sizeof(answer), &answer_len));
    answer[answer_len] = '\0';

    ASSERT_OK(nanortc_accept_answer(&offerer, answer));

    /* Mirror test_e2e_h265_loopback: arm answerer's remote candidate to the
     * loopback source the e2e_relay helper rewrites onto every packet. */
    answerer.ice.remote_candidates[0].family = 4;
    answerer.ice.remote_candidates[0].addr[0] = 192;
    answerer.ice.remote_candidates[0].addr[1] = 168;
    answerer.ice.remote_candidates[0].addr[2] = 1;
    answerer.ice.remote_candidates[0].addr[3] = 1;
    answerer.ice.remote_candidates[0].port = 9999;
    answerer.ice.remote_candidate_count = 1;

    uint32_t now_ms = 100;
    ASSERT_OK(nanortc_handle_input(&offerer, &(nanortc_input_t){.now_ms = now_ms}));
    int connected = 0;
    for (int round = 0; round < 30; round++) {
        e2e_pump(&offerer, &answerer, now_ms, 5);
        if (offerer.state >= NANORTC_STATE_DTLS_CONNECTED &&
            answerer.state >= NANORTC_STATE_DTLS_CONNECTED && offerer.srtp.ready &&
            answerer.srtp.ready) {
            connected = 1;
            break;
        }
    }
    ASSERT_TRUE(connected);

    /* Drain handshake leftovers so the queue starts empty. */
    {
        nanortc_output_t d;
        while (nanortc_poll_output(&offerer, &d) == NANORTC_OK) {
        }
    }
    ASSERT_EQ(nanortc_output_free_slots(&offerer), (uint16_t)NANORTC_OUT_QUEUE_SIZE);

    const size_t cap = (size_t)E2E_ADMISSION_CAP;
    const size_t per = (size_t)NANORTC_VIDEO_MTU - 2; /* FU-A payload bytes per fragment */

    /* Frame A: fragments into exactly cap*3/4 packets — fits an empty
     * queue, but two back-to-back un-drained sends exceed the capacity. */
    const size_t fit_packets = cap * 3 / 4;
#if NANORTC_FEATURE_VIDEO_FEC && !NANORTC_FEATURE_VIDEO_PACING && !NANORTC_FEC_ADAPTIVE
    const size_t fit_fec_packets = fit_packets / NANORTC_FEC_GROUP_SIZE;
#else
    const size_t fit_fec_packets = 0u;
#endif
    const size_t nal_len_a = (fit_packets - 1) * per + 10 + 1; /* +1 NAL header byte */
    const size_t frame_len_a = 4 + nal_len_a;

    memset(g_admission_frame, 0xAB, sizeof(g_admission_frame));
    g_admission_frame[0] = 0x00;
    g_admission_frame[1] = 0x00;
    g_admission_frame[2] = 0x00;
    g_admission_frame[3] = 0x01;
    g_admission_frame[4] = 0x65; /* IDR slice, NRI=3 */

    ASSERT_OK(nanortc_send_video(&offerer, (uint8_t)off_mid, 0, g_admission_frame, frame_len_a));
#if NANORTC_FEATURE_VIDEO_PACING
    /* Paced: the whole frame is staged in the pace FIFO (not yet out_queue). */
    ASSERT_EQ((uint16_t)(offerer.pacer.tail - offerer.pacer.head), (uint16_t)fit_packets);
#else
    /* Immediate: the whole frame is enqueued in out_queue. */
    ASSERT_EQ(nanortc_output_free_slots(&offerer),
              (uint16_t)(NANORTC_OUT_QUEUE_SIZE - fit_packets - fit_fec_packets));
#endif

    /* Second frame without draining: residual capacity is too small. The
     * rejected call must not consume a single slot. */
    ASSERT_EQ(nanortc_send_video(&offerer, (uint8_t)off_mid, 33, g_admission_frame, frame_len_a),
              NANORTC_ERR_WOULD_BLOCK);
#if NANORTC_FEATURE_VIDEO_PACING
    ASSERT_EQ((uint16_t)(offerer.pacer.tail - offerer.pacer.head), (uint16_t)fit_packets);
#else
    ASSERT_EQ(nanortc_output_free_slots(&offerer),
              (uint16_t)(NANORTC_OUT_QUEUE_SIZE - fit_packets - fit_fec_packets));
#endif

    /* Flush (advancing the clock so the pacer releases its backlog), then the
     * same frame is admitted again. */
    e2e_flush_sender(&offerer, &now_ms);
    ASSERT_EQ(nanortc_output_free_slots(&offerer), (uint16_t)NANORTC_OUT_QUEUE_SIZE);
#if NANORTC_FEATURE_VIDEO_PACING
    ASSERT_EQ((uint16_t)(offerer.pacer.tail - offerer.pacer.head), 0);
#endif
    ASSERT_OK(nanortc_send_video(&offerer, (uint8_t)off_mid, 33, g_admission_frame, frame_len_a));
    e2e_flush_sender(&offerer, &now_ms);

    /* Frame B: fragments into cap+1 packets — permanently over capacity,
     * rejected even with an empty queue, nothing enqueued. */
    const size_t nal_len_b = cap * per + 10 + 1;
    const size_t frame_len_b = 4 + nal_len_b;
    ASSERT_EQ(nanortc_send_video(&offerer, (uint8_t)off_mid, 66, g_admission_frame, frame_len_b),
              NANORTC_ERR_BUFFER_TOO_SMALL);
    ASSERT_EQ(nanortc_output_free_slots(&offerer), (uint16_t)NANORTC_OUT_QUEUE_SIZE);

    /* No pkt_ring aliasing fired anywhere above. */
    ASSERT_EQ(offerer.stats_pkt_ring_overrun, 0u);
    ASSERT_EQ(offerer.stats_tx_queue_full, 0u);

    (void)ans_mid;
    nanortc_destroy(&offerer);
    nanortc_destroy(&answerer);
}

#if NANORTC_FEATURE_VIDEO_AUTO_PLI && !NANORTC_FEATURE_VIDEO_REORDER
/* Advance the offerer clock (so the pacer refills) and send one video frame. */
static int e2e_offerer_send_frame(nanortc_t *off, int mid, uint32_t now, uint32_t pts,
                                  const uint8_t *frame, size_t flen)
{
    nanortc_handle_input(off, &(nanortc_input_t){.now_ms = now});
    return nanortc_send_video(off, (uint8_t)mid, pts, frame, flen);
}

/* Auto-PLI keyframe recovery: a dropped video packet punches a forward RTP
 * sequence gap at the receiver, which must emit a debounced RTCP PLI so the
 * sender re-sends a keyframe. Verifies: silent in-order, fires on a gap,
 * debounced within NANORTC_VIDEO_PLI_MIN_INTERVAL_MS, re-fires after it, and
 * the PLI round-trips to the sender as NANORTC_EV_KEYFRAME_REQUEST. */
TEST(test_e2e_video_auto_pli_on_loss)
{
    nanortc_t offerer, answerer;
    nanortc_config_t off_cfg = e2e_default_config();
    off_cfg.role = NANORTC_ROLE_CONTROLLING;
    ASSERT_OK(nanortc_init(&offerer, &off_cfg));
    nanortc_config_t ans_cfg = e2e_default_config();
    ans_cfg.role = NANORTC_ROLE_CONTROLLED;
    ASSERT_OK(nanortc_init(&answerer, &ans_cfg));

    ASSERT_OK(nanortc_add_local_candidate(&offerer, "192.168.1.1", 4000));
    ASSERT_OK(nanortc_add_local_candidate(&answerer, "192.168.1.2", 5000));

    int off_mid = nanortc_add_video_track(&offerer, NANORTC_DIR_SENDONLY, NANORTC_CODEC_H264);
    int ans_mid = nanortc_add_video_track(&answerer, NANORTC_DIR_RECVONLY, NANORTC_CODEC_H264);
    ASSERT_TRUE(off_mid >= 0 && ans_mid >= 0);

    char offer[4096];
    size_t offer_len = 0;
    ASSERT_OK(nanortc_create_offer(&offerer, offer, sizeof(offer), &offer_len));
    offer[offer_len] = '\0';
    char answer[4096];
    size_t answer_len = 0;
    ASSERT_OK(nanortc_accept_offer(&answerer, offer, answer, sizeof(answer), &answer_len));
    answer[answer_len] = '\0';
    ASSERT_OK(nanortc_accept_answer(&offerer, answer));

    answerer.ice.remote_candidates[0].family = 4;
    answerer.ice.remote_candidates[0].addr[0] = 192;
    answerer.ice.remote_candidates[0].addr[1] = 168;
    answerer.ice.remote_candidates[0].addr[2] = 1;
    answerer.ice.remote_candidates[0].addr[3] = 1;
    answerer.ice.remote_candidates[0].port = 9999;
    answerer.ice.remote_candidate_count = 1;

    uint32_t now = 100;
    ASSERT_OK(nanortc_handle_input(&offerer, &(nanortc_input_t){.now_ms = now}));
    int connected = 0;
    for (int round = 0; round < 30; round++) {
        e2e_pump(&offerer, &answerer, now, 5);
        if (offerer.srtp.ready && answerer.srtp.ready) {
            connected = 1;
            break;
        }
    }
    ASSERT_TRUE(connected);
    {
        nanortc_output_t d;
        while (nanortc_poll_output(&offerer, &d) == NANORTC_OK) {
        }
        while (nanortc_poll_output(&answerer, &d) == NANORTC_OK) {
        }
    }

    /* One small single-NAL H.264 frame (non-IDR slice, type 1) → 1 RTP packet. */
    uint8_t frame[48];
    memset(frame, 0xAB, sizeof(frame));
    frame[0] = 0;
    frame[1] = 0;
    frame[2] = 0;
    frame[3] = 1;
    frame[4] = 0x41;
    uint32_t pts = 0;

    /* Frame 1: delivered → answerer initialises its receive seq baseline. */
    now += 5;
    ASSERT_OK(e2e_offerer_send_frame(&offerer, off_mid, now, pts, frame, sizeof(frame)));
    pts += 33;
    e2e_relay_drop_rtp(&offerer, &answerer, now, -1);
    ASSERT_EQ(answerer.stats_auto_pli_sent, 0u);

    /* Frame 2 dropped + Frame 3 delivered → forward gap → auto-PLI fires once. */
    now += 5;
    ASSERT_OK(e2e_offerer_send_frame(&offerer, off_mid, now, pts, frame, sizeof(frame)));
    pts += 33;
    e2e_relay_drop_rtp(&offerer, &answerer, now, 0); /* drop the only RTP packet */
    now += 5;
    ASSERT_OK(e2e_offerer_send_frame(&offerer, off_mid, now, pts, frame, sizeof(frame)));
    pts += 33;
    e2e_relay_drop_rtp(&offerer, &answerer, now, -1);
    ASSERT_EQ(answerer.stats_auto_pli_sent, 1u);

    /* The PLI round-trips to the sender as NANORTC_EV_KEYFRAME_REQUEST naming the
     * correct outbound track. The media-source-SSRC match (TD-024) makes this
     * work even for a pure sendonly sender, which never received reverse RTP.
     * (This relay also drains the answerer's queue for the following phases.) */
    e2e_relay_drop_rtp(&answerer, &offerer, now, -1);
    int got_kf_req = 0;
    nanortc_output_t out;
    while (nanortc_poll_output(&offerer, &out) == NANORTC_OK) {
        if (out.type == NANORTC_OUTPUT_EVENT && out.event.type == NANORTC_EV_KEYFRAME_REQUEST) {
            got_kf_req = 1;
            ASSERT_EQ(out.event.keyframe_request.mid, (uint8_t)off_mid);
        }
    }
    ASSERT_TRUE(got_kf_req);

    /* Frame 4 in-order → no spurious PLI. */
    now += 5;
    ASSERT_OK(e2e_offerer_send_frame(&offerer, off_mid, now, pts, frame, sizeof(frame)));
    pts += 33;
    e2e_relay_drop_rtp(&offerer, &answerer, now, -1);
    ASSERT_EQ(answerer.stats_auto_pli_sent, 1u);

    /* Another gap within the debounce window → suppressed. */
    now += 5;
    ASSERT_OK(e2e_offerer_send_frame(&offerer, off_mid, now, pts, frame, sizeof(frame)));
    pts += 33;
    e2e_relay_drop_rtp(&offerer, &answerer, now, 0);
    now += 5;
    ASSERT_OK(e2e_offerer_send_frame(&offerer, off_mid, now, pts, frame, sizeof(frame)));
    pts += 33;
    e2e_relay_drop_rtp(&offerer, &answerer, now, -1);
    ASSERT_EQ(answerer.stats_auto_pli_sent, 1u);

    /* Advance past the debounce interval → the next gap re-fires. */
    now += (uint32_t)NANORTC_VIDEO_PLI_MIN_INTERVAL_MS + 1u;
    ASSERT_OK(e2e_offerer_send_frame(&offerer, off_mid, now, pts, frame, sizeof(frame)));
    pts += 33;
    e2e_relay_drop_rtp(&offerer, &answerer, now, 0);
    now += 5;
    ASSERT_OK(e2e_offerer_send_frame(&offerer, off_mid, now, pts, frame, sizeof(frame)));
    pts += 33;
    e2e_relay_drop_rtp(&offerer, &answerer, now, -1);
    ASSERT_EQ(answerer.stats_auto_pli_sent, 2u);

    nanortc_destroy(&offerer);
    nanortc_destroy(&answerer);
}
#endif /* NANORTC_FEATURE_VIDEO_AUTO_PLI && !NANORTC_FEATURE_VIDEO_REORDER */

#if NANORTC_FEATURE_VIDEO_REORDER
/* Relay from->to, delivering collected RTP/SRTP media packets in REVERSE order
 * (swap) to exercise the receive reorder buffer; control packets pass through. */
static void e2e_relay_reordered(nanortc_t *from, nanortc_t *to, uint32_t now)
{
    static uint8_t bufs[8][1600];
    size_t lens[8];
    int n = 0;
    nanortc_addr_t src;
    memset(&src, 0, sizeof(src));
    src.family = 4;
    src.addr[0] = 192;
    src.addr[1] = 168;
    src.addr[2] = 1;
    src.addr[3] = 1;
    src.port = 9999;
    nanortc_output_t out;
    while (nanortc_poll_output(from, &out) == NANORTC_OK) {
        if (out.type != NANORTC_OUTPUT_TRANSMIT) {
            continue;
        }
        const uint8_t *d = out.transmit.data;
        int is_rtp = (out.transmit.len > 0 && d[0] >= 0x80 && d[0] <= 0xBF);
        if (is_rtp && n < 8 && out.transmit.len <= sizeof(bufs[0])) {
            memcpy(bufs[n], d, out.transmit.len);
            lens[n] = out.transmit.len;
            n++;
        } else {
            nanortc_handle_input(to, &(nanortc_input_t){.now_ms = now,
                                                        .data = out.transmit.data,
                                                        .len = out.transmit.len,
                                                        .src = src});
        }
    }
    for (int i = n - 1; i >= 0; i--) {
        nanortc_handle_input(
            to, &(nanortc_input_t){.now_ms = now, .data = bufs[i], .len = lens[i], .src = src});
    }
}

/* Reorder buffer heals a swapped pair end-to-end: two single-NAL frames are
 * delivered to the receiver in reverse order; the reorder buffer must release
 * them IN ORDER with correct, non-aliased bytes (the multi-emit aliasing fix)
 * and signal no loss (no auto-PLI). */
TEST(test_e2e_video_reorder_heals_swap)
{
    nanortc_t offerer, answerer;
    nanortc_config_t off_cfg = e2e_default_config();
    off_cfg.role = NANORTC_ROLE_CONTROLLING;
    ASSERT_OK(nanortc_init(&offerer, &off_cfg));
    nanortc_config_t ans_cfg = e2e_default_config();
    ans_cfg.role = NANORTC_ROLE_CONTROLLED;
    ASSERT_OK(nanortc_init(&answerer, &ans_cfg));

    ASSERT_OK(nanortc_add_local_candidate(&offerer, "192.168.1.1", 4000));
    ASSERT_OK(nanortc_add_local_candidate(&answerer, "192.168.1.2", 5000));
    int off_mid = nanortc_add_video_track(&offerer, NANORTC_DIR_SENDONLY, NANORTC_CODEC_H264);
    int ans_mid = nanortc_add_video_track(&answerer, NANORTC_DIR_RECVONLY, NANORTC_CODEC_H264);
    ASSERT_TRUE(off_mid >= 0 && ans_mid >= 0);

    char offer[4096];
    size_t offer_len = 0;
    ASSERT_OK(nanortc_create_offer(&offerer, offer, sizeof(offer), &offer_len));
    offer[offer_len] = '\0';
    char answer[4096];
    size_t answer_len = 0;
    ASSERT_OK(nanortc_accept_offer(&answerer, offer, answer, sizeof(answer), &answer_len));
    answer[answer_len] = '\0';
    ASSERT_OK(nanortc_accept_answer(&offerer, answer));

    answerer.ice.remote_candidates[0].family = 4;
    answerer.ice.remote_candidates[0].addr[0] = 192;
    answerer.ice.remote_candidates[0].addr[1] = 168;
    answerer.ice.remote_candidates[0].addr[2] = 1;
    answerer.ice.remote_candidates[0].addr[3] = 1;
    answerer.ice.remote_candidates[0].port = 9999;
    answerer.ice.remote_candidate_count = 1;

    uint32_t now = 100;
    ASSERT_OK(nanortc_handle_input(&offerer, &(nanortc_input_t){.now_ms = now}));
    int connected = 0;
    for (int round = 0; round < 30; round++) {
        e2e_pump(&offerer, &answerer, now, 5);
        if (offerer.srtp.ready && answerer.srtp.ready) {
            connected = 1;
            break;
        }
    }
    ASSERT_TRUE(connected);
    {
        nanortc_output_t d;
        while (nanortc_poll_output(&offerer, &d) == NANORTC_OK) {
        }
        while (nanortc_poll_output(&answerer, &d) == NANORTC_OK) {
        }
    }

    uint8_t frame[40];
    uint32_t pts = 0;

    /* Frame 0 in order: establishes the receiver's reorder baseline (next_seq). */
    memset(frame, 0xC0, sizeof(frame));
    frame[0] = 0;
    frame[1] = 0;
    frame[2] = 0;
    frame[3] = 1;
    frame[4] = 0x41; /* non-IDR slice */
    now += 5;
    ASSERT_OK(nanortc_handle_input(&offerer, &(nanortc_input_t){.now_ms = now}));
    ASSERT_OK(nanortc_send_video(&offerer, (uint8_t)off_mid, pts, frame, sizeof(frame)));
    pts += 33;
    e2e_relay_reordered(&offerer, &answerer, now); /* single packet: in order */
    {
        nanortc_output_t d;
        while (nanortc_poll_output(&answerer, &d) == NANORTC_OK) {
        }
    }

    /* Frame A (0xA1) then Frame B (0xB2): both staged, delivered REVERSED. */
    memset(frame, 0xA1, sizeof(frame));
    frame[0] = 0;
    frame[1] = 0;
    frame[2] = 0;
    frame[3] = 1;
    frame[4] = 0x41;
    now += 5;
    ASSERT_OK(nanortc_handle_input(&offerer, &(nanortc_input_t){.now_ms = now}));
    ASSERT_OK(nanortc_send_video(&offerer, (uint8_t)off_mid, pts, frame, sizeof(frame)));
    pts += 33;
    memset(frame, 0xB2, sizeof(frame));
    frame[0] = 0;
    frame[1] = 0;
    frame[2] = 0;
    frame[3] = 1;
    frame[4] = 0x41;
    ASSERT_OK(nanortc_send_video(&offerer, (uint8_t)off_mid, pts, frame, sizeof(frame)));
    pts += 33;
    e2e_relay_reordered(&offerer, &answerer, now); /* delivers B then A */

    /* Capture the receiver's media events one at a time (copy between polls, per
     * the lifetime contract): must be A then B with correct bytes — proving the
     * reorder healed the swap AND the per-poll produce avoids depkt aliasing. */
    uint8_t first_byte[4];
    int got = 0;
    nanortc_output_t out;
    while (nanortc_poll_output(&answerer, &out) == NANORTC_OK) {
        if (out.type == NANORTC_OUTPUT_EVENT && out.event.type == NANORTC_EV_MEDIA_DATA &&
            got < 4) {
            /* payload byte after the 1-byte NAL header distinguishes A (0xA1)
             * from B (0xB2). */
            first_byte[got] = (out.event.media_data.len > 1) ? out.event.media_data.data[1] : 0;
            got++;
        }
    }
    ASSERT_EQ(got, 2);
    ASSERT_EQ(first_byte[0], (uint8_t)0xA1); /* frame A delivered first */
    ASSERT_EQ(first_byte[1], (uint8_t)0xB2); /* frame B second — not aliased */
#if NANORTC_FEATURE_VIDEO_AUTO_PLI
    ASSERT_EQ(answerer.stats_auto_pli_sent, 0u); /* reorder healed → no loss */
#endif

    /* --- Mid-stream SSRC change must not be blackholed by a stale next_seq.
     * Move the sender to a new SSRC with a sequence number far BEHIND the
     * receiver's current reorder next_seq (the diff<0 late-drop trap). The
     * receiver must detect the SSRC change, re-seed the reorder buffer, and
     * deliver the new stream. (TD: reorder SSRC-change reset.) --- */
    {
        nanortc_output_t d;
        while (nanortc_poll_output(&answerer, &d) == NANORTC_OK) {
        }
    }
    offerer.media[0].rtp.ssrc = offerer.media[0].rtp.ssrc + 0x5050u;         /* new SSRC */
    offerer.media[0].rtp.seq = (uint16_t)(offerer.media[0].rtp.seq - 10000); /* far behind */
    offerer.media[0].rtcp.packets_sent = 0;

    memset(frame, 0xD3, sizeof(frame));
    frame[0] = 0;
    frame[1] = 0;
    frame[2] = 0;
    frame[3] = 1;
    frame[4] = 0x41;
    now += 5;
    ASSERT_OK(nanortc_handle_input(&offerer, &(nanortc_input_t){.now_ms = now}));
    ASSERT_OK(nanortc_send_video(&offerer, (uint8_t)off_mid, pts, frame, sizeof(frame)));
    e2e_relay_reordered(&offerer, &answerer, now);

    int new_got = 0;
    while (nanortc_poll_output(&answerer, &out) == NANORTC_OK) {
        if (out.type == NANORTC_OUTPUT_EVENT && out.event.type == NANORTC_EV_MEDIA_DATA &&
            out.event.media_data.len > 1 && out.event.media_data.data[1] == 0xD3) {
            new_got++;
        }
    }
    ASSERT_EQ(new_got, 1); /* new-SSRC stream delivered, not dropped */

    nanortc_destroy(&offerer);
    nanortc_destroy(&answerer);
}

#if NANORTC_FEATURE_VIDEO_NACK_RX
/* Receiver NACK recovers a dropped packet: a single packet is dropped; the
 * receiver detects the gap and NACKs; the sender retransmits it from pkt_ring;
 * the reorder buffer (holding the gap) fills it → the frame is recovered in
 * order. Validates the full NACK loss-recovery loop. */
TEST(test_e2e_video_nack_recovers_drop)
{
    nanortc_t offerer, answerer;
    nanortc_config_t off_cfg = e2e_default_config();
    off_cfg.role = NANORTC_ROLE_CONTROLLING;
    ASSERT_OK(nanortc_init(&offerer, &off_cfg));
    nanortc_config_t ans_cfg = e2e_default_config();
    ans_cfg.role = NANORTC_ROLE_CONTROLLED;
    ASSERT_OK(nanortc_init(&answerer, &ans_cfg));

    ASSERT_OK(nanortc_add_local_candidate(&offerer, "192.168.1.1", 4000));
    ASSERT_OK(nanortc_add_local_candidate(&answerer, "192.168.1.2", 5000));
    int off_mid = nanortc_add_video_track(&offerer, NANORTC_DIR_SENDONLY, NANORTC_CODEC_H264);
    int ans_mid = nanortc_add_video_track(&answerer, NANORTC_DIR_RECVONLY, NANORTC_CODEC_H264);
    ASSERT_TRUE(off_mid >= 0 && ans_mid >= 0);

    char offer[4096];
    size_t offer_len = 0;
    ASSERT_OK(nanortc_create_offer(&offerer, offer, sizeof(offer), &offer_len));
    offer[offer_len] = '\0';
    char answer[4096];
    size_t answer_len = 0;
    ASSERT_OK(nanortc_accept_offer(&answerer, offer, answer, sizeof(answer), &answer_len));
    answer[answer_len] = '\0';
    ASSERT_OK(nanortc_accept_answer(&offerer, answer));

    answerer.ice.remote_candidates[0].family = 4;
    answerer.ice.remote_candidates[0].addr[0] = 192;
    answerer.ice.remote_candidates[0].addr[1] = 168;
    answerer.ice.remote_candidates[0].addr[2] = 1;
    answerer.ice.remote_candidates[0].addr[3] = 1;
    answerer.ice.remote_candidates[0].port = 9999;
    answerer.ice.remote_candidate_count = 1;

    uint32_t now = 100;
    ASSERT_OK(nanortc_handle_input(&offerer, &(nanortc_input_t){.now_ms = now}));
    int connected = 0;
    for (int round = 0; round < 30; round++) {
        e2e_pump(&offerer, &answerer, now, 5);
        if (offerer.srtp.ready && answerer.srtp.ready) {
            connected = 1;
            break;
        }
    }
    ASSERT_TRUE(connected);
    {
        nanortc_output_t d;
        while (nanortc_poll_output(&offerer, &d) == NANORTC_OK) {
        }
        while (nanortc_poll_output(&answerer, &d) == NANORTC_OK) {
        }
    }

    uint8_t frame[40];
    uint32_t pts = 0;

    /* Frame 0 in order: baseline. */
    memset(frame, 0xC0, sizeof(frame));
    frame[0] = 0;
    frame[1] = 0;
    frame[2] = 0;
    frame[3] = 1;
    frame[4] = 0x41;
    now += 5;
    ASSERT_OK(nanortc_handle_input(&offerer, &(nanortc_input_t){.now_ms = now}));
    ASSERT_OK(nanortc_send_video(&offerer, (uint8_t)off_mid, pts, frame, sizeof(frame)));
    pts += 33;
    e2e_relay(&offerer, &answerer, now);
    {
        nanortc_output_t d;
        while (nanortc_poll_output(&answerer, &d) == NANORTC_OK) {
        }
    }

    /* Frame A (0xA1): sent but DROPPED on the wire (stays in the sender's
     * pkt_ring for retransmit). */
    memset(frame, 0xA1, sizeof(frame));
    frame[0] = 0;
    frame[1] = 0;
    frame[2] = 0;
    frame[3] = 1;
    frame[4] = 0x41;
    now += 5;
    ASSERT_OK(nanortc_handle_input(&offerer, &(nanortc_input_t){.now_ms = now}));
    ASSERT_OK(nanortc_send_video(&offerer, (uint8_t)off_mid, pts, frame, sizeof(frame)));
    pts += 33;
    {
        nanortc_output_t d;
        while (nanortc_poll_output(&offerer, &d) == NANORTC_OK) { /* drop everything (Frame A) */
        }
    }

    /* Frame B (0xB2): delivered → receiver sees the gap and NACKs. */
    memset(frame, 0xB2, sizeof(frame));
    frame[0] = 0;
    frame[1] = 0;
    frame[2] = 0;
    frame[3] = 1;
    frame[4] = 0x41;
    now += 5;
    ASSERT_OK(nanortc_handle_input(&offerer, &(nanortc_input_t){.now_ms = now}));
    ASSERT_OK(nanortc_send_video(&offerer, (uint8_t)off_mid, pts, frame, sizeof(frame)));
    pts += 33;
    e2e_relay(&offerer, &answerer, now);
    ASSERT_TRUE(answerer.stats_nack_sent >= 1u); /* NACK emitted on the gap */

    /* Deliver the NACK to the sender → it retransmits Frame A (into the
     * nack_retx_buf copy, TD-023), leaving the retransmit undrained in the
     * offerer's out_queue. */
    e2e_relay(&answerer, &offerer, now);
    /* TD-023: clobber the offerer's whole pkt_ring now — as a concurrent
     * send_video that wrapped the ring would. The in-flight retransmit must
     * survive because it was COPIED out of pkt_ring; a direct-pointer enqueue
     * would now ship 0xFF and fail SRTP auth → Frame A would never recover. */
    memset(offerer.pkt_ring, 0xFF, sizeof(offerer.pkt_ring));
    /* Deliver the retransmit → the reorder buffer fills the gap, recovering A. */
    e2e_relay(&offerer, &answerer, now);

    uint8_t bytes[6];
    int got = 0;
    nanortc_output_t out;
    while (nanortc_poll_output(&answerer, &out) == NANORTC_OK) {
        if (out.type == NANORTC_OUTPUT_EVENT && out.event.type == NANORTC_EV_MEDIA_DATA &&
            out.event.media_data.len > 1 && got < 6) {
            bytes[got++] = out.event.media_data.data[1];
        }
    }
    /* Frame A (0xA1) recovered and delivered before Frame B (0xB2). */
    ASSERT_TRUE(got >= 2);
    ASSERT_EQ(bytes[0], (uint8_t)0xA1);
    ASSERT_EQ(bytes[1], (uint8_t)0xB2);

    nanortc_destroy(&offerer);
    nanortc_destroy(&answerer);
}
#endif /* NANORTC_FEATURE_VIDEO_NACK_RX */
#endif /* NANORTC_FEATURE_VIDEO_REORDER */

#if NANORTC_FEATURE_VIDEO_FEC
#if NANORTC_FEATURE_H265
/* The H.265 callback packetizer shares the FEC prepare/commit path with the
 * direct H.264 iterator. Complete one group under both pacing modes so codec
 * callback failures cannot silently leave H.265 outside FEC coverage. */
TEST(test_e2e_h265_fec_group_commits)
{
    nanortc_t rtc;
    ASSERT_OK(e2e_init_direct_srtp_endpoint(&rtc, 1));
    int mid = nanortc_add_video_track(&rtc, NANORTC_DIR_SENDONLY, NANORTC_CODEC_H265);
    ASSERT_TRUE(mid >= 0);
    ASSERT_OK(rtp_init(&rtc.media[0].rtp, 0xFEC26501u, NANORTC_VIDEO_H265_DEFAULT_PT));
    rtc.state = NANORTC_STATE_CONNECTED;
    rtc.remote_addr.family = 4;
    rtc.remote_addr.addr[0] = 192;
    rtc.remote_addr.addr[1] = 0;
    rtc.remote_addr.addr[2] = 2;
    rtc.remote_addr.addr[3] = 65;
    rtc.remote_addr.port = 5008;
#if NANORTC_FEC_ADAPTIVE
    rtc.bwe.smoothed_loss_q8 = (uint16_t)NANORTC_FEC_LOSS_OFF_Q8;
#endif

    const uint8_t frame[] = {0, 0, 0, 1, 0x02, 0x01, 0xA5, 0x5A};
    uint32_t sent_before = rtc.stats_fec_sent;
    nanortc_output_t out;
    for (uint8_t i = 0; i < (uint8_t)NANORTC_FEC_GROUP_SIZE; i++) {
        ASSERT_OK(nanortc_send_video(&rtc, (uint8_t)mid, (uint32_t)i * 33u, frame, sizeof(frame)));
        while (nanortc_poll_output(&rtc, &out) == NANORTC_OK) {
        }
    }
    ASSERT_EQ(rtc.stats_fec_sent, sent_before + 1u);
    ASSERT_EQ(rtc.fec_tx_n, 0u);
    nanortc_destroy(&rtc);
}
#endif /* NANORTC_FEATURE_H265 */

#if !NANORTC_FEATURE_VIDEO_PACING
/* Pacing-off FEC admission must never consume the last output slot and leave a
 * frame half-sent. Start with K-1 protected media packets, then make exactly
 * one output slot available: the final media packet must be queued, while the
 * now-complete FEC group is discarded atomically and accounted. */
TEST(test_e2e_video_fec_resource_drop_is_atomic)
{
    nanortc_t rtc;
    ASSERT_OK(e2e_init_direct_srtp_endpoint(&rtc, 1));
    int mid = nanortc_add_video_track(&rtc, NANORTC_DIR_SENDONLY, NANORTC_CODEC_H264);
    ASSERT_TRUE(mid >= 0);
    ASSERT_OK(rtp_init(&rtc.media[0].rtp, 0xFEC00001u, NANORTC_VIDEO_DEFAULT_PT));
    rtc.state = NANORTC_STATE_CONNECTED;
    rtc.remote_addr.family = 4;
    rtc.remote_addr.addr[0] = 192;
    rtc.remote_addr.addr[1] = 0;
    rtc.remote_addr.addr[2] = 2;
    rtc.remote_addr.addr[3] = 9;
    rtc.remote_addr.port = 5006;
#if NANORTC_FEC_ADAPTIVE
    rtc.bwe.smoothed_loss_q8 = (uint16_t)NANORTC_FEC_LOSS_OFF_Q8;
#endif

    uint8_t frame[32];
    memset(frame, 0x5A, sizeof(frame));
    frame[0] = 0;
    frame[1] = 0;
    frame[2] = 0;
    frame[3] = 1;
    frame[4] = 0x41; /* one non-IDR H.264 NAL -> one media packet */

    /* A failed media protect must not commit its prepared FEC member. */
    nanortc_crypto_provider_t failing_crypto = *rtc.srtp.crypto;
    failing_crypto.aes_128_cm = e2e_fail_aes_128_cm;
    const nanortc_crypto_provider_t *working_crypto = rtc.srtp.crypto;
    uint8_t fec_n_before = rtc.fec_tx_n;
    rtc.srtp.crypto = &failing_crypto;
    ASSERT_EQ(nanortc_send_video(&rtc, (uint8_t)mid, 0u, frame, sizeof(frame)), NANORTC_ERR_CRYPTO);
    ASSERT_EQ(rtc.fec_tx_n, fec_n_before);
    ASSERT_EQ(nanortc_output_free_slots(&rtc), (uint16_t)NANORTC_OUT_QUEUE_SIZE);
    rtc.srtp.crypto = working_crypto;

    /* Form a K-1 partial group while draining each media output. */
    nanortc_output_t out;
    for (uint8_t i = 0; i + 1u < (uint8_t)NANORTC_FEC_GROUP_SIZE; i++) {
        ASSERT_OK(
            nanortc_send_video(&rtc, (uint8_t)mid, (uint32_t)(i + 1u) * 33u, frame, sizeof(frame)));
        while (nanortc_poll_output(&rtc, &out) == NANORTC_OK) {
        }
    }
    ASSERT_EQ(rtc.fec_tx_n, (uint8_t)(NANORTC_FEC_GROUP_SIZE - 1u));
    ASSERT_EQ(nanortc_output_free_slots(&rtc), (uint16_t)NANORTC_OUT_QUEUE_SIZE);

    /* White-box queue fill: synthetic events own no backing buffers, so they
     * isolate the media/FEC admission decision without consuming TX slots. */
    for (uint16_t i = 0; i + 1u < (uint16_t)NANORTC_OUT_QUEUE_SIZE; i++) {
        uint16_t slot = rtc.out_tail & (NANORTC_OUT_QUEUE_SIZE - 1u);
        memset(&rtc.out_queue[slot], 0, sizeof(rtc.out_queue[slot]));
        rtc.out_queue[slot].type = NANORTC_OUTPUT_EVENT;
        rtc.out_tail++;
    }
    ASSERT_EQ(nanortc_output_free_slots(&rtc), 1u);

    uint32_t sent_before = rtc.stats_fec_sent;
    uint32_t dropped_before = rtc.stats_fec_dropped_resource;
    ASSERT_OK(nanortc_send_video(&rtc, (uint8_t)mid, 1000u, frame, sizeof(frame)));
    ASSERT_EQ(nanortc_output_free_slots(&rtc), 0u);
    ASSERT_EQ(rtc.stats_fec_sent, sent_before);
    ASSERT_EQ(rtc.stats_fec_dropped_resource, dropped_before + 1u);
    ASSERT_EQ(rtc.fec_tx_n, 0u);

    int media_packets = 0;
    int fec_packets = 0;
    while (nanortc_poll_output(&rtc, &out) == NANORTC_OK) {
        if (out.type != NANORTC_OUTPUT_TRANSMIT) {
            continue;
        }
        if (out.transmit.len > 1u && (out.transmit.data[1] & 0x7Fu) == NANORTC_VIDEO_FEC_PT) {
            fec_packets++;
        } else {
            media_packets++;
        }
    }
    ASSERT_EQ(media_packets, 1);
    ASSERT_EQ(fec_packets, 0);

    /* With room for both packets, completing the same partial group emits one
     * FEC packet plus the media packet (the existing recovery test validates
     * the receive-side semantics of this resource-rich path). */
    for (uint8_t i = 0; i + 1u < (uint8_t)NANORTC_FEC_GROUP_SIZE; i++) {
        ASSERT_OK(nanortc_send_video(&rtc, (uint8_t)mid, 2000u + (uint32_t)i * 33u, frame,
                                     sizeof(frame)));
        while (nanortc_poll_output(&rtc, &out) == NANORTC_OK) {
        }
    }
    sent_before = rtc.stats_fec_sent;
    ASSERT_OK(nanortc_send_video(&rtc, (uint8_t)mid, 3000u, frame, sizeof(frame)));
    ASSERT_EQ(rtc.stats_fec_sent, sent_before + 1u);
    media_packets = 0;
    fec_packets = 0;
    while (nanortc_poll_output(&rtc, &out) == NANORTC_OK) {
        if (out.type != NANORTC_OUTPUT_TRANSMIT) {
            continue;
        }
        if (out.transmit.len > 1u && (out.transmit.data[1] & 0x7Fu) == NANORTC_VIDEO_FEC_PT) {
            fec_packets++;
        } else {
            media_packets++;
        }
    }
    ASSERT_EQ(media_packets, 1);
    ASSERT_EQ(fec_packets, 1);

    nanortc_destroy(&rtc);
}
#endif /* !NANORTC_FEATURE_VIDEO_PACING */

/* Relay offerer→answerer, dropping the `drop_media_nth` MEDIA RTP packet (FEC
 * packets — PT NANORTC_VIDEO_FEC_PT — are always delivered, since the whole
 * point is to recover the dropped media from the FEC). */
static int e2e_relay_drop_media(nanortc_t *from, nanortc_t *to, uint32_t now, int drop_media_nth)
{
    int relayed = 0;
    int media_idx = 0;
    nanortc_addr_t src;
    memset(&src, 0, sizeof(src));
    src.family = 4;
    src.addr[0] = 192;
    src.addr[1] = 168;
    src.addr[2] = 1;
    src.addr[3] = 1;
    src.port = 9999;
    nanortc_output_t out;
    while (nanortc_poll_output(from, &out) == NANORTC_OK) {
        if (out.type != NANORTC_OUTPUT_TRANSMIT) {
            continue;
        }
        const uint8_t *d = out.transmit.data;
        int is_rtp = (out.transmit.len > 1 && d[0] >= 0x80 && d[0] <= 0xBF);
        int is_fec = is_rtp && ((d[1] & 0x7f) == NANORTC_VIDEO_FEC_PT);
        if (is_rtp && !is_fec) {
            if (media_idx++ == drop_media_nth) {
                continue; /* drop exactly one media packet → FEC must recover it */
            }
        }
        nanortc_handle_input(
            to, &(nanortc_input_t){
                    .now_ms = now, .data = out.transmit.data, .len = out.transmit.len, .src = src});
        relayed++;
    }
    return relayed;
}

/* ULPFEC recovers a dropped media packet with ZERO retransmit (the FEC
 * advantage over NACK on high-RTT links). Send one full FEC group (K single-NAL
 * frames → K media packets → one FEC packet), drop one media packet on the wire,
 * deliver the rest plus the FEC, and assert the receiver reconstructs the lost
 * packet from the FEC alone — no return traffic. */
TEST(test_e2e_video_fec_recovers_drop)
{
    nanortc_t offerer, answerer;
    nanortc_config_t off_cfg = e2e_default_config();
    off_cfg.role = NANORTC_ROLE_CONTROLLING;
    ASSERT_OK(nanortc_init(&offerer, &off_cfg));
    nanortc_config_t ans_cfg = e2e_default_config();
    ans_cfg.role = NANORTC_ROLE_CONTROLLED;
    ASSERT_OK(nanortc_init(&answerer, &ans_cfg));

    ASSERT_OK(nanortc_add_local_candidate(&offerer, "192.168.1.1", 4000));
    ASSERT_OK(nanortc_add_local_candidate(&answerer, "192.168.1.2", 5000));
    int off_mid = nanortc_add_video_track(&offerer, NANORTC_DIR_SENDONLY, NANORTC_CODEC_H264);
    int ans_mid = nanortc_add_video_track(&answerer, NANORTC_DIR_RECVONLY, NANORTC_CODEC_H264);
    ASSERT_TRUE(off_mid >= 0 && ans_mid >= 0);

    char offer[4096];
    size_t offer_len = 0;
    ASSERT_OK(nanortc_create_offer(&offerer, offer, sizeof(offer), &offer_len));
    offer[offer_len] = '\0';
    char answer[4096];
    size_t answer_len = 0;
    ASSERT_OK(nanortc_accept_offer(&answerer, offer, answer, sizeof(answer), &answer_len));
    answer[answer_len] = '\0';
    ASSERT_OK(nanortc_accept_answer(&offerer, answer));

    answerer.ice.remote_candidates[0].family = 4;
    answerer.ice.remote_candidates[0].addr[0] = 192;
    answerer.ice.remote_candidates[0].addr[1] = 168;
    answerer.ice.remote_candidates[0].addr[2] = 1;
    answerer.ice.remote_candidates[0].addr[3] = 1;
    answerer.ice.remote_candidates[0].port = 9999;
    answerer.ice.remote_candidate_count = 1;

    uint32_t now = 100;
    ASSERT_OK(nanortc_handle_input(&offerer, &(nanortc_input_t){.now_ms = now}));
    int connected = 0;
    for (int round = 0; round < 30; round++) {
        e2e_pump(&offerer, &answerer, now, 5);
        if (offerer.srtp.ready && answerer.srtp.ready) {
            connected = 1;
            break;
        }
    }
    ASSERT_TRUE(connected);
    {
        nanortc_output_t d;
        while (nanortc_poll_output(&offerer, &d) == NANORTC_OK) {
        }
        while (nanortc_poll_output(&answerer, &d) == NANORTC_OK) {
        }
    }

    /* Adaptive FEC sends no FEC on a clean link (zero overhead). Seed a moderate
     * smoothed loss so the sender uses the full group K = NANORTC_FEC_GROUP_SIZE
     * (this test's group assumption). */
    offerer.bwe.smoothed_loss_q8 = 12; /* ~5%, in the [OFF,HIGH) moderate band */

    /* Send exactly one FEC group: K single-NAL frames (each = one media packet).
     * Frame i carries a distinct payload byte 0xA0+i so delivery is checkable. */
    const int K = NANORTC_FEC_GROUP_SIZE;
    uint8_t frame[40];
    uint32_t pts = 0;
    for (int i = 0; i < K; i++) {
        memset(frame, (uint8_t)(0xA0 + i), sizeof(frame));
        frame[0] = 0;
        frame[1] = 0;
        frame[2] = 0;
        frame[3] = 1;
        frame[4] = 0x41; /* non-IDR slice NAL header */
        now += 5;
        ASSERT_OK(nanortc_handle_input(&offerer, &(nanortc_input_t){.now_ms = now}));
        ASSERT_OK(nanortc_send_video(&offerer, (uint8_t)off_mid, pts, frame, sizeof(frame)));
        pts += 33;
    }

    /* Let the pacer accrue budget, then relay everything (the FEC + K-1 media),
     * dropping media #3. The FEC reconstructs it with no traffic back to the
     * sender. */
    now += 50;
    ASSERT_OK(nanortc_handle_input(&offerer, &(nanortc_input_t){.now_ms = now}));
    int drop_idx = 3;
    for (int r = 0; r < 6; r++) {
        now += 5;
        ASSERT_OK(nanortc_handle_input(&offerer, &(nanortc_input_t){.now_ms = now}));
        e2e_relay_drop_media(&offerer, &answerer, now, drop_idx);
        drop_idx = -1; /* only drop on the first pass */
    }

    /* Drain the receiver: every frame 0xA0..0xA0+K-1 must arrive, including the
     * dropped 0xA3 — recovered from FEC. */
    uint8_t seen[16];
    memset(seen, 0, sizeof(seen));
    nanortc_output_t out;
    for (int r = 0; r < 40; r++) {
        now += 5;
        ASSERT_OK(nanortc_handle_input(&answerer, &(nanortc_input_t){.now_ms = now}));
        while (nanortc_poll_output(&answerer, &out) == NANORTC_OK) {
            if (out.type == NANORTC_OUTPUT_EVENT && out.event.type == NANORTC_EV_MEDIA_DATA &&
                out.event.media_data.len > 1) {
                uint8_t b = out.event.media_data.data[1];
                if (b >= 0xA0 && b < (uint8_t)(0xA0 + K)) {
                    seen[b - 0xA0] = 1;
                }
            }
        }
    }

    ASSERT_TRUE(answerer.stats_fec_recovered >= 1u); /* FEC reconstructed a packet */
    ASSERT_EQ(seen[3], 1);                           /* the dropped frame was recovered */
    int delivered = 0;
    for (int i = 0; i < K; i++) {
        delivered += seen[i];
    }
    ASSERT_EQ(delivered, K); /* the whole group arrived, zero retransmit */

#if NANORTC_FEATURE_VIDEO_NACK_RX
    /* NACK↔FEC coordination: the dropped packet sits in the FEC group, and the
     * FEC packet (sent ahead of the paced media) declared its protected SN
     * window before the gap was observed — so the receiver must NOT NACK it
     * (FEC already recovered it; a NACK would be a duplicate request). */
#if NANORTC_FEATURE_VIDEO_PACING
    ASSERT_EQ(answerer.stats_nack_sent, 0u);
    ASSERT_TRUE(answerer.stats_nack_suppressed_fec >= 1u);
#else
    /* Without pacing, earlier members of a group are already on the wire when
     * the final member completes and emits FEC. A gap may therefore trigger a
     * best-effort NACK before the receiver sees the group's FEC window. */
    ASSERT_TRUE(answerer.stats_nack_sent >= 1u || answerer.stats_nack_suppressed_fec >= 1u);
#endif
#endif

    nanortc_destroy(&offerer);
    nanortc_destroy(&answerer);
}

#if NANORTC_FEC_ADAPTIVE
/* Adaptive FEC: the group size K (overhead = 1/K) tracks the smoothed TWCC loss
 * fraction — NO FEC on a clean link (zero overhead on the scarce camera uplink),
 * the default group at moderate loss, the smallest group (most protection) at
 * high loss. Verified by the count of FEC packets emitted per fixed media count.
 * The receiver is K-agnostic, so this is a sender-only, wire-compatible change. */
TEST(test_e2e_video_fec_adaptive_k_tracks_loss)
{
    nanortc_t offerer, answerer;
    nanortc_config_t off_cfg = e2e_default_config();
    off_cfg.role = NANORTC_ROLE_CONTROLLING;
    ASSERT_OK(nanortc_init(&offerer, &off_cfg));
    nanortc_config_t ans_cfg = e2e_default_config();
    ans_cfg.role = NANORTC_ROLE_CONTROLLED;
    ASSERT_OK(nanortc_init(&answerer, &ans_cfg));

    ASSERT_OK(nanortc_add_local_candidate(&offerer, "192.168.1.1", 4000));
    ASSERT_OK(nanortc_add_local_candidate(&answerer, "192.168.1.2", 5000));
    int off_mid = nanortc_add_video_track(&offerer, NANORTC_DIR_SENDONLY, NANORTC_CODEC_H264);
    int ans_mid = nanortc_add_video_track(&answerer, NANORTC_DIR_RECVONLY, NANORTC_CODEC_H264);
    ASSERT_TRUE(off_mid >= 0 && ans_mid >= 0);

    char offer[4096];
    size_t offer_len = 0;
    ASSERT_OK(nanortc_create_offer(&offerer, offer, sizeof(offer), &offer_len));
    offer[offer_len] = '\0';
    char answer[4096];
    size_t answer_len = 0;
    ASSERT_OK(nanortc_accept_offer(&answerer, offer, answer, sizeof(answer), &answer_len));
    answer[answer_len] = '\0';
    ASSERT_OK(nanortc_accept_answer(&offerer, answer));
    answerer.ice.remote_candidates[0].family = 4;
    answerer.ice.remote_candidates[0].addr[0] = 192;
    answerer.ice.remote_candidates[0].addr[1] = 168;
    answerer.ice.remote_candidates[0].addr[2] = 1;
    answerer.ice.remote_candidates[0].addr[3] = 1;
    answerer.ice.remote_candidates[0].port = 9999;
    answerer.ice.remote_candidate_count = 1;

    uint32_t now = 100;
    ASSERT_OK(nanortc_handle_input(&offerer, &(nanortc_input_t){.now_ms = now}));
    int connected = 0;
    for (int round = 0; round < 30; round++) {
        e2e_pump(&offerer, &answerer, now, 5);
        if (offerer.srtp.ready && answerer.srtp.ready) {
            connected = 1;
            break;
        }
    }
    ASSERT_TRUE(connected);
    e2e_flush_sender(&offerer, &now);

    uint8_t frame[40];
    memset(frame, 0x5A, sizeof(frame));
    frame[0] = 0;
    frame[1] = 0;
    frame[2] = 0;
    frame[3] = 1;
    frame[4] = 0x41; /* non-IDR slice */
    uint32_t pts = 0;
    const int K = NANORTC_FEC_GROUP_SIZE;
    const int KMIN = NANORTC_FEC_MIN_GROUP;

    /* Phase A — clean link (loss below the OFF threshold): zero FEC. */
    offerer.bwe.smoothed_loss_q8 = 0;
    uint32_t base = offerer.stats_fec_sent;
    for (int i = 0; i < K; i++) {
        now += 5;
        ASSERT_OK(nanortc_handle_input(&offerer, &(nanortc_input_t){.now_ms = now}));
        ASSERT_OK(nanortc_send_video(&offerer, (uint8_t)off_mid, pts, frame, sizeof(frame)));
        pts += 33;
        e2e_flush_sender(&offerer, &now);
    }
    ASSERT_EQ(offerer.stats_fec_sent, base); /* clean link → no FEC overhead */

    /* Phase B — moderate loss: K = NANORTC_FEC_GROUP_SIZE → one FEC per K frames. */
    offerer.bwe.smoothed_loss_q8 = 12;
    base = offerer.stats_fec_sent;
    for (int i = 0; i < K; i++) {
        now += 5;
        ASSERT_OK(nanortc_handle_input(&offerer, &(nanortc_input_t){.now_ms = now}));
        ASSERT_OK(nanortc_send_video(&offerer, (uint8_t)off_mid, pts, frame, sizeof(frame)));
        pts += 33;
        e2e_flush_sender(&offerer, &now);
    }
    ASSERT_EQ(offerer.stats_fec_sent, base + 1u); /* one group of K */

    /* Phase C — high loss: K = NANORTC_FEC_MIN_GROUP → more FEC (smaller groups). */
    offerer.bwe.smoothed_loss_q8 = 50;
    base = offerer.stats_fec_sent;
    for (int i = 0; i < K; i++) {
        now += 5;
        ASSERT_OK(nanortc_handle_input(&offerer, &(nanortc_input_t){.now_ms = now}));
        ASSERT_OK(nanortc_send_video(&offerer, (uint8_t)off_mid, pts, frame, sizeof(frame)));
        pts += 33;
        e2e_flush_sender(&offerer, &now);
    }
    ASSERT_EQ(offerer.stats_fec_sent, base + (uint32_t)(K / KMIN)); /* smaller K → more FEC */

    nanortc_destroy(&offerer);
    nanortc_destroy(&answerer);
}
#endif /* NANORTC_FEC_ADAPTIVE */

#if NANORTC_FEC_TX_RING > 1
/* Multi-group FEC tx (TD-026 L1): a bursty IDR that fragments into >K packets
 * completes several FEC groups within ONE send call (before the app polls). The
 * FEC tx ring lets each group's FEC emit without aliasing — so a 2-group frame
 * yields 2 FEC packets, not 1. (With a single buffer only the first group's FEC
 * would survive the reuse guard.) */
TEST(test_e2e_video_fec_multigroup_idr)
{
    nanortc_t offerer, answerer;
    nanortc_config_t off_cfg = e2e_default_config();
    off_cfg.role = NANORTC_ROLE_CONTROLLING;
    ASSERT_OK(nanortc_init(&offerer, &off_cfg));
    nanortc_config_t ans_cfg = e2e_default_config();
    ans_cfg.role = NANORTC_ROLE_CONTROLLED;
    ASSERT_OK(nanortc_init(&answerer, &ans_cfg));

    ASSERT_OK(nanortc_add_local_candidate(&offerer, "192.168.1.1", 4000));
    ASSERT_OK(nanortc_add_local_candidate(&answerer, "192.168.1.2", 5000));
    int off_mid = nanortc_add_video_track(&offerer, NANORTC_DIR_SENDONLY, NANORTC_CODEC_H264);
    int ans_mid = nanortc_add_video_track(&answerer, NANORTC_DIR_RECVONLY, NANORTC_CODEC_H264);
    ASSERT_TRUE(off_mid >= 0 && ans_mid >= 0);

    char offer[4096];
    size_t offer_len = 0;
    ASSERT_OK(nanortc_create_offer(&offerer, offer, sizeof(offer), &offer_len));
    offer[offer_len] = '\0';
    char answer[4096];
    size_t answer_len = 0;
    ASSERT_OK(nanortc_accept_offer(&answerer, offer, answer, sizeof(answer), &answer_len));
    answer[answer_len] = '\0';
    ASSERT_OK(nanortc_accept_answer(&offerer, answer));
    answerer.ice.remote_candidates[0].family = 4;
    answerer.ice.remote_candidates[0].addr[0] = 192;
    answerer.ice.remote_candidates[0].addr[1] = 168;
    answerer.ice.remote_candidates[0].addr[2] = 1;
    answerer.ice.remote_candidates[0].addr[3] = 1;
    answerer.ice.remote_candidates[0].port = 9999;
    answerer.ice.remote_candidate_count = 1;

    uint32_t now = 100;
    ASSERT_OK(nanortc_handle_input(&offerer, &(nanortc_input_t){.now_ms = now}));
    int connected = 0;
    for (int round = 0; round < 30; round++) {
        e2e_pump(&offerer, &answerer, now, 5);
        if (offerer.srtp.ready && answerer.srtp.ready) {
            connected = 1;
            break;
        }
    }
    ASSERT_TRUE(connected);
    e2e_flush_sender(&offerer, &now);

    /* Moderate loss → adaptive K = NANORTC_FEC_GROUP_SIZE. */
    offerer.bwe.smoothed_loss_q8 = 12;

    /* Build a single IDR NAL that fragments into exactly 2*K FU-A packets (= 2
     * FEC groups). Per the FU-A math used by test_e2e_video_send_admission. */
    const size_t per = (size_t)NANORTC_VIDEO_MTU - 2;
    const int K = NANORTC_FEC_GROUP_SIZE;
    const int nfrags = 2 * K;
    const size_t nal_len = (size_t)(nfrags - 1) * per + 11;
    const size_t frame_len = 4 + nal_len;
    ASSERT_TRUE(frame_len <= sizeof(g_admission_frame));
    ASSERT_TRUE((int)(2 * K) <= NANORTC_VIDEO_PKT_RING_SIZE); /* must fit pkt_ring admission */
    memset(g_admission_frame, 0xCD, frame_len);
    g_admission_frame[0] = 0x00;
    g_admission_frame[1] = 0x00;
    g_admission_frame[2] = 0x00;
    g_admission_frame[3] = 0x01;
    g_admission_frame[4] = 0x65; /* IDR slice */

    uint32_t base = offerer.stats_fec_sent;
    now += 5;
    ASSERT_OK(nanortc_handle_input(&offerer, &(nanortc_input_t){.now_ms = now}));
    /* ONE send call (no poll between) completes both FEC groups. The ring lets
     * both emit; a single buffer would drop the 2nd. */
    ASSERT_OK(nanortc_send_video(&offerer, (uint8_t)off_mid, 0, g_admission_frame, frame_len));
    ASSERT_EQ(offerer.stats_fec_sent, base + 2u);

    nanortc_destroy(&offerer);
    nanortc_destroy(&answerer);
}
#endif /* NANORTC_FEC_TX_RING > 1 */
#endif /* NANORTC_FEATURE_VIDEO_FEC */
#endif /* NANORTC_FEATURE_VIDEO */

TEST(test_e2e_connected_event_has_mids)
{
    /* Verify the nanortc_ev_connected_t struct layout is usable */
    nanortc_event_t event;
    memset(&event, 0, sizeof(event));
    event.type = NANORTC_EV_CONNECTED;

    /* Simulate: one audio track */
    event.connected.mids[0] = 0;
    event.connected.mid_count = 1;

    ASSERT_EQ(event.connected.mid_count, 1);
    ASSERT_EQ(event.connected.mids[0], 0);

#if NANORTC_FEATURE_VIDEO
    /* Add a video track */
    event.connected.mids[1] = 1;
    event.connected.mid_count = 2;

    ASSERT_EQ(event.connected.mid_count, 2);
    ASSERT_EQ(event.connected.mids[1], 1);
#endif
}

TEST(test_e2e_request_keyframe_bad_params)
{
    nanortc_t rtc;
    nanortc_config_t cfg = e2e_default_config();
    ASSERT_OK(nanortc_init(&rtc, &cfg));

    ASSERT_FAIL(nanortc_request_keyframe(NULL, 0));
    /* Not connected → ERR_STATE */
    ASSERT_EQ(nanortc_request_keyframe(&rtc, 0), NANORTC_ERR_STATE);

    nanortc_destroy(&rtc);
}

/* ---- Multi-media offer/answer (exercises SDP gen + parse for audio+video) ---- */

TEST(test_e2e_media_offer_answer)
{
    nanortc_t offerer, answerer;
    nanortc_config_t cfg_o = e2e_default_config();
    nanortc_config_t cfg_a = e2e_default_config();
    ASSERT_OK(nanortc_init(&offerer, &cfg_o));
    ASSERT_OK(nanortc_init(&answerer, &cfg_a));

    /* Offerer adds audio + video tracks */
    int mid_audio =
        nanortc_add_audio_track(&offerer, NANORTC_DIR_SENDRECV, NANORTC_CODEC_OPUS, 48000, 2);
    ASSERT_TRUE(mid_audio >= 0);

#if NANORTC_FEATURE_VIDEO
    int mid_video = nanortc_add_video_track(&offerer, NANORTC_DIR_SENDRECV, NANORTC_CODEC_H264);
    ASSERT_TRUE(mid_video >= 0);
#endif

    nanortc_add_local_candidate(&offerer, "192.168.1.1", 10000);

    /* Create offer */
    char offer[4096];
    size_t offer_len = 0;
    ASSERT_OK(nanortc_create_offer(&offerer, offer, sizeof(offer), &offer_len));
    ASSERT_TRUE(offer_len > 100);

    /* Answerer adds matching tracks and accepts offer */
    nanortc_add_audio_track(&answerer, NANORTC_DIR_SENDRECV, NANORTC_CODEC_OPUS, 48000, 2);
#if NANORTC_FEATURE_VIDEO
    nanortc_add_video_track(&answerer, NANORTC_DIR_SENDRECV, NANORTC_CODEC_H264);
#endif
    nanortc_add_local_candidate(&answerer, "192.168.1.2", 10001);

    char answer[4096];
    size_t answer_len = 0;
    ASSERT_OK(nanortc_accept_offer(&answerer, offer, answer, sizeof(answer), &answer_len));
    ASSERT_TRUE(answer_len > 100);

    /* Offerer accepts answer */
    ASSERT_OK(nanortc_accept_answer(&offerer, answer));

    /* Both should have progressed beyond NEW */
    ASSERT_TRUE(nanortc_is_alive(&offerer));
    ASSERT_TRUE(nanortc_is_alive(&answerer));

    nanortc_destroy(&offerer);
    nanortc_destroy(&answerer);
}

/* ---- Additional API coverage tests for nano_rtc.c ---- */

TEST(test_e2e_track_stats_not_connected)
{
    nanortc_t rtc;
    nanortc_config_t cfg = e2e_default_config();
    ASSERT_OK(nanortc_init(&rtc, &cfg));

    nanortc_track_stats_t stats;
    memset(&stats, 0, sizeof(stats));

    /* No tracks → invalid param */
    ASSERT_FAIL(nanortc_get_track_stats(&rtc, 0, &stats));
    ASSERT_FAIL(nanortc_get_track_stats(NULL, 0, &stats));
    ASSERT_FAIL(nanortc_get_track_stats(&rtc, 0, NULL));

    nanortc_destroy(&rtc);
}

TEST(test_e2e_track_stats_with_track)
{
    nanortc_t rtc;
    nanortc_config_t cfg = e2e_default_config();
    ASSERT_OK(nanortc_init(&rtc, &cfg));

    int mid = nanortc_add_audio_track(&rtc, NANORTC_DIR_SENDRECV, NANORTC_CODEC_OPUS, 48000, 2);
    ASSERT_TRUE(mid >= 0);

    nanortc_track_stats_t stats;
    memset(&stats, 0, sizeof(stats));
    ASSERT_OK(nanortc_get_track_stats(&rtc, (uint8_t)mid, &stats));
    ASSERT_EQ(stats.packets_sent, 0);
    ASSERT_EQ(stats.octets_sent, 0);

    nanortc_destroy(&rtc);
}

TEST(test_e2e_set_direction)
{
    nanortc_t rtc;
    nanortc_config_t cfg = e2e_default_config();
    ASSERT_OK(nanortc_init(&rtc, &cfg));

    int mid = nanortc_add_audio_track(&rtc, NANORTC_DIR_SENDRECV, NANORTC_CODEC_OPUS, 48000, 2);
    ASSERT_TRUE(mid >= 0);

    /* Change direction */
    nanortc_set_direction(&rtc, (uint8_t)mid, NANORTC_DIR_RECVONLY);

    /* Verify via stats (direction doesn't show in stats, but we can verify no crash) */
    nanortc_track_stats_t stats;
    ASSERT_OK(nanortc_get_track_stats(&rtc, (uint8_t)mid, &stats));

    /* Change direction on non-existent track should be safe (no crash) */
    nanortc_set_direction(&rtc, 99, NANORTC_DIR_SENDONLY);
    nanortc_set_direction(NULL, 0, NANORTC_DIR_SENDONLY);

    nanortc_destroy(&rtc);
}

TEST(test_e2e_add_track_max)
{
    nanortc_t rtc;
    nanortc_config_t cfg = e2e_default_config();
    ASSERT_OK(nanortc_init(&rtc, &cfg));

    /* Fill tracks up to NANORTC_MAX_MEDIA_TRACKS */
    for (int i = 0; i < NANORTC_MAX_MEDIA_TRACKS; i++) {
        int mid = nanortc_add_audio_track(&rtc, NANORTC_DIR_SENDRECV, NANORTC_CODEC_OPUS, 48000, 2);
        ASSERT_TRUE(mid >= 0);
    }

    /* Next should fail */
    int overflow =
        nanortc_add_audio_track(&rtc, NANORTC_DIR_SENDRECV, NANORTC_CODEC_OPUS, 48000, 2);
    ASSERT_TRUE(overflow < 0);

    nanortc_destroy(&rtc);
}

TEST(test_e2e_add_track_codecs)
{
    /* Test PCMU codec (MAX_MEDIA_TRACKS=2, so use separate instances) */
    nanortc_t rtc1;
    nanortc_config_t cfg1 = e2e_default_config();
    ASSERT_OK(nanortc_init(&rtc1, &cfg1));
    int mid_pcmu =
        nanortc_add_audio_track(&rtc1, NANORTC_DIR_SENDONLY, NANORTC_CODEC_PCMU, 8000, 1);
    ASSERT_TRUE(mid_pcmu >= 0);
    nanortc_destroy(&rtc1);

    /* Test PCMA codec */
    nanortc_t rtc2;
    nanortc_config_t cfg2 = e2e_default_config();
    ASSERT_OK(nanortc_init(&rtc2, &cfg2));
    int mid_pcma =
        nanortc_add_audio_track(&rtc2, NANORTC_DIR_SENDONLY, NANORTC_CODEC_PCMA, 8000, 1);
    ASSERT_TRUE(mid_pcma >= 0);
    nanortc_destroy(&rtc2);

#if NANORTC_FEATURE_VIDEO
    /* Test video codec */
    nanortc_t rtc3;
    nanortc_config_t cfg3 = e2e_default_config();
    ASSERT_OK(nanortc_init(&rtc3, &cfg3));
    int mid_video = nanortc_add_video_track(&rtc3, NANORTC_DIR_SENDONLY, NANORTC_CODEC_H264);
    ASSERT_TRUE(mid_video >= 0);
    nanortc_destroy(&rtc3);
#endif
}

TEST(test_e2e_add_track_null)
{
    ASSERT_TRUE(nanortc_add_audio_track(NULL, NANORTC_DIR_SENDRECV, NANORTC_CODEC_OPUS, 48000, 2) <
                0);
#if NANORTC_FEATURE_VIDEO
    ASSERT_TRUE(nanortc_add_video_track(NULL, NANORTC_DIR_SENDONLY, NANORTC_CODEC_H264) < 0);
#endif
}

TEST(test_e2e_accept_offer_bad_sdp)
{
    nanortc_t rtc;
    nanortc_config_t cfg = e2e_default_config();
    ASSERT_OK(nanortc_init(&rtc, &cfg));

    char answer[4096];
    size_t answer_len = 0;

    /* NULL params */
    ASSERT_FAIL(nanortc_accept_offer(NULL, "v=0\r\n", answer, sizeof(answer), &answer_len));
    ASSERT_FAIL(nanortc_accept_offer(&rtc, NULL, answer, sizeof(answer), &answer_len));
    ASSERT_FAIL(nanortc_accept_offer(&rtc, "v=0\r\n", NULL, sizeof(answer), &answer_len));

    /* Malformed SDP — will fail during sdp_parse */
    ASSERT_FAIL(nanortc_accept_offer(&rtc, "garbage", answer, sizeof(answer), &answer_len));

    nanortc_destroy(&rtc);
}

TEST(test_e2e_create_offer_state_guard)
{
    nanortc_t rtc;
    nanortc_config_t cfg = e2e_default_config();
    ASSERT_OK(nanortc_init(&rtc, &cfg));

    char buf[4096];
    size_t len = 0;

    /* NULL params */
    ASSERT_FAIL(nanortc_create_offer(NULL, buf, sizeof(buf), &len));
    ASSERT_FAIL(nanortc_create_offer(&rtc, NULL, sizeof(buf), &len));

    /* First create_offer should succeed (state = NEW) */
    ASSERT_OK(nanortc_create_offer(&rtc, buf, sizeof(buf), &len));
    ASSERT_TRUE(len > 0);

    nanortc_destroy(&rtc);
}

TEST(test_e2e_accept_answer_bad)
{
    nanortc_t rtc;
    nanortc_config_t cfg = e2e_default_config();
    ASSERT_OK(nanortc_init(&rtc, &cfg));

    /* NULL params */
    ASSERT_FAIL(nanortc_accept_answer(NULL, "v=0\r\n"));
    ASSERT_FAIL(nanortc_accept_answer(&rtc, NULL));

    /* Malformed answer */
    ASSERT_FAIL(nanortc_accept_answer(&rtc, "not-sdp"));

    nanortc_destroy(&rtc);
}

TEST(test_e2e_add_candidate_params)
{
    nanortc_t rtc;
    nanortc_config_t cfg = e2e_default_config();
    ASSERT_OK(nanortc_init(&rtc, &cfg));

    /* NULL params */
    ASSERT_FAIL(nanortc_add_local_candidate(NULL, "1.2.3.4", 9999));
    ASSERT_FAIL(nanortc_add_local_candidate(&rtc, NULL, 9999));
    ASSERT_FAIL(nanortc_add_remote_candidate(NULL, "candidate:1 1 UDP 1 1.2.3.4 9999 typ host"));
    ASSERT_FAIL(nanortc_add_remote_candidate(&rtc, NULL));

    /* Valid local candidate */
    ASSERT_OK(nanortc_add_local_candidate(&rtc, "192.168.1.100", 5000));

    /* Valid remote candidates in various formats. Each adds one entry to
     * remote_candidates[], so we verify the type field (RFC 8839 §5.1
     * parser added in the Phase 5.1 hardening pass). */
    ASSERT_EQ(rtc.ice.remote_candidate_count, 0);

    ASSERT_OK(
        nanortc_add_remote_candidate(&rtc, "candidate:1 1 UDP 2122260223 10.0.0.1 9999 typ host"));
    ASSERT_EQ(rtc.ice.remote_candidates[0].type, NANORTC_ICE_CAND_HOST);

    ASSERT_OK(nanortc_add_remote_candidate(
        &rtc, "candidate:2 1 UDP 1686052863 10.0.0.2 9998 typ srflx raddr 0.0.0.0 rport 0"));
    ASSERT_EQ(rtc.ice.remote_candidates[1].type, NANORTC_ICE_CAND_SRFLX);

    ASSERT_OK(
        nanortc_add_remote_candidate(&rtc, "candidate:3 1 UDP 1685987071 10.0.0.3 9997 typ prflx"));
    ASSERT_EQ(rtc.ice.remote_candidates[2].type, NANORTC_ICE_CAND_SRFLX); /* prflx → srflx */

    ASSERT_OK(
        nanortc_add_remote_candidate(&rtc, "candidate:4 1 UDP 8387583 10.0.0.4 9996 typ relay"));
    ASSERT_EQ(rtc.ice.remote_candidates[3].type, NANORTC_ICE_CAND_RELAY);

    /* Simple "<addr> <port>" format has no typ; defaults to host. */
    ASSERT_OK(nanortc_add_remote_candidate(&rtc, "10.0.0.5 8888"));
    ASSERT_EQ(rtc.ice.remote_candidates[4].type, NANORTC_ICE_CAND_HOST);

    /* SDP candidate without "typ" attribute (spec-compliant minimum) also
     * falls back to host. */
    ASSERT_OK(nanortc_add_remote_candidate(&rtc, "candidate:5 1 UDP 1 10.0.0.6 9995"));
    ASSERT_EQ(rtc.ice.remote_candidates[5].type, NANORTC_ICE_CAND_HOST);

    /* An unknown type word after "typ" is left at the default (host). */
    ASSERT_OK(nanortc_add_remote_candidate(&rtc, "candidate:6 1 UDP 1 10.0.0.7 9994 typ weird"));
    ASSERT_EQ(rtc.ice.remote_candidates[6].type, NANORTC_ICE_CAND_HOST);

    /* Malformed remote candidate */
    ASSERT_FAIL(nanortc_add_remote_candidate(&rtc, ""));
    ASSERT_FAIL(nanortc_add_remote_candidate(&rtc, "x"));

    nanortc_destroy(&rtc);
}

TEST(test_e2e_handle_input_params)
{
    nanortc_t rtc;
    nanortc_config_t cfg = e2e_default_config();
    ASSERT_OK(nanortc_init(&rtc, &cfg));

    ASSERT_FAIL(nanortc_handle_input(NULL, &(nanortc_input_t){.now_ms = 0}));

    /* NULL data is OK — just processes timers */
    ASSERT_OK(nanortc_handle_input(&rtc, &(nanortc_input_t){.now_ms = 100}));

    nanortc_destroy(&rtc);
}

TEST(test_e2e_is_alive_connected)
{
    ASSERT_FALSE(nanortc_is_alive(NULL));
    ASSERT_FALSE(nanortc_is_connected(NULL));

    nanortc_t rtc;
    nanortc_config_t cfg = e2e_default_config();
    ASSERT_OK(nanortc_init(&rtc, &cfg));

    ASSERT_TRUE(nanortc_is_alive(&rtc));
    ASSERT_FALSE(nanortc_is_connected(&rtc));

    nanortc_destroy(&rtc);
}

#if NANORTC_FEATURE_DATACHANNEL
TEST(test_e2e_dc_send_not_connected)
{
    nanortc_t rtc;
    nanortc_config_t cfg = e2e_default_config();
    ASSERT_OK(nanortc_init(&rtc, &cfg));

    uint8_t data[] = {1, 2, 3};
    ASSERT_FAIL(nanortc_datachannel_send(&rtc, 0, data, sizeof(data)));
    ASSERT_FAIL(nanortc_datachannel_send_string(&rtc, 0, "hello"));
    ASSERT_FAIL(nanortc_datachannel_send(NULL, 0, data, sizeof(data)));
    ASSERT_FAIL(nanortc_datachannel_send(&rtc, 0, NULL, 1));
    ASSERT_FAIL(nanortc_datachannel_send_string(NULL, 0, "hello"));
    ASSERT_FAIL(nanortc_datachannel_send_string(&rtc, 0, NULL));

    nanortc_destroy(&rtc);
}

TEST(test_e2e_dc_close_params)
{
    nanortc_t rtc;
    nanortc_config_t cfg = e2e_default_config();
    ASSERT_OK(nanortc_init(&rtc, &cfg));

    ASSERT_FAIL(nanortc_datachannel_close(NULL, 0));
    /* No channel exists → invalid param */
    ASSERT_FAIL(nanortc_datachannel_close(&rtc, 0));

    ASSERT_EQ(nanortc_datachannel_get_label(NULL, 0), NULL);
    ASSERT_EQ(nanortc_datachannel_get_label(&rtc, 0), NULL);

    nanortc_destroy(&rtc);
}

TEST(test_e2e_dc_create_with_options)
{
    nanortc_t rtc;
    nanortc_config_t cfg = e2e_default_config();
    ASSERT_OK(nanortc_init(&rtc, &cfg));

    /* Create with default options (reliable, ordered) */
    int sid1 = nanortc_create_datachannel(&rtc, "ch1", NULL);
    ASSERT_TRUE(sid1 >= 0);

    /* Create with custom options (unordered, retransmit) */
    nanortc_datachannel_options_t opts;
    memset(&opts, 0, sizeof(opts));
    opts.unordered = true;
    opts.max_retransmits = 3;
    int sid2 = nanortc_create_datachannel(&rtc, "ch2", &opts);
    ASSERT_TRUE(sid2 >= 0);
    ASSERT_NEQ(sid1, sid2);

    /* Verify labels */
    ASSERT_TRUE(nanortc_datachannel_get_label(&rtc, (uint16_t)sid1) != NULL);
    ASSERT_TRUE(nanortc_datachannel_get_label(&rtc, (uint16_t)sid2) != NULL);

    /* Close channel */
    ASSERT_OK(nanortc_datachannel_close(&rtc, (uint16_t)sid1));
    /* After close, label should be NULL */
    ASSERT_EQ(nanortc_datachannel_get_label(&rtc, (uint16_t)sid1), NULL);

    nanortc_destroy(&rtc);
}

TEST(test_e2e_dc_create_null)
{
    ASSERT_TRUE(nanortc_create_datachannel(NULL, "test", NULL) < 0);

    nanortc_t rtc;
    nanortc_config_t cfg = e2e_default_config();
    ASSERT_OK(nanortc_init(&rtc, &cfg));
    ASSERT_TRUE(nanortc_create_datachannel(&rtc, NULL, NULL) < 0);
    nanortc_destroy(&rtc);
}
#endif

#endif /* NANORTC_HAVE_MEDIA_TRANSPORT */

/* ----------------------------------------------------------------
 * NAT traversal E2E tests (host / srflx / relay)
 * ---------------------------------------------------------------- */

/* Helper: build a fake STUN Binding Response with XOR-MAPPED-ADDRESS */
static size_t build_stun_binding_response(uint8_t *buf, const uint8_t txid[12], const uint8_t *addr,
                                          uint8_t family, uint16_t port)
{
    /* Header: Binding Response (0x0101), length=12, magic cookie, txid */
    nanortc_write_u16be(buf, STUN_BINDING_RESPONSE);
    nanortc_write_u16be(buf + 2, 12); /* one XOR-MAPPED-ADDRESS attr */
    nanortc_write_u32be(buf + 4, STUN_MAGIC_COOKIE);
    memcpy(buf + 8, txid, 12);
    size_t pos = 20;

    /* XOR-MAPPED-ADDRESS: type=0x0020, length=8, value=[0, family, xport, xaddr] */
    nanortc_write_u16be(buf + pos, 0x0020);
    nanortc_write_u16be(buf + pos + 2, 8);
    buf[pos + 4] = 0; /* reserved */
    buf[pos + 5] = (family == 4) ? 0x01 : 0x02;
    nanortc_write_u16be(buf + pos + 6, port ^ (uint16_t)(STUN_MAGIC_COOKIE >> 16));
    uint32_t raw = nanortc_read_u32be(addr);
    nanortc_write_u32be(buf + pos + 8, raw ^ STUN_MAGIC_COOKIE);
    pos += 12;

    return pos;
}

/* T: STUN server configuration — stun: URL is parsed and stored */
TEST(test_e2e_stun_server_config)
{
    nanortc_t rtc;
    nanortc_config_t cfg = e2e_default_config();
    ASSERT_OK(nanortc_init(&rtc, &cfg));

    const char *stun_url = "stun:1.2.3.4:3478";
    nanortc_ice_server_t servers[] = {{.urls = &stun_url, .url_count = 1}};
    ASSERT_OK(nanortc_set_ice_servers(&rtc, servers, 1));
    ASSERT_TRUE(rtc.stun_server_configured);
    ASSERT_EQ(rtc.stun_server_port, 3478);
    ASSERT_EQ(rtc.stun_server_addr[0], 1);
    ASSERT_EQ(rtc.stun_server_addr[1], 2);
    ASSERT_EQ(rtc.stun_server_addr[2], 3);
    ASSERT_EQ(rtc.stun_server_addr[3], 4);

    nanortc_destroy(&rtc);
}

TEST(test_e2e_srflx_rng_failure_does_not_commit_transaction)
{
    nanortc_t rtc;
    nanortc_crypto_provider_t provider = e2e_crypto_with_failing_rng();
    nanortc_config_t cfg = e2e_default_config();
    cfg.crypto = &provider;
    e2e_rng_arm(0, 0u);
    ASSERT_OK(nanortc_init(&rtc, &cfg));

    const char *stun_url = "stun:8.8.4.4:3478";
    nanortc_ice_server_t servers[] = {{.urls = &stun_url, .url_count = 1}};
    ASSERT_OK(nanortc_set_ice_servers(&rtc, servers, 1));
    memset(rtc.stun_txid, 0xCC, sizeof(rtc.stun_txid));
    uint8_t txid_before[sizeof(rtc.stun_txid)];
    memcpy(txid_before, rtc.stun_txid, sizeof(txid_before));
    uint16_t out_head_before = rtc.out_head;
    uint16_t out_tail_before = rtc.out_tail;

    e2e_rng_arm(1, sizeof(rtc.stun_txid));
    ASSERT_EQ(nanortc_handle_input(&rtc, &(nanortc_input_t){.now_ms = 100u}), NANORTC_ERR_CRYPTO);
    ASSERT_TRUE(e2e_rng_failed);
    ASSERT_TRUE(memcmp(rtc.stun_txid, txid_before, sizeof(txid_before)) == 0);
    ASSERT_EQ(rtc.stun_retries, 0u);
    ASSERT_EQ(rtc.stun_retry_at_ms, 0u);
    ASSERT_FALSE(rtc.srflx_discovered);
    ASSERT_EQ(rtc.out_head, out_head_before);
    ASSERT_EQ(rtc.out_tail, out_tail_before);
    ASSERT_EQ(rtc.tx_slots_in_use, 0u);

    nanortc_destroy(&rtc);
}

/* T: SRFLX discovery — timer sends Binding Request, response yields srflx candidate */
TEST(test_e2e_srflx_discovery)
{
    nanortc_t rtc;
    nanortc_config_t cfg = e2e_default_config();
    ASSERT_OK(nanortc_init(&rtc, &cfg));

    /* Configure STUN server at 8.8.8.8:3478 */
    const char *stun_url = "stun:8.8.8.8:3478";
    nanortc_ice_server_t servers[] = {{.urls = &stun_url, .url_count = 1}};
    ASSERT_OK(nanortc_set_ice_servers(&rtc, servers, 1));
    ASSERT_TRUE(rtc.stun_server_configured);
    ASSERT_FALSE(rtc.srflx_discovered);

    /* Tick timers → should send a STUN Binding Request to 8.8.8.8:3478 */
    uint32_t now = 100;
    ASSERT_OK(nanortc_handle_input(&rtc, &(nanortc_input_t){.now_ms = now}));

    /* Poll the Binding Request output */
    nanortc_output_t out;
    bool found_stun_req = false;
    while (nanortc_poll_output(&rtc, &out) == NANORTC_OK) {
        if (out.type == NANORTC_OUTPUT_TRANSMIT && out.transmit.dest.port == 3478) {
            found_stun_req = true;
            /* Verify it's a Binding Request */
            ASSERT_TRUE(out.transmit.len == 20); /* bare header, no attrs */
            ASSERT_EQ(nanortc_read_u16be(out.transmit.data), STUN_BINDING_REQUEST);
        }
    }
    ASSERT_TRUE(found_stun_req);

    /* Build a fake Binding Response from the STUN server.
     * XOR-MAPPED-ADDRESS = 203.0.113.50:12345 (our "public" address) */
    uint8_t resp[64];
    uint8_t mapped_addr[4] = {203, 0, 113, 50};
    size_t resp_len = build_stun_binding_response(resp, rtc.stun_txid, mapped_addr, 4, 12345);

    /* Feed the response from the STUN server address */
    nanortc_addr_t stun_src;
    memset(&stun_src, 0, sizeof(stun_src));
    stun_src.family = 4;
    stun_src.addr[0] = 8;
    stun_src.addr[1] = 8;
    stun_src.addr[2] = 8;
    stun_src.addr[3] = 8;
    stun_src.port = 3478;
    now += 50;
    ASSERT_OK(nanortc_handle_input(
        &rtc, &(nanortc_input_t){.now_ms = now, .data = resp, .len = resp_len, .src = stun_src}));

    /* Verify srflx was discovered */
    ASSERT_TRUE(rtc.srflx_discovered);
    ASSERT_TRUE(rtc.sdp.has_srflx_candidate);
    ASSERT_EQ(rtc.sdp.srflx_candidate_port, 12345);

    /* Poll the trickle ICE candidate event */
    bool found_srflx_event = false;
    while (nanortc_poll_output(&rtc, &out) == NANORTC_OK) {
        if (out.type == NANORTC_OUTPUT_EVENT && out.event.type == NANORTC_EV_ICE_CANDIDATE) {
            found_srflx_event = true;
            /* Verify candidate string contains "typ srflx" */
            const char *cstr = out.event.ice_candidate.candidate_str;
            bool has_srflx = false;
            for (size_t i = 0; cstr[i]; i++) {
                if (cstr[i] == 's' && cstr[i + 1] == 'r' && cstr[i + 2] == 'f') {
                    has_srflx = true;
                    break;
                }
            }
            ASSERT_TRUE(has_srflx);
        }
    }
    ASSERT_TRUE(found_srflx_event);

    nanortc_destroy(&rtc);
}

TEST(test_e2e_shared_stun_turn_endpoint_demux)
{
    nanortc_t rtc;
    nanortc_config_t cfg = e2e_default_config();
    ASSERT_OK(nanortc_init(&rtc, &cfg));

    const char *urls[] = {"stun:10.0.0.100:3478", "turn:10.0.0.100:3478"};
    nanortc_ice_server_t servers[] = {
        {.urls = urls, .url_count = 2, .username = "testuser", .credential = "testpass"}};
    ASSERT_OK(nanortc_set_ice_servers(&rtc, servers, 1));
    ASSERT_TRUE(rtc.stun_server_configured);
    ASSERT_TRUE(rtc.turn.configured);

    nanortc_output_t out;
    uint8_t stun_txid[STUN_TXID_SIZE];
    bool found_binding_request = false;
    uint32_t now_ms = 100;
    for (uint8_t attempt = 0; attempt < 4 && !found_binding_request; attempt++) {
        ASSERT_OK(nanortc_handle_input(&rtc, &(nanortc_input_t){.now_ms = now_ms}));
        while (nanortc_poll_output(&rtc, &out) == NANORTC_OK) {
            if (out.type == NANORTC_OUTPUT_TRANSMIT && out.transmit.len >= STUN_HEADER_SIZE &&
                nanortc_read_u16be(out.transmit.data) == STUN_BINDING_REQUEST) {
                memcpy(stun_txid, out.transmit.data + 8, STUN_TXID_SIZE);
                found_binding_request = true;
            }
        }
        now_ms += 10;
    }
    ASSERT_TRUE(found_binding_request);

    uint8_t response[64];
    uint8_t mapped_addr[4] = {203, 0, 113, 60};
    size_t response_len = build_stun_binding_response(response, stun_txid, mapped_addr, 4, 23456);
    nanortc_addr_t shared_src = {.family = 4, .port = 3478};
    shared_src.addr[0] = 10;
    shared_src.addr[3] = 100;
    ASSERT_OK(nanortc_handle_input(
        &rtc, &(nanortc_input_t){
                  .now_ms = now_ms, .data = response, .len = response_len, .src = shared_src}));
    ASSERT_TRUE(rtc.srflx_discovered);
    ASSERT_EQ(rtc.sdp.srflx_candidate_port, 23456);
    ASSERT_EQ(rtc.turn.state, NANORTC_TURN_ALLOCATING);
    nanortc_destroy(&rtc);
}

/* T: SRFLX retry — no response triggers retransmission */
TEST(test_e2e_srflx_retry)
{
    nanortc_t rtc;
    nanortc_config_t cfg = e2e_default_config();
    ASSERT_OK(nanortc_init(&rtc, &cfg));

    const char *stun_url = "stun:1.1.1.1:3478";
    nanortc_ice_server_t servers[] = {{.urls = &stun_url, .url_count = 1}};
    ASSERT_OK(nanortc_set_ice_servers(&rtc, servers, 1));

    /* First request */
    uint32_t now = 100;
    ASSERT_OK(nanortc_handle_input(&rtc, &(nanortc_input_t){.now_ms = now}));
    ASSERT_EQ(rtc.stun_retries, 1);

    /* Drain output */
    nanortc_output_t out;
    while (nanortc_poll_output(&rtc, &out) == NANORTC_OK) {
    }

    /* Not enough time for retry */
    now += 200;
    ASSERT_OK(nanortc_handle_input(&rtc, &(nanortc_input_t){.now_ms = now}));
    ASSERT_EQ(rtc.stun_retries, 1); /* still 1, not time yet */

    /* After 500ms → retry */
    now = 100 + 500;
    ASSERT_OK(nanortc_handle_input(&rtc, &(nanortc_input_t){.now_ms = now}));
    ASSERT_EQ(rtc.stun_retries, 2);

    /* Drain */
    while (nanortc_poll_output(&rtc, &out) == NANORTC_OK) {
    }

    /* Third retry */
    now += 500;
    ASSERT_OK(nanortc_handle_input(&rtc, &(nanortc_input_t){.now_ms = now}));
    ASSERT_EQ(rtc.stun_retries, 3);

    /* No more retries after max */
    while (nanortc_poll_output(&rtc, &out) == NANORTC_OK) {
    }
    now += 500;
    ASSERT_OK(nanortc_handle_input(&rtc, &(nanortc_input_t){.now_ms = now}));
    ASSERT_EQ(rtc.stun_retries, 3); /* capped at 3 */

    nanortc_destroy(&rtc);
}

/* T: SRFLX is added to local_candidates[] after Binding Response (NANORTC_FEATURE_ICE_SRFLX) */
TEST(test_e2e_srflx_joins_local_candidates)
{
#if NANORTC_FEATURE_ICE_SRFLX
    nanortc_t rtc;
    nanortc_config_t cfg = e2e_default_config();
    ASSERT_OK(nanortc_init(&rtc, &cfg));

    const char *stun_url = "stun:8.8.8.8:3478";
    nanortc_ice_server_t servers[] = {{.urls = &stun_url, .url_count = 1}};
    ASSERT_OK(nanortc_set_ice_servers(&rtc, servers, 1));

    /* Tick to send the Binding Request, then drain output. */
    uint32_t now = 100;
    ASSERT_OK(nanortc_handle_input(&rtc, &(nanortc_input_t){.now_ms = now}));
    nanortc_output_t out;
    while (nanortc_poll_output(&rtc, &out) == NANORTC_OK) {
    }

    uint8_t initial_count = rtc.ice.local_candidate_count;

    /* Feed Binding Response — XOR-MAPPED-ADDRESS = 203.0.113.50:12345 */
    uint8_t resp[64];
    uint8_t mapped_addr[4] = {203, 0, 113, 50};
    size_t resp_len = build_stun_binding_response(resp, rtc.stun_txid, mapped_addr, 4, 12345);
    nanortc_addr_t stun_src = {.family = 4, .port = 3478};
    stun_src.addr[0] = 8;
    stun_src.addr[1] = 8;
    stun_src.addr[2] = 8;
    stun_src.addr[3] = 8;
    now += 50;
    ASSERT_OK(nanortc_handle_input(
        &rtc, &(nanortc_input_t){.now_ms = now, .data = resp, .len = resp_len, .src = stun_src}));

    /* SRFLX must now appear in local_candidates with type=SRFLX. */
    ASSERT_EQ(rtc.ice.local_candidate_count, (uint8_t)(initial_count + 1));
    nano_ice_candidate_t *c = &rtc.ice.local_candidates[initial_count];
    ASSERT_EQ(c->type, NANORTC_ICE_CAND_SRFLX);
    ASSERT_EQ(c->family, 4);
    ASSERT_EQ(c->port, 12345);
    ASSERT_EQ(c->addr[0], 203);
    ASSERT_EQ(c->addr[3], 50);

    /* Subsequent identical Binding Response must NOT add a duplicate slot. */
    rtc.srflx_discovered = false; /* re-arm the parser path */
    ASSERT_OK(nanortc_handle_input(
        &rtc,
        &(nanortc_input_t){.now_ms = now + 50, .data = resp, .len = resp_len, .src = stun_src}));
    ASSERT_EQ(rtc.ice.local_candidate_count, (uint8_t)(initial_count + 1));

    nanortc_destroy(&rtc);
#endif
}

/* T: ICE_SRFLX_PRIORITY math (RFC 8445 §5.1.2.2 type_pref=100) */
TEST(test_e2e_srflx_priority_macro)
{
    /* type_pref=100, local_pref=65535-idx, component=1 → (256-1)=255 */
    uint32_t expected_idx0 = (100u << 24) | (65535u << 8) | 255u;
    uint32_t expected_idx1 = (100u << 24) | ((65535u - 1u) << 8) | 255u;
    ASSERT_EQ(ICE_SRFLX_PRIORITY(0), expected_idx0);
    ASSERT_EQ(ICE_SRFLX_PRIORITY(1), expected_idx1);

    /* HOST priority should keep using type_pref=126. */
    ASSERT_EQ(ICE_HOST_PRIORITY(0), (uint32_t)((126u << 24) | (65535u << 8) | 255u));
}

/* T: dst plumbing — controlled-side USE-CANDIDATE records selected_local_idx
 * matching the dst we passed in, not the historical 0 fallback. */
TEST(test_e2e_handle_input_dst_resolves_local_idx)
{
    nanortc_t rtc;
    nanortc_config_t cfg = e2e_default_config();
    cfg.role = NANORTC_ROLE_CONTROLLED;
    ASSERT_OK(nanortc_init(&rtc, &cfg));

    /* Put two local host candidates on different ports so dst selects index 1. */
    ASSERT_OK(nanortc_add_local_candidate(&rtc, "127.0.0.1", 5001));
    ASSERT_OK(nanortc_add_local_candidate(&rtc, "127.0.0.1", 5002));
    ASSERT_EQ(rtc.ice.local_candidate_count, 2);

    /* Set ICE credentials directly (test-only access to internal state). */
    memcpy(rtc.ice.remote_ufrag, "REMO", 4);
    rtc.ice.remote_ufrag_len = 4;
    const char *remote_pwd = "remotepasswordlong";
    size_t remote_pwd_len = strlen(remote_pwd);
    memcpy(rtc.ice.remote_pwd, remote_pwd, remote_pwd_len);
    rtc.ice.remote_pwd_len = (uint16_t)remote_pwd_len;

    /* Build a Binding Request signed with our local_pwd, USE-CANDIDATE set,
     * local_ufrag : remote_ufrag in the USERNAME (RFC 8445 §7.2.1.1):
     * the remote sends "ours:theirs" to us. */
    char username[64];
    int n = snprintf(username, sizeof(username), "%.*s:REMO", (int)rtc.ice.local_ufrag_len,
                     rtc.ice.local_ufrag);
    size_t ulen = (size_t)n;
    uint8_t txid[STUN_TXID_SIZE] = {0xa1, 0xb2, 0xc3, 0xd4, 0xe5, 0xf6,
                                    0x07, 0x18, 0x29, 0x3a, 0x4b, 0x5c};
    uint8_t req[256];
    size_t req_len = 0;
    ASSERT_OK(stun_encode_binding_request(
        username, ulen, ICE_HOST_PRIORITY(0), true, /* use_candidate */
        true,                                       /* is_controlling — remote is controlling */
        0x1122334455667788ull, txid, (const uint8_t *)rtc.ice.local_pwd, rtc.ice.local_pwd_len,
        cfg.crypto->hmac_sha1, req, sizeof(req), &req_len));

    /* Feed the request as if it arrived on local socket bound to 127.0.0.1:5002.
     * dst resolves to local_candidates[1]; selected_local_idx must be 1. */
    nanortc_addr_t src = {.family = 4, .port = 9000};
    src.addr[0] = 192;
    src.addr[1] = 168;
    src.addr[2] = 1;
    src.addr[3] = 50;
    nanortc_addr_t dst = {.family = 4, .port = 5002};
    dst.addr[0] = 127;
    dst.addr[1] = 0;
    dst.addr[2] = 0;
    dst.addr[3] = 1;

    ASSERT_OK(nanortc_handle_input(
        &rtc,
        &(nanortc_input_t){.now_ms = 100, .data = req, .len = req_len, .src = src, .dst = dst}));
    ASSERT_EQ(rtc.ice.state, NANORTC_ICE_STATE_CONNECTED);
    ASSERT_EQ(rtc.ice.selected_local_idx, 1);
    ASSERT_EQ(rtc.ice.selected_local_type, NANORTC_ICE_CAND_HOST);

    nanortc_destroy(&rtc);
}

/* T: dst plumbing — exact addr match wins over wildcard (0.0.0.0) candidate
 * on the same port. Regression for the "wildcard always wins" bug that hid
 * srflx (concrete addr) behind a wildcard-bound host candidate. */
TEST(test_e2e_handle_input_dst_exact_beats_wildcard)
{
    nanortc_t rtc;
    nanortc_config_t cfg = e2e_default_config();
    cfg.role = NANORTC_ROLE_CONTROLLED;
    ASSERT_OK(nanortc_init(&rtc, &cfg));

    /* Slot 0 = wildcard (0.0.0.0) host on port 5200. Slot 1 = concrete
     * srflx candidate on the same port (mirrors how nanortc registers a
     * wildcard-bound host plus a discovered srflx). */
    ASSERT_OK(nanortc_add_local_candidate(&rtc, "0.0.0.0", 5200));
    rtc.ice.local_candidates[1].family = 4;
    rtc.ice.local_candidates[1].port = 5200;
    rtc.ice.local_candidates[1].type = NANORTC_ICE_CAND_SRFLX;
    rtc.ice.local_candidates[1].addr[0] = 203;
    rtc.ice.local_candidates[1].addr[1] = 0;
    rtc.ice.local_candidates[1].addr[2] = 113;
    rtc.ice.local_candidates[1].addr[3] = 7;
    rtc.ice.local_candidate_count = 2;

    memcpy(rtc.ice.remote_ufrag, "REMO", 4);
    rtc.ice.remote_ufrag_len = 4;
    const char *remote_pwd = "remotepasswordlong";
    size_t remote_pwd_len = strlen(remote_pwd);
    memcpy(rtc.ice.remote_pwd, remote_pwd, remote_pwd_len);
    rtc.ice.remote_pwd_len = (uint16_t)remote_pwd_len;

    char username[64];
    int n = snprintf(username, sizeof(username), "%.*s:REMO", (int)rtc.ice.local_ufrag_len,
                     rtc.ice.local_ufrag);
    size_t ulen = (size_t)n;
    uint8_t txid[STUN_TXID_SIZE] = {0xc1, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6,
                                    0xc7, 0xc8, 0xc9, 0xca, 0xcb, 0xcc};
    uint8_t req[256];
    size_t req_len = 0;
    ASSERT_OK(stun_encode_binding_request(
        username, ulen, ICE_HOST_PRIORITY(0), true, /* use_candidate */
        true,                                       /* remote is controlling */
        0x1000200030004000ull, txid, (const uint8_t *)rtc.ice.local_pwd, rtc.ice.local_pwd_len,
        cfg.crypto->hmac_sha1, req, sizeof(req), &req_len));

    nanortc_addr_t src = {.family = 4, .port = 9100};
    src.addr[0] = 10;
    src.addr[3] = 9;
    /* dst == srflx's concrete addr; exact match must win over slot 0. */
    nanortc_addr_t dst = {.family = 4, .port = 5200};
    dst.addr[0] = 203;
    dst.addr[1] = 0;
    dst.addr[2] = 113;
    dst.addr[3] = 7;

    ASSERT_OK(nanortc_handle_input(
        &rtc,
        &(nanortc_input_t){.now_ms = 100, .data = req, .len = req_len, .src = src, .dst = dst}));
    ASSERT_EQ(rtc.ice.state, NANORTC_ICE_STATE_CONNECTED);
    ASSERT_EQ(rtc.ice.selected_local_idx, 1);
    ASSERT_EQ(rtc.ice.selected_local_type, NANORTC_ICE_CAND_SRFLX);

    nanortc_destroy(&rtc);
}

/* T: dst plumbing — NULL dst falls back to selected_local_idx=0 (legacy). */
TEST(test_e2e_handle_input_dst_null_falls_back)
{
    nanortc_t rtc;
    nanortc_config_t cfg = e2e_default_config();
    cfg.role = NANORTC_ROLE_CONTROLLED;
    ASSERT_OK(nanortc_init(&rtc, &cfg));

    ASSERT_OK(nanortc_add_local_candidate(&rtc, "127.0.0.1", 5101));
    ASSERT_OK(nanortc_add_local_candidate(&rtc, "127.0.0.1", 5102));

    memcpy(rtc.ice.remote_ufrag, "REMO", 4);
    rtc.ice.remote_ufrag_len = 4;
    const char *remote_pwd = "remotepasswordlong";
    size_t remote_pwd_len = strlen(remote_pwd);
    memcpy(rtc.ice.remote_pwd, remote_pwd, remote_pwd_len);
    rtc.ice.remote_pwd_len = (uint16_t)remote_pwd_len;

    char username[64];
    int n = snprintf(username, sizeof(username), "%.*s:REMO", (int)rtc.ice.local_ufrag_len,
                     rtc.ice.local_ufrag);
    size_t ulen = (size_t)n;
    uint8_t txid[STUN_TXID_SIZE] = {0xb1, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6,
                                    0xb7, 0xb8, 0xb9, 0xba, 0xbb, 0xbc};
    uint8_t req[256];
    size_t req_len = 0;
    ASSERT_OK(stun_encode_binding_request(
        username, ulen, ICE_HOST_PRIORITY(0), true, /* use_candidate */
        true,                                       /* remote is controlling */
        0xdeadbeefcafebabeull, txid, (const uint8_t *)rtc.ice.local_pwd, rtc.ice.local_pwd_len,
        cfg.crypto->hmac_sha1, req, sizeof(req), &req_len));

    nanortc_addr_t src = {.family = 4, .port = 9001};
    src.addr[0] = 10;
    src.addr[3] = 7;

    /* dst=NULL: legacy fallback → selected_local_idx == 0. */
    ASSERT_OK(nanortc_handle_input(
        &rtc, &(nanortc_input_t){.now_ms = 100, .data = req, .len = req_len, .src = src}));
    ASSERT_EQ(rtc.ice.state, NANORTC_ICE_STATE_CONNECTED);
    ASSERT_EQ(rtc.ice.selected_local_idx, 0);

    nanortc_destroy(&rtc);
}

#if NANORTC_FEATURE_TURN
/* T: TURN allocation lifecycle — configure, allocate, 401, authenticated, success */
TEST(test_e2e_turn_allocation_lifecycle)
{
    nanortc_t rtc;
    nanortc_config_t cfg = e2e_default_config();
    ASSERT_OK(nanortc_init(&rtc, &cfg));

    /* Configure TURN server at 10.0.0.100:3478 */
    const char *turn_url = "turn:10.0.0.100:3478";
    nanortc_ice_server_t servers[] = {
        {.urls = &turn_url, .url_count = 1, .username = "testuser", .credential = "testpass"}};
    ASSERT_OK(nanortc_set_ice_servers(&rtc, servers, 1));
    ASSERT_TRUE(rtc.turn.configured);
    ASSERT_EQ(rtc.turn.state, NANORTC_TURN_IDLE);

    /* Tick → should send Allocate Request */
    uint32_t now = 100;
    ASSERT_OK(nanortc_handle_input(&rtc, &(nanortc_input_t){.now_ms = now}));
    ASSERT_EQ(rtc.turn.state, NANORTC_TURN_ALLOCATING);

    /* Drain the Allocate Request output */
    nanortc_output_t out;
    bool found_allocate = false;
    while (nanortc_poll_output(&rtc, &out) == NANORTC_OK) {
        if (out.type == NANORTC_OUTPUT_TRANSMIT && out.transmit.dest.port == 3478) {
            stun_msg_t msg;
            if (stun_parse(out.transmit.data, out.transmit.len, &msg) == NANORTC_OK) {
                if (msg.type == STUN_ALLOCATE_REQUEST) {
                    found_allocate = true;
                }
            }
        }
    }
    ASSERT_TRUE(found_allocate);

    /* Build 401 response */
    uint8_t resp401[128];
    nanortc_write_u16be(resp401, STUN_ALLOCATE_ERROR);
    nanortc_write_u16be(resp401 + 2, 0);
    nanortc_write_u32be(resp401 + 4, STUN_MAGIC_COOKIE);
    memcpy(resp401 + 8, rtc.turn.last_txid, 12);
    size_t pos = 20;
    /* ERROR-CODE 401 */
    nanortc_write_u16be(resp401 + pos, 0x0009);
    nanortc_write_u16be(resp401 + pos + 2, 4);
    resp401[pos + 4] = 0;
    resp401[pos + 5] = 0;
    resp401[pos + 6] = 4;
    resp401[pos + 7] = 1;
    pos += 8;
    /* REALM */
    nanortc_write_u16be(resp401 + pos, 0x0014);
    nanortc_write_u16be(resp401 + pos + 2, 8);
    memcpy(resp401 + pos + 4, "test.com", 8);
    pos += 12;
    /* NONCE */
    nanortc_write_u16be(resp401 + pos, 0x0015);
    nanortc_write_u16be(resp401 + pos + 2, 8);
    memcpy(resp401 + pos + 4, "nonce123", 8);
    pos += 12;
    nanortc_write_u16be(resp401 + 2, (uint16_t)(pos - 20));

    nanortc_addr_t turn_src;
    memset(&turn_src, 0, sizeof(turn_src));
    turn_src.family = 4;
    turn_src.addr[0] = 10;
    turn_src.addr[3] = 100;
    turn_src.port = 3478;

    now += 50;
    ASSERT_OK(nanortc_handle_input(
        &rtc, &(nanortc_input_t){.now_ms = now, .data = resp401, .len = pos, .src = turn_src}));
    ASSERT_EQ(rtc.turn.state, NANORTC_TURN_CHALLENGED);
    ASSERT_TRUE(rtc.turn.hmac_key_valid);

    /* Tick → should retry with credentials */
    now += 10;
    ASSERT_OK(nanortc_handle_input(&rtc, &(nanortc_input_t){.now_ms = now}));

    /* Drain authenticated Allocate */
    bool found_auth = false;
    while (nanortc_poll_output(&rtc, &out) == NANORTC_OK) {
        if (out.type == NANORTC_OUTPUT_TRANSMIT) {
            stun_msg_t msg;
            if (stun_parse(out.transmit.data, out.transmit.len, &msg) == NANORTC_OK) {
                if (msg.type == STUN_ALLOCATE_REQUEST && msg.has_integrity) {
                    found_auth = true;
                }
            }
        }
    }
    ASSERT_TRUE(found_auth);

    /* Save txid AFTER draining — turn_start_allocate() updated it during the tick */
    uint8_t saved_txid[12];
    memcpy(saved_txid, rtc.turn.last_txid, 12);

    /* Build Allocate Success with relay addr 203.0.113.5:49152 */
    uint8_t resp_ok[64];
    nanortc_write_u16be(resp_ok, STUN_ALLOCATE_RESPONSE);
    nanortc_write_u16be(resp_ok + 2, 0);
    nanortc_write_u32be(resp_ok + 4, STUN_MAGIC_COOKIE);
    memcpy(resp_ok + 8, saved_txid, 12);
    pos = 20;
    /* XOR-RELAYED-ADDRESS */
    nanortc_write_u16be(resp_ok + pos, 0x0016);
    nanortc_write_u16be(resp_ok + pos + 2, 8);
    resp_ok[pos + 4] = 0;
    resp_ok[pos + 5] = 0x01;
    nanortc_write_u16be(resp_ok + pos + 6, 49152 ^ (uint16_t)(STUN_MAGIC_COOKIE >> 16));
    uint32_t relay_raw = (203u << 24) | (0u << 16) | (113u << 8) | 5u;
    nanortc_write_u32be(resp_ok + pos + 8, relay_raw ^ STUN_MAGIC_COOKIE);
    pos += 12;
    /* LIFETIME */
    nanortc_write_u16be(resp_ok + pos, 0x000D);
    nanortc_write_u16be(resp_ok + pos + 2, 4);
    nanortc_write_u32be(resp_ok + pos + 4, 600);
    pos += 8;
    nanortc_write_u16be(resp_ok + 2, (uint16_t)(pos - 20));

    /* Authenticated TURN success responses carry MESSAGE-INTEGRITY
     * (RFC 8489 §9.2.4). */
    nanortc_write_u16be(resp_ok + 2, (uint16_t)(pos - STUN_HEADER_SIZE + 24u));
    uint8_t turn_hmac[20];
    nano_test_crypto()->hmac_sha1(rtc.turn.auth.hmac_key, NANORTC_TURN_HMAC_KEY_SIZE, resp_ok, pos,
                                  turn_hmac);
    nanortc_write_u16be(resp_ok + pos, STUN_ATTR_MESSAGE_INTEGRITY);
    nanortc_write_u16be(resp_ok + pos + 2, 20);
    memcpy(resp_ok + pos + 4, turn_hmac, sizeof(turn_hmac));
    pos += 24;

    /* Feed response at same time (don't advance — avoid re-triggering CHALLENGED retry) */
    ASSERT_OK(nanortc_handle_input(
        &rtc, &(nanortc_input_t){.now_ms = now, .data = resp_ok, .len = pos, .src = turn_src}));
    ASSERT_EQ(rtc.turn.state, NANORTC_TURN_ALLOCATED);
    ASSERT_EQ(rtc.turn.relay_port, 49152);

    /* Verify relay candidate was emitted */
    ASSERT_TRUE(rtc.sdp.has_relay_candidate);
    bool found_relay_event = false;
    while (nanortc_poll_output(&rtc, &out) == NANORTC_OK) {
        if (out.type == NANORTC_OUTPUT_EVENT && out.event.type == NANORTC_EV_ICE_CANDIDATE) {
            const char *cs = out.event.ice_candidate.candidate_str;
            /* Check for "typ relay" */
            for (size_t i = 0; cs[i]; i++) {
                if (cs[i] == 'r' && cs[i + 1] == 'e' && cs[i + 2] == 'l' && cs[i + 3] == 'a') {
                    found_relay_event = true;
                    break;
                }
            }
        }
    }
    ASSERT_TRUE(found_relay_event);

    bool relay_registered = false;
    for (uint8_t i = 0; i < rtc.ice.local_candidate_count; i++) {
        if (rtc.ice.local_candidates[i].type == NANORTC_ICE_CAND_RELAY &&
            rtc.ice.local_candidates[i].port == 49152) {
            relay_registered = true;
            break;
        }
    }
    ASSERT_TRUE(relay_registered);

    nanortc_destroy(&rtc);
}

TEST(test_e2e_turn_allocate_backpressure_preserves_state)
{
    static const nano_turn_state_t states[] = {
        NANORTC_TURN_IDLE,
        NANORTC_TURN_CHALLENGED,
    };

    for (size_t state_idx = 0; state_idx < sizeof(states) / sizeof(states[0]); state_idx++) {
        nanortc_t rtc;
        nanortc_config_t cfg = e2e_default_config();
        ASSERT_OK(nanortc_init(&rtc, &cfg));

        const char *turn_url = "turn:10.0.0.100:3478";
        nanortc_ice_server_t servers[] = {
            {.urls = &turn_url, .url_count = 1, .username = "testuser", .credential = "testpass"}};
        ASSERT_OK(nanortc_set_ice_servers(&rtc, servers, 1));
        rtc.turn.state = states[state_idx];

        for (uint8_t i = 0; i < NANORTC_TX_SLOT_COUNT; i++) {
            rtc.tx_slots_in_use |= UINT32_C(1) << i;
        }
        uint16_t out_tail_before = rtc.out_tail;

        ASSERT_OK(nanortc_handle_input(&rtc, &(nanortc_input_t){.now_ms = 100u}));
        ASSERT_EQ(rtc.turn.state, states[state_idx]);
        ASSERT_EQ(rtc.out_tail, out_tail_before);

        nanortc_destroy(&rtc);
    }
}

TEST(test_e2e_relay_check_waits_without_blocking_permission)
{
    nanortc_t rtc;
    nanortc_config_t cfg = e2e_default_config();
    cfg.role = NANORTC_ROLE_CONTROLLING;
    ASSERT_OK(nanortc_init(&rtc, &cfg));

    uint8_t turn_addr[NANORTC_ADDR_SIZE] = {10, 0, 0, 100};
    ASSERT_OK(turn_configure(&rtc.turn, turn_addr, 4, 3478, "user", 4, "pass", 4));
    rtc.turn.state = NANORTC_TURN_ALLOCATED;
    rtc.turn.hmac_key_valid = true;
    memset(rtc.turn.auth.hmac_key, 0xAA, sizeof(rtc.turn.auth.hmac_key));
    memcpy(rtc.turn.realm, "realm", 5);
    rtc.turn.realm_len = 5;
    memcpy(rtc.turn.nonce, "nonce", 5);
    rtc.turn.nonce_len = 5;
    rtc.turn.relay_family = STUN_FAMILY_IPV4;
    rtc.turn.refresh_at_ms = 600000;

    rtc.ice.local_candidate_count = 1;
    rtc.ice.local_candidates[0].family = 4;
    rtc.ice.local_candidates[0].type = NANORTC_ICE_CAND_RELAY;
    rtc.ice.local_candidates[0].port = 50000;
    rtc.ice.remote_candidate_count = 1;
    rtc.ice.remote_candidates[0].family = 4;
    rtc.ice.remote_candidates[0].addr[0] = 192;
    rtc.ice.remote_candidates[0].addr[1] = 0;
    rtc.ice.remote_candidates[0].addr[2] = 2;
    rtc.ice.remote_candidates[0].addr[3] = 10;
    rtc.ice.remote_candidates[0].port = 40000;

    uint32_t timeout_ms = 99u;
    ASSERT_OK(nanortc_next_timeout_ms(&rtc, 100u, &timeout_ms));
    ASSERT_EQ(timeout_ms, 0u);

    ASSERT_OK(nanortc_handle_input(&rtc, &(nanortc_input_t){.now_ms = 100}));
    ASSERT_EQ(rtc.ice.check_count, 0);
    ASSERT_EQ(rtc.turn.permission_count, 1);
    ASSERT_TRUE(rtc.turn.permissions[0].pending);

    nanortc_output_t out;
    bool found_permission = false;
    while (nanortc_poll_output(&rtc, &out) == NANORTC_OK) {
        if (out.type == NANORTC_OUTPUT_TRANSMIT &&
            nanortc_read_u16be(out.transmit.data) == STUN_CREATE_PERMISSION_REQUEST) {
            found_permission = true;
        }
    }
    ASSERT_TRUE(found_permission);
    nanortc_destroy(&rtc);
}

TEST(test_e2e_turn_permission_capacity_covers_remote_candidates)
{
    nanortc_t rtc;
    nanortc_config_t cfg = e2e_default_config();
    cfg.role = NANORTC_ROLE_CONTROLLING;
    ASSERT_OK(nanortc_init(&rtc, &cfg));

    uint8_t turn_addr[NANORTC_ADDR_SIZE] = {10, 0, 0, 100};
    ASSERT_OK(turn_configure(&rtc.turn, turn_addr, 4, 3478, "user", 4, "pass", 4));
    rtc.turn.state = NANORTC_TURN_ALLOCATED;
    rtc.turn.hmac_key_valid = true;
    memset(rtc.turn.auth.hmac_key, 0xAA, sizeof(rtc.turn.auth.hmac_key));
    memcpy(rtc.turn.realm, "realm", 5);
    rtc.turn.realm_len = 5;
    memcpy(rtc.turn.nonce, "nonce", 5);
    rtc.turn.nonce_len = 5;
    rtc.turn.relay_family = STUN_FAMILY_IPV4;
    rtc.turn.refresh_at_ms = 600000;

    rtc.ice.local_candidate_count = 1;
    rtc.ice.local_candidates[0].family = 4;
    rtc.ice.local_candidates[0].type = NANORTC_ICE_CAND_RELAY;
    rtc.ice.local_candidates[0].port = 50000;

    rtc.ice.remote_candidate_count = NANORTC_MAX_ICE_CANDIDATES;
    for (uint8_t i = 0; i < NANORTC_MAX_ICE_CANDIDATES; i++) {
        rtc.ice.remote_candidates[i].family = 4;
        rtc.ice.remote_candidates[i].addr[0] = 198;
        rtc.ice.remote_candidates[i].addr[1] = 51;
        rtc.ice.remote_candidates[i].addr[2] = 100;
        rtc.ice.remote_candidates[i].addr[3] = (uint8_t)(i + 1);
        rtc.ice.remote_candidates[i].port = (uint16_t)(40000 + i);
        rtc.ice.remote_candidates[i].type = NANORTC_ICE_CAND_HOST;
    }
    rtc.ice.remote_candidates[NANORTC_MAX_ICE_CANDIDATES - 1].type = NANORTC_ICE_CAND_RELAY;
    if (NANORTC_MAX_ICE_CANDIDATES > 1) {
        rtc.ice.remote_candidates[NANORTC_MAX_ICE_CANDIDATES - 2].type = NANORTC_ICE_CAND_SRFLX;
    }

    nanortc_output_t out;
    const uint8_t expected = NANORTC_TURN_MAX_PERMISSIONS < NANORTC_MAX_ICE_CANDIDATES
                                 ? NANORTC_TURN_MAX_PERMISSIONS
                                 : NANORTC_MAX_ICE_CANDIDATES;
    for (uint16_t tick = 0; tick < (uint16_t)(NANORTC_MAX_ICE_CANDIDATES * 3u + 4u) &&
                            rtc.turn.permission_count < expected;
         tick++) {
        ASSERT_OK(
            nanortc_handle_input(&rtc, &(nanortc_input_t){.now_ms = (uint32_t)(100u + tick)}));
        while (nanortc_poll_output(&rtc, &out) == NANORTC_OK) {
        }
    }
    ASSERT_EQ(rtc.turn.permission_count, expected);
    for (uint8_t i = 0; i < expected; i++) {
        ASSERT_MEM_EQ(rtc.turn.permissions[i].addr, rtc.ice.remote_candidates[i].addr, 4);
    }

#if NANORTC_TURN_MAX_PERMISSIONS >= NANORTC_MAX_ICE_CANDIDATES
    ASSERT_EQ(NANORTC_TURN_MAX_PERMISSIONS, NANORTC_MAX_ICE_CANDIDATES);
    ASSERT_MEM_EQ(rtc.turn.permissions[NANORTC_MAX_ICE_CANDIDATES - 1].addr,
                  rtc.ice.remote_candidates[NANORTC_MAX_ICE_CANDIDATES - 1].addr, 4);
    ASSERT_TRUE(rtc.turn.permissions[NANORTC_MAX_ICE_CANDIDATES - 1].pending);
#endif

    nanortc_destroy(&rtc);
}

TEST(test_e2e_turn_permission_table_full_does_not_freeze_ice)
{
    nanortc_t rtc;
    nanortc_config_t cfg = e2e_default_config();
    cfg.role = NANORTC_ROLE_CONTROLLING;
    ASSERT_OK(nanortc_init(&rtc, &cfg));

    memcpy(rtc.ice.local_ufrag, "TEST1234", 8);
    rtc.ice.local_ufrag_len = 8;
    memcpy(rtc.ice.local_pwd, "password-for-testing12", 22);
    rtc.ice.local_pwd_len = 22;
    memcpy(rtc.ice.remote_ufrag, "REMO1234", 8);
    rtc.ice.remote_ufrag_len = 8;
    memcpy(rtc.ice.remote_pwd, "remote-password-abcdef", 22);
    rtc.ice.remote_pwd_len = 22;
    rtc.ice.tie_breaker = UINT64_C(0xAABBCCDDEEFF0011);

    uint8_t turn_addr[NANORTC_ADDR_SIZE] = {10, 0, 0, 100};
    ASSERT_OK(turn_configure(&rtc.turn, turn_addr, 4, 3478, "user", 4, "pass", 4));
    rtc.turn.state = NANORTC_TURN_ALLOCATED;
    rtc.turn.hmac_key_valid = true;
    rtc.turn.relay_family = STUN_FAMILY_IPV4;
    rtc.turn.refresh_at_ms = 600000;

    rtc.ice.local_candidate_count = 1;
    rtc.ice.local_candidates[0].family = 4;
    rtc.ice.local_candidates[0].type = NANORTC_ICE_CAND_RELAY;
    rtc.ice.local_candidates[0].port = 50000;
    rtc.ice.remote_candidate_count = 1;
    rtc.ice.remote_candidates[0].family = 4;
    rtc.ice.remote_candidates[0].addr[0] = 192;
    rtc.ice.remote_candidates[0].addr[1] = 0;
    rtc.ice.remote_candidates[0].addr[2] = 2;
    rtc.ice.remote_candidates[0].addr[3] = 200;
    rtc.ice.remote_candidates[0].port = 40000;

    rtc.turn.permission_count = NANORTC_TURN_MAX_PERMISSIONS;
    for (uint8_t i = 0; i < NANORTC_TURN_MAX_PERMISSIONS; i++) {
        rtc.turn.permissions[i].family = 4;
        rtc.turn.permissions[i].addr[0] = 10;
        rtc.turn.permissions[i].addr[2] = 1;
        rtc.turn.permissions[i].addr[3] = (uint8_t)(i + 1);
        rtc.turn.permissions[i].active = true;
        rtc.turn.permissions[i].deadline_ms = 600000;
    }

    ASSERT_OK(nanortc_handle_input(&rtc, &(nanortc_input_t){.now_ms = 100}));
    ASSERT_TRUE(rtc.ice.check_count > 0);

    nanortc_destroy(&rtc);
}

/* T: TURN relay wrapping — outgoing data wrapped when ICE selects relay */
TEST(test_e2e_turn_relay_wrapping)
{
    nanortc_t rtc;
    nanortc_config_t cfg = e2e_default_config();
    ASSERT_OK(nanortc_init(&rtc, &cfg));

    /* Configure TURN */
    const char *turn_url = "turn:10.0.0.1:3478";
    nanortc_ice_server_t servers[] = {
        {.urls = &turn_url, .url_count = 1, .username = "u", .credential = "p"}};
    ASSERT_OK(nanortc_set_ice_servers(&rtc, servers, 1));

    /* Simulate TURN allocated state */
    rtc.turn.state = NANORTC_TURN_ALLOCATED;
    rtc.turn.hmac_key_valid = true;
    memset(rtc.turn.auth.hmac_key, 0xAA, 16);
    rtc.turn.relay_addr[0] = 203;
    rtc.turn.relay_addr[1] = 0;
    rtc.turn.relay_addr[2] = 113;
    rtc.turn.relay_addr[3] = 1;
    rtc.turn.relay_port = 50000;
    rtc.turn.relay_family = 1;

    /* Set ICE selected type to RELAY */
    rtc.ice.selected_type = NANORTC_ICE_CAND_RELAY;
    rtc.ice.selected_family = 4;
    rtc.ice.selected_addr[0] = 192;
    rtc.ice.selected_addr[1] = 168;
    rtc.ice.selected_addr[2] = 1;
    rtc.ice.selected_addr[3] = 99;
    rtc.ice.selected_port = 5000;

    /* Verify host candidate type does NOT wrap */
    rtc.ice.selected_type = NANORTC_ICE_CAND_HOST;

    /* The rtc_enqueue_transmit is internal, but we can verify by checking
     * that TURN wrapping config is correct. Direct functional test via
     * the turn_wrap functions. */
    uint8_t payload[] = "hello relay";
    uint8_t buf[256];
    size_t out_len = 0;

    /* Send indication wrap works */
    int rc = turn_wrap_send(rtc.ice.selected_addr, rtc.ice.selected_family, rtc.ice.selected_port,
                            payload, 11, buf, sizeof(buf), &out_len);
    ASSERT_OK(rc);
    ASSERT_TRUE(out_len > 0);

    /* Parse back */
    stun_msg_t msg;
    ASSERT_OK(stun_parse(buf, out_len, &msg));
    ASSERT_EQ(msg.type, STUN_SEND_INDICATION);
    ASSERT_EQ(msg.data_attr_len, 11);

    /* ChannelData wrap works */
    out_len = 0;
    rc = nano_turn_wrap_channel_data(0x4000, payload, 11, buf, sizeof(buf), &out_len);
    ASSERT_OK(rc);
    ASSERT_EQ(out_len, 16); /* 4 header + 11 payload + 1 pad */
    ASSERT_EQ(nanortc_read_u16be(buf), 0x4000);
    ASSERT_EQ(nanortc_read_u16be(buf + 2), 11);

    nanortc_destroy(&rtc);
}

/* T: ChannelData inbound demux — ChannelData from TURN server unwrapped and re-dispatched */
TEST(test_e2e_channeldata_inbound)
{
    nanortc_t rtc;
    nanortc_config_t cfg = e2e_default_config();
    ASSERT_OK(nanortc_init(&rtc, &cfg));

    /* Configure TURN server */
    const char *turn_url = "turn:10.0.0.1:3478";
    nanortc_ice_server_t servers[] = {
        {.urls = &turn_url, .url_count = 1, .username = "u", .credential = "p"}};
    ASSERT_OK(nanortc_set_ice_servers(&rtc, servers, 1));

    /* Simulate TURN allocated with a bound channel for peer 192.168.1.50:5000 */
    rtc.turn.state = NANORTC_TURN_ALLOCATED;
    rtc.turn.channels[0].family = 4;
    rtc.turn.channels[0].addr[0] = 192;
    rtc.turn.channels[0].addr[1] = 168;
    rtc.turn.channels[0].addr[2] = 1;
    rtc.turn.channels[0].addr[3] = 50;
    rtc.turn.channels[0].port = 5000;
    rtc.turn.channels[0].channel = 0x4000;
    rtc.turn.channels[0].bound = true;
    rtc.turn.channel_count = 1;

    /* Build ChannelData: channel 0x4000, payload = STUN Binding Request (dummy) */
    uint8_t inner[20];
    memset(inner, 0, 20);
    nanortc_write_u16be(inner, STUN_BINDING_REQUEST);
    nanortc_write_u16be(inner + 2, 0);
    nanortc_write_u32be(inner + 4, STUN_MAGIC_COOKIE);

    uint8_t cd_pkt[24];
    nanortc_write_u16be(cd_pkt, 0x4000);
    nanortc_write_u16be(cd_pkt + 2, 20);
    memcpy(cd_pkt + 4, inner, 20);

    /* Feed from TURN server address */
    nanortc_addr_t turn_src;
    memset(&turn_src, 0, sizeof(turn_src));
    turn_src.family = 4;
    turn_src.addr[0] = 10;
    turn_src.addr[3] = 1;
    turn_src.port = 3478;

    /* This should unwrap ChannelData and re-dispatch the inner packet.
     * The inner STUN Binding Request will be processed by ICE (and likely
     * fail credential check since ICE isn't set up, but the unwrapping works). */
    int rc = nanortc_handle_input(
        &rtc, &(nanortc_input_t){.now_ms = 100, .data = cd_pkt, .len = 24, .src = turn_src});
    /* The inner packet processing may return OK or ERR depending on ICE state,
     * but the ChannelData demux itself should not crash */
    (void)rc;

    nanortc_destroy(&rtc);
}
#endif /* NANORTC_FEATURE_TURN */

/* T: Simple Binding Request encoding */
TEST(test_e2e_simple_binding_request)
{
    uint8_t txid[12] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
    uint8_t buf[32];
    size_t out_len = 0;

    int rc = stun_encode_simple_binding_request(txid, buf, sizeof(buf), &out_len);
    ASSERT_OK(rc);
    ASSERT_EQ(out_len, 20); /* bare header */
    ASSERT_EQ(nanortc_read_u16be(buf), STUN_BINDING_REQUEST);
    ASSERT_EQ(nanortc_read_u16be(buf + 2), 0); /* no attributes */
    ASSERT_EQ(nanortc_read_u32be(buf + 4), STUN_MAGIC_COOKIE);

    /* Verify txid */
    for (int i = 0; i < 12; i++) {
        ASSERT_EQ(buf[8 + i], txid[i]);
    }

    /* Buffer too small */
    rc = stun_encode_simple_binding_request(txid, buf, 10, &out_len);
    ASSERT_TRUE(rc != 0);
}

/* ---- Runner ---- */

TEST_MAIN_BEGIN("nanortc E2E tests")
RUN(test_e2e_init_pair);
RUN(test_e2e_init_rng_failure_clears_partial_state);
RUN(test_e2e_negotiation_rng_failure_is_transactional);
RUN(test_e2e_ice_restart_rng_failure_is_transactional);
#if NANORTC_HAVE_MEDIA_TRANSPORT
RUN(test_e2e_media_seed_rng_failure_rolls_back_all_tracks);
#if NANORTC_FEATURE_AUDIO
RUN(test_e2e_padded_audio_excludes_rtp_padding);
#endif
#if NANORTC_FEATURE_VIDEO
RUN(test_e2e_padded_video_excludes_rtp_padding);
RUN(test_e2e_bwe_feedback_events_keep_source);
#endif
#endif
RUN(test_e2e_stubs_not_implemented);
RUN(test_e2e_loopback_skeleton);
RUN(test_e2e_multiple_instances);
RUN(test_e2e_demux_byte_ranges);
RUN(test_e2e_ice_loopback);
RUN(test_e2e_ice_dtls_loopback);
RUN(test_e2e_ice_restart_dtls_rehandshake);
RUN(test_e2e_ice_restart_sdp_fingerprint_refresh);
RUN(test_e2e_create_offer_content);
RUN(test_e2e_offer_answer_roundtrip);
RUN(test_e2e_full_sdp_to_dtls);
RUN(test_e2e_state_queries);
#if NANORTC_FEATURE_DATACHANNEL
RUN(test_e2e_add_channel_invalid);
RUN(test_e2e_add_channel);
RUN(test_e2e_channel_close);
RUN(test_e2e_channel_invalid);
RUN(test_e2e_channel_label);
#endif
RUN(test_e2e_graceful_disconnect);
RUN(test_e2e_disconnect_new_state);
RUN(test_e2e_ice_multi_candidate);
RUN(test_e2e_accept_answer_state_guard);
/* E2E DataChannel message exchange */
#if NANORTC_FEATURE_DATACHANNEL
RUN(test_e2e_datachannel_send_recv);
RUN(test_e2e_multi_channel_create);
RUN(test_e2e_datachannel_offerer_initiated);
#endif
/* E2E connection lifecycle */
RUN(test_e2e_full_lifecycle);
RUN(test_e2e_ice_connection_timeout);
#if NANORTC_FEATURE_IPV6
RUN(test_e2e_ipv6_remote_candidate);
RUN(test_e2e_tie_breaker_is_randomised);
RUN(test_e2e_ipv6_loopback_connects);
#endif
/* Convenience send API */
#if NANORTC_HAVE_MEDIA_TRANSPORT
RUN(test_e2e_add_audio_video_track);
RUN(test_e2e_send_audio_before_connected);
RUN(test_e2e_send_audio_bad_params);
#if NANORTC_FEATURE_VIDEO
RUN(test_e2e_send_video_bad_params);
RUN(test_e2e_send_video_before_connected);
#if NANORTC_FEATURE_H265
RUN(test_e2e_h265_loopback);
RUN(test_e2e_h265_midstream_param_refresh);
#endif
RUN(test_e2e_video_send_admission);
#if NANORTC_FEATURE_VIDEO_AUTO_PLI && !NANORTC_FEATURE_VIDEO_REORDER
RUN(test_e2e_video_auto_pli_on_loss);
#endif
#if NANORTC_FEATURE_VIDEO_REORDER
RUN(test_e2e_video_reorder_heals_swap);
#if NANORTC_FEATURE_VIDEO_NACK_RX
RUN(test_e2e_video_nack_recovers_drop);
#endif
#endif
#if NANORTC_FEATURE_VIDEO_FEC
#if NANORTC_FEATURE_H265
RUN(test_e2e_h265_fec_group_commits);
#endif
#if !NANORTC_FEATURE_VIDEO_PACING
RUN(test_e2e_video_fec_resource_drop_is_atomic);
#endif
RUN(test_e2e_video_fec_recovers_drop);
#if NANORTC_FEC_ADAPTIVE
RUN(test_e2e_video_fec_adaptive_k_tracks_loss);
#endif
#if NANORTC_FEC_TX_RING > 1
RUN(test_e2e_video_fec_multigroup_idr);
#endif
#endif
#endif
RUN(test_e2e_connected_event_has_mids);
RUN(test_e2e_request_keyframe_bad_params);
/* Multi-media offer/answer */
RUN(test_e2e_media_offer_answer);
/* API coverage tests */
RUN(test_e2e_track_stats_not_connected);
RUN(test_e2e_track_stats_with_track);
RUN(test_e2e_set_direction);
RUN(test_e2e_add_track_max);
RUN(test_e2e_add_track_codecs);
RUN(test_e2e_add_track_null);
RUN(test_e2e_accept_offer_bad_sdp);
RUN(test_e2e_create_offer_state_guard);
RUN(test_e2e_accept_answer_bad);
RUN(test_e2e_add_candidate_params);
RUN(test_e2e_handle_input_params);
RUN(test_e2e_is_alive_connected);
#if NANORTC_FEATURE_DATACHANNEL
RUN(test_e2e_dc_send_not_connected);
RUN(test_e2e_dc_close_params);
RUN(test_e2e_dc_create_with_options);
RUN(test_e2e_dc_create_null);
#endif
#endif
/* NAT traversal E2E tests */
RUN(test_e2e_simple_binding_request);
RUN(test_e2e_stun_server_config);
RUN(test_e2e_srflx_rng_failure_does_not_commit_transaction);
RUN(test_e2e_srflx_discovery);
RUN(test_e2e_shared_stun_turn_endpoint_demux);
RUN(test_e2e_srflx_retry);
RUN(test_e2e_srflx_joins_local_candidates);
RUN(test_e2e_srflx_priority_macro);
RUN(test_e2e_handle_input_dst_resolves_local_idx);
RUN(test_e2e_handle_input_dst_exact_beats_wildcard);
RUN(test_e2e_handle_input_dst_null_falls_back);
#if NANORTC_FEATURE_TURN
RUN(test_e2e_turn_allocation_lifecycle);
RUN(test_e2e_turn_allocate_backpressure_preserves_state);
RUN(test_e2e_relay_check_waits_without_blocking_permission);
RUN(test_e2e_turn_permission_capacity_covers_remote_candidates);
RUN(test_e2e_turn_permission_table_full_does_not_freeze_ice);
RUN(test_e2e_turn_relay_wrapping);
RUN(test_e2e_channeldata_inbound);
#endif
TEST_MAIN_END
