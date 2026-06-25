/*
 * nanortc — ULPFEC codec (RFC 5109 level-0) — internal interface
 * @internal Not part of the public API.
 *
 * XOR-based forward error correction: a FEC packet protects a group of up to 16
 * media RTP packets so the receiver can reconstruct ANY ONE lost member without
 * a retransmit. Pure logic (no allocation, no I/O); operates on plaintext RTP
 * packets (FEC is generated before SRTP / carried in RED at the integration
 * layer — Phase 13 PR-2/3/4). Gated by NANORTC_FEATURE_VIDEO_FEC (opt-in).
 *
 * Wire format (the bytes AFTER the FEC packet's own RTP header):
 *   FEC header (10 B, RFC 5109 §7.3) + FEC level-0 header (4 B, §7.4) + payload.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef NANORTC_FEC_H_
#define NANORTC_FEC_H_

#include "nanortc_config.h"

#include <stddef.h>
#include <stdint.h>

#if NANORTC_FEATURE_VIDEO && NANORTC_FEATURE_VIDEO_FEC

#define FEC_HEADER_SIZE        10 /* RFC 5109 §7.3 */
#define FEC_LEVEL0_HEADER_SIZE 4  /* §7.4: protection length (2) + mask (2) */
#define FEC_OVERHEAD           (FEC_HEADER_SIZE + FEC_LEVEL0_HEADER_SIZE)
#define FEC_MAX_GROUP          16 /* 16-bit level-0 mask */
#define FEC_RTP_HDR            12 /* fixed RTP header protected by the codec */

/**
 * Encode one ULPFEC packet (the FEC header + level-0 header + payload, i.e. the
 * bytes after the FEC packet's own RTP header) protecting @p count media RTP
 * packets. @p pkts[i] is a full plaintext RTP packet of length @p lens[i] (each
 * >= FEC_RTP_HDR). The packets' sequence numbers must span < 16. @count in
 * 1..FEC_MAX_GROUP.
 *
 * @return NANORTC_OK with *out_len set, or NANORTC_ERR_INVALID_PARAM /
 * NANORTC_ERR_BUFFER_TOO_SMALL.
 */
int fec_encode(const uint8_t *const *pkts, const uint16_t *lens, uint8_t count, uint8_t *out,
               size_t out_cap, size_t *out_len);

/**
 * Recover the single missing member of a FEC group. @p fec/@p fec_len is the
 * FEC packet body (post-RTP-header, as produced by fec_encode). @p recv[] are
 * the received members of the same group (a subset; the codec finds the one
 * protected SN that is absent). @p media_ssrc is the SSRC to stamp on the
 * recovered RTP packet (ULPFEC does not carry it). Recovers exactly one loss;
 * returns NANORTC_ERR_NO_DATA if zero or ≥2 protected members are missing.
 *
 * @return NANORTC_OK with the recovered full RTP packet in @p out (*out_len, and
 * its sequence number in *out_seq), else a negative error.
 */
int fec_recover(const uint8_t *fec, size_t fec_len, uint32_t media_ssrc, const uint8_t *const *recv,
                const uint16_t *recv_lens, uint8_t n_recv, uint8_t *out, size_t out_cap,
                size_t *out_len, uint16_t *out_seq);

#endif /* NANORTC_FEATURE_VIDEO && NANORTC_FEATURE_VIDEO_FEC */
#endif /* NANORTC_FEC_H_ */
