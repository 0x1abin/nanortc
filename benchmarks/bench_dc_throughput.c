/*
 * nanortc — DataChannel throughput benchmark
 *
 * Measures msg/s and MB/s at various payload sizes via Sans I/O loopback.
 * Both endpoints fully connected (ICE + DTLS + SCTP) using SDP offer/answer.
 *
 * SPDX-License-Identifier: MIT
 */

#include "bench_common.h"
#include "nanortc.h"
#include "nanortc_crypto.h"
#include "nano_test_config.h"
#include "nano_ice.h"
#include <string.h>

/* ----------------------------------------------------------------
 * E2E helpers
 * ---------------------------------------------------------------- */

static nanortc_config_t bench_default_config(void)
{
    nanortc_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.crypto = nano_test_crypto();
    cfg.role = NANORTC_ROLE_CONTROLLED;
#if NANORTC_FEATURE_AUDIO
    cfg.jitter_depth_ms = 100;
#endif
    return cfg;
}

static int bench_relay(nanortc_t *from, nanortc_t *to, uint32_t now_ms)
{
    int relayed = 0;
    nanortc_output_t out;
    while (nanortc_poll_output(from, &out) == NANORTC_OK) {
        if (out.type == NANORTC_OUTPUT_TRANSMIT) {
            nanortc_addr_t src;
            memset(&src, 0, sizeof(src));
            src.family = 4;
            src.addr[0] = 192; src.addr[1] = 168; src.addr[2] = 1; src.addr[3] = 1;
            src.port = 9999;
            nanortc_handle_input(to, now_ms, out.transmit.data, out.transmit.len, &src);
            relayed++;
        }
    }
    return relayed;
}

static int bench_pump(nanortc_t *a, nanortc_t *b, uint32_t now_ms, int max_rounds)
{
    int total = 0;
    for (int i = 0; i < max_rounds; i++) {
        int ra = bench_relay(a, b, now_ms);
        int rb = bench_relay(b, a, now_ms);
        if (ra <= 0 && rb <= 0) break;
        total += (ra > 0 ? ra : 0) + (rb > 0 ? rb : 0);
    }
    return total;
}

/*
 * Full connection via SDP offer/answer + ICE + DTLS + SCTP.
 * Returns 0 on success, sets *out_now to final time.
 */
static int bench_full_connect_sdp(nanortc_t *offerer, nanortc_t *answerer, uint32_t *out_now)
{
    /* Register DC on offerer */
    int sid = nanortc_create_datachannel(offerer, "bench", NULL);
    if (sid < 0) return -1;

    /* Add local candidates */
    nanortc_add_local_candidate(offerer, "192.168.1.1", 4000);
    nanortc_add_local_candidate(answerer, "192.168.1.2", 5000);

    /* SDP offer/answer */
    char offer[4096];
    size_t offer_len = 0;
    if (nanortc_create_offer(offerer, offer, sizeof(offer), &offer_len) != 0) return -1;
    offer[offer_len] = '\0';

    char answer[4096];
    size_t answer_len = 0;
    if (nanortc_accept_offer(answerer, offer, answer, sizeof(answer), &answer_len) != 0) return -1;
    answer[answer_len] = '\0';

    if (nanortc_accept_answer(offerer, answer) != 0) return -1;

    /* Ensure answerer has offerer's candidate (may not be in SDP) */
    answerer->ice.remote_candidates[0].family = 4;
    answerer->ice.remote_candidates[0].addr[0] = 192;
    answerer->ice.remote_candidates[0].addr[1] = 168;
    answerer->ice.remote_candidates[0].addr[2] = 1;
    answerer->ice.remote_candidates[0].addr[3] = 1;
    answerer->ice.remote_candidates[0].port = 4000;
    if (answerer->ice.remote_candidate_count == 0)
        answerer->ice.remote_candidate_count = 1;

    /* Drive ICE + DTLS + SCTP handshake */
    uint32_t now_ms = 100;
    for (int round = 0; round < 400; round++) {
        nanortc_handle_input(offerer, now_ms, NULL, 0, NULL);
        nanortc_handle_input(answerer, now_ms, NULL, 0, NULL);
        bench_pump(offerer, answerer, now_ms, 30);

        if (offerer->state == NANORTC_STATE_CONNECTED &&
            answerer->state == NANORTC_STATE_CONNECTED) {
            *out_now = now_ms;
            return sid;
        }
        now_ms += 10;
    }
    return -1;
}

/* ----------------------------------------------------------------
 * Throughput benchmark at a given payload size
 * ---------------------------------------------------------------- */

static void bench_dc_throughput_at_size(size_t payload_size, int n_messages)
{
    nanortc_t sender, receiver;

    nanortc_config_t s_cfg = bench_default_config();
    s_cfg.role = NANORTC_ROLE_CONTROLLING;
    nanortc_init(&sender, &s_cfg);

    nanortc_config_t r_cfg = bench_default_config();
    r_cfg.role = NANORTC_ROLE_CONTROLLED;
    nanortc_init(&receiver, &r_cfg);

    uint32_t now_ms = 0;
    int sid = bench_full_connect_sdp(&sender, &receiver, &now_ms);
    if (sid < 0) {
        fprintf(stderr, "  SKIP: connection failed for payload_size=%zu (sender=%d receiver=%d)\n",
                payload_size, sender.state, receiver.state);
        nanortc_destroy(&sender);
        nanortc_destroy(&receiver);
        return;
    }

    /* Drain post-connection outputs (DCEP, events) */
    for (int i = 0; i < 20; i++) {
        now_ms += 10;
        nanortc_handle_input(&sender, now_ms, NULL, 0, NULL);
        nanortc_handle_input(&receiver, now_ms, NULL, 0, NULL);
        bench_pump(&sender, &receiver, now_ms, 20);
    }

    /* Prepare payload */
    uint8_t *payload = (uint8_t *)malloc(payload_size);
    memset(payload, 0xAB, payload_size);

    char label[64];
    snprintf(label, sizeof(label), "dc_throughput_%zuB", payload_size);

    BENCH_THROUGHPUT_BEGIN(tp);

    for (int i = 0; i < n_messages; i++) {
        int rc = nanortc_datachannel_send(&sender, (uint16_t)sid, payload, payload_size);
        if (rc == 0) {
            BENCH_THROUGHPUT_ADD(tp, payload_size, 1);
        }

        /* Pump periodically to prevent queue full */
        if ((i & 0xF) == 0xF) {
            now_ms += 1;
            nanortc_handle_input(&sender, now_ms, NULL, 0, NULL);
            nanortc_handle_input(&receiver, now_ms, NULL, 0, NULL);
            bench_pump(&sender, &receiver, now_ms, 20);
        }
    }

    /* Final pump to drain */
    for (int i = 0; i < 20; i++) {
        now_ms += 1;
        nanortc_handle_input(&sender, now_ms, NULL, 0, NULL);
        nanortc_handle_input(&receiver, now_ms, NULL, 0, NULL);
        bench_pump(&sender, &receiver, now_ms, 20);
    }

    BENCH_THROUGHPUT_END(tp, label);

    free(payload);
    nanortc_destroy(&sender);
    nanortc_destroy(&receiver);
}

/* ----------------------------------------------------------------
 * Main
 * ---------------------------------------------------------------- */

BENCH_MAIN_BEGIN("bench_dc_throughput")

    struct { size_t size; int msgs; } tests[] = {
        {   64, 10000 },
        {  256,  5000 },
        { 1024,  2000 },
        { 4096,  1000 },
    };

    for (int i = 0; i < 4; i++) {
        bench_dc_throughput_at_size(tests[i].size, tests[i].msgs);
    }

BENCH_MAIN_END
