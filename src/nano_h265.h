/*
 * nanortc — H.265/HEVC RTP packetization/depacketization (RFC 7798)
 * @internal Not part of the public API.
 *
 * Scope (first pass, matches the Phase 3.5 exec plan):
 *   - Single NAL Unit Packet      — RFC 7798 §4.4.1   (send + receive)
 *   - Aggregation Packet (AP)     — RFC 7798 §4.4.2   (send + receive)
 *   - Fragmentation Unit (FU)     — RFC 7798 §4.4.3   (send + receive)
 *   - Single-stream tx-mode SRST  — RFC 7798 §4.1     (only)
 *   - sprop-max-don-diff == 0     — RFC 7798 §7.1     (DON field never emitted,
 *                                                      rejected on receive if non-zero)
 *
 * Out of scope:
 *   - PACI Packet                 — RFC 7798 §4.4.4   (MAY; dropped on receive)
 *   - MSST / MSMT transmission    — RFC 7798 §4.1     (multi-session, conference use only)
 *   - DON reordering              — RFC 7798 §6.1     (first pass assumes decode order == rx order)
 *
 * The packetizer is callback-driven and holds no internal buffering, matching
 * the nano_h264.c pattern. The depacketizer holds a single reassembly buffer
 * per track, sized via NANORTC_VIDEO_NAL_BUF_SIZE.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef NANORTC_H265_H_
#define NANORTC_H265_H_

#include "nanortc_config.h"
#include "nano_annex_b.h"

#include <stdint.h>
#include <stddef.h>

/* ================================================================
 * RFC 7798 §1.1.4 — HEVC NAL unit header (2 bytes)
 *
 *   +---------------+---------------+
 *   |0|1|2|3|4|5|6|7|0|1|2|3|4|5|6|7|
 *   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *   |F|   Type    |  LayerId  | TID |
 *   +---------------+---------------+
 *
 *   F  (1 bit)  forbidden_zero_bit           — MUST be 0.
 *   Type (6)    nal_unit_type                — 0..63.
 *   LayerId(6)  nuh_layer_id                 — 0..63.
 *   TID  (3)    nuh_temporal_id_plus1        — MUST be > 0.
 *
 * Byte layout:
 *   byte[0] = F(1) | Type(6)         | LayerId[5]
 *   byte[1] = LayerId[4:0](5) | TID(3)
 *
 * Type extraction:   (byte[0] >> 1) & 0x3F
 * LayerId:           ((byte[0] & 0x01) << 5) | ((byte[1] >> 3) & 0x1F)
 * TID:               byte[1] & 0x07
 * ================================================================ */

#define H265_NAL_HEADER_SIZE 2
#define H265_NAL_F_BIT       0x80 /* byte[0] bit 7 */

/* RFC 7798 §1.1.4 / H.265 Table 7-1 — selected NAL unit type codes */
#define H265_NAL_TRAIL_N        0
#define H265_NAL_TRAIL_R        1
#define H265_NAL_TSA_N          2
#define H265_NAL_TSA_R          3
#define H265_NAL_STSA_N         4
#define H265_NAL_STSA_R         5
#define H265_NAL_RADL_N         6
#define H265_NAL_RADL_R         7
#define H265_NAL_RASL_N         8
#define H265_NAL_RASL_R         9
#define H265_NAL_BLA_W_LP       16 /* IRAP range start */
#define H265_NAL_BLA_W_RADL     17
#define H265_NAL_BLA_N_LP       18
#define H265_NAL_IDR_W_RADL     19
#define H265_NAL_IDR_N_LP       20
#define H265_NAL_CRA_NUT        21
#define H265_NAL_RSV_IRAP_VCL22 22
#define H265_NAL_RSV_IRAP_VCL23 23 /* IRAP range end */
#define H265_NAL_VPS_NUT        32
#define H265_NAL_SPS_NUT        33
#define H265_NAL_PPS_NUT        34
#define H265_NAL_AUD_NUT        35
#define H265_NAL_EOS_NUT        36
#define H265_NAL_EOB_NUT        37
#define H265_NAL_FD_NUT         38
#define H265_NAL_PREFIX_SEI_NUT 39
#define H265_NAL_SUFFIX_SEI_NUT 40

/* RFC 7798 §4.4 — RTP payload structure types (nal_unit_type values used in
 * the PayloadHdr to signal the RTP packetization mode) */
#define H265_PKT_AP   48 /* Aggregation Packet    §4.4.2 */
#define H265_PKT_FU   49 /* Fragmentation Unit    §4.4.3 */
#define H265_PKT_PACI 50 /* PACI Packet           §4.4.4 (ignored) */

/* RFC 7798 §4.4.3 — FU header bit layout
 *
 *   +---------------+
 *   |0|1|2|3|4|5|6|7|
 *   +-+-+-+-+-+-+-+-+
 *   |S|E|  FuType   |
 *   +---------------+
 *
 *   S (1 bit)  Start bit      — set on the first FU of a fragmented NAL.
 *   E (1 bit)  End bit        — set on the last FU of a fragmented NAL.
 *   FuType(6)  Original NAL unit type (pre-fragmentation).
 */
#define H265_FU_S_BIT     0x80
#define H265_FU_E_BIT     0x40
#define H265_FU_TYPE_MASK 0x3F

/* FU overhead: 2-byte PayloadHdr (type=49, LayerId/TID copied from the original
 * NAL header) + 1-byte FU header. DON field is absent under sprop-max-don-diff=0. */
#define H265_FU_PAYLOAD_HDR_SIZE 2
#define H265_FU_HEADER_SIZE      3

/* AP overhead: 2-byte PayloadHdr (type=48, LayerId/TID = min of aggregated NALs)
 * + per-NAL 2-byte length prefix. DONL/DOND fields absent under sprop-max-don-diff=0. */
#define H265_AP_PAYLOAD_HDR_SIZE 2
#define H265_AP_NALU_LEN_SIZE    2

/* Extraction helpers for the 2-byte NAL header. Always operate on a
 * two-element uint8_t array. */
#define H265_NAL_TYPE(hdr)     (uint8_t)(((hdr)[0] >> 1) & 0x3F)
#define H265_NAL_LAYER_ID(hdr) (uint8_t)((((hdr)[0] & 0x01) << 5) | (((hdr)[1] >> 3) & 0x1F))
#define H265_NAL_TID(hdr)      (uint8_t)((hdr)[1] & 0x07)
#define H265_IS_IRAP(type)     ((type) >= H265_NAL_BLA_W_LP && (type) <= H265_NAL_RSV_IRAP_VCL23)

/* ================================================================
 * Packetizer API — callback pattern, no internal buffering
 * ================================================================ */

/** Reference to a single NAL unit in the caller's buffer.
 *  Lifetime: only during the h265_packetize_au() call. */
typedef struct h265_nal_ref {
    const uint8_t *data;
    size_t len;
} h265_nal_ref_t;

/**
 * Callback invoked by the H.265 packetizer for each emitted RTP payload.
 *
 * Signature is binary-compatible with h264_packet_cb so the caller can
 * reuse the same video_send_ctx pattern in nano_rtc.c.
 *
 * @param payload   Fully-formed RTP payload bytes ready to be SRTP-protected.
 * @param len       Payload length in bytes.
 * @param marker    Non-zero on the final payload of an access unit (RTP M bit).
 * @param userdata  Opaque pointer passed to the packetizer.
 * @return 0 on success; any non-zero value aborts packetization.
 */
typedef int (*h265_packet_cb)(const uint8_t *payload, size_t len, int marker, void *userdata);

/**
 * Packetize a single HEVC NAL unit.
 *
 * - NAL ≤ mtu     → emitted as a Single NAL Unit Packet (RFC 7798 §4.4.1).
 * - NAL > mtu     → fragmented into FU packets         (RFC 7798 §4.4.3).
 *
 * The caller owns setting the marker bit only for the last NAL of an
 * access unit; this function emits marker=1 on the last RTP payload it
 * generates, which is correct when used for a single-NAL AU. For
 * multi-NAL AUs, use h265_packetize_au() instead.
 *
 * @param nalu     Raw NAL unit bytes (2-byte NAL header + RBSP payload,
 *                 no Annex-B start code).
 * @param nalu_len Length in bytes (must be ≥ H265_NAL_HEADER_SIZE).
 * @param mtu      Maximum RTP payload size (excluding RTP header).
 * @param cb       Callback invoked once per emitted payload.
 * @param userdata Opaque pointer passed to cb.
 * @return NANORTC_OK on success, negative error code otherwise.
 */
int h265_packetize(const uint8_t *nalu, size_t nalu_len, size_t mtu, h265_packet_cb cb,
                   void *userdata);

/**
 * Packetize an access unit (ordered list of NAL units) using a greedy
 * Aggregation Packet / Single NAL / Fragmentation Unit strategy.
 *
 * Packing rules:
 *   1. Each NAL larger than the MTU is emitted as an FU sequence (§4.4.3).
 *   2. Runs of small NALs are packed into a single AP (§4.4.2) as long as
 *      the total (2-byte PayloadHdr + 2-byte length prefix per NAL + NAL
 *      bytes) fits in the MTU. A run must contain at least TWO NALs — a
 *      single NAL is more efficient as a Single NAL Unit packet.
 *   3. Any NAL that falls between the two regimes above is emitted as a
 *      Single NAL Unit Packet (§4.4.1).
 *
 * The marker bit is set to 1 on the final emitted RTP payload only,
 * matching RTP §5.1 "marker on last packet of access unit".
 *
 * @param nals       Array of NAL references (in decode order).
 * @param nal_count  Number of entries in @p nals (must be ≥ 1).
 * @param mtu        Maximum RTP payload size.
 * @param cb         Callback invoked once per emitted payload.
 * @param userdata   Opaque pointer passed to cb.
 * @return NANORTC_OK on success, negative error code otherwise.
 */
int h265_packetize_au(const h265_nal_ref_t *nals, size_t nal_count, size_t mtu, h265_packet_cb cb,
                      void *userdata);

/* ================================================================
 * Depacketizer — stateful FU reassembly, AP first-NAL tolerance
 * ================================================================ */

/** H.265 depacketizer state. One per video track. Mutually exclusive with
 *  nano_h264_depkt_t via the per-track union in nanortc_track_t. */
typedef struct nano_h265_depkt {
    uint8_t buf[NANORTC_VIDEO_NAL_BUF_SIZE]; /* Reassembly scratch */
    size_t len;                              /* Bytes currently in buf */
    uint8_t nal_hdr[H265_NAL_HEADER_SIZE];   /* Reconstructed original NAL header */
    uint8_t in_progress;                     /* 1 while an FU is being assembled */
} nano_h265_depkt_t;

/**
 * Initialize an H.265 depacketizer. Must be called before any
 * h265_depkt_push() invocation.
 */
int h265_depkt_init(nano_h265_depkt_t *d);

/**
 * Push a single RTP payload into the depacketizer.
 *
 * Handles:
 *   - Single NAL Unit (§4.4.1): pass-through into d->buf, returned immediately.
 *   - AP (§4.4.2): first aggregated NAL unit is extracted and returned.
 *                  Subsequent AUs are silently dropped (matches the H.264
 *                  STAP-A precedent at nano_h264.c).
 *   - FU (§4.4.3): fragments accumulated; reassembly completes on E=1.
 *   - PACI (§4.4.4, type 50): logged at debug and silently dropped.
 *   - Unknown types: logged at debug, no output produced.
 *
 * On a successful complete NAL unit, @p nalu_out points into d->buf and
 * @p nalu_len holds the NAL length (including the 2-byte NAL header).
 *
 * @param d         Depacketizer state.
 * @param payload   RTP payload bytes (after RTP header strip, after SRTP unprotect).
 * @param len       Payload length.
 * @param marker    RTP marker bit (RFC 3550 §5.1) — currently informational.
 * @param nalu_out  [out] Pointer to reassembled NAL or NULL if not yet complete.
 * @param nalu_len  [out] Length of reassembled NAL, or 0.
 * @return NANORTC_OK on success, NANORTC_ERR_PARSE on malformed input,
 *         NANORTC_ERR_BUFFER_TOO_SMALL if reassembly exceeds NAL_BUF_SIZE.
 */
int h265_depkt_push(nano_h265_depkt_t *d, const uint8_t *payload, size_t len, int marker,
                    const uint8_t **nalu_out, size_t *nalu_len);

/**
 * Stateless IRAP (keyframe) detection.
 *
 * Per RFC 7798 §1.1.4 and the H.265 specification (§7.4.2.2), IRAP NAL
 * unit types are 16..23 (BLA_W_LP .. RSV_IRAP_VCL23).
 *
 * Inspects:
 *   - Single NAL      — report IRAP iff NAL type is in [16..23].
 *   - Aggregation Packet — report IRAP iff any aggregated NAL type is IRAP.
 *   - Fragmentation Unit — report IRAP iff S=1 and FuType is in [16..23].
 *
 * @param rtp_payload  RTP payload bytes.
 * @param len          Payload length.
 * @return 1 if the payload begins a keyframe, 0 otherwise.
 */
int h265_is_keyframe(const uint8_t *rtp_payload, size_t len);

#endif /* NANORTC_H265_H_ */
