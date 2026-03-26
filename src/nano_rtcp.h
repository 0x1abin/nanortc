/*
 * nanortc — RTCP internal interface
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef NANO_RTCP_H_
#define NANO_RTCP_H_

#include <stdint.h>
#include <stddef.h>

/* RTCP packet types */
#define RTCP_SR    200
#define RTCP_RR    201
#define RTCP_RTPFB 205 /* NACK */
#define RTCP_PSFB  206 /* PLI */

typedef struct nano_rtcp {
    uint32_t ssrc;
    uint32_t packets_sent;
    uint32_t octets_sent;
    uint32_t packets_received;
    uint32_t packets_lost;
} nano_rtcp_t;

int rtcp_init(nano_rtcp_t *rtcp, uint32_t ssrc);
int rtcp_generate_sr(nano_rtcp_t *rtcp, uint32_t ntp_sec, uint32_t ntp_frac, uint32_t rtp_ts,
                     uint8_t *buf, size_t buf_len, size_t *out_len);
int rtcp_generate_rr(nano_rtcp_t *rtcp, uint32_t remote_ssrc, uint8_t *buf, size_t buf_len,
                     size_t *out_len);
int rtcp_generate_nack(uint32_t ssrc, uint32_t media_ssrc, uint16_t seq, uint8_t *buf,
                       size_t buf_len, size_t *out_len);

#endif /* NANO_RTCP_H_ */
