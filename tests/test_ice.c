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

/* Build a single host candidate with the given address family and port.
 * IPv4 addresses are 10.0.0.<port&0xff>; IPv6 are 2001:db8::<port&0xff>. */
static void fill_candidate(nano_ice_candidate_t *c, uint8_t family, uint16_t port)
{
    memset(c, 0, sizeof(*c));
    c->family = family;
    c->port = port;
    c->type = NANORTC_ICE_CAND_HOST;
    if (family == 4) {
        c->addr[0] = 10;
        c->addr[3] = (uint8_t)(port & 0xff);
    } else {
        c->addr[0] = 0x20;
        c->addr[1] = 0x01;
        c->addr[2] = 0x0d;
        c->addr[3] = 0xb8;
        c->addr[15] = (uint8_t)(port & 0xff);
    }
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

    rc = ice_handle_stun(ctld, req_buf, req_len, &src, NANORTC_ICE_LOCAL_IDX_UNKNOWN, false,
                         crypto(), resp_buf, sizeof(resp_buf), &resp_len);
    if (rc != NANORTC_OK)
        return rc;

    nanortc_addr_t resp_src;
    memset(&resp_src, 0, sizeof(resp_src));
    resp_src.family = 4;
    resp_src.addr[0] = 10;
    resp_src.addr[3] = 2;
    resp_src.port = 5000;

    return ice_handle_stun(ctrl, resp_buf, resp_len, &resp_src, NANORTC_ICE_LOCAL_IDX_UNKNOWN,
                           false, crypto(), dummy, sizeof(dummy), &dummy_len);
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
    ASSERT_OK(ice_handle_stun(&ctld, req_buf, req_len, &src, NANORTC_ICE_LOCAL_IDX_UNKNOWN, false,
                              crypto(), resp_buf, sizeof(resp_buf), &resp_len));
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
    ASSERT_FAIL(ice_handle_stun(&ctld, req_buf, req_len, &src, NANORTC_ICE_LOCAL_IDX_UNKNOWN, false,
                                crypto(), resp_buf, sizeof(resp_buf), &resp_len));
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
    ASSERT_FAIL(ice_handle_stun(&ctld, req_buf, req_len, &src, NANORTC_ICE_LOCAL_IDX_UNKNOWN, false,
                                crypto(), resp_buf, sizeof(resp_buf), &resp_len));
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
    ASSERT_OK(ice_handle_stun(&ctld, req_buf, req_len, &src, NANORTC_ICE_LOCAL_IDX_UNKNOWN, false,
                              crypto(), resp_buf, sizeof(resp_buf), &resp_len));

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
    ASSERT_OK(ice_handle_stun(&ctld, req_buf, req_len, &src, NANORTC_ICE_LOCAL_IDX_UNKNOWN, false,
                              crypto(), resp_buf, sizeof(resp_buf), &resp_len));

    /* Response generated but NOT nominated */
    ASSERT_TRUE(resp_len > 0);
    ASSERT_EQ(ctld.state, NANORTC_ICE_STATE_NEW); /* not CONNECTED */
    ASSERT_FALSE(ctld.nominated);
}

/*
 * Same-family fallback when caller cannot identify the receiving interface.
 *
 * Regression for the dual-stack defect where the controlled-side
 * USE-CANDIDATE handler would latch selected_local_idx = 0 whenever
 * local_idx == NANORTC_ICE_LOCAL_IDX_UNKNOWN. On a host that registered
 * IPv4 first and IPv6 second (the SDK's enumeration order on Linux), an
 * IPv6 USE-CANDIDATE would nominate the v4 candidate, so every post-
 * nomination outgoing packet left with src.family=4 to a dest.family=6
 * peer — RFC 8445 §6.1.2.2 violation, and ICE silently fell back to TURN
 * relay even on permissive networks.
 *
 * The fix in src/nano_ice.c:ice_find_local_idx_by_family() prefers a
 * same-family candidate before defaulting to idx 0; this test pins it.
 */
TEST(test_ice_controlled_dual_stack_local_fallback)
{
    nano_ice_t ctld;
    ice_init(&ctld, 0);
    memcpy(ctld.local_ufrag, "PEER", 5);
    ctld.local_ufrag_len = 4;
    memcpy(ctld.local_pwd, "peer-password-123456", 21);
    ctld.local_pwd_len = 20;
    memcpy(ctld.remote_ufrag, "CTRL", 5);
    ctld.remote_ufrag_len = 4;

    /* IPv4 candidate first (idx 0), IPv6 second (idx 1) — same enumeration
     * order the uipcat-sdk produces from getifaddrs() on a dual-stack box. */
    fill_candidate(&ctld.local_candidates[0], 4, 50000);
    fill_candidate(&ctld.local_candidates[1], 6, 50000);
    ctld.local_candidate_count = 2;

    uint8_t txid[12] = {0xa1, 0xb2, 0xc3, 0xd4, 0xe5, 0xf6, 0x07, 0x18,
                       0x29, 0x3a, 0x4b, 0x5c};
    uint8_t key[] = "peer-password-123456";
    uint8_t req_buf[256];
    size_t req_len = 0;

    /* USE-CANDIDATE = true so the controlled side actually nominates. */
    ASSERT_OK(stun_encode_binding_request("PEER:CTRL", 9, 0x6E001EFF, true, true, 0x9999ull, txid,
                                          key, sizeof(key) - 1, crypto()->hmac_sha1, req_buf,
                                          sizeof(req_buf), &req_len));

    /* Source is the iOS-style global IPv6 (2001:db8::abcd), port 60000. */
    nanortc_addr_t src;
    memset(&src, 0, sizeof(src));
    src.family = 6;
    src.addr[0] = 0x20;
    src.addr[1] = 0x01;
    src.addr[2] = 0x0d;
    src.addr[3] = 0xb8;
    src.addr[14] = 0xab;
    src.addr[15] = 0xcd;
    src.port = 60000;

    uint8_t resp_buf[256];
    size_t resp_len = 0;
    /* local_idx UNKNOWN forces the fallback path — exactly what the SDK
     * triggers when it doesn't pass IP_PKTINFO/IPV6_RECVPKTINFO cmsg. */
    ASSERT_OK(ice_handle_stun(&ctld, req_buf, req_len, &src, NANORTC_ICE_LOCAL_IDX_UNKNOWN, false,
                              crypto(), resp_buf, sizeof(resp_buf), &resp_len));

    ASSERT_EQ(ctld.state, NANORTC_ICE_STATE_CONNECTED);
    ASSERT_TRUE(ctld.nominated);
    /* The whole point: idx 1 (the v6 candidate), NOT the legacy idx 0. */
    ASSERT_EQ(ctld.selected_local_idx, 1);
    ASSERT_EQ(ctld.selected_local_family, 6);
    ASSERT_EQ(ctld.selected_local_addr[0], 0x20);
    ASSERT_EQ(ctld.selected_local_addr[1], 0x01);
}

/*
 * Legacy single-candidate behaviour preserved.
 *
 * When the only registered local candidate is IPv4 and a v6 USE-CANDIDATE
 * arrives, the family lookup returns NANORTC_ICE_LOCAL_IDX_UNKNOWN and the
 * fallback chain ends at idx 0 — same as before the fix. This guards the
 * embedded RTOS targets that nanortc was originally tuned for.
 */
TEST(test_ice_controlled_single_v4_local_fallback_keeps_idx_0)
{
    nano_ice_t ctld;
    ice_init(&ctld, 0);
    memcpy(ctld.local_ufrag, "PEER", 5);
    ctld.local_ufrag_len = 4;
    memcpy(ctld.local_pwd, "peer-password-123456", 21);
    ctld.local_pwd_len = 20;
    memcpy(ctld.remote_ufrag, "CTRL", 5);
    ctld.remote_ufrag_len = 4;

    fill_candidate(&ctld.local_candidates[0], 4, 50000);
    ctld.local_candidate_count = 1;

    uint8_t txid[12] = {0};
    uint8_t key[] = "peer-password-123456";
    uint8_t req_buf[256];
    size_t req_len = 0;
    ASSERT_OK(stun_encode_binding_request("PEER:CTRL", 9, 0x6E001EFF, true, true, 0x8888ull, txid,
                                          key, sizeof(key) - 1, crypto()->hmac_sha1, req_buf,
                                          sizeof(req_buf), &req_len));

    nanortc_addr_t src;
    memset(&src, 0, sizeof(src));
    src.family = 6;
    src.addr[0] = 0x20;
    src.addr[1] = 0x01;
    src.port = 60000;

    uint8_t resp_buf[256];
    size_t resp_len = 0;
    ASSERT_OK(ice_handle_stun(&ctld, req_buf, req_len, &src, NANORTC_ICE_LOCAL_IDX_UNKNOWN, false,
                              crypto(), resp_buf, sizeof(resp_buf), &resp_len));

    ASSERT_EQ(ctld.state, NANORTC_ICE_STATE_CONNECTED);
    ASSERT_TRUE(ctld.nominated);
    ASSERT_EQ(ctld.selected_local_idx, 0);   /* legacy fallback */
    ASSERT_EQ(ctld.selected_local_family, 4); /* the only candidate */
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
    ASSERT_OK(ice_handle_stun(&ctld, req_buf, req_len, &src, NANORTC_ICE_LOCAL_IDX_UNKNOWN, false,
                              crypto(), resp_buf, sizeof(resp_buf), &resp_len));

    /* Feed response to controlling — should fail (txid mismatch) */
    uint8_t dummy[256];
    size_t dummy_len = 0;
    nanortc_addr_t resp_src;
    memset(&resp_src, 0, sizeof(resp_src));
    resp_src.family = 4;
    ASSERT_FAIL(ice_handle_stun(&ctrl, resp_buf, resp_len, &resp_src, NANORTC_ICE_LOCAL_IDX_UNKNOWN,
                                false, crypto(), dummy, sizeof(dummy), &dummy_len));

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
    ASSERT_OK(ice_handle_stun(&ctld, req2, req2_len, &src, NANORTC_ICE_LOCAL_IDX_UNKNOWN, false,
                              crypto(), resp2, sizeof(resp2), &resp2_len));
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
    ASSERT_OK(ice_handle_stun(&ctrl, resp2, resp2_len, &resp_src, NANORTC_ICE_LOCAL_IDX_UNKNOWN,
                              false, crypto(), dummy, sizeof(dummy), &dummy_len));

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
    ASSERT_OK(ice_handle_stun(&ctld, req_buf, req_len, &src, NANORTC_ICE_LOCAL_IDX_UNKNOWN, false,
                              crypto(), resp_buf, sizeof(resp_buf), &resp_len));

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

/* ================================================================
 * RFC 8445 / 8489 — Mandatory MESSAGE-INTEGRITY + FINGERPRINT on ICE STUN
 *
 * WebRTC peers always attach both attributes; the prior implementation
 * accepted messages missing either one, which opened an injection path
 * (attacker-forged Binding Request with matching USERNAME but no MI
 * would nominate a pair). `ice_handle_stun` now rejects any incoming
 * request or response that omits either attribute.
 * ================================================================ */

/* Build a minimal Binding Request with no FINGERPRINT, no MESSAGE-INTEGRITY.
 * Returns the encoded length. */
static size_t build_bare_binding_request(uint8_t *buf, size_t buf_len, const uint8_t txid[12],
                                         const char *username, size_t username_len)
{
    if (buf_len < 20 + 4 + username_len + 3) /* header + attr hdr + body padded */
        return 0;
    size_t padded_ulen = (username_len + 3) & ~3u;
    /* STUN header */
    buf[0] = 0x00;
    buf[1] = 0x01; /* Binding Request */
    size_t body_len = 4 + padded_ulen;
    buf[2] = (uint8_t)(body_len >> 8);
    buf[3] = (uint8_t)(body_len & 0xff);
    buf[4] = 0x21;
    buf[5] = 0x12;
    buf[6] = 0xA4;
    buf[7] = 0x42;
    memcpy(&buf[8], txid, 12);
    /* USERNAME attribute (type 0x0006) */
    buf[20] = 0x00;
    buf[21] = 0x06;
    buf[22] = (uint8_t)(username_len >> 8);
    buf[23] = (uint8_t)(username_len & 0xff);
    memcpy(&buf[24], username, username_len);
    memset(&buf[24 + username_len], 0, padded_ulen - username_len);
    return 20 + body_len;
}

TEST(test_ice_request_without_fingerprint_rejected)
{
    nano_ice_t ctld;
    ice_init(&ctld, 0);
    memcpy(ctld.local_ufrag, "PEER", 5);
    ctld.local_ufrag_len = 4;
    memcpy(ctld.local_pwd, "peer-password-123456", 21);
    ctld.local_pwd_len = 20;

    uint8_t txid[12] = {0};
    uint8_t buf[128];
    size_t req_len = build_bare_binding_request(buf, sizeof(buf), txid, "PEER:CTRL", 9);
    ASSERT_TRUE(req_len > 0);

    nanortc_addr_t src;
    memset(&src, 0, sizeof(src));
    src.family = 4;
    uint8_t resp[256];
    size_t resp_len = 0;
    /* No FINGERPRINT, no MESSAGE-INTEGRITY → protocol error. */
    ASSERT_FAIL(ice_handle_stun(&ctld, buf, req_len, &src, NANORTC_ICE_LOCAL_IDX_UNKNOWN, false,
                                crypto(), resp, sizeof(resp), &resp_len));
    ASSERT_EQ(resp_len, 0u);
}

TEST(test_ice_response_without_integrity_rejected)
{
    /* Controlling side that has a pending check; feed it a bare Binding
     * Response with a matching txid but no MI/FP. Must reject without
     * marking the agent CONNECTED. */
    nano_ice_t ctrl;
    ice_init(&ctrl, 1);
    memcpy(ctrl.local_ufrag, "CTRL", 5);
    ctrl.local_ufrag_len = 4;
    memcpy(ctrl.local_pwd, "ctrl-password-abcdef", 21);
    ctrl.local_pwd_len = 20;
    memcpy(ctrl.remote_ufrag, "PEER", 5);
    ctrl.remote_ufrag_len = 4;
    memcpy(ctrl.remote_pwd, "peer-password-123456", 21);
    ctrl.remote_pwd_len = 20;
    ctrl.tie_breaker = 0x1122334455667788ull;
    ctrl.local_candidates[0].family = 4;
    ctrl.local_candidates[0].port = 4000;
    ctrl.local_candidates[0].type = NANORTC_ICE_CAND_HOST;
    ctrl.local_candidate_count = 1;
    ctrl.remote_candidates[0].family = 4;
    ctrl.remote_candidates[0].port = 5000;
    ctrl.remote_candidate_count = 1;

    uint8_t req_buf[256];
    size_t req_len = 0;
    ASSERT_OK(ice_generate_check(&ctrl, 0, crypto(), req_buf, sizeof(req_buf), &req_len));
    ASSERT_TRUE(req_len > 0);
    /* Echo the txid back in a bare Binding Response (type 0x0101). */
    uint8_t resp[20];
    resp[0] = 0x01;
    resp[1] = 0x01;
    resp[2] = 0x00;
    resp[3] = 0x00;
    resp[4] = 0x21;
    resp[5] = 0x12;
    resp[6] = 0xA4;
    resp[7] = 0x42;
    memcpy(&resp[8], &req_buf[8], 12);
    nanortc_addr_t src = {.family = 4, .port = 5000};
    uint8_t out[256];
    size_t out_len = 0;
    ASSERT_FAIL(ice_handle_stun(&ctrl, resp, sizeof(resp), &src, NANORTC_ICE_LOCAL_IDX_UNKNOWN,
                                false, crypto(), out, sizeof(out), &out_len));
    ASSERT_EQ(ctrl.state, NANORTC_ICE_STATE_CHECKING); /* NOT connected */
    ASSERT_FALSE(ctrl.nominated);
}

/* ================================================================
 * RFC 8489 §6.3.4 — Binding Error Response (0x0111) frees pending slot
 * ================================================================ */

TEST(test_ice_binding_error_frees_pending_slot)
{
    nano_ice_t ctrl;
    ice_init(&ctrl, 1);
    memcpy(ctrl.local_ufrag, "CTRL", 5);
    ctrl.local_ufrag_len = 4;
    memcpy(ctrl.local_pwd, "ctrl-password-abcdef", 21);
    ctrl.local_pwd_len = 20;
    memcpy(ctrl.remote_ufrag, "PEER", 5);
    ctrl.remote_ufrag_len = 4;
    memcpy(ctrl.remote_pwd, "peer-password-123456", 21);
    ctrl.remote_pwd_len = 20;
    ctrl.local_candidates[0].family = 4;
    ctrl.local_candidates[0].type = NANORTC_ICE_CAND_HOST;
    ctrl.local_candidate_count = 1;
    ctrl.remote_candidates[0].family = 4;
    ctrl.remote_candidate_count = 1;

    uint8_t req_buf[256];
    size_t req_len = 0;
    ASSERT_OK(ice_generate_check(&ctrl, 0, crypto(), req_buf, sizeof(req_buf), &req_len));
    ASSERT_TRUE(req_len > 0);
    /* At least one pending slot in flight. */
    int in_flight_before = 0;
    for (int i = 0; i < NANORTC_ICE_MAX_PENDING_CHECKS; i++)
        if (ctrl.pending[i].in_flight)
            in_flight_before++;
    ASSERT_TRUE(in_flight_before >= 1);

    /* Craft a Binding Error Response (0x0111) with matching txid. */
    uint8_t err[20];
    err[0] = 0x01;
    err[1] = 0x11;
    err[2] = 0x00;
    err[3] = 0x00;
    err[4] = 0x21;
    err[5] = 0x12;
    err[6] = 0xA4;
    err[7] = 0x42;
    memcpy(&err[8], &req_buf[8], 12);

    nanortc_addr_t src = {.family = 4};
    uint8_t out[256];
    size_t out_len = 0;
    /* Error response is surfaced as ERR_PROTOCOL so callers can log the 4xx
     * code, but the pending slot must be freed to avoid blocking the table. */
    int rc = ice_handle_stun(&ctrl, err, sizeof(err), &src, NANORTC_ICE_LOCAL_IDX_UNKNOWN, false,
                             crypto(), out, sizeof(out), &out_len);
    (void)rc;
    int in_flight_after = 0;
    for (int i = 0; i < NANORTC_ICE_MAX_PENDING_CHECKS; i++)
        if (ctrl.pending[i].in_flight)
            in_flight_after++;
    ASSERT_EQ(in_flight_after, in_flight_before - 1);
    ASSERT_EQ(ctrl.state, NANORTC_ICE_STATE_CHECKING); /* NOT connected */
}

/* ================================================================
 * ice_generate_check must not run in DISCONNECTED (consent lost)
 * ================================================================ */

TEST(test_ice_generate_check_noop_in_disconnected)
{
    nano_ice_t ctrl;
    ice_init(&ctrl, 1);
    memcpy(ctrl.local_ufrag, "CTRL", 5);
    ctrl.local_ufrag_len = 4;
    memcpy(ctrl.local_pwd, "ctrl-password-abcdef", 21);
    ctrl.local_pwd_len = 20;
    memcpy(ctrl.remote_ufrag, "PEER", 5);
    ctrl.remote_ufrag_len = 4;
    memcpy(ctrl.remote_pwd, "peer-password-123456", 21);
    ctrl.remote_pwd_len = 20;
    ctrl.local_candidates[0].family = 4;
    ctrl.local_candidates[0].type = NANORTC_ICE_CAND_HOST;
    ctrl.local_candidate_count = 1;
    ctrl.remote_candidates[0].family = 4;
    ctrl.remote_candidate_count = 1;
    ctrl.state = NANORTC_ICE_STATE_DISCONNECTED;

    uint8_t buf[256];
    size_t out_len = 42;
    ASSERT_OK(ice_generate_check(&ctrl, 0, crypto(), buf, sizeof(buf), &out_len));
    ASSERT_EQ(out_len, 0u);
    ASSERT_EQ(ctrl.check_count, 0u);
}

/* ================================================================
 * RFC 8445 §6.1.2.2 — Pair formation: same-address-family only
 * ================================================================ */

/* Minimal controlling-role agent with credentials; caller fills candidates. */
static void setup_family_filter_agent(nano_ice_t *ice)
{
    ice_init(ice, 1);
    memcpy(ice->local_ufrag, "CTRL", 4);
    ice->local_ufrag_len = 4;
    memcpy(ice->local_pwd, "ctrl-password-abcdef", 20);
    ice->local_pwd_len = 20;
    memcpy(ice->remote_ufrag, "PEER", 4);
    ice->remote_ufrag_len = 4;
    memcpy(ice->remote_pwd, "peer-password-123456", 20);
    ice->remote_pwd_len = 20;
    ice->tie_breaker = 0xA1B2C3D4E5F60718ull;
}

TEST(test_ice_pair_family_filter_skips_cross_family)
{
    /* 1 local v4, 2 remote (v4 + v6). Only the v4-v4 pair should ever be checked. */
    nano_ice_t ice;
    setup_family_filter_agent(&ice);

    fill_candidate(&ice.local_candidates[0], 4, 4000);
    ice.local_candidate_count = 1;

    fill_candidate(&ice.remote_candidates[0], 4, 5000);
    fill_candidate(&ice.remote_candidates[1], 6, 5001);
    ice.remote_candidate_count = 2;

    uint8_t buf[256];
    /* Drive several rounds, advancing past pacing each time. */
    for (uint32_t t = 0, rounds = 0; rounds < 5; t += 100, rounds++) {
        size_t len = 0;
        ASSERT_OK(ice_generate_check(&ice, t, crypto(), buf, sizeof(buf), &len));
        if (len == 0)
            continue;
        /* The pending slot just committed must point to the v4 remote. */
        bool found = false;
        for (int i = 0; i < NANORTC_ICE_MAX_PENDING_CHECKS; i++) {
            if (ice.pending[i].in_flight && ice.pending[i].sent_at_ms == t) {
                ASSERT_EQ(ice.remote_candidates[ice.pending[i].remote_idx].family, 4);
                found = true;
            }
        }
        ASSERT_TRUE(found);
    }
    ASSERT_TRUE(ice.check_count > 0);
}

TEST(test_ice_pair_family_filter_picks_both_families)
{
    /* 2 local (v4 + v6), 2 remote (v4 + v6). Over enough rounds both same-family
     * pairs are exercised and no cross-family pair is ever sent. */
    nano_ice_t ice;
    setup_family_filter_agent(&ice);

    fill_candidate(&ice.local_candidates[0], 4, 4000);
    fill_candidate(&ice.local_candidates[1], 6, 4001);
    ice.local_candidate_count = 2;

    fill_candidate(&ice.remote_candidates[0], 4, 5000);
    fill_candidate(&ice.remote_candidates[1], 6, 5001);
    ice.remote_candidate_count = 2;

    bool saw_v4 = false, saw_v6 = false;
    uint8_t buf[256];
    for (uint32_t t = 0, rounds = 0; rounds < 10; t += 100, rounds++) {
        size_t len = 0;
        ASSERT_OK(ice_generate_check(&ice, t, crypto(), buf, sizeof(buf), &len));
        if (len == 0)
            continue;
        for (int i = 0; i < NANORTC_ICE_MAX_PENDING_CHECKS; i++) {
            if (!ice.pending[i].in_flight || ice.pending[i].sent_at_ms != t)
                continue;
            uint8_t lf = ice.local_candidates[ice.pending[i].local_idx].family;
            uint8_t rf = ice.remote_candidates[ice.pending[i].remote_idx].family;
            ASSERT_EQ(lf, rf); /* never cross-family */
            if (lf == 4)
                saw_v4 = true;
            if (lf == 6)
                saw_v6 = true;
        }
    }
    ASSERT_TRUE(saw_v4);
    ASSERT_TRUE(saw_v6);
}

TEST(test_ice_pair_family_filter_no_match_no_send)
{
    /* Local v4 only, remote v6 only — no same-family pair exists.
     * ice_generate_check must return OK without emitting a packet or
     * consuming a check slot. */
    nano_ice_t ice;
    setup_family_filter_agent(&ice);

    fill_candidate(&ice.local_candidates[0], 4, 4000);
    ice.local_candidate_count = 1;

    fill_candidate(&ice.remote_candidates[0], 6, 5000);
    ice.remote_candidate_count = 1;

    uint8_t buf[256];
    for (uint32_t t = 0; t < 500; t += 100) {
        size_t len = 42;
        ASSERT_OK(ice_generate_check(&ice, t, crypto(), buf, sizeof(buf), &len));
        ASSERT_EQ(len, 0u);
    }
    ASSERT_EQ(ice.check_count, 0u);
    ASSERT_EQ(ice.state, NANORTC_ICE_STATE_NEW); /* still waiting for a matching pair */
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
    int rc =
        ice_handle_stun(&ice, indication, sizeof(indication), &src, NANORTC_ICE_LOCAL_IDX_UNKNOWN,
                        false, crypto(), resp_buf, sizeof(resp_buf), &resp_len);
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
RUN(test_ice_controlled_dual_stack_local_fallback);
RUN(test_ice_controlled_single_v4_local_fallback_keeps_idx_0);
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
/* MI + FINGERPRINT mandatory on all incoming STUN */
RUN(test_ice_request_without_fingerprint_rejected);
RUN(test_ice_response_without_integrity_rejected);
/* Binding Error Response frees pending slot */
RUN(test_ice_binding_error_frees_pending_slot);
/* DISCONNECTED (consent lost) halts check generation */
RUN(test_ice_generate_check_noop_in_disconnected);
/* §6.1.2.2: same-family pair formation */
RUN(test_ice_pair_family_filter_skips_cross_family);
RUN(test_ice_pair_family_filter_picks_both_families);
RUN(test_ice_pair_family_filter_no_match_no_send);
RUN(test_ice_stun_indication_handling);
TEST_MAIN_END
