/*
 * Fuzz harness for the ULPFEC codec — nano_fec.c
 *
 * Targets fec_encode() and (the network-facing attack surface) fec_recover():
 * adversarial FEC packets (protlen/mask/length-recovery) parsed against
 * synthesized media members, plus encode of fuzzer-derived media packets.
 * Asserts no crash / OOB on any input.
 *
 * Built standalone with NANORTC_FEATURE_VIDEO_FEC=1 (default off).
 *
 * SPDX-License-Identifier: MIT
 */

#include "nano_fec.h"
#include "nanortc.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if NANORTC_FEATURE_VIDEO && NANORTC_FEATURE_VIDEO_FEC

#define MAXP 8
#define PKT_CAP 512

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    static uint8_t store[MAXP][PKT_CAP];
    const uint8_t *pkts[MAXP];
    uint16_t lens[MAXP];
    uint8_t out[2048];
    size_t ol = 0;
    uint16_t oseq = 0;

    /* 1) Parse-heavy path: treat the input itself as a FEC packet and recover
     * against a couple of synthesized members (lengths/seqs from the input). */
    if (size >= 2) {
        uint16_t base_seq = (uint16_t)(((uint16_t)data[0] << 8) | data[1]);
        for (uint8_t i = 0; i < 2; i++) {
            uint16_t plen = (uint16_t)((data[(i + 1) % size] % 64u) + 0u);
            lens[i] = (uint16_t)(12 + plen);
            memset(store[i], (int)data[i % size], lens[i]);
            store[i][0] = 0x80;
            nanortc_write_u16be(store[i] + 2, (uint16_t)(base_seq + i));
            pkts[i] = store[i];
        }
        fec_recover(data, size, 0x1234u, pkts, lens, 2, out, sizeof(out), &ol, &oseq);
    }

    /* 2) Encode fuzzer-derived media, then recover a subset. */
    uint8_t n = (uint8_t)((size ? data[0] : 0) % MAXP) + 1u;
    size_t off = 1;
    uint16_t base = (uint16_t)(size > 1 ? data[1] : 0);
    for (uint8_t i = 0; i < n; i++) {
        uint16_t plen = (uint16_t)((off < size ? data[off] : 0) % (PKT_CAP - 12));
        off++;
        lens[i] = (uint16_t)(12 + plen);
        memset(store[i], (int)(0xA0 + i), lens[i]);
        store[i][0] = 0x80;
        store[i][1] = 96;
        nanortc_write_u16be(store[i] + 2, (uint16_t)(base + i)); /* contiguous, span < 16 */
        pkts[i] = store[i];
    }
    uint8_t fec[PKT_CAP + 32];
    size_t fl = 0;
    if (fec_encode(pkts, lens, n, fec, sizeof(fec), &fl) == NANORTC_OK && n >= 1) {
        /* drop member (data-chosen) and recover. */
        uint8_t drop = (uint8_t)((size > 2 ? data[2] : 0) % n);
        const uint8_t *recv[MAXP];
        uint16_t rl[MAXP];
        uint8_t nr = 0;
        for (uint8_t i = 0; i < n; i++) {
            if (i != drop) {
                recv[nr] = pkts[i];
                rl[nr] = lens[i];
                nr++;
            }
        }
        fec_recover(fec, fl, 0x1234u, recv, rl, nr, out, sizeof(out), &ol, &oseq);
    }
    return 0;
}

#else
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    (void)data;
    (void)size;
    return 0;
}
#endif
