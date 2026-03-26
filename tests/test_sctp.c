/*
 * nanortc — SCTP codec tests
 *
 * Tests for SCTP chunk parser/encoder with real packet data.
 * Reference: RFC 4960, libpeer CORPUS_CONNECT captures.
 *
 * SPDX-License-Identifier: MIT
 */

#include "nanortc.h"
#include "nano_rtc_internal.h"
#include "nano_sctp.h"
#include "nano_crc32c.h"
#include "nano_test.h"
#include "nano_test_config.h"
#include <string.h>

/* ================================================================
 * Helper: build a valid SCTP packet with correct CRC-32c
 * ================================================================ */

static size_t build_sctp_packet(uint8_t *buf, uint16_t src_port,
                                uint16_t dst_port, uint32_t vtag,
                                const uint8_t *chunk_data, size_t chunk_len)
{
    size_t pos = sctp_encode_header(buf, src_port, dst_port, vtag);
    memcpy(buf + pos, chunk_data, chunk_len);
    pos += chunk_len;
    /* Pad to 4 bytes */
    while (pos & 3) {
        buf[pos++] = 0;
    }
    sctp_finalize_checksum(buf, pos);
    return pos;
}

/* ================================================================
 * Parser tests
 * ================================================================ */

TEST(test_parse_header_basic)
{
    uint8_t pkt[12];
    sctp_encode_header(pkt, 5000, 5000, 0x12345678);

    sctp_header_t hdr;
    ASSERT_OK(sctp_parse_header(pkt, sizeof(pkt), &hdr));
    ASSERT_EQ(hdr.src_port, 5000);
    ASSERT_EQ(hdr.dst_port, 5000);
    ASSERT_EQ(hdr.vtag, 0x12345678u);
}

TEST(test_parse_header_too_short)
{
    uint8_t pkt[8] = {0};
    sctp_header_t hdr;
    ASSERT_FAIL(sctp_parse_header(pkt, 8, &hdr));
}

TEST(test_parse_header_null)
{
    sctp_header_t hdr;
    ASSERT_FAIL(sctp_parse_header(NULL, 12, &hdr));
    uint8_t pkt[12] = {0};
    ASSERT_FAIL(sctp_parse_header(pkt, 12, NULL));
}

TEST(test_checksum_roundtrip)
{
    /* Build an INIT packet, finalize checksum, verify it */
    uint8_t pkt[64];
    memset(pkt, 0, sizeof(pkt));

    size_t pos = sctp_encode_header(pkt, 5000, 5000, 0);
    pos += sctp_encode_init(pkt + pos, SCTP_CHUNK_INIT, 0xAABBCCDD,
                            0x100000, 0xFFFF, 0xFFFF, 1234, NULL, 0);
    sctp_finalize_checksum(pkt, pos);

    ASSERT_OK(sctp_verify_checksum(pkt, pos));
}

TEST(test_checksum_corruption)
{
    uint8_t pkt[64];
    memset(pkt, 0, sizeof(pkt));

    size_t pos = sctp_encode_header(pkt, 5000, 5000, 0);
    pos += sctp_encode_init(pkt + pos, SCTP_CHUNK_INIT, 0xAABBCCDD,
                            0x100000, 0xFFFF, 0xFFFF, 1234, NULL, 0);
    sctp_finalize_checksum(pkt, pos);

    /* Corrupt one byte */
    pkt[pos - 1] ^= 0xFF;
    ASSERT_FAIL(sctp_verify_checksum(pkt, pos));
}

TEST(test_encode_parse_init_roundtrip)
{
    uint8_t chunk[64];
    size_t n = sctp_encode_init(chunk, SCTP_CHUNK_INIT, 0x12345678,
                                0x100000, 10, 10, 42, NULL, 0);

    ASSERT_EQ(chunk[0], SCTP_CHUNK_INIT);
    ASSERT_TRUE(n >= 20); /* 4 hdr + 16 body */

    sctp_init_t init;
    ASSERT_OK(sctp_parse_init(chunk, n, &init));
    ASSERT_EQ(init.initiate_tag, 0x12345678u);
    ASSERT_EQ(init.a_rwnd, 0x100000u);
    ASSERT_EQ(init.num_ostreams, 10);
    ASSERT_EQ(init.num_istreams, 10);
    ASSERT_EQ(init.initial_tsn, 42u);
    ASSERT(init.cookie == NULL);
    ASSERT_EQ(init.cookie_len, 0);
}

TEST(test_encode_parse_init_ack_with_cookie)
{
    uint8_t chunk[64];
    uint8_t cookie[] = {0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02, 0x03, 0x04};
    size_t n = sctp_encode_init(chunk, SCTP_CHUNK_INIT_ACK, 0xABCD0001,
                                0x100000, 0xFFFF, 0xFFFF, 100,
                                cookie, sizeof(cookie));

    ASSERT_EQ(chunk[0], SCTP_CHUNK_INIT_ACK);

    sctp_init_t init;
    ASSERT_OK(sctp_parse_init(chunk, n, &init));
    ASSERT_EQ(init.initiate_tag, 0xABCD0001u);
    ASSERT_EQ(init.initial_tsn, 100u);
    ASSERT_TRUE(init.cookie != NULL);
    ASSERT_EQ(init.cookie_len, sizeof(cookie));
    ASSERT_MEM_EQ(init.cookie, cookie, sizeof(cookie));
}

TEST(test_encode_parse_data_roundtrip)
{
    uint8_t chunk[64];
    uint8_t payload[] = "Hello, SCTP!";
    uint8_t flags = SCTP_DATA_FLAG_BEGIN | SCTP_DATA_FLAG_END; /* 0x03 */

    size_t n = sctp_encode_data(chunk, 42, 0, 1, 51, flags,
                                payload, sizeof(payload) - 1);

    sctp_data_t data;
    ASSERT_OK(sctp_parse_data(chunk, n, &data));
    ASSERT_EQ(data.tsn, 42u);
    ASSERT_EQ(data.stream_id, 0);
    ASSERT_EQ(data.ssn, 1);
    ASSERT_EQ(data.ppid, 51u);
    ASSERT_EQ(data.flags, flags);
    ASSERT_EQ(data.payload_len, sizeof(payload) - 1);
    ASSERT_MEM_EQ(data.payload, payload, sizeof(payload) - 1);
}

TEST(test_encode_parse_data_empty_payload)
{
    uint8_t chunk[32];
    size_t n = sctp_encode_data(chunk, 1, 0, 0, 56, 0x03, NULL, 0);

    sctp_data_t data;
    ASSERT_OK(sctp_parse_data(chunk, n, &data));
    ASSERT_EQ(data.tsn, 1u);
    ASSERT_EQ(data.ppid, 56u); /* PPID_STRING_EMPTY */
    ASSERT_EQ(data.payload_len, 0);
}

TEST(test_encode_parse_sack_roundtrip)
{
    uint8_t chunk[32];
    size_t n = sctp_encode_sack(chunk, 42, 0x20000);

    sctp_sack_t sack;
    ASSERT_OK(sctp_parse_sack(chunk, n, &sack));
    ASSERT_EQ(sack.cumulative_tsn, 42u);
    ASSERT_EQ(sack.a_rwnd, 0x20000u);
    ASSERT_EQ(sack.num_gap_blocks, 0);
    ASSERT_EQ(sack.num_dup_tsns, 0);
}

TEST(test_parse_data_too_short)
{
    uint8_t chunk[8] = {SCTP_CHUNK_DATA, 0x03, 0, 8, 0, 0, 0, 0};
    sctp_data_t data;
    ASSERT_FAIL(sctp_parse_data(chunk, 8, &data));
}

TEST(test_parse_sack_too_short)
{
    uint8_t chunk[8] = {SCTP_CHUNK_SACK, 0, 0, 8, 0, 0, 0, 0};
    sctp_sack_t sack;
    ASSERT_FAIL(sctp_parse_sack(chunk, 8, &sack));
}

TEST(test_encode_cookie_echo_roundtrip)
{
    uint8_t buf[32];
    uint8_t cookie[] = {0x01, 0x02, 0x03, 0x04};

    size_t n = sctp_encode_cookie_echo(buf, cookie, sizeof(cookie));
    ASSERT_EQ(buf[0], SCTP_CHUNK_COOKIE_ECHO);
    ASSERT_TRUE(n >= 8); /* 4 hdr + 4 cookie */

    /* Verify cookie bytes are at offset 4 */
    ASSERT_MEM_EQ(buf + 4, cookie, sizeof(cookie));
}

TEST(test_encode_cookie_ack)
{
    uint8_t buf[4];
    size_t n = sctp_encode_cookie_ack(buf);
    ASSERT_EQ(n, 4u);
    ASSERT_EQ(buf[0], SCTP_CHUNK_COOKIE_ACK);
}

TEST(test_encode_heartbeat_roundtrip)
{
    uint8_t buf[32];
    uint8_t nonce[] = {0xAA, 0xBB, 0xCC, 0xDD};

    size_t n = sctp_encode_heartbeat(buf, nonce, sizeof(nonce));
    ASSERT_EQ(buf[0], SCTP_CHUNK_HEARTBEAT);
    ASSERT_TRUE(n >= 12); /* 4 chunk hdr + 4 param hdr + 4 nonce */

    /* Verify Heartbeat Info TLV: type=1, length=8 */
    uint16_t ptype = nano_ntohs(*(uint16_t *)(buf + 4));
    ASSERT_EQ(ptype, 1);
    uint16_t plen = nano_ntohs(*(uint16_t *)(buf + 6));
    ASSERT_EQ(plen, 8);
    ASSERT_MEM_EQ(buf + 8, nonce, sizeof(nonce));
}

TEST(test_encode_heartbeat_ack)
{
    uint8_t buf[32];
    uint8_t nonce[] = {0x11, 0x22};
    size_t n = sctp_encode_heartbeat_ack(buf, nonce, sizeof(nonce));
    ASSERT_EQ(buf[0], SCTP_CHUNK_HEARTBEAT_ACK);
    ASSERT_TRUE(n > 0);
}

TEST(test_encode_forward_tsn)
{
    uint8_t buf[8];
    size_t n = sctp_encode_forward_tsn(buf, 999);
    ASSERT_EQ(n, 8u);
    ASSERT_EQ(buf[0], SCTP_CHUNK_FORWARD_TSN);

    uint32_t tsn = nano_ntohl(*(uint32_t *)(buf + 4));
    ASSERT_EQ(tsn, 999u);
}

TEST(test_encode_shutdown)
{
    uint8_t buf[8];
    size_t n = sctp_encode_shutdown(buf, 42);
    ASSERT_EQ(n, 8u);
    ASSERT_EQ(buf[0], SCTP_CHUNK_SHUTDOWN);

    uint32_t tsn = nano_ntohl(*(uint32_t *)(buf + 4));
    ASSERT_EQ(tsn, 42u);
}

/* ================================================================
 * Full packet parse tests
 * ================================================================ */

TEST(test_handle_data_init_packet)
{
    nano_sctp_t sctp;
    sctp_init(&sctp);

    /* Build INIT packet: src=5000, dst=5000, vtag=0 */
    uint8_t pkt[64];
    memset(pkt, 0, sizeof(pkt));

    uint8_t chunk[32];
    size_t clen = sctp_encode_init(chunk, SCTP_CHUNK_INIT, 0xAABBCCDD,
                                   0x100000, 0xFFFF, 0xFFFF, 100, NULL, 0);
    size_t plen = build_sctp_packet(pkt, 5000, 5000, 0, chunk, clen);

    /* Should parse without error (FSM not yet implemented) */
    ASSERT_OK(sctp_handle_data(&sctp, pkt, plen));
}

TEST(test_handle_data_checksum_fail)
{
    nano_sctp_t sctp;
    sctp_init(&sctp);

    uint8_t pkt[64];
    memset(pkt, 0, sizeof(pkt));

    uint8_t chunk[32];
    size_t clen = sctp_encode_init(chunk, SCTP_CHUNK_INIT, 0x11111111,
                                   0x100000, 0xFFFF, 0xFFFF, 1, NULL, 0);
    size_t plen = build_sctp_packet(pkt, 5000, 5000, 0, chunk, clen);

    /* Corrupt payload */
    pkt[20] ^= 0xFF;
    ASSERT_FAIL(sctp_handle_data(&sctp, pkt, plen));
}

TEST(test_handle_data_multi_chunk)
{
    nano_sctp_t sctp;
    sctp_init(&sctp);

    /* Build a packet with DATA + padding then a SACK in the same packet */
    uint8_t pkt[128];
    memset(pkt, 0, sizeof(pkt));

    size_t pos = sctp_encode_header(pkt, 5000, 5000, 0x12345678);

    /* DATA chunk */
    uint8_t payload[] = {0x48, 0x69}; /* "Hi" */
    pos += sctp_encode_data(pkt + pos, 1, 0, 0, 51,
                            SCTP_DATA_FLAG_BEGIN | SCTP_DATA_FLAG_END,
                            payload, sizeof(payload));

    /* SACK chunk right after */
    pos += sctp_encode_sack(pkt + pos, 1, 0x20000);

    sctp_finalize_checksum(pkt, pos);
    ASSERT_OK(sctp_handle_data(&sctp, pkt, pos));
}

TEST(test_handle_data_too_short)
{
    nano_sctp_t sctp;
    sctp_init(&sctp);

    uint8_t pkt[4] = {0};
    ASSERT_FAIL(sctp_handle_data(&sctp, pkt, 4));
}

TEST(test_handle_data_abort)
{
    nano_sctp_t sctp;
    sctp_init(&sctp);
    sctp.state = NANO_SCTP_STATE_ESTABLISHED;

    /* Build ABORT packet */
    uint8_t pkt[32];
    memset(pkt, 0, sizeof(pkt));
    size_t pos = sctp_encode_header(pkt, 5000, 5000, 0x12345678);
    /* ABORT chunk: type=6, flags=0, length=4 */
    pkt[pos] = SCTP_CHUNK_ABORT;
    pkt[pos + 1] = 0;
    *(uint16_t *)(pkt + pos + 2) = nano_htons(4);
    pos += 4;

    sctp_finalize_checksum(pkt, pos);
    ASSERT_OK(sctp_handle_data(&sctp, pkt, pos));
    ASSERT_EQ(sctp.state, NANO_SCTP_STATE_CLOSED);
}

/* ================================================================
 * Init/lifecycle tests
 * ================================================================ */

TEST(test_sctp_init_defaults)
{
    nano_sctp_t sctp;
    ASSERT_OK(sctp_init(&sctp));
    ASSERT_EQ(sctp.state, NANO_SCTP_STATE_CLOSED);
    ASSERT_EQ(sctp.local_port, 5000);
    ASSERT_EQ(sctp.remote_port, 5000);
    ASSERT_EQ(sctp.rto_ms, (uint32_t)NANO_SCTP_RTO_INITIAL_MS);
}

TEST(test_sctp_init_null)
{
    ASSERT_FAIL(sctp_init(NULL));
}

TEST(test_poll_output_empty)
{
    nano_sctp_t sctp;
    sctp_init(&sctp);

    uint8_t buf[256];
    size_t out_len = 0;
    ASSERT_EQ(sctp_poll_output(&sctp, buf, sizeof(buf), &out_len),
              NANO_ERR_NO_DATA);
}

/* ================================================================
 * 4-byte alignment tests
 * ================================================================ */

TEST(test_data_chunk_padding)
{
    /* Encode DATA with 3-byte payload — should pad to 4 */
    uint8_t buf[32];
    uint8_t payload[] = {0x41, 0x42, 0x43}; /* "ABC" */
    size_t n = sctp_encode_data(buf, 1, 0, 0, 51, 0x03,
                                payload, sizeof(payload));

    /* chunk_len = 4+12+3 = 19, padded to 20 */
    ASSERT_EQ(n, 20u);
    /* Verify the padding byte is zero */
    ASSERT_EQ(buf[19], 0);
}

TEST(test_cookie_echo_padding)
{
    uint8_t buf[16];
    uint8_t cookie[] = {0x01, 0x02, 0x03}; /* 3 bytes → pad to 4 */
    size_t n = sctp_encode_cookie_echo(buf, cookie, sizeof(cookie));

    /* chunk_len = 4+3 = 7, padded to 8 */
    ASSERT_EQ(n, 8u);
}

/* ================================================================
 * Logging integration test
 * ================================================================ */

static int log_call_count;
static nano_log_level_t last_log_level;

static void test_log_callback(const nano_log_message_t *msg, void *ctx)
{
    (void)ctx;
    log_call_count++;
    last_log_level = msg->level;
}

TEST(test_sctp_logging_on_parse)
{
    /* Set up logging */
    nano_rtc_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.crypto = nano_test_crypto();
    cfg.log.level = NANO_LOG_TRACE;
    cfg.log.callback = test_log_callback;
    cfg.log.user_data = NULL;

    nano_rtc_t rtc;
    ASSERT_OK(nano_rtc_init(&rtc, &cfg));

    log_call_count = 0;

    /* Feed a valid INIT packet into the SCTP parser */
    uint8_t pkt[64];
    memset(pkt, 0, sizeof(pkt));
    uint8_t chunk[32];
    size_t clen = sctp_encode_init(chunk, SCTP_CHUNK_INIT, 0x11111111,
                                   0x100000, 0xFFFF, 0xFFFF, 1, NULL, 0);
    size_t plen = build_sctp_packet(pkt, 5000, 5000, 0, chunk, clen);

    sctp_handle_data(&rtc.sctp, pkt, plen);

    /* Should have logged at least one message (INIT received) */
    ASSERT_TRUE(log_call_count > 0);

    nano_rtc_destroy(&rtc);
}

/* ================================================================
 * Two-instance loopback tests (FSM)
 * ================================================================ */

/** Helper: pump output from src to dst. Returns bytes transferred. */
static size_t pump(nano_sctp_t *src, nano_sctp_t *dst)
{
    uint8_t buf[NANO_SCTP_MTU];
    size_t out_len = 0;
    size_t total = 0;

    while (sctp_poll_output(src, buf, sizeof(buf), &out_len) == NANO_OK &&
           out_len > 0) {
        sctp_handle_data(dst, buf, out_len);
        total += out_len;
        out_len = 0;
    }
    return total;
}

TEST(test_two_instance_handshake_server_client)
{
    /* Server (answerer) and Client (offerer) handshake */
    nano_sctp_t server, client;
    sctp_init(&server);
    sctp_init(&client);

    const nano_crypto_provider_t *crypto = nano_test_crypto();
    server.crypto = crypto;
    client.crypto = crypto;

    /* Client sends INIT */
    ASSERT_OK(sctp_start(&client));
    ASSERT_EQ(client.state, NANO_SCTP_STATE_COOKIE_WAIT);

    /* INIT → Server → INIT-ACK */
    pump(&client, &server);

    /* INIT-ACK → Client → COOKIE-ECHO */
    pump(&server, &client);
    ASSERT_EQ(client.state, NANO_SCTP_STATE_COOKIE_ECHOED);

    /* COOKIE-ECHO → Server → COOKIE-ACK + ESTABLISHED */
    pump(&client, &server);
    ASSERT_EQ(server.state, NANO_SCTP_STATE_ESTABLISHED);

    /* COOKIE-ACK → Client → ESTABLISHED */
    pump(&server, &client);
    ASSERT_EQ(client.state, NANO_SCTP_STATE_ESTABLISHED);
}

TEST(test_two_instance_data_exchange)
{
    nano_sctp_t a, b;
    sctp_init(&a);
    sctp_init(&b);

    const nano_crypto_provider_t *crypto = nano_test_crypto();
    a.crypto = crypto;
    b.crypto = crypto;

    /* Handshake: a=client, b=server */
    ASSERT_OK(sctp_start(&a));
    pump(&a, &b);  /* INIT */
    pump(&b, &a);  /* INIT-ACK */
    pump(&a, &b);  /* COOKIE-ECHO */
    pump(&b, &a);  /* COOKIE-ACK */
    ASSERT_EQ(a.state, NANO_SCTP_STATE_ESTABLISHED);
    ASSERT_EQ(b.state, NANO_SCTP_STATE_ESTABLISHED);

    /* A sends DATA to B */
    uint8_t msg[] = "Hello SCTP";
    ASSERT_OK(sctp_send(&a, 0, 51, msg, sizeof(msg) - 1));

    /* Pump DATA from A to B */
    pump(&a, &b);

    /* Verify B received the data */
    ASSERT_TRUE(b.has_delivered);
    ASSERT_EQ(b.delivered_len, sizeof(msg) - 1);
    ASSERT_EQ(b.delivered_ppid, 51u);
    ASSERT_MEM_EQ(b.delivered_data, msg, sizeof(msg) - 1);

    /* B should send SACK back */
    pump(&b, &a);

    /* A's send queue should be drained */
    /* (SACK was processed, entry acked) */
}

TEST(test_two_instance_bidirectional)
{
    nano_sctp_t a, b;
    sctp_init(&a);
    sctp_init(&b);

    const nano_crypto_provider_t *crypto = nano_test_crypto();
    a.crypto = crypto;
    b.crypto = crypto;

    /* Handshake */
    ASSERT_OK(sctp_start(&a));
    pump(&a, &b);
    pump(&b, &a);
    pump(&a, &b);
    pump(&b, &a);
    ASSERT_EQ(a.state, NANO_SCTP_STATE_ESTABLISHED);
    ASSERT_EQ(b.state, NANO_SCTP_STATE_ESTABLISHED);

    /* A → B */
    uint8_t msg1[] = "from A";
    ASSERT_OK(sctp_send(&a, 0, 51, msg1, sizeof(msg1) - 1));
    pump(&a, &b);
    ASSERT_TRUE(b.has_delivered);
    ASSERT_EQ(b.delivered_ppid, 51u);
    ASSERT_MEM_EQ(b.delivered_data, msg1, sizeof(msg1) - 1);
    pump(&b, &a); /* SACK */

    /* B → A */
    uint8_t msg2[] = "from B";
    ASSERT_OK(sctp_send(&b, 0, 53, msg2, sizeof(msg2) - 1));
    pump(&b, &a);
    ASSERT_TRUE(a.has_delivered);
    ASSERT_EQ(a.delivered_ppid, 53u);
    ASSERT_MEM_EQ(a.delivered_data, msg2, sizeof(msg2) - 1);
    pump(&a, &b); /* SACK */
}

TEST(test_send_before_established)
{
    nano_sctp_t sctp;
    sctp_init(&sctp);

    uint8_t data[] = "test";
    ASSERT_EQ(sctp_send(&sctp, 0, 51, data, 4), NANO_ERR_STATE);
}

TEST(test_forward_tsn_advances)
{
    nano_sctp_t a, b;
    sctp_init(&a);
    sctp_init(&b);

    const nano_crypto_provider_t *crypto = nano_test_crypto();
    a.crypto = crypto;
    b.crypto = crypto;

    /* Handshake */
    ASSERT_OK(sctp_start(&a));
    pump(&a, &b);
    pump(&b, &a);
    pump(&a, &b);
    pump(&b, &a);

    /* Send FORWARD-TSN from A to B, advancing B's cumulative TSN */
    uint32_t old_tsn = b.cumulative_tsn;

    uint8_t pkt[64];
    size_t pos = sctp_encode_header(pkt, 5000, 5000, b.local_vtag);
    pos += sctp_encode_forward_tsn(pkt + pos, old_tsn + 5);
    sctp_finalize_checksum(pkt, pos);

    sctp_handle_data(&b, pkt, pos);
    ASSERT_EQ(b.cumulative_tsn, old_tsn + 5);
}

/* ================================================================
 * Test runner
 * ================================================================ */

TEST_MAIN_BEGIN("test_sctp")
    /* Parser tests */
    RUN(test_parse_header_basic);
    RUN(test_parse_header_too_short);
    RUN(test_parse_header_null);
    RUN(test_checksum_roundtrip);
    RUN(test_checksum_corruption);
    RUN(test_encode_parse_init_roundtrip);
    RUN(test_encode_parse_init_ack_with_cookie);
    RUN(test_encode_parse_data_roundtrip);
    RUN(test_encode_parse_data_empty_payload);
    RUN(test_encode_parse_sack_roundtrip);
    RUN(test_parse_data_too_short);
    RUN(test_parse_sack_too_short);
    RUN(test_encode_cookie_echo_roundtrip);
    RUN(test_encode_cookie_ack);
    RUN(test_encode_heartbeat_roundtrip);
    RUN(test_encode_heartbeat_ack);
    RUN(test_encode_forward_tsn);
    RUN(test_encode_shutdown);
    /* Full packet tests */
    RUN(test_handle_data_init_packet);
    RUN(test_handle_data_checksum_fail);
    RUN(test_handle_data_multi_chunk);
    RUN(test_handle_data_too_short);
    RUN(test_handle_data_abort);
    /* Init/lifecycle */
    RUN(test_sctp_init_defaults);
    RUN(test_sctp_init_null);
    RUN(test_poll_output_empty);
    /* Alignment */
    RUN(test_data_chunk_padding);
    RUN(test_cookie_echo_padding);
    /* Logging */
    RUN(test_sctp_logging_on_parse);
    /* FSM — two-instance loopback */
    RUN(test_two_instance_handshake_server_client);
    RUN(test_two_instance_data_exchange);
    RUN(test_two_instance_bidirectional);
    RUN(test_send_before_established);
    RUN(test_forward_tsn_advances);
TEST_MAIN_END
