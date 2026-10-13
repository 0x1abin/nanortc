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
#include "nanortc.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>

#define FUZZ_H265_NALS       2
#define FUZZ_H265_PACKET_CAP 512

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

    /* The send iterator handles both aggregation and fragmentation. Vary the
     * output capacity to check that retryable failures never consume a NAL. */
    if (size >= 4) {
        size_t split = 2u + data[0] % (size - 3u);
        h265_nal_ref_t nals[FUZZ_H265_NALS] = {{data, split}, {data + split, size - split}};
        h265_packet_iter_t it;
        size_t mtu = 4u + data[1];
        if (h265_packet_iter_init(&it, nals, FUZZ_H265_NALS, mtu) == NANORTC_OK) {
            uint8_t scratch[FUZZ_H265_PACKET_CAP];
            for (unsigned step = 0; step < 64 && h265_packet_iter_has_next(&it); step++) {
                h265_packet_iter_t before = it;
                const uint8_t *payload;
                size_t payload_len;
                int marker;
                int rc =
                    h265_packet_iter_next(&it, scratch, data[2], &payload, &payload_len, &marker);
                if (rc == NANORTC_ERR_BUFFER_TOO_SMALL) {
                    assert(memcmp(&it, &before, sizeof(it)) == 0);
                    rc = h265_packet_iter_next(&it, scratch, sizeof(scratch), &payload,
                                               &payload_len, &marker);
                }
                assert(rc == NANORTC_OK);
                assert(payload != NULL && payload_len <= mtu);
                assert(marker == !h265_packet_iter_has_next(&it));
            }
        }
    }

    return 0;
}
