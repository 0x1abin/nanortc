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

    turn_configure(turn, server_addr, 4, 3478, "testuser", 8, "testpass", 8);
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

    int rc = turn_wrap_send(peer_addr, 4, 5000, payload, 10, buf, sizeof(buf), &out_len);
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

    int rc =
        turn_unwrap_data(buf, buf_len, out_addr, &out_family, &out_port, &out_payload, &out_len);
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

    int rc = turn_create_permission(&turn, peer, 4, 7000, crypto(), buf, sizeof(buf), &out_len);
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

/* T13: Permission deduplication — same address twice */
static void test_turn_permission_dedup(void)
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

    int rc = turn_create_permission(&turn, peer, 4, 7000, crypto(), buf, sizeof(buf), &out_len);
    TEST_ASSERT_EQUAL_INT(NANORTC_OK, rc);
    TEST_ASSERT_EQUAL_INT(1, turn.permission_count);

    /* Same address again — should NOT add a second entry */
    out_len = 0;
    rc = turn_create_permission(&turn, peer, 4, 7000, crypto(), buf, sizeof(buf), &out_len);
    TEST_ASSERT_EQUAL_INT(NANORTC_OK, rc);
    TEST_ASSERT_EQUAL_INT(1, turn.permission_count);
    TEST_ASSERT_TRUE(turn.permissions[0].active);
}

/* T14: Permission distinct addresses */
static void test_turn_permission_distinct(void)
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

    uint8_t peer1[NANORTC_ADDR_SIZE] = {172, 16, 0, 1};
    uint8_t peer2[NANORTC_ADDR_SIZE] = {172, 16, 0, 2};
    uint8_t buf[512];
    size_t out_len = 0;

    turn_create_permission(&turn, peer1, 4, 7000, crypto(), buf, sizeof(buf), &out_len);
    TEST_ASSERT_EQUAL_INT(1, turn.permission_count);

    out_len = 0;
    turn_create_permission(&turn, peer2, 4, 8000, crypto(), buf, sizeof(buf), &out_len);
    TEST_ASSERT_EQUAL_INT(2, turn.permission_count);
}

/* Helper: set up a TURN client in ALLOCATED state with valid HMAC key */
static void setup_turn_allocated(nano_turn_t *turn)
{
    setup_turn(turn);
    turn->state = NANORTC_TURN_ALLOCATED;
    turn->hmac_key_valid = true;
    memset(turn->hmac_key, 0xBB, 16);
    memcpy(turn->realm, "test.com", 9);
    turn->realm_len = 8;
    memcpy(turn->nonce, "nonce456", 9);
    turn->nonce_len = 8;
}

/* T15: ChannelBind request encoding */
static void test_turn_channel_bind_request(void)
{
    nano_turn_t turn;
    setup_turn_allocated(&turn);

    uint8_t peer[NANORTC_ADDR_SIZE] = {192, 168, 1, 50};
    uint8_t buf[512];
    size_t out_len = 0;

    int rc = turn_channel_bind(&turn, peer, 4, 9000, crypto(), buf, sizeof(buf), &out_len);
    TEST_ASSERT_EQUAL_INT(NANORTC_OK, rc);
    TEST_ASSERT_TRUE(out_len > 0);
    TEST_ASSERT_EQUAL_INT(1, turn.channel_count);
    TEST_ASSERT_EQUAL_HEX16(0x4000, turn.channels[0].channel);

    /* Parse the generated request */
    stun_msg_t msg;
    rc = stun_parse(buf, out_len, &msg);
    TEST_ASSERT_EQUAL_INT(NANORTC_OK, rc);
    TEST_ASSERT_EQUAL_HEX16(STUN_CHANNEL_BIND_REQUEST, msg.type);
    TEST_ASSERT_EQUAL_HEX16(0x4000, msg.channel_number);
    TEST_ASSERT_EQUAL_INT(STUN_FAMILY_IPV4, msg.peer_family);
    TEST_ASSERT_TRUE(msg.has_integrity);
}

/* T16: ChannelBind success response */
static void test_turn_channel_bind_success(void)
{
    nano_turn_t turn;
    setup_turn_allocated(&turn);

    uint8_t peer[NANORTC_ADDR_SIZE] = {192, 168, 1, 50};
    uint8_t buf[512];
    size_t out_len = 0;

    turn_channel_bind(&turn, peer, 4, 9000, crypto(), buf, sizeof(buf), &out_len);
    TEST_ASSERT_FALSE(turn.channels[0].bound);

    /* Build a ChannelBind success response matching the txid */
    uint8_t resp[64];
    nanortc_write_u16be(resp, STUN_CHANNEL_BIND_RESPONSE);
    nanortc_write_u16be(resp + 2, 0);
    nanortc_write_u32be(resp + 4, STUN_MAGIC_COOKIE);
    memcpy(resp + 8, turn.channels[0].txid, STUN_TXID_SIZE);

    int rc = turn_handle_response(&turn, resp, STUN_HEADER_SIZE, crypto());
    TEST_ASSERT_EQUAL_INT(NANORTC_OK, rc);
    TEST_ASSERT_TRUE(turn.channels[0].bound);
}

/* T17: ChannelData wrap */
static void test_nano_turn_wrap_channel_data(void)
{
    const uint8_t payload[] = "hello channel";
    uint8_t buf[256];
    size_t out_len = 0;

    int rc = nano_turn_wrap_channel_data(0x4000, payload, 13, buf, sizeof(buf), &out_len);
    TEST_ASSERT_EQUAL_INT(NANORTC_OK, rc);
    /* Header (4) + payload (13) padded to 16 = 20 total */
    TEST_ASSERT_EQUAL_size_t(20, out_len);
    /* Verify header */
    TEST_ASSERT_EQUAL_HEX16(0x4000, nanortc_read_u16be(buf));
    TEST_ASSERT_EQUAL_INT(13, nanortc_read_u16be(buf + 2));
    TEST_ASSERT_EQUAL_MEMORY("hello channel", buf + 4, 13);
}

/* T18: ChannelData unwrap */
static void test_turn_unwrap_channel_data(void)
{
    /* Build ChannelData: channel 0x4001, "test" (4 bytes) */
    uint8_t data[8];
    nanortc_write_u16be(data, 0x4001);
    nanortc_write_u16be(data + 2, 4);
    memcpy(data + 4, "test", 4);

    uint16_t channel = 0;
    const uint8_t *payload = NULL;
    size_t payload_len = 0;

    int rc = turn_unwrap_channel_data(data, 8, &channel, &payload, &payload_len);
    TEST_ASSERT_EQUAL_INT(NANORTC_OK, rc);
    TEST_ASSERT_EQUAL_HEX16(0x4001, channel);
    TEST_ASSERT_EQUAL_INT(4, payload_len);
    TEST_ASSERT_EQUAL_MEMORY("test", payload, 4);
}

/* T19: Channel lookup (forward + reverse) */
static void test_turn_channel_lookup(void)
{
    nano_turn_t turn;
    setup_turn_allocated(&turn);

    uint8_t peer[NANORTC_ADDR_SIZE] = {10, 0, 0, 5};
    uint8_t buf[512];
    size_t out_len = 0;

    /* Bind and mark as bound */
    turn_channel_bind(&turn, peer, 4, 5000, crypto(), buf, sizeof(buf), &out_len);
    turn.channels[0].bound = true;

    /* Forward lookup: peer → channel */
    uint16_t ch = 0;
    TEST_ASSERT_TRUE(turn_find_channel_for_peer(&turn, peer, 4, &ch));
    TEST_ASSERT_EQUAL_HEX16(0x4000, ch);

    /* Reverse lookup: channel → peer */
    uint8_t out_addr[NANORTC_ADDR_SIZE];
    uint8_t out_family = 0;
    uint16_t out_port = 0;
    TEST_ASSERT_TRUE(turn_find_peer_for_channel(&turn, 0x4000, out_addr, &out_family, &out_port));
    TEST_ASSERT_EQUAL_INT(4, out_family);
    TEST_ASSERT_EQUAL_INT(5000, out_port);
    TEST_ASSERT_EQUAL_INT(10, out_addr[0]);

    /* Lookup unbound peer returns false */
    uint8_t unknown[NANORTC_ADDR_SIZE] = {99, 99, 99, 99};
    TEST_ASSERT_FALSE(turn_find_channel_for_peer(&turn, unknown, 4, &ch));
    TEST_ASSERT_FALSE(turn_find_peer_for_channel(&turn, 0x4FFF, out_addr, &out_family, &out_port));
}

/* T20: turn_is_channel_data first-byte detection */
static void test_turn_is_channel_data(void)
{
    uint8_t cd[4] = {0x40, 0x00, 0x00, 0x04}; /* channel 0x4000, len 4 */
    TEST_ASSERT_TRUE(turn_is_channel_data(cd, 4));

    uint8_t cd2[4] = {0x7F, 0xFF, 0x00, 0x00};
    TEST_ASSERT_TRUE(turn_is_channel_data(cd2, 4));

    uint8_t stun[4] = {0x00, 0x01, 0x00, 0x00}; /* STUN binding request */
    TEST_ASSERT_FALSE(turn_is_channel_data(stun, 4));

    uint8_t dtls[4] = {0x14, 0x03, 0x01, 0x00};
    TEST_ASSERT_FALSE(turn_is_channel_data(dtls, 4));

    /* Too short */
    TEST_ASSERT_FALSE(turn_is_channel_data(cd, 3));
    TEST_ASSERT_FALSE(turn_is_channel_data(NULL, 0));
}

/* T21: Channel number allocation — unique sequential */
static void test_turn_channel_number_allocation(void)
{
    nano_turn_t turn;
    setup_turn_allocated(&turn);

    uint8_t buf[512];
    size_t out_len = 0;

    uint8_t peer1[NANORTC_ADDR_SIZE] = {10, 0, 0, 1};
    uint8_t peer2[NANORTC_ADDR_SIZE] = {10, 0, 0, 2};
    uint8_t peer3[NANORTC_ADDR_SIZE] = {10, 0, 0, 3};

    turn_channel_bind(&turn, peer1, 4, 5001, crypto(), buf, sizeof(buf), &out_len);
    turn_channel_bind(&turn, peer2, 4, 5002, crypto(), buf, sizeof(buf), &out_len);
    turn_channel_bind(&turn, peer3, 4, 5003, crypto(), buf, sizeof(buf), &out_len);

    TEST_ASSERT_EQUAL_INT(3, turn.channel_count);
    TEST_ASSERT_EQUAL_HEX16(0x4000, turn.channels[0].channel);
    TEST_ASSERT_EQUAL_HEX16(0x4001, turn.channels[1].channel);
    TEST_ASSERT_EQUAL_HEX16(0x4002, turn.channels[2].channel);

    /* Same peer reuses existing channel */
    out_len = 0;
    turn_channel_bind(&turn, peer1, 4, 5001, crypto(), buf, sizeof(buf), &out_len);
    TEST_ASSERT_EQUAL_INT(3, turn.channel_count); /* no new channel */
}

/* T22: Permission refresh generates output when due */
static void test_turn_permission_refresh(void)
{
    nano_turn_t turn;
    setup_turn_allocated(&turn);

    /* Add a permission first */
    uint8_t peer[NANORTC_ADDR_SIZE] = {172, 16, 0, 1};
    uint8_t buf[512];
    size_t out_len = 0;
    turn_create_permission(&turn, peer, 4, 7000, crypto(), buf, sizeof(buf), &out_len);
    TEST_ASSERT_EQUAL_INT(1, turn.permission_count);

    /* Force refresh due */
    turn.permission_at_ms = 0;
    out_len = 0;
    int rc = turn_generate_permission_refresh(&turn, 1000, crypto(), buf, sizeof(buf), &out_len);
    TEST_ASSERT_EQUAL_INT(NANORTC_OK, rc);
    TEST_ASSERT_TRUE(out_len > 0);
    TEST_ASSERT_TRUE(turn.permission_at_ms > 1000);
}

/* T23: Permission refresh not due yet */
static void test_turn_permission_refresh_not_due(void)
{
    nano_turn_t turn;
    setup_turn_allocated(&turn);

    uint8_t peer[NANORTC_ADDR_SIZE] = {172, 16, 0, 1};
    uint8_t buf[512];
    size_t out_len = 0;
    turn_create_permission(&turn, peer, 4, 7000, crypto(), buf, sizeof(buf), &out_len);

    turn.permission_at_ms = 999999; /* far future */
    out_len = 0;
    int rc = turn_generate_permission_refresh(&turn, 1000, crypto(), buf, sizeof(buf), &out_len);
    TEST_ASSERT_EQUAL_INT(NANORTC_OK, rc);
    TEST_ASSERT_EQUAL_size_t(0, out_len);
}

/* T24: Channel refresh generates ChannelBind when due */
static void test_turn_channel_refresh(void)
{
    nano_turn_t turn;
    setup_turn_allocated(&turn);

    uint8_t peer[NANORTC_ADDR_SIZE] = {10, 0, 0, 10};
    uint8_t buf[512];
    size_t out_len = 0;
    turn_channel_bind(&turn, peer, 4, 6000, crypto(), buf, sizeof(buf), &out_len);
    turn.channels[0].bound = true;
    turn.channels[0].refresh_at_ms = 0; /* force due */

    out_len = 0;
    int rc = turn_generate_channel_refresh(&turn, 1000, crypto(), buf, sizeof(buf), &out_len);
    TEST_ASSERT_EQUAL_INT(NANORTC_OK, rc);
    TEST_ASSERT_TRUE(out_len > 0);

    /* Verify it's a ChannelBind request */
    stun_msg_t msg;
    rc = stun_parse(buf, out_len, &msg);
    TEST_ASSERT_EQUAL_INT(NANORTC_OK, rc);
    TEST_ASSERT_EQUAL_HEX16(STUN_CHANNEL_BIND_REQUEST, msg.type);

    /* Refresh timer should be updated */
    TEST_ASSERT_TRUE(turn.channels[0].refresh_at_ms > 1000);
}

/* Helper: build a generic error response with error code + optional NONCE */
static size_t build_error_response(uint8_t *buf, uint16_t msg_type,
                                   const uint8_t txid[STUN_TXID_SIZE], uint16_t error_code,
                                   const char *nonce)
{
    nanortc_write_u16be(buf, msg_type);
    nanortc_write_u16be(buf + 2, 0); /* length placeholder */
    nanortc_write_u32be(buf + 4, STUN_MAGIC_COOKIE);
    memcpy(buf + 8, txid, STUN_TXID_SIZE);
    size_t pos = STUN_HEADER_SIZE;

    /* ERROR-CODE */
    uint8_t ec[4] = {0, 0, (uint8_t)(error_code / 100), (uint8_t)(error_code % 100)};
    nanortc_write_u16be(buf + pos, STUN_ATTR_ERROR_CODE);
    nanortc_write_u16be(buf + pos + 2, 4);
    memcpy(buf + pos + 4, ec, 4);
    pos += 8;

    /* NONCE (optional) */
    if (nonce) {
        size_t nlen = 0;
        while (nonce[nlen])
            nlen++;
        nanortc_write_u16be(buf + pos, STUN_ATTR_NONCE);
        nanortc_write_u16be(buf + pos + 2, (uint16_t)nlen);
        memcpy(buf + pos + 4, nonce, nlen);
        size_t padded = (nlen + 3) & ~3u;
        if (padded > nlen)
            memset(buf + pos + 4 + nlen, 0, padded - nlen);
        pos += 4 + padded;
    }

    nanortc_write_u16be(buf + 2, (uint16_t)(pos - STUN_HEADER_SIZE));
    return pos;
}

/* Helper: build a success response (header only, no attributes) */
static size_t build_success_response(uint8_t *buf, uint16_t msg_type,
                                     const uint8_t txid[STUN_TXID_SIZE])
{
    nanortc_write_u16be(buf, msg_type);
    nanortc_write_u16be(buf + 2, 0);
    nanortc_write_u32be(buf + 4, STUN_MAGIC_COOKIE);
    memcpy(buf + 8, txid, STUN_TXID_SIZE);
    return STUN_HEADER_SIZE;
}

/* T25: 438 Stale Nonce on Allocate — updates nonce and retries */
static void test_turn_allocate_438_stale_nonce(void)
{
    nano_turn_t turn;
    setup_turn(&turn);

    uint8_t buf[512];
    size_t out_len = 0;
    turn_start_allocate(&turn, crypto(), buf, sizeof(buf), &out_len);

    /* Feed 438 with new nonce */
    uint8_t resp[256];
    size_t resp_len =
        build_error_response(resp, STUN_ALLOCATE_ERROR, turn.last_txid, 438, "newnonce99");

    int rc = turn_handle_response(&turn, resp, resp_len, crypto());
    TEST_ASSERT_EQUAL_INT(NANORTC_OK, rc);
    TEST_ASSERT_EQUAL_INT(NANORTC_TURN_CHALLENGED, turn.state);
    TEST_ASSERT_EQUAL_STRING("newnonce99", turn.nonce);
}

/* T26: 438 Stale Nonce on Allocate without nonce attr — fails */
static void test_turn_allocate_438_no_nonce(void)
{
    nano_turn_t turn;
    setup_turn(&turn);

    uint8_t buf[512];
    size_t out_len = 0;
    turn_start_allocate(&turn, crypto(), buf, sizeof(buf), &out_len);

    uint8_t resp[256];
    size_t resp_len =
        build_error_response(resp, STUN_ALLOCATE_ERROR, turn.last_txid, 438, NULL);

    int rc = turn_handle_response(&turn, resp, resp_len, crypto());
    TEST_ASSERT_EQUAL_INT(NANORTC_ERR_PROTOCOL, rc);
    TEST_ASSERT_EQUAL_INT(NANORTC_TURN_FAILED, turn.state);
}

/* T27: Unknown allocate error (e.g. 500) — fails */
static void test_turn_allocate_unknown_error(void)
{
    nano_turn_t turn;
    setup_turn(&turn);

    uint8_t buf[512];
    size_t out_len = 0;
    turn_start_allocate(&turn, crypto(), buf, sizeof(buf), &out_len);

    uint8_t resp[256];
    size_t resp_len =
        build_error_response(resp, STUN_ALLOCATE_ERROR, turn.last_txid, 500, NULL);

    int rc = turn_handle_response(&turn, resp, resp_len, crypto());
    TEST_ASSERT_EQUAL_INT(NANORTC_ERR_PROTOCOL, rc);
    TEST_ASSERT_EQUAL_INT(NANORTC_TURN_FAILED, turn.state);
}

/* T28: Refresh success response */
static void test_turn_refresh_success_response(void)
{
    nano_turn_t turn;
    setup_turn_allocated(&turn);
    turn.lifetime_s = 600;
    turn.refresh_at_ms = 0;

    /* Generate refresh request to set last_txid */
    uint8_t buf[512];
    size_t out_len = 0;
    turn_generate_refresh(&turn, 1000, crypto(), buf, sizeof(buf), &out_len);

    /* Build Refresh Success Response with LIFETIME */
    uint8_t resp[128];
    nanortc_write_u16be(resp, STUN_REFRESH_RESPONSE);
    nanortc_write_u16be(resp + 2, 0);
    nanortc_write_u32be(resp + 4, STUN_MAGIC_COOKIE);
    memcpy(resp + 8, turn.last_txid, STUN_TXID_SIZE);
    size_t pos = STUN_HEADER_SIZE;
    /* LIFETIME: 900 seconds */
    nanortc_write_u16be(resp + pos, STUN_ATTR_LIFETIME);
    nanortc_write_u16be(resp + pos + 2, 4);
    nanortc_write_u32be(resp + pos + 4, 900);
    pos += 8;
    nanortc_write_u16be(resp + 2, (uint16_t)(pos - STUN_HEADER_SIZE));

    int rc = turn_handle_response(&turn, resp, pos, crypto());
    TEST_ASSERT_EQUAL_INT(NANORTC_OK, rc);
    TEST_ASSERT_EQUAL_INT(900, turn.lifetime_s);
}

/* T29: Refresh error 438 — stale nonce, retry */
static void test_turn_refresh_438_stale_nonce(void)
{
    nano_turn_t turn;
    setup_turn_allocated(&turn);
    turn.refresh_at_ms = 0;

    uint8_t buf[512];
    size_t out_len = 0;
    turn_generate_refresh(&turn, 1000, crypto(), buf, sizeof(buf), &out_len);

    uint8_t resp[256];
    size_t resp_len =
        build_error_response(resp, STUN_REFRESH_ERROR, turn.last_txid, 438, "fresh_nonce");

    int rc = turn_handle_response(&turn, resp, resp_len, crypto());
    TEST_ASSERT_EQUAL_INT(NANORTC_OK, rc);
    TEST_ASSERT_EQUAL_STRING("fresh_nonce", turn.nonce);
    TEST_ASSERT_EQUAL_UINT32(0, turn.refresh_at_ms); /* retry immediately */
}

/* T30: Refresh error non-438 — fails */
static void test_turn_refresh_error_fails(void)
{
    nano_turn_t turn;
    setup_turn_allocated(&turn);
    turn.refresh_at_ms = 0;

    uint8_t buf[512];
    size_t out_len = 0;
    turn_generate_refresh(&turn, 1000, crypto(), buf, sizeof(buf), &out_len);

    uint8_t resp[256];
    size_t resp_len =
        build_error_response(resp, STUN_REFRESH_ERROR, turn.last_txid, 403, NULL);

    int rc = turn_handle_response(&turn, resp, resp_len, crypto());
    TEST_ASSERT_EQUAL_INT(NANORTC_ERR_PROTOCOL, rc);
    TEST_ASSERT_EQUAL_INT(NANORTC_TURN_FAILED, turn.state);
}

/* T31: CreatePermission success response — must echo our txid (F1) */
static void test_turn_permission_success_response(void)
{
    nano_turn_t turn;
    setup_turn_allocated(&turn);

    uint8_t peer[NANORTC_ADDR_SIZE] = {172, 16, 0, 1};
    uint8_t buf[512];
    size_t out_len = 0;
    turn_create_permission(&turn, peer, 4, 7000, crypto(), buf, sizeof(buf), &out_len);

    /* Build success response echoing the per-permission txid stored
     * during turn_create_permission(). */
    uint8_t resp[64];
    size_t resp_len = build_success_response(resp, STUN_CREATE_PERMISSION_RESPONSE,
                                             turn.permissions[0].txid);

    int rc = turn_handle_response(&turn, resp, resp_len, crypto());
    TEST_ASSERT_EQUAL_INT(NANORTC_OK, rc);
}

/* T32: CreatePermission error 438 — stale nonce, must echo our txid (F1) */
static void test_turn_permission_438_stale_nonce(void)
{
    nano_turn_t turn;
    setup_turn_allocated(&turn);

    /* First send a permission so the client has a txid in flight. */
    uint8_t peer[NANORTC_ADDR_SIZE] = {172, 16, 0, 1};
    uint8_t buf[512];
    size_t out_len = 0;
    turn_create_permission(&turn, peer, 4, 7000, crypto(), buf, sizeof(buf), &out_len);

    uint8_t resp[256];
    size_t resp_len = build_error_response(resp, STUN_CREATE_PERMISSION_ERROR,
                                           turn.permissions[0].txid, 438, "perm_nonce");

    int rc = turn_handle_response(&turn, resp, resp_len, crypto());
    TEST_ASSERT_EQUAL_INT(NANORTC_OK, rc);
    TEST_ASSERT_EQUAL_STRING("perm_nonce", turn.nonce);
}

/* T33: CreatePermission error non-438 — fails (F1: must echo our txid first) */
static void test_turn_permission_error_fails(void)
{
    nano_turn_t turn;
    setup_turn_allocated(&turn);

    /* Send a permission so the txid is in flight. */
    uint8_t peer[NANORTC_ADDR_SIZE] = {172, 16, 0, 1};
    uint8_t buf[512];
    size_t out_len = 0;
    turn_create_permission(&turn, peer, 4, 7000, crypto(), buf, sizeof(buf), &out_len);

    uint8_t resp[256];
    size_t resp_len =
        build_error_response(resp, STUN_CREATE_PERMISSION_ERROR, turn.permissions[0].txid, 403,
                             NULL);

    int rc = turn_handle_response(&turn, resp, resp_len, crypto());
    TEST_ASSERT_EQUAL_INT(NANORTC_ERR_PROTOCOL, rc);
}

/* T34: ChannelBind error 438 — stale nonce */
static void test_turn_channel_bind_438_stale_nonce(void)
{
    nano_turn_t turn;
    setup_turn_allocated(&turn);

    uint8_t peer[NANORTC_ADDR_SIZE] = {10, 0, 0, 5};
    uint8_t buf[512];
    size_t out_len = 0;
    turn_channel_bind(&turn, peer, 4, 5000, crypto(), buf, sizeof(buf), &out_len);

    uint8_t resp[256];
    size_t resp_len =
        build_error_response(resp, STUN_CHANNEL_BIND_ERROR, turn.channels[0].txid, 438,
                             "chan_nonce");

    int rc = turn_handle_response(&turn, resp, resp_len, crypto());
    TEST_ASSERT_EQUAL_INT(NANORTC_OK, rc);
    TEST_ASSERT_EQUAL_STRING("chan_nonce", turn.nonce);
}

/* T35: ChannelBind error non-438 — fails */
static void test_turn_channel_bind_error_fails(void)
{
    nano_turn_t turn;
    setup_turn_allocated(&turn);

    uint8_t peer[NANORTC_ADDR_SIZE] = {10, 0, 0, 5};
    uint8_t buf[512];
    size_t out_len = 0;
    turn_channel_bind(&turn, peer, 4, 5000, crypto(), buf, sizeof(buf), &out_len);

    uint8_t resp[256];
    size_t resp_len =
        build_error_response(resp, STUN_CHANNEL_BIND_ERROR, turn.channels[0].txid, 403, NULL);

    int rc = turn_handle_response(&turn, resp, resp_len, crypto());
    TEST_ASSERT_EQUAL_INT(NANORTC_ERR_PROTOCOL, rc);
}

/* T36: ChannelData unwrap error paths */
static void test_turn_unwrap_channel_data_errors(void)
{
    uint16_t ch = 0;
    const uint8_t *payload = NULL;
    size_t payload_len = 0;

    /* NULL params */
    TEST_ASSERT_EQUAL_INT(NANORTC_ERR_INVALID_PARAM,
                          turn_unwrap_channel_data(NULL, 8, &ch, &payload, &payload_len));
    /* Too short */
    uint8_t short_data[2] = {0x40, 0x00};
    TEST_ASSERT_EQUAL_INT(NANORTC_ERR_PARSE,
                          turn_unwrap_channel_data(short_data, 2, &ch, &payload, &payload_len));
    /* Invalid channel range (below 0x4000) */
    uint8_t bad_chan[8] = {0};
    nanortc_write_u16be(bad_chan, 0x3FFF);
    nanortc_write_u16be(bad_chan + 2, 4);
    TEST_ASSERT_EQUAL_INT(NANORTC_ERR_PROTOCOL,
                          turn_unwrap_channel_data(bad_chan, 8, &ch, &payload, &payload_len));
    /* Payload length exceeds buffer */
    uint8_t trunc[6];
    nanortc_write_u16be(trunc, 0x4000);
    nanortc_write_u16be(trunc + 2, 100); /* claims 100 bytes but only 2 available */
    TEST_ASSERT_EQUAL_INT(NANORTC_ERR_PARSE,
                          turn_unwrap_channel_data(trunc, 6, &ch, &payload, &payload_len));
}

/* T37: find_channel/find_peer null params */
static void test_turn_find_null_params(void)
{
    uint16_t ch = 0;
    uint8_t addr[NANORTC_ADDR_SIZE];
    uint8_t fam = 0;
    uint16_t port = 0;

    TEST_ASSERT_FALSE(turn_find_channel_for_peer(NULL, addr, 4, &ch));
    TEST_ASSERT_FALSE(turn_find_peer_for_channel(NULL, 0x4000, addr, &fam, &port));
}

/* T38: generate_permission_refresh null/state errors */
static void test_turn_permission_refresh_errors(void)
{
    uint8_t buf[512];
    size_t out_len = 0;

    TEST_ASSERT_EQUAL_INT(NANORTC_ERR_INVALID_PARAM,
                          turn_generate_permission_refresh(NULL, 0, crypto(), buf, sizeof(buf),
                                                          &out_len));

    /* Not allocated state */
    nano_turn_t turn;
    turn_init(&turn);
    out_len = 0;
    int rc = turn_generate_permission_refresh(&turn, 0, crypto(), buf, sizeof(buf), &out_len);
    TEST_ASSERT_EQUAL_INT(NANORTC_OK, rc);
    TEST_ASSERT_EQUAL_size_t(0, out_len);
}

/* T39: generate_channel_refresh null/state errors */
static void test_turn_channel_refresh_errors(void)
{
    uint8_t buf[512];
    size_t out_len = 0;

    TEST_ASSERT_EQUAL_INT(NANORTC_ERR_INVALID_PARAM,
                          turn_generate_channel_refresh(NULL, 0, crypto(), buf, sizeof(buf),
                                                       &out_len));

    /* Not allocated state */
    nano_turn_t turn;
    turn_init(&turn);
    out_len = 0;
    int rc = turn_generate_channel_refresh(&turn, 0, crypto(), buf, sizeof(buf), &out_len);
    TEST_ASSERT_EQUAL_INT(NANORTC_OK, rc);
    TEST_ASSERT_EQUAL_size_t(0, out_len);
}

/* ----------------------------------------------------------------
 * Phase 5.1 hardening tests — RFC 5766 / RFC 8656 / RFC 8489
 * ---------------------------------------------------------------- */

/* Helper: count outstanding occurrences of a STUN attribute in a parsed message buffer */
static int rfc_count_attribute(const uint8_t *buf, size_t len, uint16_t target)
{
    if (len < STUN_HEADER_SIZE) {
        return -1;
    }
    int count = 0;
    size_t pos = STUN_HEADER_SIZE;
    while (pos + 4 <= len) {
        uint16_t type = (uint16_t)((buf[pos] << 8) | buf[pos + 1]);
        uint16_t alen = (uint16_t)((buf[pos + 2] << 8) | buf[pos + 3]);
        if (type == target) {
            count++;
        }
        size_t padded = ((size_t)alen + 3u) & ~3u;
        if (pos + 4 + padded > len) {
            break;
        }
        pos += 4 + padded;
    }
    return count;
}

/* H1 (F2): RFC 8656 §12 channel range — 0x4FFF and 0x5000 are not valid binding numbers */
static void test_turn_channel_number_range(void)
{
    nano_turn_t turn;
    setup_turn_allocated(&turn);

    /* Bypass the per-instance MAX_CHANNELS limit (which is 4 by default) so
     * that we can exhaust the channel-number space directly. */
    turn.next_channel = 0x4FFE; /* the very last legal binding */

    uint8_t buf[512];
    size_t out_len = 0;
    uint8_t peer1[NANORTC_ADDR_SIZE] = {10, 0, 0, 1};
    int rc = turn_channel_bind(&turn, peer1, 4, 5001, crypto(), buf, sizeof(buf), &out_len);
    TEST_ASSERT_EQUAL_INT(NANORTC_OK, rc);
    TEST_ASSERT_EQUAL_HEX16(0x4FFE, turn.channels[0].channel);

    /* Next allocation must be rejected: 0x4FFF is reserved (8656 §12). */
    turn.channel_count = 0; /* reset slot reuse but keep next_channel advanced */
    out_len = 0;
    uint8_t peer2[NANORTC_ADDR_SIZE] = {10, 0, 0, 2};
    rc = turn_channel_bind(&turn, peer2, 4, 5002, crypto(), buf, sizeof(buf), &out_len);
    TEST_ASSERT_EQUAL_INT(NANORTC_ERR_BUFFER_TOO_SMALL, rc);
    TEST_ASSERT_EQUAL_size_t(0, out_len);
}

/* H2: ChannelData padding to 4-byte boundary (RFC 5766 §11.5) */
static void test_turn_channel_data_padding(void)
{
    uint8_t payload[5] = {0xDE, 0xAD, 0xBE, 0xEF, 0x42};
    uint8_t buf[64];
    size_t out_len = 0;

    int rc = nano_turn_wrap_channel_data(0x4001, payload, sizeof(payload), buf, sizeof(buf),
                                         &out_len);
    TEST_ASSERT_EQUAL_INT(NANORTC_OK, rc);
    /* 4 (header) + 5 (payload) + 3 (padding to 8) = 12 */
    TEST_ASSERT_EQUAL_size_t(12, out_len);
    /* Length field is the *unpadded* payload length per RFC 5766 §11.4 */
    TEST_ASSERT_EQUAL_HEX16(5, (uint16_t)((buf[2] << 8) | buf[3]));
    /* Padding bytes should be zero. */
    TEST_ASSERT_EQUAL_INT(0, buf[9]);
    TEST_ASSERT_EQUAL_INT(0, buf[10]);
    TEST_ASSERT_EQUAL_INT(0, buf[11]);
}

/* H3: Send Indication MUST NOT carry MESSAGE-INTEGRITY (RFC 5766 §10.1) */
static void test_turn_send_indication_no_integrity(void)
{
    uint8_t peer_addr[NANORTC_ADDR_SIZE] = {192, 168, 1, 1};
    const uint8_t payload[] = "no integrity here";
    uint8_t buf[256];
    size_t out_len = 0;

    int rc = turn_wrap_send(peer_addr, 4, 5000, payload, sizeof(payload) - 1, buf, sizeof(buf),
                            &out_len);
    TEST_ASSERT_EQUAL_INT(NANORTC_OK, rc);

    int mi_count = rfc_count_attribute(buf, out_len, STUN_ATTR_MESSAGE_INTEGRITY);
    TEST_ASSERT_EQUAL_INT(0, mi_count);

    /* Sanity: must contain XOR-PEER-ADDRESS and DATA */
    TEST_ASSERT_EQUAL_INT(1, rfc_count_attribute(buf, out_len, STUN_ATTR_XOR_PEER_ADDRESS));
    TEST_ASSERT_EQUAL_INT(1, rfc_count_attribute(buf, out_len, STUN_ATTR_DATA));
}

/* H4: CreatePermission MUST carry MESSAGE-INTEGRITY (RFC 5766 §9, §14.4) */
static void test_turn_create_permission_has_integrity(void)
{
    nano_turn_t turn;
    setup_turn_allocated(&turn);

    uint8_t peer[NANORTC_ADDR_SIZE] = {172, 16, 0, 1};
    uint8_t buf[512];
    size_t out_len = 0;
    int rc = turn_create_permission(&turn, peer, 4, 7000, crypto(), buf, sizeof(buf), &out_len);
    TEST_ASSERT_EQUAL_INT(NANORTC_OK, rc);
    TEST_ASSERT_EQUAL_INT(1, rfc_count_attribute(buf, out_len, STUN_ATTR_MESSAGE_INTEGRITY));
    TEST_ASSERT_EQUAL_INT(1, rfc_count_attribute(buf, out_len, STUN_ATTR_USERNAME));
    TEST_ASSERT_EQUAL_INT(1, rfc_count_attribute(buf, out_len, STUN_ATTR_REALM));
    TEST_ASSERT_EQUAL_INT(1, rfc_count_attribute(buf, out_len, STUN_ATTR_NONCE));
}

/* H5: ChannelBind MUST carry MESSAGE-INTEGRITY (RFC 5766 §11.2) */
static void test_turn_channel_bind_has_integrity(void)
{
    nano_turn_t turn;
    setup_turn_allocated(&turn);

    uint8_t peer[NANORTC_ADDR_SIZE] = {192, 168, 1, 50};
    uint8_t buf[512];
    size_t out_len = 0;
    int rc = turn_channel_bind(&turn, peer, 4, 9000, crypto(), buf, sizeof(buf), &out_len);
    TEST_ASSERT_EQUAL_INT(NANORTC_OK, rc);
    TEST_ASSERT_EQUAL_INT(1, rfc_count_attribute(buf, out_len, STUN_ATTR_MESSAGE_INTEGRITY));
    TEST_ASSERT_EQUAL_INT(1, rfc_count_attribute(buf, out_len, STUN_ATTR_CHANNEL_NUMBER));
    TEST_ASSERT_EQUAL_INT(1, rfc_count_attribute(buf, out_len, STUN_ATTR_XOR_PEER_ADDRESS));
}

/* H6 (F3): Refresh with LIFETIME=0 explicitly deallocates (RFC 5766 §7) */
static void test_turn_refresh_zero_lifetime_deallocate(void)
{
    nano_turn_t turn;
    setup_turn_allocated(&turn);
    turn.relay_family = STUN_FAMILY_IPV4;
    turn.relay_port = 49152;

    uint8_t buf[512];
    size_t out_len = 0;
    int rc = turn_deallocate(&turn, crypto(), buf, sizeof(buf), &out_len);
    TEST_ASSERT_EQUAL_INT(NANORTC_OK, rc);
    TEST_ASSERT_TRUE(out_len > 0);

    /* Local state must be reset. */
    TEST_ASSERT_EQUAL_INT(NANORTC_TURN_IDLE, turn.state);
    TEST_ASSERT_EQUAL_INT(0, turn.lifetime_s);
    TEST_ASSERT_EQUAL_INT(0, turn.permission_count);
    TEST_ASSERT_EQUAL_INT(0, turn.channel_count);

    /* Wire format: must be REFRESH-REQUEST with LIFETIME=0 + auth + MI. */
    stun_msg_t msg;
    rc = stun_parse(buf, out_len, &msg);
    TEST_ASSERT_EQUAL_INT(NANORTC_OK, rc);
    TEST_ASSERT_EQUAL_HEX16(STUN_REFRESH_REQUEST, msg.type);
    TEST_ASSERT_EQUAL_INT(0, msg.lifetime);
    TEST_ASSERT_TRUE(msg.has_integrity);
    TEST_ASSERT_NOT_NULL(msg.username);
    TEST_ASSERT_NOT_NULL(msg.realm);
    TEST_ASSERT_NOT_NULL(msg.nonce);

    /* Calling deallocate again from IDLE should be a no-op error. */
    out_len = 0;
    rc = turn_deallocate(&turn, crypto(), buf, sizeof(buf), &out_len);
    TEST_ASSERT_EQUAL_INT(NANORTC_ERR_STATE, rc);
}

/* H7 (F1): Spoofed CreatePermission response with foreign txid is rejected */
static void test_turn_create_permission_txid_validation(void)
{
    nano_turn_t turn;
    setup_turn_allocated(&turn);

    uint8_t peer[NANORTC_ADDR_SIZE] = {172, 16, 0, 1};
    uint8_t buf[512];
    size_t out_len = 0;
    turn_create_permission(&turn, peer, 4, 7000, crypto(), buf, sizeof(buf), &out_len);

    /* Spoofed response with a txid that does not match any in-flight request. */
    uint8_t spoof_txid[STUN_TXID_SIZE];
    memset(spoof_txid, 0xAA, STUN_TXID_SIZE);

    /* Make sure we are not accidentally colliding with the real one. */
    TEST_ASSERT_FALSE(memcmp(spoof_txid, turn.permissions[0].txid, STUN_TXID_SIZE) == 0);

    /* Spoofed success response — must be rejected. */
    uint8_t resp[64];
    size_t resp_len = build_success_response(resp, STUN_CREATE_PERMISSION_RESPONSE, spoof_txid);
    int rc = turn_handle_response(&turn, resp, resp_len, crypto());
    TEST_ASSERT_EQUAL_INT(NANORTC_ERR_PROTOCOL, rc);

    /* Spoofed 438 stale-nonce error — must NOT update our nonce. */
    char stale_marker[] = "evilNonce!";
    size_t err_len = build_error_response(resp, STUN_CREATE_PERMISSION_ERROR, spoof_txid, 438,
                                          stale_marker);
    rc = turn_handle_response(&turn, resp, err_len, crypto());
    TEST_ASSERT_EQUAL_INT(NANORTC_ERR_PROTOCOL, rc);
    /* Our nonce must remain unchanged ("nonce456" from setup_turn_allocated). */
    TEST_ASSERT_EQUAL_STRING("nonce456", turn.nonce);

    /* The legitimate response (echoing our txid) is still accepted. */
    resp_len = build_success_response(resp, STUN_CREATE_PERMISSION_RESPONSE,
                                      turn.permissions[0].txid);
    rc = turn_handle_response(&turn, resp, resp_len, crypto());
    TEST_ASSERT_EQUAL_INT(NANORTC_OK, rc);
}

/* H8: MESSAGE-INTEGRITY matches an independent HMAC-SHA1 with key derived per
 * RFC 8489 §9.2.2 (long-term mechanism, key = MD5(user:realm:pass)). Drives the
 * full credential path via the 401 challenge so we catch any drift between the
 * two derivations. */
static void test_turn_message_integrity_hmac_vector(void)
{
    nano_turn_t turn;
    setup_turn(&turn); /* user="testuser", pass="testpass" */

    /* Step 1: send unauthenticated Allocate. */
    uint8_t buf[512];
    size_t out_len = 0;
    int rc = turn_start_allocate(&turn, crypto(), buf, sizeof(buf), &out_len);
    TEST_ASSERT_EQUAL_INT(NANORTC_OK, rc);

    /* Step 2: feed a 401 with realm="example.com" nonce="abc123nonce". */
    uint8_t resp[256];
    size_t resp_len = build_401_response(resp, turn.last_txid);
    rc = turn_handle_response(&turn, resp, resp_len, crypto());
    TEST_ASSERT_EQUAL_INT(NANORTC_OK, rc);
    TEST_ASSERT_EQUAL_INT(NANORTC_TURN_CHALLENGED, turn.state);

    /* Step 3: send authenticated Allocate. */
    out_len = 0;
    rc = turn_start_allocate(&turn, crypto(), buf, sizeof(buf), &out_len);
    TEST_ASSERT_EQUAL_INT(NANORTC_OK, rc);

    /* Locate the MESSAGE-INTEGRITY attribute in the wire bytes. */
    int found = 0;
    size_t mi_pos = 0;
    size_t pos = STUN_HEADER_SIZE;
    while (pos + 4 <= out_len) {
        uint16_t type = (uint16_t)((buf[pos] << 8) | buf[pos + 1]);
        uint16_t alen = (uint16_t)((buf[pos + 2] << 8) | buf[pos + 3]);
        if (type == STUN_ATTR_MESSAGE_INTEGRITY) {
            TEST_ASSERT_EQUAL_INT(20, alen);
            mi_pos = pos;
            found = 1;
            break;
        }
        pos += 4 + (((size_t)alen + 3u) & ~3u);
    }
    TEST_ASSERT_TRUE(found);

    /* Independently derive the key per RFC 8489 §9.2.2 with the credentials
     * the test driver passed (testuser/testpass) and the realm from the 401. */
    uint8_t key_input[64];
    size_t kpos = 0;
    memcpy(key_input + kpos, "testuser", 8);
    kpos += 8;
    key_input[kpos++] = ':';
    memcpy(key_input + kpos, turn.realm, turn.realm_len);
    kpos += turn.realm_len;
    key_input[kpos++] = ':';
    memcpy(key_input + kpos, "testpass", 8);
    kpos += 8;

    uint8_t key[16];
    crypto()->md5(key_input, kpos, key);

    /* Sanity: derived key must match what the client computed internally. */
    TEST_ASSERT_EQUAL_MEMORY(key, turn.hmac_key, 16);

    /* RFC 8489 §14.5: HMAC is over wire bytes [0..mi_pos) with the STUN length
     * field set to include the MESSAGE-INTEGRITY attribute (mi_pos + 24 - HDR). */
    uint8_t mi_buf[1024];
    memcpy(mi_buf, buf, mi_pos);
    mi_buf[2] = (uint8_t)((mi_pos + 24 - STUN_HEADER_SIZE) >> 8);
    mi_buf[3] = (uint8_t)((mi_pos + 24 - STUN_HEADER_SIZE) & 0xFF);

    uint8_t expected[20];
    crypto()->hmac_sha1(key, 16, mi_buf, mi_pos, expected);

    TEST_ASSERT_EQUAL_MEMORY(expected, buf + mi_pos + 4, 20);
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
    RUN_TEST(test_turn_permission_dedup);
    RUN_TEST(test_turn_permission_distinct);
    RUN_TEST(test_turn_channel_bind_request);
    RUN_TEST(test_turn_channel_bind_success);
    RUN_TEST(test_nano_turn_wrap_channel_data);
    RUN_TEST(test_turn_unwrap_channel_data);
    RUN_TEST(test_turn_channel_lookup);
    RUN_TEST(test_turn_is_channel_data);
    RUN_TEST(test_turn_channel_number_allocation);
    RUN_TEST(test_turn_permission_refresh);
    RUN_TEST(test_turn_permission_refresh_not_due);
    RUN_TEST(test_turn_channel_refresh);

    /* Response handling */
    RUN_TEST(test_turn_allocate_438_stale_nonce);
    RUN_TEST(test_turn_allocate_438_no_nonce);
    RUN_TEST(test_turn_allocate_unknown_error);
    RUN_TEST(test_turn_refresh_success_response);
    RUN_TEST(test_turn_refresh_438_stale_nonce);
    RUN_TEST(test_turn_refresh_error_fails);
    RUN_TEST(test_turn_permission_success_response);
    RUN_TEST(test_turn_permission_438_stale_nonce);
    RUN_TEST(test_turn_permission_error_fails);
    RUN_TEST(test_turn_channel_bind_438_stale_nonce);
    RUN_TEST(test_turn_channel_bind_error_fails);

    /* Error paths */
    RUN_TEST(test_turn_unwrap_channel_data_errors);
    RUN_TEST(test_turn_find_null_params);
    RUN_TEST(test_turn_permission_refresh_errors);
    RUN_TEST(test_turn_channel_refresh_errors);

    /* Phase 5.1 hardening (RFC 5766 / 8656 / 8489) */
    RUN_TEST(test_turn_channel_number_range);
    RUN_TEST(test_turn_channel_data_padding);
    RUN_TEST(test_turn_send_indication_no_integrity);
    RUN_TEST(test_turn_create_permission_has_integrity);
    RUN_TEST(test_turn_channel_bind_has_integrity);
    RUN_TEST(test_turn_refresh_zero_lifetime_deallocate);
    RUN_TEST(test_turn_create_permission_txid_validation);
    RUN_TEST(test_turn_message_integrity_hmac_vector);

    return UNITY_END();
}
