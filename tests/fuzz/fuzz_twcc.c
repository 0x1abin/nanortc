/*
 * Fuzz harness for Transport-Wide Congestion Control feedback parsing.
 *
 * Targets: twcc_parse_feedback(), including run-length/status-vector chunks,
 * signed receive deltas, RTCP length validation, sequence wrap, and the
 * optional per-packet callback path.
 *
 * SPDX-License-Identifier: MIT
 */

#include "nano_twcc.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define FUZZ_TWCC_FRAME_CAP 512

typedef struct {
    uint32_t callbacks;
    uint32_t checksum;
} fuzz_twcc_state_t;

static void fuzz_twcc_packet(uint16_t seq, nano_twcc_status_t status, int32_t delta_us, void *user)
{
    fuzz_twcc_state_t *state = (fuzz_twcc_state_t *)user;
    state->callbacks++;
    state->checksum ^= (uint32_t)seq;
    state->checksum ^= (uint32_t)status << 16;
    state->checksum ^= (uint32_t)delta_us;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    nano_twcc_summary_t summary;
    fuzz_twcc_state_t state = {0, 0};

    /* Exercise both the summary-only fast path and the callback path. */
    twcc_parse_feedback(data, size, &summary, NULL, NULL);
    twcc_parse_feedback(data, size, &summary, fuzz_twcc_packet, &state);

    /* Also place the bytes behind a valid fixed TWCC header. This reaches the
     * chunk/delta parser immediately from an empty corpus while the direct call
     * above continues to fuzz version, type, length, and truncation checks. */
    if (size <= FUZZ_TWCC_FRAME_CAP - 20u) {
        uint8_t framed[FUZZ_TWCC_FRAME_CAP];
        size_t framed_len = 20u + size;
        framed_len = (framed_len + 3u) & ~(size_t)3u;
        memset(framed, 0, framed_len);
        framed[0] = 0x8Fu; /* V=2, FMT=15 */
        framed[1] = 205u;  /* RTPFB */
        framed[2] = (uint8_t)(((framed_len / 4u) - 1u) >> 8);
        framed[3] = (uint8_t)((framed_len / 4u) - 1u);
        if (size > 0) {
            uint16_t count = (uint16_t)(data[0] % 128u);
            framed[14] = (uint8_t)(count >> 8);
            framed[15] = (uint8_t)count;
            memcpy(framed + 20, data, size);
        }
        twcc_parse_feedback(framed, framed_len, &summary, fuzz_twcc_packet, &state);
    }

    return (int)(state.checksum & 0u);
}
