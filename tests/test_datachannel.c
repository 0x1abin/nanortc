/*
 * nanortc — DataChannel unit tests (DCEP codec + channel management)
 *
 * Tests the DataChannel module in isolation without requiring SCTP/DTLS.
 * Covers DCEP OPEN/ACK encoding/parsing, channel state transitions,
 * and edge cases (max channels, idempotent re-OPEN, etc.).
 *
 * SPDX-License-Identifier: MIT
 */

#include "nanortc.h"
#include "nano_datachannel.h"
#include "nano_test.h"
#include <string.h>

/* ================================================================
 * dc_init
 * ================================================================ */

TEST(test_dc_init)
{
    nano_dc_t dc;
    ASSERT_OK(dc_init(&dc));
    ASSERT_EQ(dc.channel_count, 0);
    ASSERT_FALSE(dc_has_pending_output(&dc));
}

TEST(test_dc_init_null)
{
    ASSERT_FAIL(dc_init(NULL));
}

/* ================================================================
 * dc_open — generates DCEP OPEN message
 * ================================================================ */

TEST(test_dc_open_reliable_ordered)
{
    nano_dc_t dc;
    dc_init(&dc);

    ASSERT_OK(dc_open(&dc, 0, "test", true, 0));
    ASSERT_EQ(dc.channel_count, 1);
    ASSERT_TRUE(dc_has_pending_output(&dc));
    ASSERT_EQ(dc.channels[0].state, NANORTC_DC_STATE_OPENING);
    ASSERT_TRUE(dc.channels[0].ordered);

    /* Poll DCEP OPEN */
    uint8_t buf[256];
    size_t out_len = 0;
    uint16_t stream = 0;
    ASSERT_OK(dc_peek_output(&dc, buf, sizeof(buf), &out_len, &stream));
    dc_commit_output(&dc, stream);
    ASSERT_EQ(stream, 0);
    ASSERT_TRUE(out_len >= 12);
    ASSERT_EQ(buf[0], DCEP_DATA_CHANNEL_OPEN);
    ASSERT_EQ(buf[1], DCEP_CHANNEL_RELIABLE);
}

TEST(test_dc_open_unordered)
{
    nano_dc_t dc;
    dc_init(&dc);

    ASSERT_OK(dc_open(&dc, 2, "unord", false, 0));
    ASSERT_FALSE(dc.channels[0].ordered);

    uint8_t buf[256];
    size_t out_len = 0;
    uint16_t stream = 0;
    ASSERT_OK(dc_peek_output(&dc, buf, sizeof(buf), &out_len, &stream));
    dc_commit_output(&dc, stream);
    ASSERT_EQ(buf[1], DCEP_CHANNEL_RELIABLE_UNORDERED);
}

TEST(test_dc_open_rexmit)
{
    nano_dc_t dc;
    dc_init(&dc);

    ASSERT_OK(dc_open(&dc, 0, "rexmit", true, 3));

    uint8_t buf[256];
    size_t out_len = 0;
    uint16_t stream = 0;
    ASSERT_OK(dc_peek_output(&dc, buf, sizeof(buf), &out_len, &stream));
    dc_commit_output(&dc, stream);
    ASSERT_EQ(buf[1], DCEP_CHANNEL_REXMIT);
    /* Reliability param at bytes 4-7 = 3 */
    uint32_t rel = (uint32_t)buf[4] << 24 | (uint32_t)buf[5] << 16 | (uint32_t)buf[6] << 8 | buf[7];
    ASSERT_EQ(rel, 3);
}

TEST(test_dc_open_rexmit_unordered)
{
    nano_dc_t dc;
    dc_init(&dc);

    ASSERT_OK(dc_open(&dc, 0, "rexmit-u", false, 5));

    uint8_t buf[256];
    size_t out_len = 0;
    uint16_t stream = 0;
    ASSERT_OK(dc_peek_output(&dc, buf, sizeof(buf), &out_len, &stream));
    dc_commit_output(&dc, stream);
    ASSERT_EQ(buf[1], DCEP_CHANNEL_REXMIT_UNORDERED);
}

TEST(test_dc_open_null_params)
{
    nano_dc_t dc;
    dc_init(&dc);
    ASSERT_FAIL(dc_open(NULL, 0, "test", true, 0));
    ASSERT_FAIL(dc_open(&dc, 0, NULL, true, 0));
}

TEST(test_dc_open_max_channels)
{
    nano_dc_t dc;
    dc_init(&dc);

    /* Fill all channel slots */
    for (int i = 0; i < NANORTC_MAX_DATACHANNELS; i++) {
        ASSERT_OK(dc_open(&dc, (uint16_t)(i * 2), "ch", true, 0));
        /* Drain output */
        uint8_t buf[256];
        size_t out_len = 0;
        uint16_t stream = 0;
        dc_peek_output(&dc, buf, sizeof(buf), &out_len, &stream);
        dc_commit_output(&dc, stream);
    }

    /* Next should fail */
    ASSERT_FAIL(dc_open(&dc, 100, "overflow", true, 0));
}

/* ================================================================
 * dc_handle_message — DCEP OPEN from remote peer
 * ================================================================ */

TEST(test_dc_handle_open)
{
    bool opened;
    nano_dc_t dc;
    dc_init(&dc);

    /* Build a valid DCEP OPEN: type=0x03, channel_type=RELIABLE(0x00),
     * priority=0, reliability=0, label="hello", protocol="" */
    uint8_t open_msg[] = {
        0x03,                       /* DCEP_DATA_CHANNEL_OPEN */
        0x00,                       /* channel type: RELIABLE */
        0x00, 0x00,                 /* priority */
        0x00, 0x00, 0x00, 0x00,     /* reliability param */
        0x00, 0x05,                 /* label length = 5 */
        0x00, 0x00,                 /* protocol length = 0 */
        'h',  'e',  'l',  'l',  'o' /* label */
    };

    ASSERT_OK(dc_handle_message(&dc, 0, DCEP_PPID_CONTROL, open_msg, sizeof(open_msg), &opened));
    ASSERT_EQ(dc.channel_count, 1);
    ASSERT_EQ(dc.channels[0].state, NANORTC_DC_STATE_OPEN);
    ASSERT_TRUE(dc_has_pending_output(&dc));
    ASSERT_TRUE(opened);

    /* Output should be DCEP ACK (0x02) */
    uint8_t buf[256];
    size_t out_len = 0;
    uint16_t stream = 0;
    ASSERT_OK(dc_peek_output(&dc, buf, sizeof(buf), &out_len, &stream));
    dc_commit_output(&dc, stream);
    ASSERT_EQ(out_len, 1);
    ASSERT_EQ(buf[0], DCEP_DATA_CHANNEL_ACK);
}

TEST(test_dc_handle_open_idempotent)
{
    bool opened;
    nano_dc_t dc;
    dc_init(&dc);

    uint8_t open_msg[] = {0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                          0x00, 0x00, 0x02, 0x00, 0x00, 'h',  'i'};

    /* First OPEN — creates channel */
    ASSERT_OK(dc_handle_message(&dc, 0, DCEP_PPID_CONTROL, open_msg, sizeof(open_msg), &opened));
    ASSERT_EQ(dc.channel_count, 1);
    ASSERT_TRUE(opened);

    /* Drain output */
    uint8_t buf[256];
    size_t out_len = 0;
    uint16_t stream = 0;
    dc_peek_output(&dc, buf, sizeof(buf), &out_len, &stream);
    dc_commit_output(&dc, stream);

    /* Second OPEN (retransmit) — same stream, should re-ACK, NOT allocate new channel */
    ASSERT_OK(dc_handle_message(&dc, 0, DCEP_PPID_CONTROL, open_msg, sizeof(open_msg), &opened));
    ASSERT_EQ(dc.channel_count, 1);          /* Still 1 */
    ASSERT_FALSE(opened);                    /* Not a new open */
    ASSERT_TRUE(dc_has_pending_output(&dc)); /* Re-ACK queued */
}

TEST(test_dc_handle_ack)
{
    bool opened;
    nano_dc_t dc;
    dc_init(&dc);

    /* Open channel locally (state = OPENING) */
    dc_open(&dc, 0, "mych", true, 0);
    ASSERT_EQ(dc.channels[0].state, NANORTC_DC_STATE_OPENING);

    /* Simulate receiving ACK */
    uint8_t ack_msg[] = {DCEP_DATA_CHANNEL_ACK};
    ASSERT_OK(dc_handle_message(&dc, 0, DCEP_PPID_CONTROL, ack_msg, sizeof(ack_msg), &opened));
    ASSERT_EQ(dc.channels[0].state, NANORTC_DC_STATE_OPEN);
}

TEST(test_dc_handle_open_malformed)
{
    bool opened;
    nano_dc_t dc;
    dc_init(&dc);

    /* Too short */
    uint8_t short_msg[] = {0x03, 0x00};
    ASSERT_FAIL(
        dc_handle_message(&dc, 0, DCEP_PPID_CONTROL, short_msg, sizeof(short_msg), &opened));

    /* Empty DCEP control */
    ASSERT_FAIL(dc_handle_message(&dc, 0, DCEP_PPID_CONTROL, short_msg, 0, &opened));

    /* Unknown message type */
    uint8_t unknown[] = {0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    ASSERT_FAIL(dc_handle_message(&dc, 0, DCEP_PPID_CONTROL, unknown, sizeof(unknown), &opened));

    /* Label length exceeds buffer */
    uint8_t bad_len[] = {0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                         0x00, 0xFF, 0xFF, 0x00, 0x00}; /* label_len=65535 but only 12 bytes */
    ASSERT_FAIL(dc_handle_message(&dc, 0, DCEP_PPID_CONTROL, bad_len, sizeof(bad_len), &opened));
}

TEST(test_dc_handle_null)
{
    bool opened;
    ASSERT_FAIL(dc_handle_message(NULL, 0, DCEP_PPID_CONTROL, (uint8_t[]){0x03}, 1, &opened));
}

TEST(test_dc_handle_data_ppid)
{
    bool opened;
    nano_dc_t dc;
    dc_init(&dc);

    /* Data PPIDs should return OK without allocating channels */
    uint8_t data[] = {'h', 'e', 'l', 'l', 'o'};
    ASSERT_OK(dc_handle_message(&dc, 0, DCEP_PPID_STRING, data, sizeof(data), &opened));
    ASSERT_OK(dc_handle_message(&dc, 0, DCEP_PPID_BINARY, data, sizeof(data), &opened));
}

/* ================================================================
 * dc_peek_output edge cases
 * ================================================================ */

TEST(test_dc_poll_no_output)
{
    nano_dc_t dc;
    dc_init(&dc);

    uint8_t buf[256];
    size_t out_len = 0;
    uint16_t stream = 0;
    ASSERT_FAIL(dc_peek_output(&dc, buf, sizeof(buf), &out_len, &stream));
}

TEST(test_dc_poll_null_params)
{
    nano_dc_t dc;
    dc_init(&dc);
    uint8_t buf[256];
    size_t out_len = 0;
    uint16_t stream = 0;

    ASSERT_FAIL(dc_peek_output(NULL, buf, sizeof(buf), &out_len, &stream));
    ASSERT_FAIL(dc_peek_output(&dc, NULL, sizeof(buf), &out_len, &stream));
    ASSERT_FAIL(dc_peek_output(&dc, buf, sizeof(buf), NULL, &stream));
    ASSERT_FAIL(dc_peek_output(&dc, buf, sizeof(buf), &out_len, NULL));
}

TEST(test_dc_poll_buffer_too_small)
{
    nano_dc_t dc;
    dc_init(&dc);
    dc_open(&dc, 0, "test", true, 0);

    uint8_t buf[1]; /* Too small for DCEP OPEN */
    size_t out_len = 0;
    uint16_t stream = 0;
    ASSERT_FAIL(dc_peek_output(&dc, buf, sizeof(buf), &out_len, &stream));
}

/* ================================================================
 * Test runner
 * ================================================================ */

TEST(test_dcep_multiple_pending_opens_survive_short_poll)
{
    nano_dc_t dc;
    dc_init(&dc);
    ASSERT_OK(dc_open(&dc, 0, "first", true, 0));
    ASSERT_OK(dc_open(&dc, 2, "second", false, 3));
    uint8_t packet[NANORTC_DC_OUT_BUF_SIZE];
    size_t len;
    uint16_t sid;
    nano_dc_t before = dc;
    ASSERT_EQ(dc_peek_output(&dc, packet, 1, &len, &sid), NANORTC_ERR_BUFFER_TOO_SMALL);
    ASSERT_MEM_EQ(&before, &dc, sizeof(dc));
    ASSERT_EQ(dc_peek_output(&dc, packet, sizeof(packet), &len, &sid), NANORTC_OK);
    ASSERT_MEM_EQ(&before, &dc, sizeof(dc));
    ASSERT_OK(dc_peek_output(&dc, packet, sizeof(packet), &len, &sid));
    dc_commit_output(&dc, sid);
    ASSERT_EQ(sid, 0);
    ASSERT_MEM_EQ(packet + 12, "first", 5);
    ASSERT_OK(dc_peek_output(&dc, packet, sizeof(packet), &len, &sid));
    dc_commit_output(&dc, sid);
    ASSERT_EQ(sid, 2);
    ASSERT_MEM_EQ(packet + 12, "second", 6);
    ASSERT_EQ(packet[1], DCEP_CHANNEL_REXMIT_UNORDERED);
    ASSERT_EQ(nanortc_read_u32be(packet + 4), 3u);
    ASSERT_EQ(dc_peek_output(&dc, packet, sizeof(packet), &len, &sid), NANORTC_ERR_NO_DATA);
}

TEST_MAIN_BEGIN("test_datachannel")
RUN(test_dcep_multiple_pending_opens_survive_short_poll);
/* Init */
RUN(test_dc_init);
RUN(test_dc_init_null);
/* Open */
RUN(test_dc_open_reliable_ordered);
RUN(test_dc_open_unordered);
RUN(test_dc_open_rexmit);
RUN(test_dc_open_rexmit_unordered);
RUN(test_dc_open_null_params);
RUN(test_dc_open_max_channels);
/* Handle message */
RUN(test_dc_handle_open);
RUN(test_dc_handle_open_idempotent);
RUN(test_dc_handle_ack);
RUN(test_dc_handle_open_malformed);
RUN(test_dc_handle_null);
RUN(test_dc_handle_data_ppid);
/* Poll output */
RUN(test_dc_poll_no_output);
RUN(test_dc_poll_null_params);
RUN(test_dc_poll_buffer_too_small);
TEST_MAIN_END
