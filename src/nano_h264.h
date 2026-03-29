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
 * Packetize a single H.264 NAL unit into RTP payloads.
 *
 * - NAL ≤ mtu: emitted as Single NAL Unit (RFC 6184 §5.6).
 * - NAL > mtu: split into FU-A fragments (RFC 6184 §5.8).
 *
 * The callback is invoked once per RTP payload. The caller is responsible
 * for wrapping each payload into an RTP packet.
 *
 * @param nalu     Raw NAL unit (including NAL header byte, no start code).
 * @param nalu_len Length of the NAL unit in bytes.
 * @param mtu      Maximum RTP payload size (excluding RTP header).
 * @param cb       Callback for each fragment.
 * @param userdata Opaque pointer passed to callback.
 * @return 0 on success, negative on error.
 */
int h264_packetize(const uint8_t *nalu, size_t nalu_len, size_t mtu, h264_packet_cb cb,
                   void *userdata);

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

#endif /* NANORTC_H264_H_ */
