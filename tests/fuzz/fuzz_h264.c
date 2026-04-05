/*
 * Fuzz harness for H.264 depacketizer — nano_h264.c
 *
 * Targets: h264_depkt_push(), h264_is_keyframe(), h264_annex_b_find_nal()
 * Attack surface: Stateful reassembly of fragmented NAL units (FU-A).
 *
 * SPDX-License-Identifier: MIT
 */

#include "nano_h264.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    /* Stateless: keyframe detection */
    h264_is_keyframe(data, size);

    /* Stateless: Annex B NAL unit search */
    size_t offset = 0;
    size_t nal_len = 0;
    h264_annex_b_find_nal(data, size, &offset, &nal_len);

    /* Stateful: FU-A depacketization.
     * Feed the fuzz input as a single RTP payload. */
    nano_h264_depkt_t depkt;
    memset(&depkt, 0, sizeof(depkt));
    h264_depkt_init(&depkt);

    const uint8_t *nalu_out = NULL;
    size_t nalu_len = 0;
    h264_depkt_push(&depkt, data, size, 0, &nalu_out, &nalu_len);

    /* Feed again with marker=1 to trigger reassembly completion */
    h264_depkt_push(&depkt, data, size, 1, &nalu_out, &nalu_len);

    return 0;
}
