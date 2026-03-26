/*
 * nanortc — RTCP reports (RFC 3550 / RFC 4585)
 *
 * SPDX-License-Identifier: MIT
 */

#include "nano_rtcp.h"
#include <string.h>

int rtcp_init(nano_rtcp_t *rtcp, uint32_t ssrc)
{
    if (!rtcp) {
        return -1;
    }
    memset(rtcp, 0, sizeof(*rtcp));
    rtcp->ssrc = ssrc;
    return 0;
}

int rtcp_generate_sr(nano_rtcp_t *rtcp, uint32_t ntp_sec, uint32_t ntp_frac,
                     uint32_t rtp_ts, uint8_t *buf, size_t buf_len, size_t *out_len)
{
    (void)rtcp;
    (void)ntp_sec;
    (void)ntp_frac;
    (void)rtp_ts;
    (void)buf;
    (void)buf_len;
    (void)out_len;
    return -1;
}

int rtcp_generate_rr(nano_rtcp_t *rtcp, uint32_t remote_ssrc,
                     uint8_t *buf, size_t buf_len, size_t *out_len)
{
    (void)rtcp;
    (void)remote_ssrc;
    (void)buf;
    (void)buf_len;
    (void)out_len;
    return -1;
}

int rtcp_generate_nack(uint32_t ssrc, uint32_t media_ssrc,
                       uint16_t seq, uint8_t *buf, size_t buf_len, size_t *out_len)
{
    (void)ssrc;
    (void)media_ssrc;
    (void)seq;
    (void)buf;
    (void)buf_len;
    (void)out_len;
    return -1;
}
