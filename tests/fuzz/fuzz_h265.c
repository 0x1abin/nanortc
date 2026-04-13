/*
 * Fuzz harness for H.265/HEVC depacketizer — nano_h265.c
 *
 * Targets: h265_depkt_push(), h265_is_keyframe()
 * Attack surface: Stateful reassembly of fragmented NAL units (FU type 49),
 * aggregation packet parsing (AP type 48), and NAL header parsing (2 bytes).
 *
 * SPDX-License-Identifier: MIT
 */

#include "nano_h265.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    /* Stateless: keyframe detection */
    h265_is_keyframe(data, size);

    /* Stateful: depacketization (Single NAL / AP / FU).
     * Feed the fuzz input as a single RTP payload. */
    nano_h265_depkt_t depkt;
    memset(&depkt, 0, sizeof(depkt));
    h265_depkt_init(&depkt);

    const uint8_t *nalu_out = NULL;
    size_t nalu_len = 0;
    h265_depkt_push(&depkt, data, size, 0, &nalu_out, &nalu_len);

    /* Feed again with marker=1 to trigger reassembly completion */
    h265_depkt_push(&depkt, data, size, 1, &nalu_out, &nalu_len);

    return 0;
}
