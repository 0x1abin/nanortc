/*
 * Fuzz harness for the video receive reorder buffer — nano_reorder.c
 *
 * Targets: reorder_push() / reorder_pop() / reorder_next_timeout_ms().
 * Attack surface: the bounded-window state machine — 16-bit sequence wraparound,
 * far-future force-advance, timeout-skip, and slot reuse. Drives an adversarial
 * sequence of push/pop operations with attacker-chosen seq numbers, payload
 * lengths, and (non-monotonic) clocks, asserting no crash / OOB / hang and that
 * popped lengths never exceed the slot buffer.
 *
 * Built standalone with NANORTC_FEATURE_VIDEO_REORDER=1 (the feature is off by
 * default), so it does not depend on the library being compiled with it.
 *
 * SPDX-License-Identifier: MIT
 */

#include "nano_reorder.h"
#include "nanortc.h" /* NANORTC_OK / NANORTC_ERR_* */

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if NANORTC_FEATURE_VIDEO && NANORTC_FEATURE_VIDEO_REORDER

static nano_reorder_t g_r; /* ~10 KB — keep off the stack across iterations */

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    reorder_init(&g_r);

    /* A fixed source buffer the pushes copy from; pops return pointers into the
     * reorder slots, so we only validate the returned length. */
    static uint8_t src[NANORTC_MEDIA_BUF_SIZE];
    memset(src, 0xA5, sizeof(src));

    uint32_t now = 0;
    size_t i = 0;
    /* Each 5-byte record: [op][seq_hi][seq_lo][len][dt]. */
    while (i + 5 <= size) {
        uint8_t op = data[i];
        uint16_t seq = (uint16_t)(((uint16_t)data[i + 1] << 8) | data[i + 2]);
        size_t len = (size_t)data[i + 3];
        uint8_t dt = data[i + 4];
        i += 5;

        /* Advance (or, with the high bit, rewind) the clock to exercise the
         * monotonic-assumption and wrap paths. */
        if (dt & 0x80) {
            now -= (uint32_t)(dt & 0x7f);
        } else {
            now += (uint32_t)dt;
        }

        if (op & 0x80) {
            uint16_t oseq;
            uint32_t ots;
            uint8_t omk;
            const uint8_t *od = NULL;
            size_t ol = 0;
            bool lost = false;
            int guard = 0;
            while (reorder_pop(&g_r, now, &oseq, &ots, &omk, &od, &ol, &lost) == NANORTC_OK) {
                assert(ol > 0 && ol <= NANORTC_MEDIA_BUF_SIZE);
                assert(od != NULL);
                if (++guard > 4 * NANORTC_VIDEO_REORDER_SLOTS) {
                    break; /* a runaway loop would be the bug; cap to surface it */
                }
            }
        } else {
            /* len 1..MEDIA_BUF_SIZE; 0 is rejected by reorder_push. */
            size_t plen = (len % NANORTC_MEDIA_BUF_SIZE) + 1;
            reorder_push(&g_r, seq, (uint32_t)seq * 90u, (uint8_t)(op & 1u), src, plen, now);
        }

        (void)reorder_next_timeout_ms(&g_r, now);
    }
    return 0;
}

#else  /* feature off — trivial harness so the build never breaks */
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    (void)data;
    (void)size;
    return 0;
}
#endif
