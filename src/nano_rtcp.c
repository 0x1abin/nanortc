/*
 * nanortc — RTCP reports (RFC 3550 / RFC 4585)
 *
 * Generates and parses RTCP Sender Reports, Receiver Reports,
 * and Generic NACKs for media quality feedback.
 *
 * SPDX-License-Identifier: MIT
 */

#include "nano_rtcp.h"
#include "nano_log.h"
#include "nanortc.h"
#include <string.h>

/*
 * Write RTCP common header (RFC 3550 §6.4):
 *   0                   1                   2                   3
 *   0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *  |V=2|P|  RC/FMT |      PT       |            length             |
 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *  |                         SSRC of sender                        |
 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 */
static void write_header(uint8_t *buf, uint8_t rc_fmt, uint8_t pt, uint16_t length_words,
                         uint32_t ssrc)
{
    buf[0] = (RTCP_VERSION << 6) | (rc_fmt & 0x1F);
    buf[1] = pt;
    nanortc_write_u16be(buf + 2, length_words);
    nanortc_write_u32be(buf + 4, ssrc);
}

/* ================================================================
 * Init
 * ================================================================ */

int rtcp_init(nano_rtcp_t *rtcp, uint32_t ssrc)
{
    if (!rtcp) {
        return NANORTC_ERR_INVALID_PARAM;
    }
    memset(rtcp, 0, sizeof(*rtcp));
    rtcp->ssrc = ssrc;
    NANORTC_LOGI("RTCP", "init ok");
    return NANORTC_OK;
}

/* ================================================================
 * Sender Report (RFC 3550 §6.4.1)
 *
 *  Header (8 bytes) + Sender Info (20 bytes) = 28 bytes total
 *  No report blocks in this minimal implementation.
 *
 *  Sender Info:
 *    NTP timestamp (64 bits) + RTP timestamp (32) +
 *    sender's packet count (32) + sender's octet count (32)
 *
 *  Length field = (28/4 - 1) = 6 (in 32-bit words minus one)
 * ================================================================ */

int rtcp_generate_sr(nano_rtcp_t *rtcp, uint32_t ntp_sec, uint32_t ntp_frac, uint32_t rtp_ts,
                     uint8_t *buf, size_t buf_len, size_t *out_len)
{
    if (!rtcp || !buf || !out_len) {
        return NANORTC_ERR_INVALID_PARAM;
    }
    if (buf_len < RTCP_SR_SIZE) {
        return NANORTC_ERR_BUFFER_TOO_SMALL;
    }

    /* RC=0 (no report blocks), PT=200, length=6 words */
    write_header(buf, 0, RTCP_SR, 6, rtcp->ssrc);

    /* Sender info (20 bytes at offset 8) */
    nanortc_write_u32be(buf + 8, ntp_sec);
    nanortc_write_u32be(buf + 12, ntp_frac);
    nanortc_write_u32be(buf + 16, rtp_ts);
    nanortc_write_u32be(buf + 20, rtcp->packets_sent);
    nanortc_write_u32be(buf + 24, rtcp->octets_sent);

    *out_len = RTCP_SR_SIZE;
    return NANORTC_OK;
}

/* ================================================================
 * Receiver Report (RFC 3550 §6.4.2)
 *
 *  Header (8 bytes) + 1 Report Block (24 bytes) = 32 bytes total
 *
 *  Report Block:
 *    SSRC_1 (32) + fraction lost (8) + cumulative lost (24) +
 *    highest seq received (32) + jitter (32) +
 *    last SR (32) + delay since last SR (32)
 *
 *  Length field = (32/4 - 1) = 7
 * ================================================================ */

int rtcp_generate_rr(nano_rtcp_t *rtcp, uint32_t remote_ssrc, uint8_t *buf, size_t buf_len,
                     size_t *out_len)
{
    if (!rtcp || !buf || !out_len) {
        return NANORTC_ERR_INVALID_PARAM;
    }
    if (buf_len < RTCP_RR_SIZE) {
        return NANORTC_ERR_BUFFER_TOO_SMALL;
    }

    /* RC=1 (one report block), PT=201, length=7 words */
    write_header(buf, 1, RTCP_RR, 7, rtcp->ssrc);

    /* Report block (24 bytes at offset 8) */
    nanortc_write_u32be(buf + 8, remote_ssrc);

    /* Fraction lost (8 bits) + cumulative lost (24 bits) */
    uint8_t fraction_lost = 0;
    if (rtcp->packets_received > 0 && rtcp->packets_lost > 0) {
        /* fraction = (lost / expected) * 256, simplified */
        uint32_t expected = rtcp->packets_received + rtcp->packets_lost;
        fraction_lost = (uint8_t)((rtcp->packets_lost * 256) / expected);
    }
    buf[12] = fraction_lost;
    /* Cumulative packets lost (24-bit signed, clamped to positive here) */
    uint32_t clost = rtcp->packets_lost & 0x00FFFFFF;
    buf[13] = (uint8_t)(clost >> 16);
    buf[14] = (uint8_t)(clost >> 8);
    buf[15] = (uint8_t)(clost);

    /* Extended highest sequence number received:
     * high 16 bits = cycles (0 for now), low 16 bits = max_seq */
    nanortc_write_u32be(buf + 16, (uint32_t)rtcp->max_seq);

    /* Interarrival jitter */
    nanortc_write_u32be(buf + 20, rtcp->jitter);

    /* Last SR (LSR): middle 32 bits of NTP timestamp from last SR */
    nanortc_write_u32be(buf + 24, rtcp->last_sr_ntp);

    /* Delay since last SR (DLSR): 0 if no SR received */
    nanortc_write_u32be(buf + 28, 0);

    *out_len = RTCP_RR_SIZE;
    return NANORTC_OK;
}

/* ================================================================
 * Generic NACK (RFC 4585 §6.2.1)
 *
 *  Header (8 bytes) + SSRC of media source (4) + FCI (4) = 16 bytes
 *
 *  FCI: PID (16 bits) + BLP (16 bits)
 *  PID = sequence number of lost packet
 *  BLP = bitmask of following 16 lost packets
 *
 *  FMT=1 for Generic NACK, PT=RTPFB(205), length=3
 * ================================================================ */

int rtcp_generate_nack(uint32_t ssrc, uint32_t media_ssrc, uint16_t seq, uint8_t *buf,
                       size_t buf_len, size_t *out_len)
{
    if (!buf || !out_len) {
        return NANORTC_ERR_INVALID_PARAM;
    }
    if (buf_len < RTCP_NACK_SIZE) {
        return NANORTC_ERR_BUFFER_TOO_SMALL;
    }

    /* FMT=1, PT=205(RTPFB), length=3 */
    write_header(buf, 1, RTCP_RTPFB, 3, ssrc);

    /* SSRC of media source */
    nanortc_write_u32be(buf + 8, media_ssrc);

    /* FCI: PID + BLP (single lost packet, no bitmask) */
    nanortc_write_u16be(buf + 12, seq);
    nanortc_write_u16be(buf + 14, 0); /* BLP=0: only the single packet */

    *out_len = RTCP_NACK_SIZE;
    return NANORTC_OK;
}

/* ================================================================
 * PLI — Picture Loss Indication (RFC 4585 §6.3.1)
 *
 *  PSFB header (4 bytes): V=2, P=0, FMT=1, PT=206, length=2
 *  Sender SSRC (4 bytes)
 *  Media SSRC (4 bytes) = SSRC of the video source to request keyframe from
 *
 *  Total: 12 bytes
 * ================================================================ */

int rtcp_generate_pli(uint32_t sender_ssrc, uint32_t media_ssrc, uint8_t *buf, size_t buf_len,
                      size_t *out_len)
{
    if (!buf || !out_len) {
        return NANORTC_ERR_INVALID_PARAM;
    }
    if (buf_len < RTCP_PLI_SIZE) {
        return NANORTC_ERR_BUFFER_TOO_SMALL;
    }

    /* FMT=1, PT=206(PSFB), length=2 words (12 bytes / 4 - 1) */
    write_header(buf, 1, RTCP_PSFB, 2, sender_ssrc);

    /* Media source SSRC */
    nanortc_write_u32be(buf + 8, media_ssrc);

    *out_len = RTCP_PLI_SIZE;
    return NANORTC_OK;
}

/* ================================================================
 * Parser (RFC 3550 §6 / RFC 4585)
 *
 * Parses a single RTCP packet and fills nano_rtcp_info_t.
 * ================================================================ */

int rtcp_parse(const uint8_t *data, size_t len, nano_rtcp_info_t *info)
{
    if (!data || !info || len < RTCP_HEADER_SIZE) {
        return NANORTC_ERR_INVALID_PARAM;
    }

    memset(info, 0, sizeof(*info));

    /* Validate version */
    uint8_t version = (data[0] >> 6) & 0x03;
    if (version != RTCP_VERSION) {
        NANORTC_LOGW("RTCP", "bad version in RTCP packet");
        return NANORTC_ERR_PARSE;
    }

    info->type = data[1];
    uint16_t length_words = nanortc_read_u16be(data + 2);
    uint32_t pkt_size = ((uint32_t)length_words + 1) * 4;
    if (pkt_size > len) {
        NANORTC_LOGW("RTCP", "truncated RTCP packet");
        return NANORTC_ERR_PARSE;
    }

    info->ssrc = nanortc_read_u32be(data + 4);

    switch (info->type) {
    case RTCP_SR:
        /* Sender Report: need at least 28 bytes */
        if (pkt_size < RTCP_SR_SIZE) {
            return NANORTC_ERR_PARSE;
        }
        info->ntp_sec = nanortc_read_u32be(data + 8);
        info->ntp_frac = nanortc_read_u32be(data + 12);
        info->rtp_ts = nanortc_read_u32be(data + 16);
        info->sr_packets = nanortc_read_u32be(data + 20);
        info->sr_octets = nanortc_read_u32be(data + 24);
        break;

    case RTCP_RR:
        /* Receiver Report: at least header (8 bytes) is valid */
        break;

    case RTCP_RTPFB: {
        /* Generic NACK: need at least 16 bytes */
        if (pkt_size < RTCP_NACK_SIZE) {
            return NANORTC_ERR_PARSE;
        }
        /* FMT field */
        uint8_t fmt = data[0] & 0x1F;
        if (fmt == 1) {
            /* media SSRC at offset 8, FCI at offset 12 */
            info->nack_pid = nanortc_read_u16be(data + 12);
            info->nack_blp = nanortc_read_u16be(data + 14);
        }
        break;
    }

    case RTCP_PSFB:
        /* PLI: just record the type, no additional parsing needed */
        break;

    default:
        /* Unknown RTCP type — not an error, just ignore contents */
        break;
    }

    return NANORTC_OK;
}
