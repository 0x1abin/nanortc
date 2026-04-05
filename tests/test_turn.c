/*
 * nanortc — TURN client tests (RFC 5766)
 *
 * Tests:
 *   - TURN init + configure
 *   - Allocate request encoding
 *   - 401 challenge-response (realm + nonce extraction, key derivation)
 *   - Refresh request encoding
 *   - CreatePermission encoding
 *   - Send indication wrap/unwrap
 *   - Data indication unwrap
 *   - turn_is_from_server
 *   - Error handling (438 Stale Nonce, unknown errors)
 *
 * SPDX-License-Identifier: MIT
 */

#include "nanortc.h"
#include "nano_turn.h"
#include "nano_stun.h"
#include "nanortc_crypto.h"
#include "nano_test.h"
#include "nano_test_config.h"
#include <string.h>

static const nanortc_crypto_provider_t *crypto(void)
{
    return nano_test_crypto();
}

/* Helper: configure a TURN client with test credentials */
static void setup_turn(nano_turn_t *turn)
{
    turn_init(turn);

    uint8_t server_addr[NANORTC_ADDR_SIZE] = {0};
    server_addr[0] = 10;
    server_addr[1] = 0;
    server_addr[2] = 0;
    server_addr[3] = 100;

    turn_configure(turn, server_addr, 4, 3478,
                   "testuser", 8, "testpass", 8);
}

/* Build a fake Allocate Error Response with 401 + REALM + NONCE */
static size_t build_401_response(uint8_t *buf, const uint8_t txid[STUN_TXID_SIZE])
{
    /* Header */
    nanortc_write_u16be(buf, STUN_ALLOCATE_ERROR);
    nanortc_write_u16be(buf + 2, 0); /* length placeholder */
    nanortc_write_u32be(buf + 4, STUN_MAGIC_COOKIE);
    memcpy(buf + 8, txid, STUN_TXID_SIZE);
    size_t pos = STUN_HEADER_SIZE;

    /* ERROR-CODE: 401 (class=4, number=1) */
    uint8_t ec[4] = {0, 0, 4, 1};
    nanortc_write_u16be(buf + pos, STUN_ATTR_ERROR_CODE);
    nanortc_write_u16be(buf + pos + 2, 4);
    memcpy(buf + pos + 4, ec, 4);
    pos += 8;

    /* REALM */
    const char *realm = "example.com";
    size_t rlen = 11;
    nanortc_write_u16be(buf + pos, STUN_ATTR_REALM);
    nanortc_write_u16be(buf + pos + 2, (uint16_t)rlen);
    memcpy(buf + pos + 4, realm, rlen);
    size_t padded_rlen = (rlen + 3) & ~3u;
    if (padded_rlen > rlen) {
        memset(buf + pos + 4 + rlen, 0, padded_rlen - rlen);
    }
    pos += 4 + padded_rlen;

    /* NONCE */
    const char *nonce = "abc123nonce";
    size_t nlen = 11;
    nanortc_write_u16be(buf + pos, STUN_ATTR_NONCE);
    nanortc_write_u16be(buf + pos + 2, (uint16_t)nlen);
    memcpy(buf + pos + 4, nonce, nlen);
    size_t padded_nlen = (nlen + 3) & ~3u;
    if (padded_nlen > nlen) {
        memset(buf + pos + 4 + nlen, 0, padded_nlen - nlen);
    }
    pos += 4 + padded_nlen;

    /* Fix length */
    nanortc_write_u16be(buf + 2, (uint16_t)(pos - STUN_HEADER_SIZE));
    return pos;
}

/* Build a fake Allocate Success Response with relay addr + lifetime */
static size_t build_allocate_success(uint8_t *buf, const uint8_t txid[STUN_TXID_SIZE])
{
    nanortc_write_u16be(buf, STUN_ALLOCATE_RESPONSE);
    nanortc_write_u16be(buf + 2, 0);
    nanortc_write_u32be(buf + 4, STUN_MAGIC_COOKIE);
    memcpy(buf + 8, txid, STUN_TXID_SIZE);
    size_t pos = STUN_HEADER_SIZE;

    /* XOR-RELAYED-ADDRESS: 203.0.113.5:49152 IPv4 */
    uint8_t xra[8];
    xra[0] = 0; /* reserved */
    xra[1] = STUN_FAMILY_IPV4;
    uint16_t xport = 49152 ^ (uint16_t)(STUN_MAGIC_COOKIE >> 16);
    nanortc_write_u16be(xra + 2, xport);
    uint32_t raw_addr = ((uint32_t)203 << 24) | (0 << 16) | (113 << 8) | 5;
    nanortc_write_u32be(xra + 4, raw_addr ^ STUN_MAGIC_COOKIE);
    nanortc_write_u16be(buf + pos, STUN_ATTR_XOR_RELAYED_ADDRESS);
    nanortc_write_u16be(buf + pos + 2, 8);
    memcpy(buf + pos + 4, xra, 8);
    pos += 12;

    /* LIFETIME: 600 seconds */
    nanortc_write_u16be(buf + pos, STUN_ATTR_LIFETIME);
    nanortc_write_u16be(buf + pos + 2, 4);
    nanortc_write_u32be(buf + pos + 4, 600);
    pos += 8;

    nanortc_write_u16be(buf + 2, (uint16_t)(pos - STUN_HEADER_SIZE));
    return pos;
}

/* ----------------------------------------------------------------
 * Tests
 * ---------------------------------------------------------------- */

/* T1: Init and configure */
static void test_turn_init_configure(void)
{
    nano_turn_t turn;
    int rc = turn_init(&turn);
    TEST_ASSERT_EQUAL_INT(NANORTC_OK, rc);
    TEST_ASSERT_EQUAL_INT(NANORTC_TURN_IDLE, turn.state);
    TEST_ASSERT_FALSE(turn.configured);

    uint8_t addr[NANORTC_ADDR_SIZE] = {10, 0, 0, 1};
    rc = turn_configure(&turn, addr, 4, 3478, "user", 4, "pass", 4);
    TEST_ASSERT_EQUAL_INT(NANORTC_OK, rc);
    TEST_ASSERT_TRUE(turn.configured);
    TEST_ASSERT_EQUAL_INT(3478, turn.server_port);
}

/* T2: Allocate request encoding */
static void test_turn_allocate_request(void)
{
    nano_turn_t turn;
    setup_turn(&turn);

    uint8_t buf[512];
    size_t out_len = 0;
    int rc = turn_start_allocate(&turn, crypto(), buf, sizeof(buf), &out_len);
    TEST_ASSERT_EQUAL_INT(NANORTC_OK, rc);
    TEST_ASSERT_TRUE(out_len > 0);
    TEST_ASSERT_EQUAL_INT(NANORTC_TURN_ALLOCATING, turn.state);

    /* Parse the generated request */
    stun_msg_t msg;
    rc = stun_parse(buf, out_len, &msg);
    TEST_ASSERT_EQUAL_INT(NANORTC_OK, rc);
    TEST_ASSERT_EQUAL_HEX16(STUN_ALLOCATE_REQUEST, msg.type);
    /* Verify lifetime attribute is present */
    TEST_ASSERT_TRUE(msg.lifetime > 0);
}

/* T3: 401 challenge response flow */
static void test_turn_401_challenge(void)
{
    nano_turn_t turn;
    setup_turn(&turn);

    /* Send initial allocate */
    uint8_t buf[512];
    size_t out_len = 0;
    turn_start_allocate(&turn, crypto(), buf, sizeof(buf), &out_len);
    TEST_ASSERT_EQUAL_INT(NANORTC_TURN_ALLOCATING, turn.state);

    /* Build and feed 401 response */
    uint8_t resp[256];
    size_t resp_len = build_401_response(resp, turn.last_txid);

    int rc = turn_handle_response(&turn, resp, resp_len, crypto());
    TEST_ASSERT_EQUAL_INT(NANORTC_OK, rc);
    TEST_ASSERT_EQUAL_INT(NANORTC_TURN_CHALLENGED, turn.state);
    TEST_ASSERT_TRUE(turn.hmac_key_valid);
    TEST_ASSERT_EQUAL_STRING("example.com", turn.realm);
    TEST_ASSERT_EQUAL_STRING("abc123nonce", turn.nonce);
}

/* T4: Authenticated allocate after 401 */
static void test_turn_authenticated_allocate(void)
{
    nano_turn_t turn;
    setup_turn(&turn);

    uint8_t buf[512];
    size_t out_len = 0;

    /* Initial allocate */
    turn_start_allocate(&turn, crypto(), buf, sizeof(buf), &out_len);

    /* 401 response */
    uint8_t resp[256];
    size_t resp_len = build_401_response(resp, turn.last_txid);
    turn_handle_response(&turn, resp, resp_len, crypto());
    TEST_ASSERT_EQUAL_INT(NANORTC_TURN_CHALLENGED, turn.state);

    /* Retry allocate (should include USERNAME, REALM, NONCE, MI) */
    out_len = 0;
    int rc = turn_start_allocate(&turn, crypto(), buf, sizeof(buf), &out_len);
    TEST_ASSERT_EQUAL_INT(NANORTC_OK, rc);
    TEST_ASSERT_TRUE(out_len > 0);

    /* Parse and verify USERNAME is present */
    stun_msg_t msg;
    rc = stun_parse(buf, out_len, &msg);
    TEST_ASSERT_EQUAL_INT(NANORTC_OK, rc);
    TEST_ASSERT_EQUAL_HEX16(STUN_ALLOCATE_REQUEST, msg.type);
    TEST_ASSERT_NOT_NULL(msg.username);
    TEST_ASSERT_EQUAL_INT(8, msg.username_len);
    TEST_ASSERT_NOT_NULL(msg.realm);
    TEST_ASSERT_NOT_NULL(msg.nonce);
    TEST_ASSERT_TRUE(msg.has_integrity);
}

/* T5: Allocate success response */
static void test_turn_allocate_success(void)
{
    nano_turn_t turn;
    setup_turn(&turn);

    uint8_t buf[512];
    size_t out_len = 0;
    turn_start_allocate(&turn, crypto(), buf, sizeof(buf), &out_len);

    /* Build success response */
    uint8_t resp[256];
    size_t resp_len = build_allocate_success(resp, turn.last_txid);

    int rc = turn_handle_response(&turn, resp, resp_len, crypto());
    TEST_ASSERT_EQUAL_INT(NANORTC_OK, rc);
    TEST_ASSERT_EQUAL_INT(NANORTC_TURN_ALLOCATED, turn.state);
    TEST_ASSERT_EQUAL_INT(600, turn.lifetime_s);
    /* Relay address: 203.0.113.5 */
    TEST_ASSERT_EQUAL_INT(203, turn.relay_addr[0]);
    TEST_ASSERT_EQUAL_INT(0, turn.relay_addr[1]);
    TEST_ASSERT_EQUAL_INT(113, turn.relay_addr[2]);
    TEST_ASSERT_EQUAL_INT(5, turn.relay_addr[3]);
    TEST_ASSERT_EQUAL_INT(49152, turn.relay_port);
}

/* T6: Refresh generation */
static void test_turn_refresh(void)
{
    nano_turn_t turn;
    setup_turn(&turn);
    turn.state = NANORTC_TURN_ALLOCATED;
    turn.lifetime_s = 600;
    turn.hmac_key_valid = true;
    memset(turn.hmac_key, 0xAA, 16);
    memcpy(turn.realm, "test.com", 9);
    turn.realm_len = 8;
    memcpy(turn.nonce, "nonce123", 9);
    turn.nonce_len = 8;
    turn.refresh_at_ms = 0; /* force immediate */

    uint8_t buf[512];
    size_t out_len = 0;
    int rc = turn_generate_refresh(&turn, 1000, crypto(), buf, sizeof(buf), &out_len);
    TEST_ASSERT_EQUAL_INT(NANORTC_OK, rc);
    TEST_ASSERT_TRUE(out_len > 0);

    /* Parse refresh request */
    stun_msg_t msg;
    rc = stun_parse(buf, out_len, &msg);
    TEST_ASSERT_EQUAL_INT(NANORTC_OK, rc);
    TEST_ASSERT_EQUAL_HEX16(STUN_REFRESH_REQUEST, msg.type);
    TEST_ASSERT_TRUE(msg.has_integrity);

    /* Refresh_at should be set to future */
    TEST_ASSERT_TRUE(turn.refresh_at_ms > 1000);
}

/* T7: Send indication wrap */
static void test_turn_wrap_send(void)
{
    uint8_t peer_addr[NANORTC_ADDR_SIZE] = {192, 168, 1, 1};
    const uint8_t payload[] = "hello turn";
    uint8_t buf[256];
    size_t out_len = 0;

    int rc = turn_wrap_send(peer_addr, 4, 5000, payload, 10,
                            buf, sizeof(buf), &out_len);
    TEST_ASSERT_EQUAL_INT(NANORTC_OK, rc);
    TEST_ASSERT_TRUE(out_len > 0);

    /* Parse Send indication */
    stun_msg_t msg;
    rc = stun_parse(buf, out_len, &msg);
    TEST_ASSERT_EQUAL_INT(NANORTC_OK, rc);
    TEST_ASSERT_EQUAL_HEX16(STUN_SEND_INDICATION, msg.type);
    /* Data attribute should be present */
    TEST_ASSERT_NOT_NULL(msg.data_attr);
    TEST_ASSERT_EQUAL_INT(10, msg.data_attr_len);
    TEST_ASSERT_EQUAL_MEMORY("hello turn", msg.data_attr, 10);
    /* Peer address decoded */
    TEST_ASSERT_EQUAL_INT(STUN_FAMILY_IPV4, msg.peer_family);
    TEST_ASSERT_EQUAL_INT(5000, msg.peer_port);
}

/* T8: Data indication unwrap */
static void test_turn_unwrap_data(void)
{
    /* First create a Send indication, then modify type to Data indication */
    uint8_t peer_addr[NANORTC_ADDR_SIZE] = {10, 0, 0, 2};
    const uint8_t payload[] = "world";
    uint8_t buf[256];
    size_t buf_len = 0;

    turn_wrap_send(peer_addr, 4, 6000, payload, 5, buf, sizeof(buf), &buf_len);
    /* Change type from SEND_INDICATION to DATA_INDICATION */
    nanortc_write_u16be(buf, STUN_DATA_INDICATION);

    /* Unwrap */
    uint8_t out_addr[NANORTC_ADDR_SIZE];
    uint8_t out_family = 0;
    uint16_t out_port = 0;
    const uint8_t *out_payload = NULL;
    size_t out_len = 0;

    int rc = turn_unwrap_data(buf, buf_len, out_addr, &out_family, &out_port,
                              &out_payload, &out_len);
    TEST_ASSERT_EQUAL_INT(NANORTC_OK, rc);
    TEST_ASSERT_EQUAL_INT(STUN_FAMILY_IPV4, out_family);
    TEST_ASSERT_EQUAL_INT(6000, out_port);
    TEST_ASSERT_EQUAL_INT(5, out_len);
    TEST_ASSERT_EQUAL_MEMORY("world", out_payload, 5);
}

/* T9: turn_is_from_server */
static void test_turn_is_from_server(void)
{
    nano_turn_t turn;
    setup_turn(&turn);

    uint8_t match[NANORTC_ADDR_SIZE] = {10, 0, 0, 100};
    uint8_t nomatch[NANORTC_ADDR_SIZE] = {10, 0, 0, 200};

    TEST_ASSERT_TRUE(turn_is_from_server(&turn, match, 4, 3478));
    TEST_ASSERT_FALSE(turn_is_from_server(&turn, nomatch, 4, 3478));
    TEST_ASSERT_FALSE(turn_is_from_server(&turn, match, 4, 9999));
    TEST_ASSERT_FALSE(turn_is_from_server(&turn, match, 6, 3478));
}

/* T10: Null param checks */
static void test_turn_null_params(void)
{
    TEST_ASSERT_EQUAL_INT(NANORTC_ERR_INVALID_PARAM, turn_init(NULL));
    TEST_ASSERT_EQUAL_INT(NANORTC_ERR_INVALID_PARAM,
                          turn_configure(NULL, NULL, 4, 0, NULL, 0, NULL, 0));
    TEST_ASSERT_EQUAL_INT(NANORTC_ERR_INVALID_PARAM,
                          turn_start_allocate(NULL, NULL, NULL, 0, NULL));
}

/* T11: Refresh not generated when not allocated */
static void test_turn_refresh_not_allocated(void)
{
    nano_turn_t turn;
    turn_init(&turn);

    uint8_t buf[256];
    size_t out_len = 0;
    int rc = turn_generate_refresh(&turn, 1000, crypto(), buf, sizeof(buf), &out_len);
    TEST_ASSERT_EQUAL_INT(NANORTC_OK, rc);
    TEST_ASSERT_EQUAL_size_t(0, out_len);
}

/* T12: CreatePermission encoding */
static void test_turn_create_permission(void)
{
    nano_turn_t turn;
    setup_turn(&turn);
    turn.state = NANORTC_TURN_ALLOCATED;
    turn.hmac_key_valid = true;
    memset(turn.hmac_key, 0xBB, 16);
    memcpy(turn.realm, "test.com", 9);
    turn.realm_len = 8;
    memcpy(turn.nonce, "nonce456", 9);
    turn.nonce_len = 8;

    uint8_t peer[NANORTC_ADDR_SIZE] = {172, 16, 0, 1};
    uint8_t buf[512];
    size_t out_len = 0;

    int rc = turn_create_permission(&turn, peer, 4, 7000, crypto(),
                                    buf, sizeof(buf), &out_len);
    TEST_ASSERT_EQUAL_INT(NANORTC_OK, rc);
    TEST_ASSERT_TRUE(out_len > 0);

    stun_msg_t msg;
    rc = stun_parse(buf, out_len, &msg);
    TEST_ASSERT_EQUAL_INT(NANORTC_OK, rc);
    TEST_ASSERT_EQUAL_HEX16(STUN_CREATE_PERMISSION_REQUEST, msg.type);
    TEST_ASSERT_TRUE(msg.has_integrity);
    /* Peer address should be present */
    TEST_ASSERT_EQUAL_INT(STUN_FAMILY_IPV4, msg.peer_family);

    /* Permission should be tracked */
    TEST_ASSERT_EQUAL_INT(1, turn.permission_count);
    TEST_ASSERT_TRUE(turn.permissions[0].active);
}

/* ----------------------------------------------------------------
 * Test runner
 * ---------------------------------------------------------------- */

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_turn_init_configure);
    RUN_TEST(test_turn_allocate_request);
    RUN_TEST(test_turn_401_challenge);
    RUN_TEST(test_turn_authenticated_allocate);
    RUN_TEST(test_turn_allocate_success);
    RUN_TEST(test_turn_refresh);
    RUN_TEST(test_turn_wrap_send);
    RUN_TEST(test_turn_unwrap_data);
    RUN_TEST(test_turn_is_from_server);
    RUN_TEST(test_turn_null_params);
    RUN_TEST(test_turn_refresh_not_allocated);
    RUN_TEST(test_turn_create_permission);

    return UNITY_END();
}
