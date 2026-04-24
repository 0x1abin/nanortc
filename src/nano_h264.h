/*
 * nanortc — H.264 RTP packetization/depacketization (RFC 6184)
 * @internal Not part of the public API.
 *
 * Supports:
 *   - Single NAL Unit mode (§5.6): NAL ≤ MTU, direct as RTP payload
 *   - FU-A fragmentation (§5.8): NAL > MTU, split into FU-A packets
 *   - STAP-A depacketization (§5.7): aggregation (receive-only)
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef NANORTC_H264_H_
#define NANORTC_H264_H_

#include "nanortc_config.h"
#include "nano_annex_b.h"

#include <stdint.h>
#include <stddef.h>

/* RFC 6184 §1.3: NAL unit type codes */
#define H264_NAL_TYPE_MASK    0x1F
#define H264_NAL_REF_IDC_MASK 0x60
#define H264_NAL_SLICE        1  /* Coded slice of a non-IDR picture */
#define H264_NAL_IDR          5  /* Instantaneous Decoding Refresh (keyframe) */
#define H264_NAL_SEI          6  /* Supplemental Enhancement Information */
#define H264_NAL_SPS          7  /* Sequence Parameter Set */
#define H264_NAL_PPS          8  /* Picture Parameter Set */
#define H264_NAL_AUD          9  /* Access Unit Delimiter */
#define H264_NAL_STAPA        24 /* Single-Time Aggregation Packet type A */
#define H264_NAL_FUA          28 /* Fragmentation Unit type A */

/* FU-A header bit masks (RFC 6184 §5.8) */
#define H264_FUA_S_BIT 0x80 /* Start bit */
#define H264_FUA_E_BIT 0x40 /* End bit */

/* FU-A overhead: 1 byte FU indicator + 1 byte FU header */
#define H264_FUA_HEADER_SIZE 2

/* STAP-A overhead: 1 byte header + 2 bytes per-NALU length prefix */
#define H264_STAPA_HEADER_SIZE   1
#define H264_STAPA_NALU_LEN_SIZE 2

/**
 * Callback invoked by h264_packetize() for each RTP payload fragment.
 *
 * @param payload  RTP payload bytes (FU-A header + data, or raw NAL).
 * @param len      Length of payload in bytes.
 * @param marker   Non-zero if this is the last fragment (RTP M bit).
 * @param userdata Opaque pointer passed to h264_packetize().
 * @return 0 on success, negative on error (aborts packetization).
 */
typedef int (*h264_packet_cb)(const uint8_t *payload, size_t len, int marker, void *userdata);

/**
 * Iterator state for driving H.264 packetization one fragment at a time.
 *
 * The iterator lets the caller pick a different scratch buffer per fragment —
 * a prerequisite for writing each FU-A payload directly into the final RTP
 * packet buffer and avoiding the extra memcpy through a shared scratch.
 * All fields are private; use the h264_fragment_iter_* functions.
 */
typedef struct h264_fragment_iter {
    const uint8_t *nalu;
    size_t nalu_len;
    size_t mtu;

    /* FU-A state (unused when single_nal != 0). */
    uint8_t fu_indicator;
    uint8_t nal_type;
    const uint8_t *data;
    size_t remaining;
    size_t max_frag;

    uint8_t single_nal; /* 1 when nalu_len <= mtu — emit pass-through. */
    uint8_t done;       /* 1 once the last fragment has been produced. */
} h264_fragment_iter_t;

/**
 * Initialize an H.264 fragment iterator.
 *
 * @return NANORTC_OK on success, NANORTC_ERR_INVALID_PARAM on bad input
 *         (null nalu, zero-length nalu, or mtu below FU-A minimum).
 */
int h264_fragment_iter_init(h264_fragment_iter_t *it, const uint8_t *nalu, size_t nalu_len,
                            size_t mtu);

/** Non-zero while more fragments remain. */
int h264_fragment_iter_has_next(const h264_fragment_iter_t *it);

/**
 * Produce the next RTP payload fragment.
 *
 * For the FU-A path the payload bytes are written into @p scratch and
 * @p payload_out is set to @p scratch. For the single-NAL fast path no bytes
 * are written to @p scratch and @p payload_out is set to the original nalu
 * pointer. In both cases the caller receives a contiguous (payload, len)
 * pair that can be fed straight to rtp_pack().
 *
 * @param scratch          Per-fragment output buffer (>= mtu bytes).
 *                         Ignored in single-NAL mode.
 * @param scratch_len      Length of the scratch buffer.
 * @param payload_out      Out: pointer to the fragment payload.
 * @param payload_len_out  Out: fragment payload length.
 * @param is_last_out      Out: non-zero if this is the last fragment (M bit).
 * @return NANORTC_OK, NANORTC_ERR_INVALID_PARAM (bad args / iterator exhausted),
 *         or NANORTC_ERR_BUFFER_TOO_SMALL if scratch_len < needed.
 */
int h264_fragment_iter_next(h264_fragment_iter_t *it, uint8_t *scratch, size_t scratch_len,
                            const uint8_t **payload_out, size_t *payload_len_out, int *is_last_out);

/**
 * Packetize a single H.264 NAL unit into RTP payloads.
 *
 * - NAL ≤ mtu: emitted as Single NAL Unit (RFC 6184 §5.6).
 * - NAL > mtu: split into FU-A fragments (RFC 6184 §5.8).
 *
 * The callback is invoked once per RTP payload. The caller is responsible
 * for wrapping each payload into an RTP packet.
 *
 * Thin wrapper over the fragment iterator; callers that want zero-copy
 * packetization should drive h264_fragment_iter_* directly and pass
 * `pkt_buf + rtp_header_len` as the per-fragment scratch.
 *
 * @param nalu        Raw NAL unit (including NAL header byte, no start code).
 * @param nalu_len    Length of the NAL unit in bytes.
 * @param mtu         Maximum RTP payload size (excluding RTP header).
 * @param scratch     Shared output buffer (>= mtu bytes). Only the FU-A path
 *                    writes to it; the single-NAL path leaves it untouched.
 * @param scratch_len Length of the scratch buffer.
 * @param cb          Callback for each fragment.
 * @param userdata    Opaque pointer passed to callback.
 * @return 0 on success, negative on error.
 */
int h264_packetize(const uint8_t *nalu, size_t nalu_len, size_t mtu, uint8_t *scratch,
                   size_t scratch_len, h264_packet_cb cb, void *userdata);

/**
 * FU-A depacketizer: reassembles fragmented NAL units from RTP payloads.
 *
 * Call h264_depkt_push() for each received RTP payload in sequence order.
 * When a complete NAL unit is available, nalu_out/nalu_len are set.
 */
typedef struct nano_h264_depkt {
    uint8_t buf[NANORTC_VIDEO_NAL_BUF_SIZE];
    size_t len;
    uint8_t nal_header;  /* Reconstructed NAL header byte (NRI | type) */
    uint8_t in_progress; /* FU-A reassembly in progress */
} nano_h264_depkt_t;

/**
 * Initialize the H.264 depacketizer.
 *
 * @param d  Depacketizer state.
 * @return 0 on success.
 */
int h264_depkt_init(nano_h264_depkt_t *d);

/**
 * Push an RTP payload into the depacketizer.
 *
 * Handles Single NAL, FU-A, and STAP-A packet types.
 *
 * @param d         Depacketizer state.
 * @param payload   RTP payload (after RTP header).
 * @param len       Length of payload.
 * @param marker    RTP marker bit (1 = last packet of frame).
 * @param nalu_out  On complete NAL: set to point into d->buf. NULL otherwise.
 * @param nalu_len  On complete NAL: set to NAL length.
 * @return 0 on success, negative on error.
 */
int h264_depkt_push(nano_h264_depkt_t *d, const uint8_t *payload, size_t len, int marker,
                    const uint8_t **nalu_out, size_t *nalu_len);

/**
 * Check if an RTP payload contains an H.264 keyframe (IDR NAL unit).
 *
 * Stateless: inspects the NAL type in the first byte(s).
 * Handles Single NAL (type 1-23), STAP-A (type 24), and
 * FU-A start fragments (type 28 with S=1).
 *
 * @param rtp_payload  RTP payload bytes.
 * @param len          Length of payload.
 * @return Non-zero if keyframe detected, 0 otherwise.
 */
int h264_is_keyframe(const uint8_t *rtp_payload, size_t len);

/**
 * Find next NAL unit in an Annex-B bitstream.
 *
 * The Annex-B scanner is codec-agnostic and now lives in nano_annex_b.{c,h}.
 * This alias is preserved so existing H.264 call sites and tests continue to
 * compile unchanged.
 */
#define h264_annex_b_find_nal nano_annex_b_find_nal

#endif /* NANORTC_H264_H_ */
