/*
 * nanortc — Trickle ICE + ICE Restart + Consent Freshness tests
 *
 * Tests:
 *   - Trickle candidate addition during CHECKING state
 *   - End-of-candidates triggers check completion
 *   - ICE restart: credential reset, state reset, generation bump
 *   - Consent freshness timeout triggers disconnection
 *   - Consent freshness refresh extends expiry
 *
 * SPDX-License-Identifier: MIT
 */

#include "nanortc.h"
#include "nano_ice.h"
#include "nano_stun.h"
#include "nano_sdp.h"
#include "nano_dtls.h"
#include "nanortc_crypto.h"
#include "nano_test.h"
#include "nano_test_config.h"
#include <string.h>

static const nanortc_crypto_provider_t *crypto(void)
{
    return nano_test_crypto();
}

/* ----------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------- */

static void setup_ice_controlling(nano_ice_t *ice)
{
    ice_init(ice, 1);
    memcpy(ice->local_ufrag, "CTRL1234", 9);
    ice->local_ufrag_len = 8;
    memcpy(ice->local_pwd, "ctrl-password-abcdef12", 23);
    ice->local_pwd_len = 22;
    memcpy(ice->remote_ufrag, "PEER5678", 9);
    ice->remote_ufrag_len = 8;
    memcpy(ice->remote_pwd, "peer-password-xyz12345", 23);
    ice->remote_pwd_len = 22;
    ice->tie_breaker = 0x1122334455667788ull;
    /* Add a default local candidate (192.168.1.100:4000) */
    ice->local_candidates[0].addr[0] = 192;
    ice->local_candidates[0].addr[1] = 168;
    ice->local_candidates[0].addr[2] = 1;
    ice->local_candidates[0].addr[3] = 100;
    ice->local_candidates[0].port = 4000;
    ice->local_candidates[0].family = 4;
    ice->local_candidates[0].type = NANORTC_ICE_CAND_HOST;
    ice->local_candidate_count = 1;
}

static void add_candidate(nano_ice_t *ice, uint8_t a, uint8_t b, uint8_t c, uint8_t d,
                          uint16_t port)
{
    uint8_t idx = ice->remote_candidate_count;
    TEST_ASSERT_TRUE_MESSAGE(idx < NANORTC_MAX_ICE_CANDIDATES, "candidate array full");
    ice->remote_candidates[idx].family = 4;
    ice->remote_candidates[idx].addr[0] = a;
    ice->remote_candidates[idx].addr[1] = b;
    ice->remote_candidates[idx].addr[2] = c;
    ice->remote_candidates[idx].addr[3] = d;
    ice->remote_candidates[idx].port = port;
    ice->remote_candidates[idx].type = NANORTC_ICE_CAND_HOST;
    ice->remote_candidate_count++;
}

/* ----------------------------------------------------------------
 * Trickle ICE tests
 * ---------------------------------------------------------------- */

/* T1: No remote candidates → stays in NEW, waits for trickle */
static void test_trickle_no_candidates_waits(void)
{
    nano_ice_t ice;
    setup_ice_controlling(&ice);
    /* No remote candidates added */

    uint8_t buf[256];
    size_t out_len = 0;
    int rc = ice_generate_check(&ice, 100, crypto(), buf, sizeof(buf), &out_len);
    TEST_ASSERT_EQUAL_INT(NANORTC_OK, rc);
    TEST_ASSERT_EQUAL_size_t(0, out_len);
    TEST_ASSERT_EQUAL_INT(NANORTC_ICE_STATE_NEW, ice.state);
}

/* T2: No candidates + end_of_candidates → FAILED */
static void test_trickle_no_candidates_eoc_fails(void)
{
    nano_ice_t ice;
    setup_ice_controlling(&ice);
    ice.end_of_candidates = true;

    uint8_t buf[256];
    size_t out_len = 0;
    int rc = ice_generate_check(&ice, 100, crypto(), buf, sizeof(buf), &out_len);
    TEST_ASSERT_EQUAL_INT(NANORTC_OK, rc);
    TEST_ASSERT_EQUAL_INT(NANORTC_ICE_STATE_FAILED, ice.state);
}

/* T3: Trickle add during CHECKING triggers new checks */
static void test_trickle_add_during_checking(void)
{
    nano_ice_t ice;
    setup_ice_controlling(&ice);
    add_candidate(&ice, 10, 0, 0, 1, 5000);

    uint8_t buf[256];
    size_t out_len = 0;

    /* First check → transitions to CHECKING */
    int rc = ice_generate_check(&ice, 0, crypto(), buf, sizeof(buf), &out_len);
    TEST_ASSERT_EQUAL_INT(NANORTC_OK, rc);
    TEST_ASSERT_TRUE(out_len > 0);
    TEST_ASSERT_EQUAL_INT(NANORTC_ICE_STATE_CHECKING, ice.state);

    /* Trickle: add second candidate while checking */
    add_candidate(&ice, 10, 0, 0, 2, 5001);
    TEST_ASSERT_EQUAL_INT(2, ice.remote_candidate_count);

    /* Next check uses round-robin: was at 0, now 2 candidates → checks 0 then advances to 1 */
    out_len = 0;
    rc = ice_generate_check(&ice, 100, crypto(), buf, sizeof(buf), &out_len);
    TEST_ASSERT_EQUAL_INT(NANORTC_OK, rc);
    TEST_ASSERT_TRUE(out_len > 0);
    /* After round-robin: remote advances 0→1, local stays 0 */
    TEST_ASSERT_EQUAL_INT(1, ice.current_remote);
}

/* T4: Candidate type field defaults to HOST */
static void test_candidate_type_default(void)
{
    nano_ice_t ice;
    ice_init(&ice, 1);
    add_candidate(&ice, 192, 168, 1, 1, 3000);
    TEST_ASSERT_EQUAL_INT(NANORTC_ICE_CAND_HOST, ice.remote_candidates[0].type);
}

/* T5: Max candidates limit — fill to max, then verify overflow is rejected via public API */
static void test_trickle_max_candidates(void)
{
    nano_ice_t ice;
    ice_init(&ice, 1);

    for (int i = 0; i < NANORTC_MAX_ICE_CANDIDATES; i++) {
        add_candidate(&ice, 10, 0, 0, (uint8_t)(i + 1), (uint16_t)(5000 + i));
    }
    TEST_ASSERT_EQUAL_INT(NANORTC_MAX_ICE_CANDIDATES, ice.remote_candidate_count);

    /* Overflow: directly attempt one more — count must not exceed max */
    uint8_t prev_count = ice.remote_candidate_count;
    if (ice.remote_candidate_count >= NANORTC_MAX_ICE_CANDIDATES) {
        /* Simulate what a well-behaved caller should check */
        TEST_ASSERT_EQUAL_INT(NANORTC_MAX_ICE_CANDIDATES, prev_count);
    }
    /* Verify the public API rejects overflow (nanortc_add_remote_candidate checks bounds) */
    nanortc_t rtc;
    memset(&rtc, 0, sizeof(rtc));
    nanortc_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.crypto = crypto();
    cfg.role = NANORTC_ROLE_CONTROLLING;
    nanortc_init(&rtc, &cfg);
    /* Fill via public API */
    for (int i = 0; i < NANORTC_MAX_ICE_CANDIDATES; i++) {
        char cand[32];
        int len = snprintf(cand, sizeof(cand), "10.0.0.%d %d", i + 1, 5000 + i);
        (void)len;
        nanortc_add_remote_candidate(&rtc, cand);
    }
    TEST_ASSERT_EQUAL_INT(NANORTC_MAX_ICE_CANDIDATES, rtc.ice.remote_candidate_count);
    /* One more must fail */
    TEST_ASSERT_EQUAL_INT(NANORTC_ERR_BUFFER_TOO_SMALL,
                          nanortc_add_remote_candidate(&rtc, "10.0.0.99 6000"));
    TEST_ASSERT_EQUAL_INT(NANORTC_MAX_ICE_CANDIDATES, rtc.ice.remote_candidate_count);
    nanortc_destroy(&rtc);
}

/* ----------------------------------------------------------------
 * ICE Restart tests
 * ---------------------------------------------------------------- */

/* T6: ice_restart resets state and bumps generation */
static void test_ice_restart_resets_state(void)
{
    nano_ice_t ice;
    setup_ice_controlling(&ice);
    add_candidate(&ice, 10, 0, 0, 1, 5000);
    ice.state = NANORTC_ICE_STATE_CONNECTED;
    ice.nominated = true;
    ice.check_count = 5;
    ice.end_of_candidates = true;
    uint8_t gen_before = ice.generation;

    int rc = ice_restart(&ice);
    TEST_ASSERT_EQUAL_INT(NANORTC_OK, rc);
    TEST_ASSERT_EQUAL_INT(NANORTC_ICE_STATE_NEW, ice.state);
    TEST_ASSERT_EQUAL_INT(0, ice.remote_candidate_count);
    TEST_ASSERT_EQUAL_INT(0, ice.check_count);
    TEST_ASSERT_FALSE(ice.nominated);
    TEST_ASSERT_FALSE(ice.end_of_candidates);
    TEST_ASSERT_EQUAL_INT(gen_before + 1, ice.generation);
    /* Role and tie_breaker preserved */
    TEST_ASSERT_TRUE(ice.is_controlling);
}

/* T7: ice_restart preserves role */
static void test_ice_restart_preserves_role(void)
{
    nano_ice_t ice;
    ice_init(&ice, 0); /* controlled */
    ice.state = NANORTC_ICE_STATE_CONNECTED;
    ice.tie_breaker = 0xAABBCCDD;

    ice_restart(&ice);
    TEST_ASSERT_FALSE(ice.is_controlling);
    TEST_ASSERT_EQUAL_HEX64(0xAABBCCDD, ice.tie_breaker);
}

/* T8: ice_restart with NULL returns error */
static void test_ice_restart_null(void)
{
    TEST_ASSERT_EQUAL_INT(NANORTC_ERR_INVALID_PARAM, ice_restart(NULL));
}

/* ----------------------------------------------------------------
 * Consent Freshness tests (RFC 7675)
 * ---------------------------------------------------------------- */

/* T9: Consent check not generated when not connected */
static void test_consent_not_generated_when_not_connected(void)
{
    nano_ice_t ice;
    setup_ice_controlling(&ice);
    ice.state = NANORTC_ICE_STATE_CHECKING;

    uint8_t buf[256];
    size_t out_len = 0;
    int rc = ice_generate_consent(&ice, 1000, crypto(), buf, sizeof(buf), &out_len);
    TEST_ASSERT_EQUAL_INT(NANORTC_OK, rc);
    TEST_ASSERT_EQUAL_size_t(0, out_len);
}

/* T10: Consent check generated when connected and due */
static void test_consent_generated_when_due(void)
{
    nano_ice_t ice;
    setup_ice_controlling(&ice);
    ice.state = NANORTC_ICE_STATE_CONNECTED;
    ice.consent_next_ms = 1000;
    ice.consent_expiry_ms = 30000;

    uint8_t buf[256];
    size_t out_len = 0;
    int rc = ice_generate_consent(&ice, 1000, crypto(), buf, sizeof(buf), &out_len);
    TEST_ASSERT_EQUAL_INT(NANORTC_OK, rc);
    TEST_ASSERT_TRUE(out_len > 0);
    TEST_ASSERT_TRUE(ice.consent_pending);
    TEST_ASSERT_EQUAL_UINT32(1000 + NANORTC_ICE_CONSENT_INTERVAL_MS, ice.consent_next_ms);
}

/* T11: Consent not generated before interval */
static void test_consent_pacing(void)
{
    nano_ice_t ice;
    setup_ice_controlling(&ice);
    ice.state = NANORTC_ICE_STATE_CONNECTED;
    ice.consent_next_ms = 2000;

    uint8_t buf[256];
    size_t out_len = 0;
    int rc = ice_generate_consent(&ice, 1500, crypto(), buf, sizeof(buf), &out_len);
    TEST_ASSERT_EQUAL_INT(NANORTC_OK, rc);
    TEST_ASSERT_EQUAL_size_t(0, out_len);
    TEST_ASSERT_FALSE(ice.consent_pending);
}

/* T12: Consent expired returns true after timeout */
static void test_consent_expiry(void)
{
    nano_ice_t ice;
    memset(&ice, 0, sizeof(ice));
    ice.state = NANORTC_ICE_STATE_CONNECTED;
    ice.consent_expiry_ms = 30000;

    TEST_ASSERT_FALSE(ice_consent_expired(&ice, 29999));
    TEST_ASSERT_TRUE(ice_consent_expired(&ice, 30000));
    TEST_ASSERT_TRUE(ice_consent_expired(&ice, 31000));
}

/* T13: Unarmed consent expiry surfaces as "expired" once CONNECTED.
 *
 * Previously this returned false to treat zero as "not yet initialized", but
 * that silently disabled the liveness timeout if a caller forgot to arm it.
 * The contract is now: `consent_expiry_ms` MUST be set when state transitions
 * to CONNECTED; a zero in CONNECTED is a programming error and the function
 * reports it as expired so the dead-peer signal surfaces rather than hiding.
 * Any non-CONNECTED state still short-circuits to false.
 */
static void test_consent_expired_when_unarmed(void)
{
    nano_ice_t ice;
    memset(&ice, 0, sizeof(ice));
    ice.state = NANORTC_ICE_STATE_CONNECTED;
    ice.consent_expiry_ms = 0;
    TEST_ASSERT_TRUE(ice_consent_expired(&ice, 50000));

    ice.state = NANORTC_ICE_STATE_CHECKING;
    TEST_ASSERT_FALSE(ice_consent_expired(&ice, 50000));
}

/* T14: Consent response clears pending flag */
static void test_consent_response_clears_pending(void)
{
    nano_ice_t ctrl, ctld;
    /* Set up controlling agent */
    setup_ice_controlling(&ctrl);
    ctrl.state = NANORTC_ICE_STATE_CONNECTED;
    ctrl.consent_next_ms = 0;
    ctrl.consent_expiry_ms = 50000;

    /* Set up controlled agent to respond */
    ice_init(&ctld, 0);
    memcpy(ctld.local_ufrag, "PEER5678", 9);
    ctld.local_ufrag_len = 8;
    memcpy(ctld.local_pwd, "peer-password-xyz12345", 23);
    ctld.local_pwd_len = 22;
    memcpy(ctld.remote_ufrag, "CTRL1234", 9);
    ctld.remote_ufrag_len = 8;
    memcpy(ctld.remote_pwd, "ctrl-password-abcdef12", 23);
    ctld.remote_pwd_len = 22;

    /* Generate consent check */
    uint8_t buf[256];
    size_t out_len = 0;
    int rc = ice_generate_consent(&ctrl, 1000, crypto(), buf, sizeof(buf), &out_len);
    TEST_ASSERT_EQUAL_INT(NANORTC_OK, rc);
    TEST_ASSERT_TRUE(out_len > 0);
    TEST_ASSERT_TRUE(ctrl.consent_pending);

    /* Feed to controlled agent → get response */
    nanortc_addr_t src;
    memset(&src, 0, sizeof(src));
    src.family = 4;
    src.addr[0] = 10;
    src.port = 4000;

    uint8_t resp[256];
    size_t resp_len = 0;
    rc = ice_handle_stun(&ctld, buf, out_len, &src, NANORTC_ICE_LOCAL_IDX_UNKNOWN, false, crypto(),
                         resp, sizeof(resp), &resp_len);
    TEST_ASSERT_EQUAL_INT(NANORTC_OK, rc);
    TEST_ASSERT_TRUE(resp_len > 0);

    /* Feed response back to controlling → should clear consent_pending */
    nanortc_addr_t resp_src;
    memset(&resp_src, 0, sizeof(resp_src));
    resp_src.family = 4;
    resp_src.addr[0] = 10;
    resp_src.addr[3] = 1;
    resp_src.port = 5000;

    uint8_t dummy[256];
    size_t dummy_len = 0;
    rc = ice_handle_stun(&ctrl, resp, resp_len, &resp_src, NANORTC_ICE_LOCAL_IDX_UNKNOWN, false,
                         crypto(), dummy, sizeof(dummy), &dummy_len);
    TEST_ASSERT_EQUAL_INT(NANORTC_OK, rc);
    TEST_ASSERT_FALSE(ctrl.consent_pending);
}

/* ----------------------------------------------------------------
 * DISCONNECTED state tests
 * ---------------------------------------------------------------- */

/* T15: DISCONNECTED state exists and is distinct */
static void test_disconnected_state_exists(void)
{
    TEST_ASSERT_NOT_EQUAL(NANORTC_ICE_STATE_CONNECTED, NANORTC_ICE_STATE_DISCONNECTED);
    TEST_ASSERT_NOT_EQUAL(NANORTC_ICE_STATE_FAILED, NANORTC_ICE_STATE_DISCONNECTED);
}

/* ----------------------------------------------------------------
 * SDP end-of-candidates parsing
 * ---------------------------------------------------------------- */

/* T16: SDP with a=end-of-candidates sets flag */
static void test_sdp_end_of_candidates_parsed(void)
{
    nano_sdp_t sdp;
    sdp_init(&sdp);

    const char *sdp_str = "v=0\r\n"
                          "o=- 0 0 IN IP4 10.0.0.1\r\n"
                          "s=-\r\n"
                          "t=0 0\r\n"
                          "m=application 9 UDP/DTLS/SCTP webrtc-datachannel\r\n"
                          "c=IN IP4 10.0.0.1\r\n"
                          "a=ice-ufrag:abcd\r\n"
                          "a=ice-pwd:efghijklmnopqr\r\n"
                          "a=fingerprint:sha-256 AA:BB:CC:DD\r\n"
                          "a=setup:actpass\r\n"
                          "a=sctp-port:5000\r\n"
                          "a=candidate:1 1 UDP 2130706431 10.0.0.1 5000 typ host\r\n"
                          "a=end-of-candidates\r\n";

    size_t len = 0;
    while (sdp_str[len])
        len++;

    int rc = sdp_parse(&sdp, sdp_str, len);
    TEST_ASSERT_EQUAL_INT(NANORTC_OK, rc);
    TEST_ASSERT_TRUE(sdp.end_of_candidates);
    TEST_ASSERT_EQUAL_INT(1, sdp.candidate_count);
}

/* T17: SDP without a=end-of-candidates leaves flag false */
static void test_sdp_no_end_of_candidates(void)
{
    nano_sdp_t sdp;
    sdp_init(&sdp);

    const char *sdp_str = "v=0\r\n"
                          "o=- 0 0 IN IP4 10.0.0.1\r\n"
                          "s=-\r\n"
                          "t=0 0\r\n"
                          "m=application 9 UDP/DTLS/SCTP webrtc-datachannel\r\n"
                          "c=IN IP4 10.0.0.1\r\n"
                          "a=ice-ufrag:abcd\r\n"
                          "a=ice-pwd:efghijklmnopqr\r\n"
                          "a=fingerprint:sha-256 AA:BB:CC:DD\r\n"
                          "a=setup:actpass\r\n"
                          "a=sctp-port:5000\r\n";

    size_t len = 0;
    while (sdp_str[len])
        len++;

    int rc = sdp_parse(&sdp, sdp_str, len);
    TEST_ASSERT_EQUAL_INT(NANORTC_OK, rc);
    TEST_ASSERT_FALSE(sdp.end_of_candidates);
}

/* ----------------------------------------------------------------
 * Public API tests (nanortc_end_of_candidates, nanortc_ice_restart)
 * ---------------------------------------------------------------- */

/* T18: nanortc_end_of_candidates sets flags */
static void test_api_end_of_candidates(void)
{
    nanortc_t rtc;
    memset(&rtc, 0, sizeof(rtc));

    nanortc_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.crypto = nano_test_crypto();
    cfg.role = NANORTC_ROLE_CONTROLLING;
    nanortc_init(&rtc, &cfg);

    TEST_ASSERT_FALSE(rtc.ice.end_of_candidates);
    int rc = nanortc_end_of_candidates(&rtc);
    TEST_ASSERT_EQUAL_INT(NANORTC_OK, rc);
    TEST_ASSERT_TRUE(rtc.ice.end_of_candidates);
    TEST_ASSERT_TRUE(rtc.sdp.end_of_candidates);

    nanortc_destroy(&rtc);
}

/* T19a: nanortc_add_remote_candidate parses "typ <type>" field */
static void test_api_remote_candidate_parses_type(void)
{
    nanortc_t rtc;
    memset(&rtc, 0, sizeof(rtc));
    nanortc_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.crypto = nano_test_crypto();
    cfg.role = NANORTC_ROLE_CONTROLLING;
    nanortc_init(&rtc, &cfg);

    /* host candidate */
    int rc = nanortc_add_remote_candidate(
        &rtc, "candidate:1 1 UDP 2130706431 192.168.1.10 5000 typ host");
    TEST_ASSERT_EQUAL_INT(NANORTC_OK, rc);
    TEST_ASSERT_EQUAL_INT(NANORTC_ICE_CAND_HOST, rtc.ice.remote_candidates[0].type);

    /* srflx candidate (with raddr/rport tail) */
    rc = nanortc_add_remote_candidate(
        &rtc,
        "candidate:2 1 UDP 1694498815 203.0.113.5 6000 typ srflx raddr 192.168.1.10 rport 5000");
    TEST_ASSERT_EQUAL_INT(NANORTC_OK, rc);
    TEST_ASSERT_EQUAL_INT(NANORTC_ICE_CAND_SRFLX, rtc.ice.remote_candidates[1].type);

    /* relay candidate */
    rc = nanortc_add_remote_candidate(
        &rtc, "candidate:3 1 UDP 16777215 198.51.100.7 7000 typ relay raddr 0.0.0.0 rport 0");
    TEST_ASSERT_EQUAL_INT(NANORTC_OK, rc);
    TEST_ASSERT_EQUAL_INT(NANORTC_ICE_CAND_RELAY, rtc.ice.remote_candidates[2].type);

    /* simple format (no candidate: prefix) defaults to HOST */
    rc = nanortc_add_remote_candidate(&rtc, "10.0.0.1 8000");
    TEST_ASSERT_EQUAL_INT(NANORTC_OK, rc);
    TEST_ASSERT_EQUAL_INT(NANORTC_ICE_CAND_HOST, rtc.ice.remote_candidates[3].type);

    nanortc_destroy(&rtc);
}

/* T19: nanortc_ice_restart resets state and generates new credentials */
static void test_api_ice_restart(void)
{
    nanortc_t rtc;
    memset(&rtc, 0, sizeof(rtc));

    nanortc_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.crypto = nano_test_crypto();
    cfg.role = NANORTC_ROLE_CONTROLLING;
    nanortc_init(&rtc, &cfg);

    /* Record original credentials */
    char orig_ufrag[NANORTC_ICE_UFRAG_SIZE];
    char orig_pwd[NANORTC_ICE_PWD_SIZE];
    memcpy(orig_ufrag, rtc.ice.local_ufrag, NANORTC_ICE_UFRAG_SIZE);
    memcpy(orig_pwd, rtc.ice.local_pwd, NANORTC_ICE_PWD_SIZE);

    /* Simulate connected state */
    rtc.state = NANORTC_STATE_CONNECTED;
    rtc.ice.state = NANORTC_ICE_STATE_CONNECTED;
    rtc.ice.nominated = true;

    int rc = nanortc_ice_restart(&rtc);
    TEST_ASSERT_EQUAL_INT(NANORTC_OK, rc);

    /* State should be reset */
    TEST_ASSERT_EQUAL_INT(NANORTC_STATE_NEW, rtc.state);
    TEST_ASSERT_EQUAL_INT(NANORTC_ICE_STATE_NEW, rtc.ice.state);
    TEST_ASSERT_FALSE(rtc.ice.nominated);
    TEST_ASSERT_EQUAL_INT(1, rtc.ice.generation);

    /* Credentials should have changed */
    TEST_ASSERT_TRUE(memcmp(orig_ufrag, rtc.ice.local_ufrag, NANORTC_ICE_UFRAG_LEN) != 0 ||
                     memcmp(orig_pwd, rtc.ice.local_pwd, NANORTC_ICE_PWD_LEN) != 0);

    /* Credentials should be synced to SDP */
    TEST_ASSERT_EQUAL_MEMORY(rtc.ice.local_ufrag, rtc.sdp.local_ufrag, NANORTC_ICE_UFRAG_LEN);

    nanortc_destroy(&rtc);
}

/* T20: nanortc_ice_restart tears down DTLS so the next handshake
 * re-initialises with a fresh cert / BIO. Without this the
 * `if (!rtc->dtls.crypto_ctx)` guard in rtc_begin_dtls_handshake would
 * skip dtls_init and reuse the previous DTLS context. */
static void test_api_ice_restart_clears_dtls(void)
{
    nanortc_t rtc;
    memset(&rtc, 0, sizeof(rtc));

    nanortc_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.crypto = nano_test_crypto();
    cfg.role = NANORTC_ROLE_CONTROLLING;
    nanortc_init(&rtc, &cfg);

    /* Initialise DTLS as a client (mirrors what rtc_begin_dtls_handshake
     * does during the create_offer path). */
    int rc = dtls_init(&rtc.dtls, cfg.crypto, 0);
    TEST_ASSERT_EQUAL_INT(NANORTC_OK, rc);
    TEST_ASSERT_NOT_NULL(rtc.dtls.crypto_ctx);

    /* Pre-populate the cached fingerprints (rtc_cache_fingerprint is a
     * write-once cache, so this mirrors a session that already ran an
     * SDP exchange before the restart). */
    memcpy(rtc.sdp.local_fingerprint, "sha-256 AA:BB:CC", 17);
    memcpy(rtc.dtls.local_fingerprint, "AA:BB:CC", 9);

    /* Simulate a connected session on top of the live DTLS context. */
    rtc.state = NANORTC_STATE_CONNECTED;
    rtc.ice.state = NANORTC_ICE_STATE_CONNECTED;
    rtc.ice.nominated = true;

    rc = nanortc_ice_restart(&rtc);
    TEST_ASSERT_EQUAL_INT(NANORTC_OK, rc);

    /* The DTLS context must have been torn down so the next
     * accept_offer/create_offer path re-runs dtls_init. */
    TEST_ASSERT_NULL(rtc.dtls.crypto_ctx);
    TEST_ASSERT_EQUAL_INT(NANORTC_DTLS_STATE_CLOSED, rtc.dtls.state);

    /* Cached fingerprints must be invalidated so rtc_cache_fingerprint
     * repopulates them from the next dtls_init's cert (otherwise the
     * peer sees a stale SDP fingerprint and DTLS verify fails). */
    TEST_ASSERT_EQUAL_INT('\0', rtc.sdp.local_fingerprint[0]);
    TEST_ASSERT_EQUAL_INT('\0', rtc.dtls.local_fingerprint[0]);

    nanortc_destroy(&rtc);
}

/* T21: nanortc_ice_restart is safe to call before any DTLS context exists.
 * dtls_destroy is documented as a no-op when crypto_ctx is NULL. */
static void test_api_ice_restart_no_dtls_safe(void)
{
    nanortc_t rtc;
    memset(&rtc, 0, sizeof(rtc));

    nanortc_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.crypto = nano_test_crypto();
    cfg.role = NANORTC_ROLE_CONTROLLING;
    nanortc_init(&rtc, &cfg);

    /* No dtls_init() yet — crypto_ctx is NULL. */
    TEST_ASSERT_NULL(rtc.dtls.crypto_ctx);

    int rc = nanortc_ice_restart(&rtc);
    TEST_ASSERT_EQUAL_INT(NANORTC_OK, rc);
    TEST_ASSERT_NULL(rtc.dtls.crypto_ctx);

    nanortc_destroy(&rtc);
}

/* ----------------------------------------------------------------
 * Test runner
 * ---------------------------------------------------------------- */

int main(void)
{
    UNITY_BEGIN();

    /* Trickle ICE */
    RUN_TEST(test_trickle_no_candidates_waits);
    RUN_TEST(test_trickle_no_candidates_eoc_fails);
    RUN_TEST(test_trickle_add_during_checking);
    RUN_TEST(test_candidate_type_default);
    RUN_TEST(test_trickle_max_candidates);

    /* ICE Restart */
    RUN_TEST(test_ice_restart_resets_state);
    RUN_TEST(test_ice_restart_preserves_role);
    RUN_TEST(test_ice_restart_null);

    /* Consent Freshness */
    RUN_TEST(test_consent_not_generated_when_not_connected);
    RUN_TEST(test_consent_generated_when_due);
    RUN_TEST(test_consent_pacing);
    RUN_TEST(test_consent_expiry);
    RUN_TEST(test_consent_expired_when_unarmed);
    RUN_TEST(test_consent_response_clears_pending);

    /* DISCONNECTED state */
    RUN_TEST(test_disconnected_state_exists);

    /* SDP end-of-candidates */
    RUN_TEST(test_sdp_end_of_candidates_parsed);
    RUN_TEST(test_sdp_no_end_of_candidates);

    /* Public API */
    RUN_TEST(test_api_end_of_candidates);
    RUN_TEST(test_api_remote_candidate_parses_type);
    RUN_TEST(test_api_ice_restart);
    RUN_TEST(test_api_ice_restart_clears_dtls);
    RUN_TEST(test_api_ice_restart_no_dtls_safe);

    return UNITY_END();
}
