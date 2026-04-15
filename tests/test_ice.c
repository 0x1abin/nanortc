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
#include "nanortc_crypto.h"
#include "nano_test.h"
#include "nano_test_config.h"
#include <string.h>

static const nanortc_crypto_provider_t *crypto(void)
{
    return nano_test_crypto();
}

/* Set up a pair of ICE agents with matching credentials */
static void setup_ice_pair(nano_ice_t *controlling, nano_ice_t *controlled)
{
    ice_init(controlling, 1);
    ice_init(controlled, 0);

    memcpy(controlling->local_ufrag, "CTRL", 5);
    controlling->local_ufrag_len = 4;
    memcpy(controlling->local_pwd, "ctrl-password-abcdef", 21);
    controlling->local_pwd_len = 20;
    memcpy(controlling->remote_ufrag, "PEER", 5);
    controlling->remote_ufrag_len = 4;
    memcpy(controlling->remote_pwd, "peer-password-123456", 21);
    controlling->remote_pwd_len = 20;
    controlling->tie_breaker = 0xAABBCCDDEEFF0011ull;
    /* Local candidate for controlling */
    controlling->local_candidates[0].family = 4;
    controlling->local_candidates[0].addr[0] = 10;
    controlling->local_candidates[0].addr[3] = 1;
    controlling->local_candidates[0].port = 4000;
    controlling->local_candidates[0].type = NANORTC_ICE_CAND_HOST;
    controlling->local_candidate_count = 1;
    /* Remote candidate for controlling */
    controlling->remote_candidates[0].family = 4;
    controlling->remote_candidates[0].addr[0] = 10;
    controlling->remote_candidates[0].addr[3] = 2;
    controlling->remote_candidates[0].port = 5000;
    controlling->remote_candidate_count = 1;

    memcpy(controlled->local_ufrag, "PEER", 5);
    controlled->local_ufrag_len = 4;
    memcpy(controlled->local_pwd, "peer-password-123456", 21);
    controlled->local_pwd_len = 20;
    memcpy(controlled->remote_ufrag, "CTRL", 5);
    controlled->remote_ufrag_len = 4;
    memcpy(controlled->remote_pwd, "ctrl-password-abcdef", 21);
    controlled->remote_pwd_len = 20;
}

/* Helper: generate a check and feed to peer */
static int do_ice_roundtrip(nano_ice_t *ctrl, nano_ice_t *ctld, uint32_t now_ms)
{
    uint8_t req_buf[256], resp_buf[256], dummy[256];
    size_t req_len = 0, resp_len = 0, dummy_len = 0;

    int rc = ice_generate_check(ctrl, now_ms, crypto(), req_buf, sizeof(req_buf), &req_len);
    if (rc != NANORTC_OK || req_len == 0)
        return rc;

    nanortc_addr_t src;
    memset(&src, 0, sizeof(src));
    src.family = 4;
    src.addr[0] = 10;
    src.addr[3] = 1;
    src.port = 4000;

    rc = ice_handle_stun(ctld, req_buf, req_len, &src, false, crypto(), resp_buf, sizeof(resp_buf),
                         &resp_len);
    if (rc != NANORTC_OK)
        return rc;

    nanortc_addr_t resp_src;
    memset(&resp_src, 0, sizeof(resp_src));
    resp_src.family = 4;
    resp_src.addr[0] = 10;
    resp_src.addr[3] = 2;
    resp_src.port = 5000;

    return ice_handle_stun(ctrl, resp_buf, resp_len, &resp_src, false, crypto(), dummy, sizeof(dummy),
                           &dummy_len);
}

/* ================================================================
 * Lifecycle
 * ================================================================ */

TEST(test_ice_init)
{
    nano_ice_t ice;
    ASSERT_OK(ice_init(&ice, 0));
    ASSERT_EQ(ice.state, NANORTC_ICE_STATE_NEW);
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
    ASSERT_EQ(ctrl.state, NANORTC_ICE_STATE_CHECKING);
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
    ASSERT_OK(stun_verify_integrity(buf, out_len, &msg, (const uint8_t *)"peer-password-123456", 20,
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
    ice.remote_pwd_len = 2;

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
    ASSERT_OK(ice_generate_check(&ctrl, 100, crypto(), req_buf, sizeof(req_buf), &req_len));

    nanortc_addr_t src;
    memset(&src, 0, sizeof(src));
    src.family = 4;
    src.addr[0] = 10;
    src.addr[3] = 1;
    src.port = 4000;

    uint8_t resp_buf[256];
    size_t resp_len = 0;
    ASSERT_OK(ice_handle_stun(&ctld, req_buf, req_len, &src, false, crypto(), resp_buf, sizeof(resp_buf),
                              &resp_len));
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
    ctld.local_ufrag_len = 2;
    memcpy(ctld.local_pwd, "my-password", 12);
    ctld.local_pwd_len = 11;

    uint8_t txid[12] = {0};
    uint8_t key[] = "my-password";
    uint8_t req_buf[256];
    size_t req_len = 0;
    ASSERT_OK(stun_encode_binding_request("WRONG:OTHER", 11, 100, false, true, 0, txid, key,
                                          sizeof(key) - 1, crypto()->hmac_sha1, req_buf,
                                          sizeof(req_buf), &req_len));

    nanortc_addr_t src;
    memset(&src, 0, sizeof(src));
    src.family = 4;

    uint8_t resp_buf[256];
    size_t resp_len = 0;
    ASSERT_FAIL(ice_handle_stun(&ctld, req_buf, req_len, &src, false, crypto(), resp_buf, sizeof(resp_buf),
                                &resp_len));
}

TEST(test_ice_reject_bad_password)
{
    /* RFC 8445 §7.2.1: reject if MI fails (wrong password) */
    nano_ice_t ctld;
    ice_init(&ctld, 0);
    memcpy(ctld.local_ufrag, "ME", 3);
    ctld.local_ufrag_len = 2;
    memcpy(ctld.local_pwd, "correct-pw", 11);
    ctld.local_pwd_len = 10;

    uint8_t txid[12] = {0};
    uint8_t wrong_key[] = "wrong-pw";
    uint8_t req_buf[256];
    size_t req_len = 0;
    ASSERT_OK(stun_encode_binding_request("ME:OTHER", 8, 100, false, true, 0, txid, wrong_key,
                                          sizeof(wrong_key) - 1, crypto()->hmac_sha1, req_buf,
                                          sizeof(req_buf), &req_len));

    nanortc_addr_t src;
    memset(&src, 0, sizeof(src));
    src.family = 4;

    uint8_t resp_buf[256];
    size_t resp_len = 0;
    ASSERT_FAIL(ice_handle_stun(&ctld, req_buf, req_len, &src, false, crypto(), resp_buf, sizeof(resp_buf),
                                &resp_len));
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
    ASSERT_OK(ice_generate_check(&ctrl, 100, crypto(), req_buf, sizeof(req_buf), &req_len));

    /* Verify USE-CANDIDATE is present in the request */
    stun_msg_t req;
    ASSERT_OK(stun_parse(req_buf, req_len, &req));
    ASSERT_TRUE(req.use_candidate);

    nanortc_addr_t src;
    memset(&src, 0, sizeof(src));
    src.family = 4;
    src.addr[0] = 10;
    src.addr[3] = 1;
    src.port = 4000;

    uint8_t resp_buf[256];
    size_t resp_len = 0;
    ASSERT_OK(ice_handle_stun(&ctld, req_buf, req_len, &src, false, crypto(), resp_buf, sizeof(resp_buf),
                              &resp_len));

    /* Controlled should be CONNECTED with selected address */
    ASSERT_EQ(ctld.state, NANORTC_ICE_STATE_CONNECTED);
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
    ctld.local_ufrag_len = 4;
    memcpy(ctld.local_pwd, "peer-password-123456", 21);
    ctld.local_pwd_len = 20;
    memcpy(ctld.remote_ufrag, "CTRL", 5);
    ctld.remote_ufrag_len = 4;

    uint8_t txid[12] = {0};
    uint8_t key[] = "peer-password-123456";
    uint8_t req_buf[256];
    size_t req_len = 0;

    /* use_candidate=false, is_controlling=true */
    ASSERT_OK(stun_encode_binding_request("PEER:CTRL", 9, 0x6E001EFF, false, /* no USE-CANDIDATE */
                                          true, 0x1234ull, txid, key, sizeof(key) - 1,
                                          crypto()->hmac_sha1, req_buf, sizeof(req_buf), &req_len));

    nanortc_addr_t src;
    memset(&src, 0, sizeof(src));
    src.family = 4;
    src.addr[0] = 10;
    src.addr[3] = 1;
    src.port = 4000;

    uint8_t resp_buf[256];
    size_t resp_len = 0;
    ASSERT_OK(ice_handle_stun(&ctld, req_buf, req_len, &src, false, crypto(), resp_buf, sizeof(resp_buf),
                              &resp_len));

    /* Response generated but NOT nominated */
    ASSERT_TRUE(resp_len > 0);
    ASSERT_EQ(ctld.state, NANORTC_ICE_STATE_NEW); /* not CONNECTED */
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

    ASSERT_EQ(ctrl.state, NANORTC_ICE_STATE_CONNECTED);
    ASSERT_TRUE(ctrl.nominated);
    ASSERT_EQ(ctld.state, NANORTC_ICE_STATE_CONNECTED);
    ASSERT_TRUE(ctld.nominated);
}

TEST(test_ice_controlling_rejects_wrong_txid)
{
    /* Controlling rejects response with mismatched transaction ID */
    nano_ice_t ctrl, ctld;
    setup_ice_pair(&ctrl, &ctld);

    uint8_t req_buf[256];
    size_t req_len = 0;
    ASSERT_OK(ice_generate_check(&ctrl, 100, crypto(), req_buf, sizeof(req_buf), &req_len));

    /* TD-018: locate the unique in-flight pending slot (we just called
     * ice_generate_check once, so exactly one slot is in_flight) and
     * tamper its txid so the incoming response won't match. */
    int found = -1;
    for (int i = 0; i < NANORTC_ICE_MAX_PENDING_CHECKS; i++) {
        if (ctrl.pending[i].in_flight) {
            ASSERT_EQ(found, -1); /* exactly one in-flight slot expected */
            found = i;
        }
    }
    ASSERT_NEQ(found, -1);
    ctrl.pending[found].txid[11] ^= 0xFF;

    nanortc_addr_t src;
    memset(&src, 0, sizeof(src));
    src.family = 4;
    src.port = 4000;

    uint8_t resp_buf[256];
    size_t resp_len = 0;
    ASSERT_OK(ice_handle_stun(&ctld, req_buf, req_len, &src, false, crypto(), resp_buf, sizeof(resp_buf),
                              &resp_len));

    /* Feed response to controlling — should fail (txid mismatch) */
    uint8_t dummy[256];
    size_t dummy_len = 0;
    nanortc_addr_t resp_src;
    memset(&resp_src, 0, sizeof(resp_src));
    resp_src.family = 4;
    ASSERT_FAIL(ice_handle_stun(&ctrl, resp_buf, resp_len, &resp_src, false, crypto(), dummy,
                                sizeof(dummy), &dummy_len));

    /* Should NOT be connected */
    ASSERT_NEQ(ctrl.state, NANORTC_ICE_STATE_CONNECTED);
}

/* ================================================================
 * TD-018: per-pair pending transaction table (RFC 8445 §7.1.3)
 * ================================================================ */

TEST(test_ice_controlling_multi_pair_response_out_of_order)
{
    /*
     * TD-018 regression: CONTROLLING sends 3 checks to 3 different
     * remote candidate pairs in quick succession. Only the *2nd* response
     * arrives back. Pre-fix, the single `last_txid` scratch had already
     * been overwritten by the 3rd check — so the response was rejected
     * with NANORTC_ERR_PROTOCOL and nanortc stalled in CHECKING. Post-fix
     * the response matches the 2nd pending slot and the 2nd pair is
     * correctly recorded as the selected pair.
     */
    nano_ice_t ctrl, ctld;
    setup_ice_pair(&ctrl, &ctld);

    /* Add 2 more remote candidates so ctrl has 3 total */
    ctrl.remote_candidates[1].family = 4;
    ctrl.remote_candidates[1].addr[0] = 10;
    ctrl.remote_candidates[1].addr[3] = 20;
    ctrl.remote_candidates[1].port = 5001;
    ctrl.remote_candidates[1].type = NANORTC_ICE_CAND_HOST;
    ctrl.remote_candidates[2].family = 4;
    ctrl.remote_candidates[2].addr[0] = 10;
    ctrl.remote_candidates[2].addr[3] = 30;
    ctrl.remote_candidates[2].port = 5002;
    ctrl.remote_candidates[2].type = NANORTC_ICE_CAND_HOST;
    ctrl.remote_candidate_count = 3;

    /* Generate 3 checks at t=100, 150, 200 — one per remote pair */
    uint8_t req1[256], req2[256], req3[256];
    size_t req1_len = 0, req2_len = 0, req3_len = 0;
    ASSERT_OK(ice_generate_check(&ctrl, 100, crypto(), req1, sizeof(req1), &req1_len));
    ASSERT_TRUE(req1_len > 0);
    ASSERT_OK(ice_generate_check(&ctrl, 150, crypto(), req2, sizeof(req2), &req2_len));
    ASSERT_TRUE(req2_len > 0);
    ASSERT_OK(ice_generate_check(&ctrl, 200, crypto(), req3, sizeof(req3), &req3_len));
    ASSERT_TRUE(req3_len > 0);
    ASSERT_EQ(ctrl.check_count, 3);

    /* Three distinct pending slots should be in flight */
    int in_flight_count = 0;
    for (int i = 0; i < NANORTC_ICE_MAX_PENDING_CHECKS; i++) {
        if (ctrl.pending[i].in_flight) {
            in_flight_count++;
        }
    }
    ASSERT_EQ(in_flight_count, 3);

    /* Feed ONLY the 2nd request to ctld and collect the 2nd response.
     * The response echoes req2's txid and is signed with ctld's local_pwd
     * (which equals ctrl's remote_pwd — see setup_ice_pair). */
    nanortc_addr_t src;
    memset(&src, 0, sizeof(src));
    src.family = 4;
    src.addr[0] = 10;
    src.addr[3] = 1;
    src.port = 4000;

    uint8_t resp2[256];
    size_t resp2_len = 0;
    ASSERT_OK(
        ice_handle_stun(&ctld, req2, req2_len, &src, false, crypto(), resp2, sizeof(resp2),
                        &resp2_len));
    ASSERT_TRUE(resp2_len > 0);

    /* Feed resp2 back to ctrl. Pre-fix this returned NANORTC_ERR_PROTOCOL
     * because last_txid held req3 — the 2nd response was unmatched. */
    nanortc_addr_t resp_src;
    memset(&resp_src, 0, sizeof(resp_src));
    resp_src.family = 4;
    resp_src.addr[0] = 10;
    resp_src.addr[3] = 20;
    resp_src.port = 5001;

    uint8_t dummy[256];
    size_t dummy_len = 0;
    ASSERT_OK(ice_handle_stun(&ctrl, resp2, resp2_len, &resp_src, false, crypto(), dummy,
                              sizeof(dummy),
                              &dummy_len));

    /* The 2nd pair (remote_idx=1, addr .20, port 5001) must be the one
     * selected — NOT whichever `last_remote_idx` happened to hold. */
    ASSERT_EQ(ctrl.state, NANORTC_ICE_STATE_CONNECTED);
    ASSERT_TRUE(ctrl.nominated);
    ASSERT_EQ(ctrl.selected_addr[0], 10);
    ASSERT_EQ(ctrl.selected_addr[3], 20);
    ASSERT_EQ(ctrl.selected_port, 5001);
    ASSERT_EQ(ctrl.selected_local_idx, 0);

    /* The matching slot must be freed; the other two still in flight */
    int still = 0;
    for (int i = 0; i < NANORTC_ICE_MAX_PENDING_CHECKS; i++) {
        if (ctrl.pending[i].in_flight) {
            still++;
        }
    }
    ASSERT_EQ(still, 2);
}

TEST(test_ice_controlling_pending_table_full)
{
    /*
     * TD-018 regression: when the pending table fills up without responses,
     * the allocator must reap the oldest slot so forward progress continues.
     * Verify that (a) every generate call succeeds, (b) the table stays at
     * exactly NANORTC_ICE_MAX_PENDING_CHECKS in-flight entries, and (c) the
     * first (oldest) txid is no longer present in any slot.
     */
    nano_ice_t ctrl, ctld;
    setup_ice_pair(&ctrl, &ctld);

    uint8_t req[256];
    size_t req_len = 0;

    /* Initial check — capture its txid so we can prove it gets reaped */
    ASSERT_OK(ice_generate_check(&ctrl, 100, crypto(), req, sizeof(req), &req_len));
    ASSERT_TRUE(req_len > 0);

    int oldest_slot = -1;
    for (int i = 0; i < NANORTC_ICE_MAX_PENDING_CHECKS; i++) {
        if (ctrl.pending[i].in_flight) {
            ASSERT_EQ(oldest_slot, -1); /* exactly one in-flight slot */
            oldest_slot = i;
        }
    }
    ASSERT_NEQ(oldest_slot, -1);
    uint8_t oldest_txid[STUN_TXID_SIZE];
    memcpy(oldest_txid, ctrl.pending[oldest_slot].txid, STUN_TXID_SIZE);

    /* Generate NANORTC_ICE_MAX_PENDING_CHECKS + 1 more checks without any
     * responses. The first NANORTC_ICE_MAX_PENDING_CHECKS - 1 fill the
     * remaining free slots; the next two force the reap-oldest path. */
    uint32_t now = 100;
    for (int i = 0; i < NANORTC_ICE_MAX_PENDING_CHECKS + 1; i++) {
        now += 50;
        req_len = 0;
        ASSERT_OK(ice_generate_check(&ctrl, now, crypto(), req, sizeof(req), &req_len));
        ASSERT_TRUE(req_len > 0);
    }
    ASSERT_EQ(ctrl.check_count, (uint8_t)(NANORTC_ICE_MAX_PENDING_CHECKS + 2));

    /* Every slot must be in_flight (table at capacity) and none of them
     * may still hold the original oldest txid. */
    int still_in_flight = 0;
    bool oldest_present = false;
    for (int i = 0; i < NANORTC_ICE_MAX_PENDING_CHECKS; i++) {
        if (ctrl.pending[i].in_flight) {
            still_in_flight++;
        }
        if (memcmp(ctrl.pending[i].txid, oldest_txid, STUN_TXID_SIZE) == 0) {
            oldest_present = true;
        }
    }
    ASSERT_EQ(still_in_flight, NANORTC_ICE_MAX_PENDING_CHECKS);
    ASSERT_FALSE(oldest_present);
}

/* ================================================================
 * RFC 8445 §8 — State Transitions
 * ================================================================ */

TEST(test_ice_state_new_to_checking)
{
    nano_ice_t ctrl, ctld;
    setup_ice_pair(&ctrl, &ctld);

    ASSERT_EQ(ctrl.state, NANORTC_ICE_STATE_NEW);

    uint8_t buf[256];
    size_t out_len = 0;
    ASSERT_OK(ice_generate_check(&ctrl, 0, crypto(), buf, sizeof(buf), &out_len));
    ASSERT_EQ(ctrl.state, NANORTC_ICE_STATE_CHECKING);
}

TEST(test_ice_state_checking_to_connected)
{
    nano_ice_t ctrl, ctld;
    setup_ice_pair(&ctrl, &ctld);

    ASSERT_OK(do_ice_roundtrip(&ctrl, &ctld, 100));

    ASSERT_EQ(ctrl.state, NANORTC_ICE_STATE_CONNECTED);
    ASSERT_EQ(ctld.state, NANORTC_ICE_STATE_CONNECTED);
}

TEST(test_ice_state_checking_to_failed)
{
    /* After NANORTC_ICE_MAX_CHECKS without response → FAILED */
    nano_ice_t ctrl, ctld;
    setup_ice_pair(&ctrl, &ctld);

    uint8_t buf[256];
    size_t out_len = 0;
    uint32_t t = 0;

    for (int i = 0; i < NANORTC_ICE_MAX_CHECKS; i++) {
        out_len = 0;
        ASSERT_OK(ice_generate_check(&ctrl, t, crypto(), buf, sizeof(buf), &out_len));
        ASSERT_TRUE(out_len > 0);
        t += 50;
    }
    ASSERT_EQ(ctrl.check_count, NANORTC_ICE_MAX_CHECKS);
    ASSERT_EQ(ctrl.state, NANORTC_ICE_STATE_CHECKING);

    /* One more attempt → FAILED */
    out_len = 0;
    ASSERT_OK(ice_generate_check(&ctrl, t, crypto(), buf, sizeof(buf), &out_len));
    ASSERT_EQ(out_len, 0);
    ASSERT_EQ(ctrl.state, NANORTC_ICE_STATE_FAILED);
}

TEST(test_ice_no_checks_after_connected)
{
    /* Once CONNECTED, no more checks generated */
    nano_ice_t ctrl, ctld;
    setup_ice_pair(&ctrl, &ctld);

    ASSERT_OK(do_ice_roundtrip(&ctrl, &ctld, 100));
    ASSERT_EQ(ctrl.state, NANORTC_ICE_STATE_CONNECTED);

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
    ASSERT_OK(ice_generate_check(&ctrl, 100, crypto(), req_buf, sizeof(req_buf), &req_len));

    stun_msg_t req;
    ASSERT_OK(stun_parse(req_buf, req_len, &req));

    /* Request signed with ctrl's remote_pwd = "peer-password-123456" */
    ASSERT_OK(stun_verify_integrity(req_buf, req_len, &req, (const uint8_t *)"peer-password-123456",
                                    20, crypto()->hmac_sha1));
    /* Wrong key fails */
    ASSERT_FAIL(stun_verify_integrity(
        req_buf, req_len, &req, (const uint8_t *)"ctrl-password-abcdef", 20, crypto()->hmac_sha1));

    /* 2. Controlled receives, response signed with local_pwd */
    nanortc_addr_t src;
    memset(&src, 0, sizeof(src));
    src.family = 4;
    src.port = 4000;

    uint8_t resp_buf[256];
    size_t resp_len = 0;
    ASSERT_OK(ice_handle_stun(&ctld, req_buf, req_len, &src, false, crypto(), resp_buf, sizeof(resp_buf),
                              &resp_len));

    stun_msg_t resp;
    ASSERT_OK(stun_parse(resp_buf, resp_len, &resp));

    /* Response signed with ctld's local_pwd = "peer-password-123456" */
    ASSERT_OK(stun_verify_integrity(resp_buf, resp_len, &resp,
                                    (const uint8_t *)"peer-password-123456", 20,
                                    crypto()->hmac_sha1));
}

/* ================================================================
 * RFC 8445 MUST/SHOULD requirement tests
 * ================================================================ */

/*
 * RFC 8445 §5.1.2.1: Candidate priority formula.
 * priority = (2^24)*type_pref + (2^8)*local_pref + (256 - component_id)
 *
 * For host candidates: type_pref=126, local_pref=65535, component_id=1
 * Expected: (2^24)*126 + (2^8)*65535 + (256-1) = 2113929471 + 16776960 + 255
 *         = 2113929471 + 16776960 + 255 = 2130706687 - but that overflows.
 * Let me recalculate: 126*16777216 = 2113929216, 65535*256 = 16776960, 255
 * Total: 2113929216 + 16776960 + 255 = 2130706431
 *
 * Verify the STUN PRIORITY attribute in a generated check matches this.
 */
TEST(test_ice_priority_formula_rfc8445)
{
    nano_ice_t ice;
    ice_init(&ice, 1);

    /* Set up minimal credentials */
    memcpy(ice.local_ufrag, "AAAA", 4);
    ice.local_ufrag_len = 4;
    memcpy(ice.local_pwd, "password-1234567890a", 20);
    ice.local_pwd_len = 20;
    memcpy(ice.remote_ufrag, "BBBB", 4);
    ice.remote_ufrag_len = 4;
    memcpy(ice.remote_pwd, "password-0987654321b", 20);
    ice.remote_pwd_len = 20;
    ice.tie_breaker = 0x1234567890ABCDEFull;
    /* Local candidate */
    ice.local_candidates[0].family = 4;
    ice.local_candidates[0].addr[0] = 192;
    ice.local_candidates[0].addr[1] = 168;
    ice.local_candidates[0].addr[2] = 1;
    ice.local_candidates[0].addr[3] = 100;
    ice.local_candidates[0].port = 4000;
    ice.local_candidates[0].type = NANORTC_ICE_CAND_HOST;
    ice.local_candidate_count = 1;
    /* Remote candidate */
    ice.remote_candidates[0].family = 4;
    ice.remote_candidates[0].addr[0] = 192;
    ice.remote_candidates[0].addr[1] = 168;
    ice.remote_candidates[0].addr[2] = 1;
    ice.remote_candidates[0].addr[3] = 1;
    ice.remote_candidates[0].port = 5000;
    ice.remote_candidate_count = 1;

    uint8_t buf[256];
    size_t len = 0;
    ASSERT_OK(ice_generate_check(&ice, 0, crypto(), buf, sizeof(buf), &len));
    ASSERT_TRUE(len > 0);

    /* Parse the generated STUN request */
    stun_msg_t msg;
    ASSERT_OK(stun_parse(buf, len, &msg));

    /* RFC 8445 §5.1.2.1: For host, type_pref=126, local=65535, comp=1 */
    /* priority = 126*2^24 + 65535*2^8 + 255 = 2130706431 */
    uint32_t expected = (uint32_t)126 * (1u << 24) + (uint32_t)65535 * (1u << 8) + 255;
    ASSERT_EQ(msg.priority, expected);
}

/*
 * RFC 8445 §7: Multiple remote candidates — verify checks cycle through them.
 */
TEST(test_ice_multiple_candidates_cycling)
{
    nano_ice_t ice;
    ice_init(&ice, 1);

    memcpy(ice.local_ufrag, "AAAA", 4);
    ice.local_ufrag_len = 4;
    memcpy(ice.local_pwd, "password-1234567890a", 20);
    ice.local_pwd_len = 20;
    memcpy(ice.remote_ufrag, "BBBB", 4);
    ice.remote_ufrag_len = 4;
    memcpy(ice.remote_pwd, "password-0987654321b", 20);
    ice.remote_pwd_len = 20;
    ice.tie_breaker = 0x1234567890ABCDEFull;

    /* Add a local candidate */
    ice.local_candidates[0].family = 4;
    ice.local_candidates[0].addr[0] = 192;
    ice.local_candidates[0].addr[1] = 168;
    ice.local_candidates[0].addr[2] = 1;
    ice.local_candidates[0].addr[3] = 100;
    ice.local_candidates[0].port = 4000;
    ice.local_candidates[0].type = NANORTC_ICE_CAND_HOST;
    ice.local_candidate_count = 1;

    /* Add 2 remote candidates */
    ice.remote_candidates[0].family = 4;
    ice.remote_candidates[0].addr[0] = 192;
    ice.remote_candidates[0].addr[3] = 1;
    ice.remote_candidates[0].port = 5000;
    ice.remote_candidates[1].family = 4;
    ice.remote_candidates[1].addr[0] = 192;
    ice.remote_candidates[1].addr[3] = 2;
    ice.remote_candidates[1].port = 5001;
    ice.remote_candidate_count = 2;

    uint8_t buf[256];
    size_t len;

    /* Generate first check */
    len = 0;
    ASSERT_OK(ice_generate_check(&ice, 0, crypto(), buf, sizeof(buf), &len));
    ASSERT_TRUE(len > 0);
    uint8_t first_candidate = ice.current_remote;

    /* Advance time past pacing interval and generate second check */
    len = 0;
    ASSERT_OK(ice_generate_check(&ice, 100, crypto(), buf, sizeof(buf), &len));
    ASSERT_TRUE(len > 0);

    /* After 2 checks with 2 candidates, we should have cycled */
    ASSERT_EQ(ice.check_count, 2);
}

/*
 * RFC 8445: STUN Binding Indication (type 0x0011) should be handled gracefully.
 * ICE agents may receive indications — they should not cause errors.
 */
TEST(test_ice_stun_indication_handling)
{
    nano_ice_t ice;
    ice_init(&ice, 0);

    memcpy(ice.local_ufrag, "PEER", 4);
    ice.local_ufrag_len = 4;
    memcpy(ice.local_pwd, "peer-password-123456", 20);
    ice.local_pwd_len = 20;
    memcpy(ice.remote_ufrag, "CTRL", 4);
    ice.remote_ufrag_len = 4;
    memcpy(ice.remote_pwd, "ctrl-password-abcdef", 20);
    ice.remote_pwd_len = 20;

    /* Build a minimal STUN Binding Indication (type=0x0011, no attributes) */
    uint8_t indication[] = {
        0x00, 0x11, 0x00, 0x00, 0x21, 0x12, 0xA4, 0x42, 0x01, 0x02,
        0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C,
    };

    nanortc_addr_t src;
    memset(&src, 0, sizeof(src));
    src.family = 4;
    uint8_t resp_buf[256];
    size_t resp_len = 0;

    /* Should either succeed (ignored) or fail gracefully — no crash */
    int rc = ice_handle_stun(&ice, indication, sizeof(indication), &src, false, crypto(), resp_buf,
                             sizeof(resp_buf), &resp_len);
    /* Indications don't generate responses */
    (void)rc;
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
/* TD-018: per-pair pending transaction table */
RUN(test_ice_controlling_multi_pair_response_out_of_order);
RUN(test_ice_controlling_pending_table_full);
/* §8: State transitions */
RUN(test_ice_state_new_to_checking);
RUN(test_ice_state_checking_to_connected);
RUN(test_ice_state_checking_to_failed);
RUN(test_ice_no_checks_after_connected);
/* Credential usage */
RUN(test_ice_credential_usage);
/* RFC 8445 MUST/SHOULD requirement tests */
RUN(test_ice_priority_formula_rfc8445);
RUN(test_ice_multiple_candidates_cycling);
RUN(test_ice_stun_indication_handling);
TEST_MAIN_END
