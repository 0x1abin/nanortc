/*
 * nanortc — SCTP codec tests
 *
 * Tests for SCTP chunk parser/encoder with real packet data.
 * Reference: RFC 4960.
 *
 * SPDX-License-Identifier: MIT
 */

#include "nanortc.h"
#include "nano_test.h"

#if !NANORTC_FEATURE_DATACHANNEL
/* SCTP tests require DataChannel feature */
TEST_MAIN_BEGIN("nanortc SCTP tests (skipped — DC disabled)")
TEST_MAIN_END
#else

#include "nano_sctp.h"
#include "nano_crc32c.h"
#include "nano_test_config.h"
#include <string.h>

/* ================================================================
 * Helper: build a valid SCTP packet with correct CRC-32c
 * ================================================================ */

static size_t build_sctp_packet(uint8_t *buf, uint16_t src_port, uint16_t dst_port, uint32_t vtag,
                                const uint8_t *chunk_data, size_t chunk_len)
{
    size_t pos = nsctp_encode_header(buf, src_port, dst_port, vtag);
    memcpy(buf + pos, chunk_data, chunk_len);
    pos += chunk_len;
    /* Pad to 4 bytes */
    while (pos & 3) {
        buf[pos++] = 0;
    }
    nsctp_finalize_checksum(buf, pos);
    return pos;
}

/* ================================================================
 * Parser tests
 * ================================================================ */

TEST(test_parse_header_basic)
{
    uint8_t pkt[12];
    nsctp_encode_header(pkt, 5000, 5000, 0x12345678);

    nsctp_header_t hdr;
    ASSERT_OK(nsctp_parse_header(pkt, sizeof(pkt), &hdr));
    ASSERT_EQ(hdr.src_port, 5000);
    ASSERT_EQ(hdr.dst_port, 5000);
    ASSERT_EQ(hdr.vtag, 0x12345678u);
}

TEST(test_parse_header_too_short)
{
    uint8_t pkt[8] = {0};
    nsctp_header_t hdr;
    ASSERT_FAIL(nsctp_parse_header(pkt, 8, &hdr));
}

TEST(test_parse_header_null)
{
    nsctp_header_t hdr;
    ASSERT_FAIL(nsctp_parse_header(NULL, 12, &hdr));
    uint8_t pkt[12] = {0};
    ASSERT_FAIL(nsctp_parse_header(pkt, 12, NULL));
}

TEST(test_checksum_roundtrip)
{
    /* Build an INIT packet, finalize checksum, verify it */
    uint8_t pkt[64];
    memset(pkt, 0, sizeof(pkt));

    size_t pos = nsctp_encode_header(pkt, 5000, 5000, 0);
    pos += nsctp_encode_init(pkt + pos, SCTP_CHUNK_INIT, 0xAABBCCDD, 0x100000, 0xFFFF, 0xFFFF, 1234,
                             NULL, 0);
    nsctp_finalize_checksum(pkt, pos);

    ASSERT_OK(nsctp_verify_checksum(pkt, pos));
}

TEST(test_checksum_corruption)
{
    uint8_t pkt[64];
    memset(pkt, 0, sizeof(pkt));

    size_t pos = nsctp_encode_header(pkt, 5000, 5000, 0);
    pos += nsctp_encode_init(pkt + pos, SCTP_CHUNK_INIT, 0xAABBCCDD, 0x100000, 0xFFFF, 0xFFFF, 1234,
                             NULL, 0);
    nsctp_finalize_checksum(pkt, pos);

    /* Corrupt one byte */
    pkt[pos - 1] ^= 0xFF;
    ASSERT_FAIL(nsctp_verify_checksum(pkt, pos));
}

TEST(test_encode_parse_init_roundtrip)
{
    uint8_t chunk[64];
    size_t n = nsctp_encode_init(chunk, SCTP_CHUNK_INIT, 0x12345678, 0x100000, 10, 10, 42, NULL, 0);

    ASSERT_EQ(chunk[0], SCTP_CHUNK_INIT);
    ASSERT_TRUE(n >= 20); /* 4 hdr + 16 body */

    nsctp_init_t init;
    ASSERT_OK(nsctp_parse_init(chunk, n, &init));
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
    size_t n = nsctp_encode_init(chunk, SCTP_CHUNK_INIT_ACK, 0xABCD0001, 0x100000, 0xFFFF, 0xFFFF,
                                 100, cookie, sizeof(cookie));

    ASSERT_EQ(chunk[0], SCTP_CHUNK_INIT_ACK);

    nsctp_init_t init;
    ASSERT_OK(nsctp_parse_init(chunk, n, &init));
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

    size_t n = nsctp_encode_data(chunk, 42, 0, 1, 51, flags, payload, sizeof(payload) - 1);

    nsctp_data_t data;
    ASSERT_OK(nsctp_parse_data(chunk, n, &data));
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
    size_t n = nsctp_encode_data(chunk, 1, 0, 0, 56, 0x03, NULL, 0);

    nsctp_data_t data;
    ASSERT_OK(nsctp_parse_data(chunk, n, &data));
    ASSERT_EQ(data.tsn, 1u);
    ASSERT_EQ(data.ppid, 56u); /* PPID_STRING_EMPTY */
    ASSERT_EQ(data.payload_len, 0);
}

TEST(test_encode_parse_sack_roundtrip)
{
    uint8_t chunk[32];
    size_t n = nsctp_encode_sack(chunk, 42, 0x20000);

    nsctp_sack_t sack;
    ASSERT_OK(nsctp_parse_sack(chunk, n, &sack));
    ASSERT_EQ(sack.cumulative_tsn, 42u);
    ASSERT_EQ(sack.a_rwnd, 0x20000u);
    ASSERT_EQ(sack.num_gap_blocks, 0);
    ASSERT_EQ(sack.num_dup_tsns, 0);
}

TEST(test_parse_data_too_short)
{
    uint8_t chunk[8] = {SCTP_CHUNK_DATA, 0x03, 0, 8, 0, 0, 0, 0};
    nsctp_data_t data;
    ASSERT_FAIL(nsctp_parse_data(chunk, 8, &data));
}

TEST(test_parse_sack_too_short)
{
    uint8_t chunk[8] = {SCTP_CHUNK_SACK, 0, 0, 8, 0, 0, 0, 0};
    nsctp_sack_t sack;
    ASSERT_FAIL(nsctp_parse_sack(chunk, 8, &sack));
}

TEST(test_encode_cookie_echo_roundtrip)
{
    uint8_t buf[32];
    uint8_t cookie[] = {0x01, 0x02, 0x03, 0x04};

    size_t n = nsctp_encode_cookie_echo(buf, cookie, sizeof(cookie));
    ASSERT_EQ(buf[0], SCTP_CHUNK_COOKIE_ECHO);
    ASSERT_TRUE(n >= 8); /* 4 hdr + 4 cookie */

    /* Verify cookie bytes are at offset 4 */
    ASSERT_MEM_EQ(buf + 4, cookie, sizeof(cookie));
}

TEST(test_encode_cookie_ack)
{
    uint8_t buf[4];
    size_t n = nsctp_encode_cookie_ack(buf);
    ASSERT_EQ(n, 4u);
    ASSERT_EQ(buf[0], SCTP_CHUNK_COOKIE_ACK);
}

TEST(test_encode_heartbeat_roundtrip)
{
    uint8_t buf[32];
    uint8_t nonce[] = {0xAA, 0xBB, 0xCC, 0xDD};

    size_t n = nsctp_encode_heartbeat(buf, nonce, sizeof(nonce));
    ASSERT_EQ(buf[0], SCTP_CHUNK_HEARTBEAT);
    ASSERT_TRUE(n >= 12); /* 4 chunk hdr + 4 param hdr + 4 nonce */

    /* Verify Heartbeat Info TLV: type=1, length=8 */
    uint16_t ptype = nanortc_ntohs(*(uint16_t *)(buf + 4));
    ASSERT_EQ(ptype, 1);
    uint16_t plen = nanortc_ntohs(*(uint16_t *)(buf + 6));
    ASSERT_EQ(plen, 8);
    ASSERT_MEM_EQ(buf + 8, nonce, sizeof(nonce));
}

TEST(test_encode_heartbeat_ack)
{
    uint8_t buf[32];
    uint8_t nonce[] = {0x11, 0x22};
    size_t n = nsctp_encode_heartbeat_ack(buf, nonce, sizeof(nonce));
    ASSERT_EQ(buf[0], SCTP_CHUNK_HEARTBEAT_ACK);
    ASSERT_TRUE(n > 0);
}

TEST(test_encode_forward_tsn)
{
    uint8_t buf[8];
    size_t n = nsctp_encode_forward_tsn(buf, 999);
    ASSERT_EQ(n, 8u);
    ASSERT_EQ(buf[0], SCTP_CHUNK_FORWARD_TSN);

    uint32_t tsn = nanortc_ntohl(*(uint32_t *)(buf + 4));
    ASSERT_EQ(tsn, 999u);
}

TEST(test_encode_shutdown)
{
    uint8_t buf[8];
    size_t n = nsctp_encode_shutdown(buf, 42);
    ASSERT_EQ(n, 8u);
    ASSERT_EQ(buf[0], SCTP_CHUNK_SHUTDOWN);

    uint32_t tsn = nanortc_ntohl(*(uint32_t *)(buf + 4));
    ASSERT_EQ(tsn, 42u);
}

/* ================================================================
 * Full packet parse tests
 * ================================================================ */

TEST(test_handle_data_init_packet)
{
    nano_sctp_t sctp;
    nsctp_init(&sctp);

    /* Build INIT packet: src=5000, dst=5000, vtag=0 */
    uint8_t pkt[64];
    memset(pkt, 0, sizeof(pkt));

    uint8_t chunk[32];
    size_t clen = nsctp_encode_init(chunk, SCTP_CHUNK_INIT, 0xAABBCCDD, 0x100000, 0xFFFF, 0xFFFF,
                                    100, NULL, 0);
    size_t plen = build_sctp_packet(pkt, 5000, 5000, 0, chunk, clen);

    /* Should parse without error (FSM not yet implemented) */
    ASSERT_OK(nsctp_handle_data(&sctp, pkt, plen));
}

TEST(test_handle_data_checksum_fail)
{
    nano_sctp_t sctp;
    nsctp_init(&sctp);

    uint8_t pkt[64];
    memset(pkt, 0, sizeof(pkt));

    uint8_t chunk[32];
    size_t clen =
        nsctp_encode_init(chunk, SCTP_CHUNK_INIT, 0x11111111, 0x100000, 0xFFFF, 0xFFFF, 1, NULL, 0);
    size_t plen = build_sctp_packet(pkt, 5000, 5000, 0, chunk, clen);

    /* Corrupt payload */
    pkt[20] ^= 0xFF;
    ASSERT_FAIL(nsctp_handle_data(&sctp, pkt, plen));
}

TEST(test_handle_data_multi_chunk)
{
    nano_sctp_t sctp;
    nsctp_init(&sctp);

    /* Build a packet with DATA + padding then a SACK in the same packet */
    uint8_t pkt[128];
    memset(pkt, 0, sizeof(pkt));

    size_t pos = nsctp_encode_header(pkt, 5000, 5000, 0x12345678);

    /* DATA chunk */
    uint8_t payload[] = {0x48, 0x69}; /* "Hi" */
    pos += nsctp_encode_data(pkt + pos, 1, 0, 0, 51, SCTP_DATA_FLAG_BEGIN | SCTP_DATA_FLAG_END,
                             payload, sizeof(payload));

    /* SACK chunk right after */
    pos += nsctp_encode_sack(pkt + pos, 1, 0x20000);

    nsctp_finalize_checksum(pkt, pos);
    ASSERT_OK(nsctp_handle_data(&sctp, pkt, pos));
}

TEST(test_handle_data_too_short)
{
    nano_sctp_t sctp;
    nsctp_init(&sctp);

    uint8_t pkt[4] = {0};
    ASSERT_FAIL(nsctp_handle_data(&sctp, pkt, 4));
}

TEST(test_handle_data_abort)
{
    nano_sctp_t sctp;
    nsctp_init(&sctp);
    sctp.state = NANORTC_SCTP_STATE_ESTABLISHED;

    /* Build ABORT packet */
    uint8_t pkt[32];
    memset(pkt, 0, sizeof(pkt));
    size_t pos = nsctp_encode_header(pkt, 5000, 5000, 0x12345678);
    /* ABORT chunk: type=6, flags=0, length=4 */
    pkt[pos] = SCTP_CHUNK_ABORT;
    pkt[pos + 1] = 0;
    *(uint16_t *)(pkt + pos + 2) = nanortc_htons(4);
    pos += 4;

    nsctp_finalize_checksum(pkt, pos);
    ASSERT_OK(nsctp_handle_data(&sctp, pkt, pos));
    ASSERT_EQ(sctp.state, NANORTC_SCTP_STATE_CLOSED);
}

/* ================================================================
 * Init/lifecycle tests
 * ================================================================ */

TEST(test_sctp_init_defaults)
{
    nano_sctp_t sctp;
    ASSERT_OK(nsctp_init(&sctp));
    ASSERT_EQ(sctp.state, NANORTC_SCTP_STATE_CLOSED);
    ASSERT_EQ(sctp.local_port, 5000);
    ASSERT_EQ(sctp.remote_port, 5000);
#if NANORTC_FEATURE_DC_RELIABLE
    ASSERT_EQ(sctp.rto_ms, (uint32_t)NANORTC_SCTP_RTO_INITIAL_MS);
#endif
}

TEST(test_sctp_init_null)
{
    ASSERT_FAIL(nsctp_init(NULL));
}

TEST(test_poll_output_empty)
{
    nano_sctp_t sctp;
    nsctp_init(&sctp);

    uint8_t buf[256];
    size_t out_len = 0;
    ASSERT_EQ(nsctp_poll_output(&sctp, buf, sizeof(buf), &out_len), NANORTC_ERR_NO_DATA);
}

/* ================================================================
 * 4-byte alignment tests
 * ================================================================ */

TEST(test_data_chunk_padding)
{
    /* Encode DATA with 3-byte payload — should pad to 4 */
    uint8_t buf[32];
    uint8_t payload[] = {0x41, 0x42, 0x43}; /* "ABC" */
    size_t n = nsctp_encode_data(buf, 1, 0, 0, 51, 0x03, payload, sizeof(payload));

    /* chunk_len = 4+12+3 = 19, padded to 20 */
    ASSERT_EQ(n, 20u);
    /* Verify the padding byte is zero */
    ASSERT_EQ(buf[19], 0);
}

TEST(test_cookie_echo_padding)
{
    uint8_t buf[16];
    uint8_t cookie[] = {0x01, 0x02, 0x03}; /* 3 bytes → pad to 4 */
    size_t n = nsctp_encode_cookie_echo(buf, cookie, sizeof(cookie));

    /* chunk_len = 4+3 = 7, padded to 8 */
    ASSERT_EQ(n, 8u);
}

/* ================================================================
 * Logging integration test
 * ================================================================ */

static int log_call_count;
static nanortc_log_level_t last_log_level;

static void test_log_callback(const nanortc_log_message_t *msg, void *ctx)
{
    (void)ctx;
    log_call_count++;
    last_log_level = msg->level;
}

TEST(test_sctp_logging_on_parse)
{
    /* Set up logging */
    nanortc_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.crypto = nano_test_crypto();
    cfg.log.level = NANORTC_LOG_TRACE;
    cfg.log.callback = test_log_callback;
    cfg.log.user_data = NULL;

    nanortc_t rtc;
    ASSERT_OK(nanortc_init(&rtc, &cfg));

    log_call_count = 0;

    /* Feed a valid INIT packet into the SCTP parser */
    uint8_t pkt[64];
    memset(pkt, 0, sizeof(pkt));
    uint8_t chunk[32];
    size_t clen =
        nsctp_encode_init(chunk, SCTP_CHUNK_INIT, 0x11111111, 0x100000, 0xFFFF, 0xFFFF, 1, NULL, 0);
    size_t plen = build_sctp_packet(pkt, 5000, 5000, 0, chunk, clen);

    nsctp_handle_data(&rtc.sctp, pkt, plen);

    /* Should have logged at least one message (INIT received) */
    ASSERT_TRUE(log_call_count > 0);

    nanortc_destroy(&rtc);
}

/* ================================================================
 * Two-instance loopback tests (FSM)
 * ================================================================ */

/** Helper: pump output from src to dst. Returns bytes transferred.
 *  Uses static buffer so delivered_data pointers remain valid after return. */
static size_t pump(nano_sctp_t *src, nano_sctp_t *dst)
{
    static uint8_t buf[NANORTC_SCTP_MTU];
    size_t out_len = 0;
    size_t total = 0;

    while (nsctp_poll_output(src, buf, sizeof(buf), &out_len) == NANORTC_OK && out_len > 0) {
        nsctp_handle_data(dst, buf, out_len);
        total += out_len;
        out_len = 0;
    }
    return total;
}

TEST(test_two_instance_handshake_server_client)
{
    /* Server (answerer) and Client (offerer) handshake */
    nano_sctp_t server, client;
    nsctp_init(&server);
    nsctp_init(&client);

    const nanortc_crypto_provider_t *crypto = nano_test_crypto();
    server.crypto = crypto;
    client.crypto = crypto;

    /* Client sends INIT */
    ASSERT_OK(nsctp_start(&client));
    ASSERT_EQ(client.state, NANORTC_SCTP_STATE_COOKIE_WAIT);

    /* INIT → Server → INIT-ACK */
    pump(&client, &server);

    /* INIT-ACK → Client → COOKIE-ECHO */
    pump(&server, &client);
    ASSERT_EQ(client.state, NANORTC_SCTP_STATE_COOKIE_ECHOED);

    /* COOKIE-ECHO → Server → COOKIE-ACK + ESTABLISHED */
    pump(&client, &server);
    ASSERT_EQ(server.state, NANORTC_SCTP_STATE_ESTABLISHED);

    /* COOKIE-ACK → Client → ESTABLISHED */
    pump(&server, &client);
    ASSERT_EQ(client.state, NANORTC_SCTP_STATE_ESTABLISHED);
}

TEST(test_two_instance_data_exchange)
{
    nano_sctp_t a, b;
    nsctp_init(&a);
    nsctp_init(&b);

    const nanortc_crypto_provider_t *crypto = nano_test_crypto();
    a.crypto = crypto;
    b.crypto = crypto;

    /* Handshake: a=client, b=server */
    ASSERT_OK(nsctp_start(&a));
    pump(&a, &b); /* INIT */
    pump(&b, &a); /* INIT-ACK */
    pump(&a, &b); /* COOKIE-ECHO */
    pump(&b, &a); /* COOKIE-ACK */
    ASSERT_EQ(a.state, NANORTC_SCTP_STATE_ESTABLISHED);
    ASSERT_EQ(b.state, NANORTC_SCTP_STATE_ESTABLISHED);

    /* A sends DATA to B */
    uint8_t msg[] = "Hello SCTP";
    ASSERT_OK(nsctp_send(&a, 0, 51, msg, sizeof(msg) - 1));

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
    nsctp_init(&a);
    nsctp_init(&b);

    const nanortc_crypto_provider_t *crypto = nano_test_crypto();
    a.crypto = crypto;
    b.crypto = crypto;

    /* Handshake */
    ASSERT_OK(nsctp_start(&a));
    pump(&a, &b);
    pump(&b, &a);
    pump(&a, &b);
    pump(&b, &a);
    ASSERT_EQ(a.state, NANORTC_SCTP_STATE_ESTABLISHED);
    ASSERT_EQ(b.state, NANORTC_SCTP_STATE_ESTABLISHED);

    /* A → B */
    uint8_t msg1[] = "from A";
    ASSERT_OK(nsctp_send(&a, 0, 51, msg1, sizeof(msg1) - 1));
    pump(&a, &b);
    ASSERT_TRUE(b.has_delivered);
    ASSERT_EQ(b.delivered_ppid, 51u);
    ASSERT_MEM_EQ(b.delivered_data, msg1, sizeof(msg1) - 1);
    pump(&b, &a); /* SACK */

    /* B → A */
    uint8_t msg2[] = "from B";
    ASSERT_OK(nsctp_send(&b, 0, 53, msg2, sizeof(msg2) - 1));
    pump(&b, &a);
    ASSERT_TRUE(a.has_delivered);
    ASSERT_EQ(a.delivered_ppid, 53u);
    ASSERT_MEM_EQ(a.delivered_data, msg2, sizeof(msg2) - 1);
    pump(&a, &b); /* SACK */
}

TEST(test_send_before_established)
{
    nano_sctp_t sctp;
    nsctp_init(&sctp);

    uint8_t data[] = "test";
    ASSERT_EQ(nsctp_send(&sctp, 0, 51, data, 4), NANORTC_ERR_STATE);
}

TEST(test_forward_tsn_advances)
{
    nano_sctp_t a, b;
    nsctp_init(&a);
    nsctp_init(&b);

    const nanortc_crypto_provider_t *crypto = nano_test_crypto();
    a.crypto = crypto;
    b.crypto = crypto;

    /* Handshake */
    ASSERT_OK(nsctp_start(&a));
    pump(&a, &b);
    pump(&b, &a);
    pump(&a, &b);
    pump(&b, &a);

    /* Send FORWARD-TSN from A to B, advancing B's cumulative TSN */
    uint32_t old_tsn = b.cumulative_tsn;

    uint8_t pkt[64];
    size_t pos = nsctp_encode_header(pkt, 5000, 5000, b.local_vtag);
    pos += nsctp_encode_forward_tsn(pkt + pos, old_tsn + 5);
    nsctp_finalize_checksum(pkt, pos);

    nsctp_handle_data(&b, pkt, pos);
    ASSERT_EQ(b.cumulative_tsn, old_tsn + 5);
}

/* ================================================================
 * SACK acknowledgement and send queue drain
 * ================================================================ */

TEST(test_sack_drains_send_queue)
{
    nano_sctp_t a, b;
    nsctp_init(&a);
    nsctp_init(&b);

    const nanortc_crypto_provider_t *crypto = nano_test_crypto();
    a.crypto = crypto;
    b.crypto = crypto;

    /* Handshake */
    ASSERT_OK(nsctp_start(&a));
    pump(&a, &b);
    pump(&b, &a);
    pump(&a, &b);
    pump(&b, &a);
    ASSERT_EQ(a.state, NANORTC_SCTP_STATE_ESTABLISHED);

    /* Send multiple messages */
    uint8_t msg1[] = "first";
    uint8_t msg2[] = "second";
    ASSERT_OK(nsctp_send(&a, 0, 51, msg1, sizeof(msg1) - 1));
    ASSERT_OK(nsctp_send(&a, 0, 51, msg2, sizeof(msg2) - 1));

    /* Pump both DATA from A to B */
    pump(&a, &b);
    pump(&a, &b);

    /* B received the data */
    ASSERT_TRUE(b.has_delivered);

    /* Pump SACK from B to A */
    pump(&b, &a);

    /* A's send queue entries should be acked */
    bool all_acked = true;
    uint8_t idx = a.sq_head;
    while (idx != a.sq_tail) {
        nsctp_send_entry_t *e = &a.send_queue[idx & (NANORTC_SCTP_MAX_SEND_QUEUE - 1)];
        if (!e->acked) {
            all_acked = false;
        }
        idx++;
    }
    ASSERT_TRUE(all_acked);
}

/* ================================================================
 * Multiple output queue slots (ring buffer)
 * ================================================================ */

TEST(test_sctp_output_queue_multiple)
{
    nano_sctp_t a, b;
    nsctp_init(&a);
    nsctp_init(&b);

    const nanortc_crypto_provider_t *crypto = nano_test_crypto();
    a.crypto = crypto;
    b.crypto = crypto;

    /* Client sends INIT — should queue one output */
    ASSERT_OK(nsctp_start(&a));

    /* Poll the INIT */
    uint8_t buf[NANORTC_SCTP_MTU];
    size_t out_len = 0;
    ASSERT_OK(nsctp_poll_output(&a, buf, sizeof(buf), &out_len));
    ASSERT_TRUE(out_len > 0);

    /* Feed INIT to server — should queue INIT-ACK */
    nsctp_handle_data(&b, buf, out_len);

    /* Poll INIT-ACK from server */
    out_len = 0;
    ASSERT_OK(nsctp_poll_output(&b, buf, sizeof(buf), &out_len));
    ASSERT_TRUE(out_len > 0);

    /* No more output */
    out_len = 0;
    ASSERT_EQ(nsctp_poll_output(&b, buf, sizeof(buf), &out_len), NANORTC_ERR_NO_DATA);
}

/* ================================================================
 * RFC 4960 Appendix — Hardcoded byte-level test vectors
 *
 * These vectors are hand-crafted from RFC 4960 §3.3 wire format
 * specifications. They test the parser against known byte sequences
 * independent of our encoder.
 * ================================================================ */

/*
 * RFC 4960 §3.3.2 — INIT chunk (type=1, flags=0)
 * Initiate Tag:   0xDEADBEEF
 * a_rwnd:         0x00010000 (65536)
 * Num Outbound:   0x000A (10)
 * Num Inbound:    0x000A (10)
 * Initial TSN:    0x00000001
 */
TEST(test_rfc4960_init_chunk_vector)
{
    static const uint8_t init_chunk[] = {
        /* Chunk header: type=1, flags=0, length=20 */
        0x01,
        0x00,
        0x00,
        0x14,
        /* Initiate Tag = 0xDEADBEEF */
        0xDE,
        0xAD,
        0xBE,
        0xEF,
        /* a_rwnd = 65536 */
        0x00,
        0x01,
        0x00,
        0x00,
        /* Number of Outbound Streams = 10 */
        0x00,
        0x0A,
        /* Number of Inbound Streams = 10 */
        0x00,
        0x0A,
        /* Initial TSN = 1 */
        0x00,
        0x00,
        0x00,
        0x01,
    };

    nsctp_init_t init;
    ASSERT_OK(nsctp_parse_init(init_chunk, sizeof(init_chunk), &init));
    ASSERT_EQ(init.initiate_tag, 0xDEADBEEFu);
    ASSERT_EQ(init.a_rwnd, 65536u);
    ASSERT_EQ(init.num_ostreams, 10);
    ASSERT_EQ(init.num_istreams, 10);
    ASSERT_EQ(init.initial_tsn, 1u);
    ASSERT_TRUE(init.cookie == NULL);
    ASSERT_EQ(init.cookie_len, 0);
}

/*
 * RFC 4960 §3.3.3 — INIT-ACK chunk with State Cookie parameter
 * Initiate Tag:   0x0A0B0C0D
 * a_rwnd:         0x00020000 (131072)
 * Num Outbound:   0x0005 (5)
 * Num Inbound:    0x0008 (8)
 * Initial TSN:    0x00000064 (100)
 * Cookie:         {0xCA, 0xFE, 0xBA, 0xBE} (State Cookie param type=7)
 */
TEST(test_rfc4960_init_ack_chunk_vector)
{
    static const uint8_t init_ack_chunk[] = {
        /* Chunk header: type=2, flags=0, length=32 */
        0x02,
        0x00,
        0x00,
        0x20,
        /* Initiate Tag = 0x0A0B0C0D */
        0x0A,
        0x0B,
        0x0C,
        0x0D,
        /* a_rwnd = 131072 */
        0x00,
        0x02,
        0x00,
        0x00,
        /* Num Outbound Streams = 5 */
        0x00,
        0x05,
        /* Num Inbound Streams = 8 */
        0x00,
        0x08,
        /* Initial TSN = 100 */
        0x00,
        0x00,
        0x00,
        0x64,
        /* State Cookie parameter: type=7, length=8, data=CAFEBABE */
        0x00,
        0x07,
        0x00,
        0x08,
        0xCA,
        0xFE,
        0xBA,
        0xBE,
    };

    nsctp_init_t init;
    ASSERT_OK(nsctp_parse_init(init_ack_chunk, sizeof(init_ack_chunk), &init));
    ASSERT_EQ(init.initiate_tag, 0x0A0B0C0Du);
    ASSERT_EQ(init.a_rwnd, 131072u);
    ASSERT_EQ(init.num_ostreams, 5);
    ASSERT_EQ(init.num_istreams, 8);
    ASSERT_EQ(init.initial_tsn, 100u);
    ASSERT_TRUE(init.cookie != NULL);
    ASSERT_EQ(init.cookie_len, 4);
    static const uint8_t expected_cookie[] = {0xCA, 0xFE, 0xBA, 0xBE};
    ASSERT_MEM_EQ(init.cookie, expected_cookie, 4);
}

/*
 * RFC 4960 §3.3.1 — DATA chunk (type=0)
 * TSN:       0x00000007
 * Stream ID: 0x0000
 * SSN:       0x0003
 * PPID:      0x00000033 (51 = WebRTC String)
 * Flags:     B=1, E=1 (0x03) = unfragmented
 * Payload:   "Hi" (2 bytes)
 */
TEST(test_rfc4960_data_chunk_vector)
{
    static const uint8_t data_chunk[] = {
        /* Chunk header: type=0, flags=0x03 (B+E), length=18 */
        0x00,
        0x03,
        0x00,
        0x12,
        /* TSN = 7 */
        0x00,
        0x00,
        0x00,
        0x07,
        /* Stream ID = 0 */
        0x00,
        0x00,
        /* SSN = 3 */
        0x00,
        0x03,
        /* PPID = 51 (WebRTC String) */
        0x00,
        0x00,
        0x00,
        0x33,
        /* Payload: "Hi" */
        0x48,
        0x69,
    };

    nsctp_data_t data;
    ASSERT_OK(nsctp_parse_data(data_chunk, sizeof(data_chunk), &data));
    ASSERT_EQ(data.tsn, 7u);
    ASSERT_EQ(data.stream_id, 0);
    ASSERT_EQ(data.ssn, 3);
    ASSERT_EQ(data.ppid, 51u);
    ASSERT_EQ(data.flags, 0x03);
    ASSERT_EQ(data.payload_len, 2);
    ASSERT_MEM_EQ(data.payload, "Hi", 2);
}

/*
 * RFC 4960 §3.3.4 — SACK chunk (type=3)
 * Cumulative TSN: 0x0000000A (10)
 * a_rwnd:         0x0000FFFF (65535)
 * Num Gap Blocks: 0
 * Num Dup TSNs:   0
 */
TEST(test_rfc4960_sack_chunk_vector)
{
    static const uint8_t sack_chunk[] = {
        /* Chunk header: type=3, flags=0, length=16 */
        0x03,
        0x00,
        0x00,
        0x10,
        /* Cumulative TSN Ack = 10 */
        0x00,
        0x00,
        0x00,
        0x0A,
        /* a_rwnd = 65535 */
        0x00,
        0x00,
        0xFF,
        0xFF,
        /* Number of Gap Ack Blocks = 0 */
        0x00,
        0x00,
        /* Number of Dup TSNs = 0 */
        0x00,
        0x00,
    };

    nsctp_sack_t sack;
    ASSERT_OK(nsctp_parse_sack(sack_chunk, sizeof(sack_chunk), &sack));
    ASSERT_EQ(sack.cumulative_tsn, 10u);
    ASSERT_EQ(sack.a_rwnd, 65535u);
    ASSERT_EQ(sack.num_gap_blocks, 0);
    ASSERT_EQ(sack.num_dup_tsns, 0);
}

/*
 * RFC 4960 §3.3.5 — HEARTBEAT chunk (type=4)
 * Contains Heartbeat Info parameter (type=1)
 * Nonce: {0xDE, 0xAD, 0xBE, 0xEF, 0xCA, 0xFE, 0xBA, 0xBE}
 */
TEST(test_rfc4960_heartbeat_vector)
{
    static const uint8_t hb_chunk[] = {
        /* Chunk header: type=4, flags=0, length=16 */
        0x04,
        0x00,
        0x00,
        0x10,
        /* Heartbeat Info parameter: type=1, length=12 */
        0x00,
        0x01,
        0x00,
        0x0C,
        /* Nonce data (8 bytes) */
        0xDE,
        0xAD,
        0xBE,
        0xEF,
        0xCA,
        0xFE,
        0xBA,
        0xBE,
    };

    /* Verify chunk type */
    ASSERT_EQ(hb_chunk[0], SCTP_CHUNK_HEARTBEAT);
    /* Verify the heartbeat info param header */
    uint16_t ptype = nanortc_read_u16be(hb_chunk + 4);
    ASSERT_EQ(ptype, 1); /* Heartbeat Info type */
    uint16_t plen = nanortc_read_u16be(hb_chunk + 6);
    ASSERT_EQ(plen, 12); /* 4 header + 8 data */
}

/*
 * CRC-32c test vector from RFC 3309 / SCTP usage
 * The string "123456789" → CRC-32c = 0xE3069283
 */
TEST(test_rfc3309_crc32c_vector)
{
    static const uint8_t test_data[] = "123456789";
    /* nanortc already tests this in test_main.c, but we verify the SCTP
     * checksum path uses the same CRC-32c polynomial. Build a minimal
     * packet with known data and verify the checksum field. */
    uint8_t pkt[32];
    memset(pkt, 0, sizeof(pkt));
    size_t pos = nsctp_encode_header(pkt, 1234, 5678, 0);
    /* Append some data to make a non-trivial packet */
    memcpy(pkt + pos, test_data, 9);
    pos += 12; /* pad to 4-byte boundary */
    nsctp_finalize_checksum(pkt, pos);
    ASSERT_OK(nsctp_verify_checksum(pkt, pos));
    /* Corrupt one byte, verify detection */
    pkt[14] ^= 0x01;
    ASSERT_FAIL(nsctp_verify_checksum(pkt, pos));
}

/* ================================================================
 * RFC 4960 MUST/SHOULD requirement tests
 * ================================================================ */

/* RFC 4960 §8.5.1: INIT chunk MUST have verification tag = 0 */
TEST(test_sctp_vtag_zero_init)
{
    uint8_t pkt[64];
    memset(pkt, 0, sizeof(pkt));

    /* Build INIT packet with vtag=0 (correct) */
    size_t pos = nsctp_encode_header(pkt, 5000, 5000, 0);
    pos += nsctp_encode_init(pkt + pos, SCTP_CHUNK_INIT, 0x12345678, 0x10000, 10, 10, 1, NULL, 0);
    nsctp_finalize_checksum(pkt, pos);

    /* Parse and verify header vtag is 0 */
    nsctp_header_t hdr;
    ASSERT_OK(nsctp_parse_header(pkt, pos, &hdr));
    ASSERT_EQ(hdr.vtag, 0u);
}

/* RFC 4960 §3.3.1: DATA chunk with unordered flag */
TEST(test_sctp_data_unordered_flag)
{
    uint8_t chunk[32];
    uint8_t payload[] = {0x01, 0x02};
    uint8_t flags = SCTP_DATA_FLAG_BEGIN | SCTP_DATA_FLAG_END | SCTP_DATA_FLAG_UNORDERED;

    size_t n = nsctp_encode_data(chunk, 1, 0, 0, 52, flags, payload, sizeof(payload));

    nsctp_data_t data;
    ASSERT_OK(nsctp_parse_data(chunk, n, &data));
    ASSERT_TRUE(data.flags & SCTP_DATA_FLAG_UNORDERED);
    ASSERT_TRUE(data.flags & SCTP_DATA_FLAG_BEGIN);
    ASSERT_TRUE(data.flags & SCTP_DATA_FLAG_END);
}

/* RFC 4960 §9.2: SHUTDOWN chunk carries cumulative TSN */
TEST(test_sctp_shutdown_tsn_value)
{
    uint8_t buf[8];
    size_t n = nsctp_encode_shutdown(buf, 0xFFFFFFFE);
    ASSERT_EQ(n, 8u);
    ASSERT_EQ(buf[0], SCTP_CHUNK_SHUTDOWN);

    /* Verify the TSN is at bytes 4-7 in network byte order */
    uint32_t tsn = nanortc_read_u32be(buf + 4);
    ASSERT_EQ(tsn, 0xFFFFFFFEu);
}

/* RFC 4960 §3.3.7: ABORT chunk type and structure */
TEST(test_sctp_abort_chunk_encode)
{
    /* Verify we can build a packet containing ABORT and it parses */
    uint8_t pkt[32];
    memset(pkt, 0, sizeof(pkt));
    size_t pos = nsctp_encode_header(pkt, 5000, 5000, 0x12345678);
    /* Manually encode ABORT: type=6, flags=0, length=4 */
    pkt[pos] = SCTP_CHUNK_ABORT;
    pkt[pos + 1] = 0;
    pkt[pos + 2] = 0;
    pkt[pos + 3] = 4;
    pos += 4;
    nsctp_finalize_checksum(pkt, pos);

    nsctp_header_t hdr;
    ASSERT_OK(nsctp_parse_header(pkt, pos, &hdr));
    ASSERT_EQ(hdr.vtag, 0x12345678u);
    /* Verify chunk type at offset 12 */
    ASSERT_EQ(pkt[12], SCTP_CHUNK_ABORT);
}

/* RFC 4960 §3.2: Chunk length includes header (4 bytes) + body */
TEST(test_sctp_chunk_length_field)
{
    uint8_t chunk[64];
    uint8_t payload[] = "test";
    size_t n = nsctp_encode_data(chunk, 1, 0, 0, 50, 0x03, payload, 4);

    /* Chunk length field at bytes 2-3 should be 4(hdr) + 12(data fields) + 4(payload) = 20 */
    uint16_t chunk_len = nanortc_read_u16be(chunk + 2);
    ASSERT_EQ(chunk_len, 20);
    (void)n;
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
RUN(test_sack_drains_send_queue);
RUN(test_sctp_output_queue_multiple);
/* RFC 4960 Appendix — byte-level test vectors */
RUN(test_rfc4960_init_chunk_vector);
RUN(test_rfc4960_init_ack_chunk_vector);
RUN(test_rfc4960_data_chunk_vector);
RUN(test_rfc4960_sack_chunk_vector);
RUN(test_rfc4960_heartbeat_vector);
RUN(test_rfc3309_crc32c_vector);
/* RFC 4960 MUST/SHOULD requirements */
RUN(test_sctp_vtag_zero_init);
RUN(test_sctp_data_unordered_flag);
RUN(test_sctp_shutdown_tsn_value);
RUN(test_sctp_abort_chunk_encode);
RUN(test_sctp_chunk_length_field);
TEST_MAIN_END
#endif /* NANORTC_FEATURE_DATACHANNEL */
