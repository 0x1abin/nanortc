/*
 * nanortc — RTCP internal interface (RFC 3550 / RFC 4585)
 * @internal Not part of the public API.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef NANORTC_RTCP_H_
#define NANORTC_RTCP_H_

#include <stdint.h>
#include <stddef.h>

/* RTCP packet types (RFC 3550 §12.1) */
#define RTCP_SR    200
#define RTCP_RR    201
#define RTCP_RTPFB 205 /* NACK (RFC 4585) */
#define RTCP_PSFB  206 /* PLI  (RFC 4585) */

/* RTCP header size (V=2, P=0, RC/FMT, PT, length) */
#define RTCP_HEADER_SIZE 8

/* Sender Report sizes (RFC 3550 §6.4.1) */
#define RTCP_SR_SIZE         28 /* header(8) + sender info(20), no report blocks */
#define RTCP_SR_WITH_RB_SIZE 52 /* header(8) + sender info(20) + 1 report block(24) */

/* Receiver Report size (RFC 3550 §6.4.2) */
#define RTCP_RR_SIZE 32 /* header(8) + 1 report block(24) */

/* Generic NACK size (RFC 4585 §6.2.1) */
#define RTCP_NACK_SIZE 16 /* header(8) + SSRC of media(4) + FCI(4) */

/* PLI size (RFC 4585 §6.3.1): FMT=1, PT=206 (PSFB) */
#define RTCP_PLI_SIZE 12 /* header(4) + sender_ssrc(4) + media_ssrc(4) */

/* RTCP version */
#define RTCP_VERSION 2

typedef struct nano_rtcp {
    uint32_t ssrc;

    /* Sender stats */
    uint32_t packets_sent;
    uint32_t octets_sent;

    /* Receiver stats */
    uint32_t packets_received;
    uint32_t packets_lost;
    uint32_t remote_ssrc;     /* SSRC of remote sender */
    uint32_t last_sr_ntp;     /* Middle 32 bits of last SR NTP timestamp (for DLSR) */
    uint32_t last_sr_recv_ms; /* Local time when last SR was received */
    uint16_t max_seq;         /* Highest seq received */
    uint32_t jitter;          /* Interarrival jitter (RFC 3550 §6.4.1) */
} nano_rtcp_t;

/* Parsed RTCP packet info */
typedef struct nano_rtcp_info {
    uint8_t type;  /* RTCP_SR, RTCP_RR, RTCP_RTPFB, RTCP_PSFB */
    uint32_t ssrc; /* Sender SSRC */

    /* SR-specific */
    uint32_t ntp_sec;
    uint32_t ntp_frac;
    uint32_t rtp_ts;
    uint32_t sr_packets;
    uint32_t sr_octets;

    /* NACK-specific */
    uint16_t nack_pid; /* Packet ID (seq) */
    uint16_t nack_blp; /* Bitmask of following lost packets */
} nano_rtcp_info_t;

int rtcp_init(nano_rtcp_t *rtcp, uint32_t ssrc);

/* Generate Sender Report (RFC 3550 §6.4.1) — 28 bytes, no report blocks */
int rtcp_generate_sr(nano_rtcp_t *rtcp, uint32_t ntp_sec, uint32_t ntp_frac, uint32_t rtp_ts,
                     uint8_t *buf, size_t buf_len, size_t *out_len);

/* Generate Receiver Report (RFC 3550 §6.4.2) — 32 bytes, 1 report block */
int rtcp_generate_rr(nano_rtcp_t *rtcp, uint32_t remote_ssrc, uint8_t *buf, size_t buf_len,
                     size_t *out_len);

/* Generate Generic NACK (RFC 4585 §6.2.1) — 16 bytes */
int rtcp_generate_nack(uint32_t ssrc, uint32_t media_ssrc, uint16_t seq, uint8_t *buf,
                       size_t buf_len, size_t *out_len);

/* Generate PLI — Picture Loss Indication (RFC 4585 §6.3.1)
 * FMT=1, PT=206 (PSFB), 12 bytes total */
int rtcp_generate_pli(uint32_t sender_ssrc, uint32_t media_ssrc, uint8_t *buf, size_t buf_len,
                      size_t *out_len);

/* Parse incoming RTCP packet */
int rtcp_parse(const uint8_t *data, size_t len, nano_rtcp_info_t *info);

#endif /* NANORTC_RTCP_H_ */
