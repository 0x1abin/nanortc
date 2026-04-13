/*
 * Fuzz harness for H.265/HEVC depacketizer — nano_h265.c
 *
 * Targets: h265_depkt_push(), h265_is_keyframe(), nano_annex_b_find_nal()
 * Attack surface: Stateful reassembly of Fragmentation Units and parsing of
 *                 Aggregation Packets (RFC 7798 §4.4.2, §4.4.3).
 *
 * SPDX-License-Identifier: MIT
 */

#include "nano_h265.h"
#include "nano_annex_b.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    /* Stateless: keyframe detection. */
    h265_is_keyframe(data, size);

    /* Stateless: Annex-B NAL unit search (codec-agnostic helper shared
     * with H.264). Covered by fuzz_h264 too, but re-exercising it here
     * widens the corpus crossover for the shared scanner. */
    size_t offset = 0;
    size_t nal_len = 0;
    nano_annex_b_find_nal(data, size, &offset, &nal_len);

    /* Stateful: H.265 depacketization. Feed the fuzz input as a single
     * RTP payload first with marker=0, then with marker=1 to also probe
     * the completion path in case the previous call left the FU state
     * in_progress. */
    nano_h265_depkt_t depkt;
    memset(&depkt, 0, sizeof(depkt));
    h265_depkt_init(&depkt);

    const uint8_t *nalu_out = NULL;
    size_t nalu_out_len = 0;
    h265_depkt_push(&depkt, data, size, 0, &nalu_out, &nalu_out_len);
    h265_depkt_push(&depkt, data, size, 1, &nalu_out, &nalu_out_len);

    return 0;
}
