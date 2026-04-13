/*
 * nanortc — H.265/HEVC RTP packetization/depacketization (RFC 7798)
 * @internal Not part of the public API.
 *
 * Supports:
 *   - Single NAL Unit mode (§4.4.1): NAL ≤ MTU, direct as RTP payload
 *   - FU fragmentation (§4.4.3): NAL > MTU, split into FU packets
 *   - AP depacketization (§4.4.2): aggregation (receive-only; first sub-NAL)
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef NANORTC_H265_H_
#define NANORTC_H265_H_

#include "nanortc_config.h"

#include <stdint.h>
#include <stddef.h>

/* RFC 7798 §1.1.4: NAL unit header layout (2 bytes)
 *   byte0: F(1) | Type(6) | LayerId_hi(1)
 *   byte1: LayerId_lo(5) | TID(3)
 * Derivations:
 *   nal_type = (byte0 >> 1) & 0x3F
 *   LayerId  = ((byte0 & 0x01) << 5) | ((byte1 >> 3) & 0x1F)
 *   TID      =  byte1 & 0x07
 */
#define H265_NAL_HEADER_SIZE 2
#define H265_NAL_TYPE_MASK   0x3F /* applied to (byte0 >> 1) */
#define H265_NAL_F_BIT       0x80 /* Forbidden zero bit (byte0 bit 7) */

/* RFC 7798 §1.1.4 Table 1 — selected NAL unit types used here.
 * IRAP set (random access pictures) = BLA/IDR/CRA = 16..21. */
#define H265_NAL_TRAIL_N    0
#define H265_NAL_TRAIL_R    1
#define H265_NAL_BLA_W_LP   16
#define H265_NAL_BLA_W_RADL 17
#define H265_NAL_BLA_N_LP   18
#define H265_NAL_IDR_W_RADL 19
#define H265_NAL_IDR_N_LP   20
#define H265_NAL_CRA_NUT    21
#define H265_NAL_VPS        32
#define H265_NAL_SPS        33
#define H265_NAL_PPS        34
#define H265_NAL_AUD        35
#define H265_NAL_PREFIX_SEI 39
#define H265_NAL_SUFFIX_SEI 40

/* RFC 7798 §4.4.2 Aggregation Packet */
#define H265_NAL_AP 48

/* RFC 7798 §4.4.3 Fragmentation Unit */
#define H265_NAL_FU 49

/* RFC 7798 §4.4.4 PACI — intentionally not implemented (rare). */
#define H265_NAL_PACI 50

/* AP overhead: 2-byte PayloadHdr + per-NALU {u16 size, NAL bytes} (§4.4.2) */
#define H265_AP_HEADER_SIZE   H265_NAL_HEADER_SIZE
#define H265_AP_NALU_LEN_SIZE 2

/* FU header (1 byte following the 2-byte PayloadHdr, §4.4.3):
 *   [S(1) | E(1) | FuType(6)]
 */
#define H265_FU_HEADER_SIZE 1
#define H265_FU_S_BIT       0x80
#define H265_FU_E_BIT       0x40
#define H265_FU_TYPE_MASK   0x3F

/* Total FU overhead: 2-byte PayloadHdr (type=49) + 1-byte FU header */
#define H265_FU_OVERHEAD (H265_NAL_HEADER_SIZE + H265_FU_HEADER_SIZE) /* 3 */

/**
 * Callback invoked by h265_packetize() for each RTP payload fragment.
 *
 * @param payload  RTP payload bytes (FU header + data, or raw NAL).
 * @param len      Length of payload in bytes.
 * @param marker   Non-zero if this is the last fragment of the access unit.
 * @param userdata Opaque pointer passed to h265_packetize().
 * @return 0 on success, negative on error (aborts packetization).
 */
typedef int (*h265_packet_cb)(const uint8_t *payload, size_t len, int marker, void *userdata);

/**
 * Packetize a single H.265 NAL unit into RTP payloads.
 *
 * - NAL ≤ mtu: emitted as Single NAL Unit (RFC 7798 §4.4.1).
 * - NAL > mtu: split into FU fragments (RFC 7798 §4.4.3).
 *
 * The input NAL must carry its 2-byte NAL header at nalu[0..1].
 * Inputs whose type field is FU (49), AP (48), or PACI (50) are rejected:
 * RFC 7798 §4.4.2 forbids re-aggregation.
 *
 * @param nalu     Raw NAL unit (NAL header + payload, no start code).
 * @param nalu_len Length of the NAL unit in bytes (≥ 2).
 * @param mtu      Maximum RTP payload size (excluding RTP header).
 * @param cb       Callback for each fragment.
 * @param userdata Opaque pointer passed to callback.
 * @return 0 on success, negative on error.
 */
int h265_packetize(const uint8_t *nalu, size_t nalu_len, size_t mtu, h265_packet_cb cb,
                   void *userdata);

/**
 * FU depacketizer: reassembles fragmented NAL units from RTP payloads.
 *
 * Call h265_depkt_push() for each received RTP payload in sequence order.
 * When a complete NAL unit is available, nalu_out/nalu_len are set.
 */
typedef struct nano_h265_depkt {
    uint8_t buf[NANORTC_VIDEO_NAL_BUF_SIZE];
    size_t len;
    uint8_t nal_hdr0;    /* Reconstructed NAL header byte 0. */
    uint8_t nal_hdr1;    /* Reconstructed NAL header byte 1. */
    uint8_t in_progress; /* FU reassembly in progress. */
} nano_h265_depkt_t;

/**
 * Initialize the H.265 depacketizer.
 *
 * @param d  Depacketizer state.
 * @return 0 on success.
 */
int h265_depkt_init(nano_h265_depkt_t *d);

/**
 * Push an RTP payload into the depacketizer.
 *
 * Handles Single NAL, AP, and FU packet types (RFC 7798 §4.4).
 *
 * @param d         Depacketizer state.
 * @param payload   RTP payload (after RTP header).
 * @param len       Length of payload.
 * @param marker    RTP marker bit (1 = last packet of frame).
 * @param nalu_out  On complete NAL: set to point into d->buf. NULL otherwise.
 * @param nalu_len  On complete NAL: set to NAL length.
 * @return 0 on success, negative on error.
 */
int h265_depkt_push(nano_h265_depkt_t *d, const uint8_t *payload, size_t len, int marker,
                    const uint8_t **nalu_out, size_t *nalu_len);

/**
 * Check if an RTP payload contains an H.265 keyframe (IRAP NAL unit).
 *
 * Stateless: inspects the NAL type in the 2-byte NAL header (or FU header
 * for FU start fragments).
 *
 * IRAP set (RFC 7798 §1.1.4): BLA_W_LP(16), BLA_W_RADL(17), BLA_N_LP(18),
 * IDR_W_RADL(19), IDR_N_LP(20), CRA_NUT(21).
 *
 * @param rtp_payload  RTP payload bytes.
 * @param len          Length of payload.
 * @return Non-zero if IRAP keyframe detected, 0 otherwise.
 */
int h265_is_keyframe(const uint8_t *rtp_payload, size_t len);

#endif /* NANORTC_H265_H_ */
