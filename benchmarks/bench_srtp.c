/*
 * nanortc — SRTP encrypt/decrypt throughput benchmark
 *
 * Measures SRTP protect/unprotect (AES-128-CM + HMAC-SHA1-80) throughput.
 * Uses synthetic keying material — no DTLS handshake needed.
 *
 * SPDX-License-Identifier: MIT
 */

#include "bench_common.h"
#include "nanortc.h"
#include "nanortc_crypto.h"
#include "nano_test_config.h"
#include "nano_rtp.h"
#include "nano_srtp.h"
#include <string.h>

/* ----------------------------------------------------------------
 * SRTP throughput benchmark
 * ---------------------------------------------------------------- */

static void bench_srtp_protect(int n_packets, size_t payload_size)
{
    /* Initialize SRTP with synthetic keying material */
    nano_srtp_t srtp_send, srtp_recv;
    const nanortc_crypto_provider_t *crypto = nano_test_crypto();

    nano_srtp_init(&srtp_send, crypto, 1 /* client */);
    nano_srtp_init(&srtp_recv, crypto, 0 /* server */);

    /* Generate deterministic keying material (60 bytes for AES-128-CM) */
    uint8_t keying_material[60];
    for (int i = 0; i < 60; i++) keying_material[i] = (uint8_t)(i * 7 + 13);

    nano_srtp_derive_keys(&srtp_send, keying_material, sizeof(keying_material));
    nano_srtp_derive_keys(&srtp_recv, keying_material, sizeof(keying_material));

    /* Prepare RTP packet template */
    nano_rtp_t rtp;
    rtp_init(&rtp, 0xDEADBEEF, 111);

    uint8_t *payload = (uint8_t *)malloc(payload_size);
    memset(payload, 0xEE, payload_size);

    /* Buffer: RTP header + payload + SRTP auth tag room */
    size_t buf_size = RTP_HEADER_SIZE + payload_size + NANORTC_SRTP_AUTH_TAG_SIZE + 16;
    uint8_t *buf = (uint8_t *)malloc(buf_size);
    uint8_t *buf_copy = (uint8_t *)malloc(buf_size);

    /* Pre-encode base RTP packet */
    size_t rtp_len = 0;
    rtp_pack(&rtp, 1000, payload, payload_size, buf, buf_size, &rtp_len);

    /* --- Protect benchmark --- */
    {
        char label[64];
        snprintf(label, sizeof(label), "srtp_protect_%zuB", payload_size);

        BENCH_THROUGHPUT_BEGIN(tp);

        for (int i = 0; i < n_packets; i++) {
            /* Reset packet (protect modifies in-place) */
            memcpy(buf_copy, buf, rtp_len);

            /* Increment seq to avoid replay */
            uint16_t new_seq = (uint16_t)(i & 0xFFFF);
            buf_copy[2] = (uint8_t)(new_seq >> 8);
            buf_copy[3] = (uint8_t)(new_seq & 0xFF);

            size_t out_len = 0;
            int rc = nano_srtp_protect(&srtp_send, buf_copy, rtp_len, &out_len);
            if (rc == 0) {
                BENCH_THROUGHPUT_ADD(tp, out_len, 1);
            }
        }

        BENCH_THROUGHPUT_END(tp, label);
    }

    /* --- Unprotect benchmark --- */
    {
        char label[64];
        snprintf(label, sizeof(label), "srtp_unprotect_%zuB", payload_size);

        /* Re-init to reset ROC state */
        nano_srtp_init(&srtp_send, crypto, 1);
        nano_srtp_init(&srtp_recv, crypto, 0);
        nano_srtp_derive_keys(&srtp_send, keying_material, sizeof(keying_material));
        nano_srtp_derive_keys(&srtp_recv, keying_material, sizeof(keying_material));
        rtp_init(&rtp, 0xDEADBEEF, 111);

        BENCH_THROUGHPUT_BEGIN(tp);

        for (int i = 0; i < n_packets; i++) {
            /* Encode fresh RTP packet */
            rtp_len = 0;
            rtp_pack(&rtp, (uint32_t)(i * 960), payload, payload_size,
                     buf, buf_size, &rtp_len);

            /* Protect it */
            size_t protected_len = 0;
            int rc = nano_srtp_protect(&srtp_send, buf, rtp_len, &protected_len);
            if (rc != 0) continue;

            /* Unprotect it */
            size_t unprotected_len = 0;
            rc = nano_srtp_unprotect(&srtp_recv, buf, protected_len, &unprotected_len);
            if (rc == 0) {
                BENCH_THROUGHPUT_ADD(tp, protected_len, 1);
            }
        }

        BENCH_THROUGHPUT_END(tp, label);
    }

    free(buf_copy);
    free(buf);
    free(payload);
}

/* ----------------------------------------------------------------
 * Main
 * ---------------------------------------------------------------- */

BENCH_MAIN_BEGIN("bench_srtp")

    struct { size_t size; int pkts; } tests[] = {
        {  160, 50000 },   /* G.711 20ms */
        {  320, 50000 },   /* Opus 20ms */
        { 1200, 20000 },   /* Video MTU */
    };

    for (int i = 0; i < 3; i++) {
        bench_srtp_protect(tests[i].pkts, tests[i].size);
    }

BENCH_MAIN_END
