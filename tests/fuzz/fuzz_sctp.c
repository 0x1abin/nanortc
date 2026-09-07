/*
 * Fuzz harness for SCTP parsers — nano_sctp.c
 *
 * Targets: nsctp_parse_header(), nsctp_parse_init(), nsctp_parse_data(),
 *          nsctp_parse_sack(), nsctp_verify_checksum()
 * Attack surface: Largest parser module (1184 lines), complex chunk parsing.
 *
 * SPDX-License-Identifier: MIT
 */

#include "nano_sctp.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    /* Parse SCTP header */
    nsctp_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    nsctp_parse_header(data, size, &hdr);

    /* Verify checksum */
    nsctp_verify_checksum(data, size);

    /* Try parsing the payload area as individual chunk types.
     * SCTP chunks start after the 12-byte common header. */
    if (size > 12) {
        const uint8_t *chunk = data + 12;
        size_t chunk_len = size - 12;

        nsctp_init_t init;
        memset(&init, 0, sizeof(init));
        nsctp_parse_init(chunk, chunk_len, &init);

        nsctp_data_t dat;
        memset(&dat, 0, sizeof(dat));
        nsctp_parse_data(chunk, chunk_len, &dat);

        nsctp_sack_t sack;
        memset(&sack, 0, sizeof(sack));
        nsctp_parse_sack(chunk, chunk_len, &sack);
    }

    /* Bounded stateful sequence. Recompute CRC to reach chunk/state handling,
     * retain multiple packets before draining, and exercise short output caps. */
    nano_sctp_t sctp;
    nsctp_init(&sctp);
    sctp.state = NANORTC_SCTP_STATE_ESTABLISHED;
    sctp.peer_forward_tsn = true;
    sctp.cumulative_tsn = 0;
    uint8_t packet[NANORTC_SCTP_MTU];
    size_t pos = 0;
    uint32_t now = 0;
    for (unsigned step = 0; step < 32 && pos < size; step++) {
        uint8_t command = data[pos++];
        if (size - pos < 2)
            break;
        size_t n = ((size_t)data[pos] << 8) | data[pos + 1];
        pos += 2;
        if (n > size - pos)
            n = size - pos;
        if (n > NANORTC_SCTP_MTU - 12u)
            n = NANORTC_SCTP_MTU - 12u;
        if (command & 1u) {
            memset(packet, 0, 12);
            memcpy(packet + 12, data + pos, n);
            nsctp_finalize_checksum(packet, n + 12);
            nsctp_handle_data(&sctp, packet, n + 12);
        } else {
            nsctp_send_options(&sctp, command % NANORTC_MAX_DATACHANNELS, 53, data + pos, n,
                               (command & 2u) != 0, -1);
        }
        pos += n;
        now += command * 100u;
        nsctp_handle_timeout(&sctp, now);
        size_t out_len = 0;
        nsctp_poll_output(&sctp, packet, command & 4u ? 16u : sizeof(packet), &out_len);
        if (command & 8u) {
            nano_sctp_message_t message;
            while (nsctp_poll_delivery(&sctp, &message) == 0)
                assert(message.len <= NANORTC_SCTP_MAX_MESSAGE_SIZE);
        }
        assert(sctp.recv_gap_count <= NANORTC_SCTP_MAX_RECV_GAP);
        assert(sctp.recv_gap_buf_used <= sizeof(sctp.recv_gap_buf));
        assert((uint8_t)(sctp.sq_tail - sctp.sq_head) <= NANORTC_SCTP_MAX_SEND_QUEUE);
    }

    return 0;
}
