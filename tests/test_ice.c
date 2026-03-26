/*
 * nanortc — ICE module tests (RFC 8445)
 *
 * Tests organized by RFC section:
 *   - §5.1: Candidate priority
 *   - §7.1.1: Controlling role — generating Binding Requests
 *   - §7.2.1: Controlled role — receiving requests, sending responses
 *   - §7.2.1.4: USE-CANDIDATE / nomination
 *   - §7.3: Processing Binding Responses
 *   - §8: Concluding ICE (state transitions)
 *   - Edge cases: bad credentials, pacing, failure
 *
 * SPDX-License-Identifier: MIT
 */

#include "nanortc.h"
#include "nano_ice.h"
#include "nano_stun.h"
#include "nano_crypto.h"
#include "nano_test.h"
#include "nano_test_config.h"
#include <string.h>

static const nano_crypto_provider_t *crypto(void)
{
    return nano_test_crypto();
}

/* Set up a pair of ICE agents with matching credentials */
static void setup_ice_pair(nano_ice_t *controlling, nano_ice_t *controlled)
{
    ice_init(controlling, 1);
    ice_init(controlled, 0);

    memcpy(controlling->local_ufrag, "CTRL", 5);
    memcpy(controlling->local_pwd, "ctrl-password-abcdef", 21);
    memcpy(controlling->remote_ufrag, "PEER", 5);
    memcpy(controlling->remote_pwd, "peer-password-123456", 21);
    controlling->tie_breaker = 0xAABBCCDDEEFF0011ull;
    controlling->remote_family = 4;
    controlling->remote_addr[0] = 10;
    controlling->remote_addr[3] = 2;
    controlling->remote_port = 5000;

    memcpy(controlled->local_ufrag, "PEER", 5);
    memcpy(controlled->local_pwd, "peer-password-123456", 21);
    memcpy(controlled->remote_ufrag, "CTRL", 5);
    memcpy(controlled->remote_pwd, "ctrl-password-abcdef", 21);
}

/* Helper: generate a check and feed to peer */
static int do_ice_roundtrip(nano_ice_t *ctrl, nano_ice_t *ctld,
                            uint32_t now_ms)
{
    uint8_t req_buf[256], resp_buf[256], dummy[256];
    size_t req_len = 0, resp_len = 0, dummy_len = 0;

    int rc = ice_generate_check(ctrl, now_ms, crypto(),
                                 req_buf, sizeof(req_buf), &req_len);
    if (rc != NANO_OK || req_len == 0) return rc;

    nano_addr_t src;
    memset(&src, 0, sizeof(src));
    src.family = 4;
    src.addr[0] = 10; src.addr[3] = 1;
    src.port = 4000;

    rc = ice_handle_stun(ctld, req_buf, req_len, &src, crypto(),
                          resp_buf, sizeof(resp_buf), &resp_len);
    if (rc != NANO_OK) return rc;

    nano_addr_t resp_src;
    memset(&resp_src, 0, sizeof(resp_src));
    resp_src.family = 4;
    resp_src.addr[0] = 10; resp_src.addr[3] = 2;
    resp_src.port = 5000;

    return ice_handle_stun(ctrl, resp_buf, resp_len, &resp_src, crypto(),
                            dummy, sizeof(dummy), &dummy_len);
}

/* ================================================================
 * Lifecycle
 * ================================================================ */

TEST(test_ice_init)
{
    nano_ice_t ice;
    ASSERT_OK(ice_init(&ice, 0));
    ASSERT_EQ(ice.state, NANO_ICE_STATE_NEW);
    ASSERT_EQ(ice.is_controlling, 0);
    ASSERT_EQ(ice.check_interval_ms, 50);

    ASSERT_OK(ice_init(&ice, 1));
    ASSERT_EQ(ice.is_controlling, 1);

    ASSERT_FAIL(ice_init(NULL, 0));
}

TEST(test_ice_is_stun)
{
    /* RFC 7983: STUN range 0x00-0x03 */
    uint8_t stun[] = {0x00, 0x01};
    ASSERT_TRUE(ice_is_stun(stun, 2));

    uint8_t stun2[] = {0x03, 0x00};
    ASSERT_TRUE(ice_is_stun(stun2, 2));

    uint8_t dtls[] = {0x14, 0xFE};
    ASSERT_FALSE(ice_is_stun(dtls, 2));

    uint8_t rtp[] = {0x80, 0x60};
    ASSERT_FALSE(ice_is_stun(rtp, 2));

    ASSERT_FALSE(ice_is_stun(NULL, 0));
}

/* ================================================================
 * RFC 8445 §7.1.1 — Controlling Role: Generating Binding Requests
 * ================================================================ */

TEST(test_ice_generate_check_basic)
{
    nano_ice_t ctrl, ctld;
    setup_ice_pair(&ctrl, &ctld);

    uint8_t buf[256];
    size_t out_len = 0;
    ASSERT_OK(ice_generate_check(&ctrl, 0, crypto(), buf, sizeof(buf), &out_len));
    ASSERT_TRUE(out_len > 0);
    ASSERT_EQ(ctrl.state, NANO_ICE_STATE_CHECKING);
    ASSERT_EQ(ctrl.check_count, 1);

    /* Verify the generated request is valid STUN */
    stun_msg_t msg;
    ASSERT_OK(stun_parse(buf, out_len, &msg));
    ASSERT_EQ(msg.type, STUN_BINDING_REQUEST);

    /* RFC 8445 §7.1.1: USERNAME = "remote_ufrag:local_ufrag" */
    ASSERT_EQ(msg.username_len, 9); /* "PEER:CTRL" */
    ASSERT_MEM_EQ(msg.username, "PEER:CTRL", 9);

    /* RFC 8445 §7.1.1: must have PRIORITY */
    ASSERT_TRUE(msg.priority > 0);

    /* RFC 8445 §7.1.1: controlling sends ICE-CONTROLLING + USE-CANDIDATE */
    ASSERT_TRUE(msg.has_ice_controlling);
    ASSERT_TRUE(msg.use_candidate);
    ASSERT_FALSE(msg.has_ice_controlled);

    /* MI and FP present */
    ASSERT_TRUE(msg.has_integrity);
    ASSERT_TRUE(msg.has_fingerprint);

    /* Signed with remote_pwd */
    ASSERT_OK(stun_verify_integrity(buf, out_len, &msg,
                                    (const uint8_t *)"peer-password-123456", 20,
                                    crypto()->hmac_sha1));
    ASSERT_OK(stun_verify_fingerprint(buf, out_len));
}

TEST(test_ice_generate_check_pacing)
{
    /* RFC 8445 §7.1.2: check pacing (Ta interval) */
    nano_ice_t ctrl, ctld;
    setup_ice_pair(&ctrl, &ctld);

    uint8_t buf[256];
    size_t out_len = 0;

    /* First check at t=0 */
    ASSERT_OK(ice_generate_check(&ctrl, 0, crypto(), buf, sizeof(buf), &out_len));
    ASSERT_TRUE(out_len > 0);
    ASSERT_EQ(ctrl.check_count, 1);

    /* t=10 (< 50ms interval) — not time yet */
    out_len = 0;
    ASSERT_OK(ice_generate_check(&ctrl, 10, crypto(), buf, sizeof(buf), &out_len));
    ASSERT_EQ(out_len, 0);
    ASSERT_EQ(ctrl.check_count, 1);

    /* t=50 — exactly at interval */
    out_len = 0;
    ASSERT_OK(ice_generate_check(&ctrl, 50, crypto(), buf, sizeof(buf), &out_len));
    ASSERT_TRUE(out_len > 0);
    ASSERT_EQ(ctrl.check_count, 2);
}

TEST(test_ice_controlled_does_not_generate)
{
    /* RFC 8445: only controlling role initiates checks */
    nano_ice_t ice;
    ice_init(&ice, 0); /* controlled */
    memcpy(ice.remote_pwd, "pw", 3);

    uint8_t buf[256];
    size_t out_len = 0;
    ASSERT_OK(ice_generate_check(&ice, 0, crypto(), buf, sizeof(buf), &out_len));
    ASSERT_EQ(out_len, 0);
}

/* ================================================================
 * RFC 8445 §7.2.1 — Controlled Role: Receiving Requests
 * ================================================================ */

TEST(test_ice_controlled_handle_request)
{
    /* Controlled receives Binding Request, produces Binding Response */
    nano_ice_t ctrl, ctld;
    setup_ice_pair(&ctrl, &ctld);

    uint8_t req_buf[256];
    size_t req_len = 0;
    ASSERT_OK(ice_generate_check(&ctrl, 100, crypto(),
                                  req_buf, sizeof(req_buf), &req_len));

    nano_addr_t src;
    memset(&src, 0, sizeof(src));
    src.family = 4;
    src.addr[0] = 10; src.addr[3] = 1;
    src.port = 4000;

    uint8_t resp_buf[256];
    size_t resp_len = 0;
    ASSERT_OK(ice_handle_stun(&ctld, req_buf, req_len, &src, crypto(),
                               resp_buf, sizeof(resp_buf), &resp_len));
    ASSERT_TRUE(resp_len > 0);

    /* Verify response is valid Binding Response */
    stun_msg_t resp;
    ASSERT_OK(stun_parse(resp_buf, resp_len, &resp));
    ASSERT_EQ(resp.type, STUN_BINDING_RESPONSE);
    ASSERT_TRUE(resp.has_integrity);
    ASSERT_TRUE(resp.has_fingerprint);

    /* RFC 8445 §7.2.2: response signed with local_pwd */
    ASSERT_OK(stun_verify_integrity(resp_buf, resp_len, &resp,
                                    (const uint8_t *)"peer-password-123456", 20,
                                    crypto()->hmac_sha1));

    /* Response contains XOR-MAPPED-ADDRESS of the source */
    ASSERT_EQ(resp.mapped_family, STUN_FAMILY_IPV4);
    ASSERT_EQ(resp.mapped_port, 4000);
    ASSERT_EQ(resp.mapped_addr[0], 10);
    ASSERT_EQ(resp.mapped_addr[3], 1);
}

TEST(test_ice_reject_bad_username)
{
    /* RFC 8445 §7.2.1: reject if USERNAME doesn't match local_ufrag */
    nano_ice_t ctld;
    ice_init(&ctld, 0);
    memcpy(ctld.local_ufrag, "ME", 3);
    memcpy(ctld.local_pwd, "my-password", 12);

    uint8_t txid[12] = {0};
    uint8_t key[] = "my-password";
    uint8_t req_buf[256];
    size_t req_len = 0;
    ASSERT_OK(stun_encode_binding_request("WRONG:OTHER", 11, 100, false,
                                          true, 0, txid,
                                          key, sizeof(key) - 1,
                                          crypto()->hmac_sha1,
                                          req_buf, sizeof(req_buf), &req_len));

    nano_addr_t src;
    memset(&src, 0, sizeof(src));
    src.family = 4;

    uint8_t resp_buf[256];
    size_t resp_len = 0;
    ASSERT_FAIL(ice_handle_stun(&ctld, req_buf, req_len, &src, crypto(),
                                 resp_buf, sizeof(resp_buf), &resp_len));
}

TEST(test_ice_reject_bad_password)
{
    /* RFC 8445 §7.2.1: reject if MI fails (wrong password) */
    nano_ice_t ctld;
    ice_init(&ctld, 0);
    memcpy(ctld.local_ufrag, "ME", 3);
    memcpy(ctld.local_pwd, "correct-pw", 11);

    uint8_t txid[12] = {0};
    uint8_t wrong_key[] = "wrong-pw";
    uint8_t req_buf[256];
    size_t req_len = 0;
    ASSERT_OK(stun_encode_binding_request("ME:OTHER", 8, 100, false, true, 0,
                                          txid, wrong_key, sizeof(wrong_key) - 1,
                                          crypto()->hmac_sha1,
                                          req_buf, sizeof(req_buf), &req_len));

    nano_addr_t src;
    memset(&src, 0, sizeof(src));
    src.family = 4;

    uint8_t resp_buf[256];
    size_t resp_len = 0;
    ASSERT_FAIL(ice_handle_stun(&ctld, req_buf, req_len, &src, crypto(),
                                 resp_buf, sizeof(resp_buf), &resp_len));
}

/* ================================================================
 * RFC 8445 §7.2.1.4 — USE-CANDIDATE / Nomination
 * ================================================================ */

TEST(test_ice_use_candidate_nominates)
{
    /*
     * RFC 8445 §7.2.1.4: When controlled receives USE-CANDIDATE,
     * it nominates the pair and transitions to CONNECTED.
     */
    nano_ice_t ctrl, ctld;
    setup_ice_pair(&ctrl, &ctld);

    uint8_t req_buf[256];
    size_t req_len = 0;
    ASSERT_OK(ice_generate_check(&ctrl, 100, crypto(),
                                  req_buf, sizeof(req_buf), &req_len));

    /* Verify USE-CANDIDATE is present in the request */
    stun_msg_t req;
    ASSERT_OK(stun_parse(req_buf, req_len, &req));
    ASSERT_TRUE(req.use_candidate);

    nano_addr_t src;
    memset(&src, 0, sizeof(src));
    src.family = 4;
    src.addr[0] = 10; src.addr[3] = 1;
    src.port = 4000;

    uint8_t resp_buf[256];
    size_t resp_len = 0;
    ASSERT_OK(ice_handle_stun(&ctld, req_buf, req_len, &src, crypto(),
                               resp_buf, sizeof(resp_buf), &resp_len));

    /* Controlled should be CONNECTED with selected address */
    ASSERT_EQ(ctld.state, NANO_ICE_STATE_CONNECTED);
    ASSERT_TRUE(ctld.nominated);
    ASSERT_EQ(ctld.selected_port, 4000);
    ASSERT_EQ(ctld.selected_addr[0], 10);
    ASSERT_EQ(ctld.selected_addr[3], 1);
}

TEST(test_ice_no_use_candidate_no_nomination)
{
    /*
     * RFC 8445 §7.2.1.4: Without USE-CANDIDATE, controlled responds
     * but does NOT nominate (stays NEW/CHECKING).
     *
     * Build a Binding Request without USE-CANDIDATE manually.
     */
    nano_ice_t ctld;
    ice_init(&ctld, 0);
    memcpy(ctld.local_ufrag, "PEER", 5);
    memcpy(ctld.local_pwd, "peer-password-123456", 21);
    memcpy(ctld.remote_ufrag, "CTRL", 5);

    uint8_t txid[12] = {0};
    uint8_t key[] = "peer-password-123456";
    uint8_t req_buf[256];
    size_t req_len = 0;

    /* use_candidate=false, is_controlling=true */
    ASSERT_OK(stun_encode_binding_request(
        "PEER:CTRL", 9, 0x6E001EFF,
        false, /* no USE-CANDIDATE */
        true, 0x1234ull, txid,
        key, sizeof(key) - 1,
        crypto()->hmac_sha1,
        req_buf, sizeof(req_buf), &req_len));

    nano_addr_t src;
    memset(&src, 0, sizeof(src));
    src.family = 4;
    src.addr[0] = 10; src.addr[3] = 1;
    src.port = 4000;

    uint8_t resp_buf[256];
    size_t resp_len = 0;
    ASSERT_OK(ice_handle_stun(&ctld, req_buf, req_len, &src, crypto(),
                               resp_buf, sizeof(resp_buf), &resp_len));

    /* Response generated but NOT nominated */
    ASSERT_TRUE(resp_len > 0);
    ASSERT_EQ(ctld.state, NANO_ICE_STATE_NEW); /* not CONNECTED */
    ASSERT_FALSE(ctld.nominated);
}

/* ================================================================
 * RFC 8445 §7.3 — Processing Binding Responses (Controlling)
 * ================================================================ */

TEST(test_ice_controlling_receives_response)
{
    /* Full roundtrip: controlling sends, controlled responds,
     * controlling processes response → CONNECTED */
    nano_ice_t ctrl, ctld;
    setup_ice_pair(&ctrl, &ctld);

    ASSERT_OK(do_ice_roundtrip(&ctrl, &ctld, 100));

    ASSERT_EQ(ctrl.state, NANO_ICE_STATE_CONNECTED);
    ASSERT_TRUE(ctrl.nominated);
    ASSERT_EQ(ctld.state, NANO_ICE_STATE_CONNECTED);
    ASSERT_TRUE(ctld.nominated);
}

TEST(test_ice_controlling_rejects_wrong_txid)
{
    /* Controlling rejects response with mismatched transaction ID */
    nano_ice_t ctrl, ctld;
    setup_ice_pair(&ctrl, &ctld);

    uint8_t req_buf[256];
    size_t req_len = 0;
    ASSERT_OK(ice_generate_check(&ctrl, 100, crypto(),
                                  req_buf, sizeof(req_buf), &req_len));

    /* Tamper: change the last txid byte in ctrl so response won't match */
    ctrl.last_txid[11] ^= 0xFF;

    nano_addr_t src;
    memset(&src, 0, sizeof(src));
    src.family = 4; src.port = 4000;

    uint8_t resp_buf[256];
    size_t resp_len = 0;
    ASSERT_OK(ice_handle_stun(&ctld, req_buf, req_len, &src, crypto(),
                               resp_buf, sizeof(resp_buf), &resp_len));

    /* Feed response to controlling — should fail (txid mismatch) */
    uint8_t dummy[256];
    size_t dummy_len = 0;
    nano_addr_t resp_src;
    memset(&resp_src, 0, sizeof(resp_src));
    resp_src.family = 4;
    ASSERT_FAIL(ice_handle_stun(&ctrl, resp_buf, resp_len, &resp_src, crypto(),
                                 dummy, sizeof(dummy), &dummy_len));

    /* Should NOT be connected */
    ASSERT_NEQ(ctrl.state, NANO_ICE_STATE_CONNECTED);
}

/* ================================================================
 * RFC 8445 §8 — State Transitions
 * ================================================================ */

TEST(test_ice_state_new_to_checking)
{
    nano_ice_t ctrl, ctld;
    setup_ice_pair(&ctrl, &ctld);

    ASSERT_EQ(ctrl.state, NANO_ICE_STATE_NEW);

    uint8_t buf[256];
    size_t out_len = 0;
    ASSERT_OK(ice_generate_check(&ctrl, 0, crypto(), buf, sizeof(buf), &out_len));
    ASSERT_EQ(ctrl.state, NANO_ICE_STATE_CHECKING);
}

TEST(test_ice_state_checking_to_connected)
{
    nano_ice_t ctrl, ctld;
    setup_ice_pair(&ctrl, &ctld);

    ASSERT_OK(do_ice_roundtrip(&ctrl, &ctld, 100));

    ASSERT_EQ(ctrl.state, NANO_ICE_STATE_CONNECTED);
    ASSERT_EQ(ctld.state, NANO_ICE_STATE_CONNECTED);
}

TEST(test_ice_state_checking_to_failed)
{
    /* After ICE_MAX_CHECKS without response → FAILED */
    nano_ice_t ctrl, ctld;
    setup_ice_pair(&ctrl, &ctld);

    uint8_t buf[256];
    size_t out_len = 0;
    uint32_t t = 0;

    for (int i = 0; i < ICE_MAX_CHECKS; i++) {
        out_len = 0;
        ASSERT_OK(ice_generate_check(&ctrl, t, crypto(),
                                      buf, sizeof(buf), &out_len));
        ASSERT_TRUE(out_len > 0);
        t += 50;
    }
    ASSERT_EQ(ctrl.check_count, ICE_MAX_CHECKS);
    ASSERT_EQ(ctrl.state, NANO_ICE_STATE_CHECKING);

    /* One more attempt → FAILED */
    out_len = 0;
    ASSERT_OK(ice_generate_check(&ctrl, t, crypto(), buf, sizeof(buf), &out_len));
    ASSERT_EQ(out_len, 0);
    ASSERT_EQ(ctrl.state, NANO_ICE_STATE_FAILED);
}

TEST(test_ice_no_checks_after_connected)
{
    /* Once CONNECTED, no more checks generated */
    nano_ice_t ctrl, ctld;
    setup_ice_pair(&ctrl, &ctld);

    ASSERT_OK(do_ice_roundtrip(&ctrl, &ctld, 100));
    ASSERT_EQ(ctrl.state, NANO_ICE_STATE_CONNECTED);

    uint8_t buf[256];
    size_t out_len = 0;
    ASSERT_OK(ice_generate_check(&ctrl, 200, crypto(), buf, sizeof(buf), &out_len));
    ASSERT_EQ(out_len, 0); /* no check generated */
}

/* ================================================================
 * RFC 8445 §7.2.1.1 — Credential Verification
 *
 * Incoming request: MI verified with local_pwd
 * Outgoing response: signed with local_pwd
 * Outgoing request: signed with remote_pwd
 * Incoming response: MI verified with remote_pwd
 * ================================================================ */

TEST(test_ice_credential_usage)
{
    /* Verify the correct key is used at each step */
    nano_ice_t ctrl, ctld;
    setup_ice_pair(&ctrl, &ctld);

    /* 1. Controlling generates request signed with remote_pwd */
    uint8_t req_buf[256];
    size_t req_len = 0;
    ASSERT_OK(ice_generate_check(&ctrl, 100, crypto(),
                                  req_buf, sizeof(req_buf), &req_len));

    stun_msg_t req;
    ASSERT_OK(stun_parse(req_buf, req_len, &req));

    /* Request signed with ctrl's remote_pwd = "peer-password-123456" */
    ASSERT_OK(stun_verify_integrity(req_buf, req_len, &req,
                                    (const uint8_t *)"peer-password-123456", 20,
                                    crypto()->hmac_sha1));
    /* Wrong key fails */
    ASSERT_FAIL(stun_verify_integrity(req_buf, req_len, &req,
                                      (const uint8_t *)"ctrl-password-abcdef", 20,
                                      crypto()->hmac_sha1));

    /* 2. Controlled receives, response signed with local_pwd */
    nano_addr_t src;
    memset(&src, 0, sizeof(src));
    src.family = 4; src.port = 4000;

    uint8_t resp_buf[256];
    size_t resp_len = 0;
    ASSERT_OK(ice_handle_stun(&ctld, req_buf, req_len, &src, crypto(),
                               resp_buf, sizeof(resp_buf), &resp_len));

    stun_msg_t resp;
    ASSERT_OK(stun_parse(resp_buf, resp_len, &resp));

    /* Response signed with ctld's local_pwd = "peer-password-123456" */
    ASSERT_OK(stun_verify_integrity(resp_buf, resp_len, &resp,
                                    (const uint8_t *)"peer-password-123456", 20,
                                    crypto()->hmac_sha1));
}

/* ---- Runner ---- */

TEST_MAIN_BEGIN("nanortc ICE tests")
    /* Lifecycle */
    RUN(test_ice_init);
    RUN(test_ice_is_stun);
    /* §7.1.1: Controlling generates requests */
    RUN(test_ice_generate_check_basic);
    RUN(test_ice_generate_check_pacing);
    RUN(test_ice_controlled_does_not_generate);
    /* §7.2.1: Controlled receives requests */
    RUN(test_ice_controlled_handle_request);
    RUN(test_ice_reject_bad_username);
    RUN(test_ice_reject_bad_password);
    /* §7.2.1.4: USE-CANDIDATE / nomination */
    RUN(test_ice_use_candidate_nominates);
    RUN(test_ice_no_use_candidate_no_nomination);
    /* §7.3: Processing responses */
    RUN(test_ice_controlling_receives_response);
    RUN(test_ice_controlling_rejects_wrong_txid);
    /* §8: State transitions */
    RUN(test_ice_state_new_to_checking);
    RUN(test_ice_state_checking_to_connected);
    RUN(test_ice_state_checking_to_failed);
    RUN(test_ice_no_checks_after_connected);
    /* Credential usage */
    RUN(test_ice_credential_usage);
TEST_MAIN_END
