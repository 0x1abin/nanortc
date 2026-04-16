/*
 * nanortc — Transport-Wide Congestion Control (TWCC) feedback parsing
 *
 * Implements draft-holmer-rmcat-transport-wide-cc-extensions-01 §3.1
 * (the de-facto format used by Chrome/libwebrtc/libdatachannel).
 *
 * Feedback layout:
 *
 *   0                   1                   2                   3
 *   0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *  |V=2|P|  FMT=15 |    PT=205     |           length              |
 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *  |                     SSRC of packet sender                     |
 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *  |                      SSRC of media source                     |
 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *  |      base sequence number     |      packet status count      |
 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *  |                 reference time                | fb pkt. count |
 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *  |                          packet chunks                        |
 *  .                                                               .
 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *  |                         receive deltas                        |
 *  .                                                               .
 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *
 * Packet chunks (§3.1.1) are one of:
 *   Run Length:    [0][SS ][run_length   ]  (13-bit run of identical status)
 *   Status Vector: [1][S ][symbols       ]  (14 x 1-bit or 7 x 2-bit)
 *
 * SPDX-License-Identifier: MIT
 */

#include "nano_twcc.h"
#include "nanortc.h"
#include <string.h>

#define RTCP_RTPFB   205
#define RTCP_VERSION 2

/* Reference time unit (draft §3.1): 64 ms per tick, 24-bit signed */
#define TWCC_REF_TIME_UNIT_US 64000LL

/* Receive delta resolution (draft §3.1.2): 250 us per tick */
#define TWCC_DELTA_UNIT_US 250

/* Packet chunk type bits.
 *   TWCC_CHUNK_TYPE_MASK:      0 = run-length chunk, 1 = status-vector chunk
 *   TWCC_STATUS_VEC_SYMSZ_BIT: 0 = 1-bit symbols, 1 = 2-bit symbols (valid
 *                              only when the chunk is a status vector) */
#define TWCC_CHUNK_TYPE_MASK      0x8000
#define TWCC_STATUS_VEC_SYMSZ_BIT 0x4000

static int parse_chunks(const uint8_t *data, size_t pkt_size, size_t *offset, uint16_t packet_count,
                        uint8_t *statuses, uint16_t *received_count)
{
    size_t off = *offset;
    uint16_t remaining = packet_count;
    uint16_t emitted = 0;

    while (remaining > 0) {
        if (off + 2 > pkt_size) {
            return NANORTC_ERR_PARSE;
        }
        uint16_t chunk = nanortc_read_u16be(data + off);
        off += 2;

        if ((chunk & TWCC_CHUNK_TYPE_MASK) == 0) {
            /* Run-length: [0][SS(2)][run(13)] */
            nano_twcc_status_t symbol = (nano_twcc_status_t)((chunk >> 13) & 0x3);
            uint16_t run = chunk & 0x1FFF;
            if (run == 0) {
                return NANORTC_ERR_PARSE;
            }
            uint16_t n = (run > remaining) ? remaining : run;
            for (uint16_t i = 0; i < n; i++) {
                statuses[emitted++] = (uint8_t)symbol;
                if (symbol != NANO_TWCC_STATUS_NOT_RECEIVED) {
                    (*received_count)++;
                }
            }
            remaining -= n;
        } else if ((chunk & TWCC_STATUS_VEC_SYMSZ_BIT) == 0) {
            /* Status vector S=0: 14 symbols, 1 bit each.
             * 0 = not received, 1 = received (small delta). */
            for (int i = 0; i < 14 && remaining > 0; i++) {
                int shift = 13 - i;
                uint8_t bit = (uint8_t)((chunk >> shift) & 0x1);
                nano_twcc_status_t symbol =
                    bit ? NANO_TWCC_STATUS_SMALL_DELTA : NANO_TWCC_STATUS_NOT_RECEIVED;
                statuses[emitted++] = (uint8_t)symbol;
                if (bit) {
                    (*received_count)++;
                }
                remaining--;
            }
        } else {
            /* Status vector S=1: 7 symbols, 2 bits each. */
            for (int i = 0; i < 7 && remaining > 0; i++) {
                int shift = (6 - i) * 2;
                nano_twcc_status_t symbol = (nano_twcc_status_t)((chunk >> shift) & 0x3);
                statuses[emitted++] = (uint8_t)symbol;
                if (symbol != NANO_TWCC_STATUS_NOT_RECEIVED) {
                    (*received_count)++;
                }
                remaining--;
            }
        }
    }

    *offset = off;
    return NANORTC_OK;
}

int twcc_parse_feedback(const uint8_t *data, size_t len, nano_twcc_summary_t *out,
                        nano_twcc_packet_cb_t cb, void *user)
{
    if (!data || !out) {
        return NANORTC_ERR_INVALID_PARAM;
    }

    /* Minimum header: RTCP header (4) + sender SSRC (4) + media SSRC (4)
     * + base_seq (2) + packet_status_count (2) + reference_time (3) +
     * fb_pkt_count (1) = 20 bytes. A real feedback with at least one
     * chunk is 22+ bytes (padded to 24). */
    if (len < 20) {
        return NANORTC_ERR_PARSE;
    }

    /* Validate V=2, FMT=15, PT=205. */
    if ((data[0] >> 6) != RTCP_VERSION) {
        return NANORTC_ERR_PARSE;
    }
    if ((data[0] & 0x1F) != TWCC_FMT) {
        return NANORTC_ERR_PARSE;
    }
    if (data[1] != RTCP_RTPFB) {
        return NANORTC_ERR_PARSE;
    }

    /* RTCP length field: 32-bit words minus 1. */
    uint16_t length_words = nanortc_read_u16be(data + 2);
    size_t pkt_size = ((size_t)length_words + 1) * 4;
    if (pkt_size > len || pkt_size < 20) {
        return NANORTC_ERR_PARSE;
    }

    memset(out, 0, sizeof(*out));
    out->sender_ssrc = nanortc_read_u32be(data + 4);
    out->media_ssrc = nanortc_read_u32be(data + 8);
    out->base_seq = nanortc_read_u16be(data + 12);
    out->packet_status_count = nanortc_read_u16be(data + 14);

    /* Reference time: 24-bit signed (big-endian), then 8-bit fb_pkt_count. */
    int32_t ref_raw = ((int32_t)data[16] << 16) | ((int32_t)data[17] << 8) | (int32_t)data[18];
    if (ref_raw & 0x00800000) {
        ref_raw |= (int32_t)0xFF000000;
    }
    out->reference_time_us = (int64_t)ref_raw * TWCC_REF_TIME_UNIT_US;
    out->fb_pkt_count = data[19];

    if (out->packet_status_count == 0) {
        return NANORTC_OK;
    }
    if (out->packet_status_count > NANORTC_TWCC_MAX_PACKETS_PER_FB) {
        return NANORTC_ERR_PARSE;
    }

    /* Pass 1: read chunks, collect per-packet status into a stack buffer. */
    uint8_t statuses[NANORTC_TWCC_MAX_PACKETS_PER_FB];
    size_t off = 20;
    int rc = parse_chunks(data, pkt_size, &off, out->packet_status_count, statuses,
                          &out->received_count);
    if (rc != NANORTC_OK) {
        return rc;
    }

    /* Pass 2: read deltas. Fire callbacks if provided, else just validate
     * that the expected bytes are present. */
    for (uint16_t i = 0; i < out->packet_status_count; i++) {
        nano_twcc_status_t status = (nano_twcc_status_t)statuses[i];
        int32_t delta_us = 0;

        if (status == NANO_TWCC_STATUS_SMALL_DELTA) {
            if (off + 1 > pkt_size) {
                return NANORTC_ERR_PARSE;
            }
            delta_us = (int32_t)data[off] * TWCC_DELTA_UNIT_US;
            off += 1;
        } else if (status == NANO_TWCC_STATUS_LARGE_DELTA) {
            if (off + 2 > pkt_size) {
                return NANORTC_ERR_PARSE;
            }
            int16_t raw = (int16_t)nanortc_read_u16be(data + off);
            delta_us = (int32_t)raw * TWCC_DELTA_UNIT_US;
            off += 2;
        }

        if (cb) {
            uint16_t seq = (uint16_t)(out->base_seq + i);
            cb(seq, status, delta_us, user);
        }
    }

    return NANORTC_OK;
}
