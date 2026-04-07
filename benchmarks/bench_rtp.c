/*
 * nanortc — RTP pack/unpack micro-benchmark
 *
 * Measures raw RTP header encoding and parsing throughput (no SRTP).
 *
 * SPDX-License-Identifier: MIT
 */

#include "bench_common.h"
#include "nanortc.h"
#include "nano_rtp.h"
#include <string.h>

/* ----------------------------------------------------------------
 * RTP pack benchmark
 * ---------------------------------------------------------------- */

static void bench_rtp_pack(int n_packets, size_t payload_size)
{
    nano_rtp_t rtp;
    rtp_init(&rtp, 0x12345678, 111);

    uint8_t *payload = (uint8_t *)malloc(payload_size);
    memset(payload, 0xBB, payload_size);

    /* Output buffer: RTP header + payload */
    size_t buf_size = RTP_HEADER_SIZE + payload_size;
    uint8_t *buf = (uint8_t *)malloc(buf_size);

    char label[64];
    snprintf(label, sizeof(label), "rtp_pack_%zuB", payload_size);

    BENCH_THROUGHPUT_BEGIN(tp);

    for (int i = 0; i < n_packets; i++) {
        size_t out_len = 0;
        int rc = rtp_pack(&rtp, (uint32_t)(i * 960), payload, payload_size,
                          buf, buf_size, &out_len);
        if (rc == 0) {
            BENCH_THROUGHPUT_ADD(tp, out_len, 1);
        }
    }

    BENCH_THROUGHPUT_END(tp, label);

    free(buf);
    free(payload);
}

/* ----------------------------------------------------------------
 * RTP unpack benchmark
 * ---------------------------------------------------------------- */

static void bench_rtp_unpack(int n_packets, size_t payload_size)
{
    nano_rtp_t rtp;
    rtp_init(&rtp, 0x12345678, 111);

    uint8_t *payload = (uint8_t *)malloc(payload_size);
    memset(payload, 0xCC, payload_size);

    size_t pkt_size = RTP_HEADER_SIZE + payload_size;
    uint8_t *pkt = (uint8_t *)malloc(pkt_size);

    /* Pre-encode one packet for repeated parsing */
    size_t out_len = 0;
    rtp_pack(&rtp, 1000, payload, payload_size, pkt, pkt_size, &out_len);

    char label[64];
    snprintf(label, sizeof(label), "rtp_unpack_%zuB", payload_size);

    BENCH_THROUGHPUT_BEGIN(tp);

    for (int i = 0; i < n_packets; i++) {
        uint8_t pt;
        uint16_t seq;
        uint32_t ts, ssrc;
        const uint8_t *pl;
        size_t pl_len;
        int rc = rtp_unpack(pkt, out_len, &pt, &seq, &ts, &ssrc, &pl, &pl_len);
        if (rc == 0) {
            BENCH_THROUGHPUT_ADD(tp, out_len, 1);
        }
    }

    BENCH_THROUGHPUT_END(tp, label);

    free(pkt);
    free(payload);
}

/* ----------------------------------------------------------------
 * Main
 * ---------------------------------------------------------------- */

BENCH_MAIN_BEGIN("bench_rtp")

    /* Pack/unpack with typical audio/video payload sizes */
    struct { size_t size; int pkts; } tests[] = {
        {  160, 100000 },  /* G.711 20ms */
        {  320, 100000 },  /* Opus 20ms @ 128kbps */
        { 1200,  50000 },  /* Video MTU */
    };

    for (int i = 0; i < 3; i++) {
        bench_rtp_pack(tests[i].pkts, tests[i].size);
        bench_rtp_unpack(tests[i].pkts, tests[i].size);
    }

BENCH_MAIN_END
