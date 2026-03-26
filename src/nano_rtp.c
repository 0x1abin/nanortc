/*
 * nanortc — RTP packing/unpacking (RFC 3550)
 *
 * SPDX-License-Identifier: MIT
 */

#include "nano_rtp.h"
#include <string.h>

int rtp_init(nano_rtp_t *rtp, uint32_t ssrc, uint8_t pt)
{
    if (!rtp) {
        return -1;
    }
    memset(rtp, 0, sizeof(*rtp));
    rtp->ssrc = ssrc;
    rtp->payload_type = pt;
    return 0;
}

int rtp_pack(nano_rtp_t *rtp, uint32_t timestamp, const uint8_t *payload, size_t payload_len,
             uint8_t *buf, size_t buf_len, size_t *out_len)
{
    (void)rtp;
    (void)timestamp;
    (void)payload;
    (void)payload_len;
    (void)buf;
    (void)buf_len;
    (void)out_len;
    return -1;
}

int rtp_unpack(const uint8_t *data, size_t len, uint8_t *pt, uint16_t *seq, uint32_t *ts,
               uint32_t *ssrc, const uint8_t **payload, size_t *payload_len)
{
    (void)data;
    (void)len;
    (void)pt;
    (void)seq;
    (void)ts;
    (void)ssrc;
    (void)payload;
    (void)payload_len;
    return -1;
}
