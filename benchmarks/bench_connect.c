/*
 * nanortc — Connection establishment benchmark
 *
 * Measures ICE + DTLS handshake latency using Sans I/O loopback.
 * Two nanortc_t instances wired together in memory, no network.
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
 * E2E helpers (same pattern as test_e2e.c)
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

static void bench_setup_ice_creds(nanortc_t *offerer, nanortc_t *answerer)
{
    memcpy(offerer->ice.local_ufrag, "OFF", 4);
    offerer->ice.local_ufrag_len = 3;
    memcpy(offerer->ice.local_pwd, "offerer-password-1234", 22);
    offerer->ice.local_pwd_len = 21;
    memcpy(offerer->ice.remote_ufrag, "ANS", 4);
    offerer->ice.remote_ufrag_len = 3;
    memcpy(offerer->ice.remote_pwd, "answerer-password-5678", 23);
    offerer->ice.remote_pwd_len = 22;
    offerer->ice.tie_breaker = 0x1234567890ABCDEFull;
    offerer->ice.local_candidates[0].family = 4;
    offerer->ice.local_candidates[0].addr[0] = 192;
    offerer->ice.local_candidates[0].addr[1] = 168;
    offerer->ice.local_candidates[0].addr[2] = 1;
    offerer->ice.local_candidates[0].addr[3] = 1;
    offerer->ice.local_candidates[0].port = 4000;
    offerer->ice.local_candidates[0].type = NANORTC_ICE_CAND_HOST;
    offerer->ice.local_candidate_count = 1;

    memcpy(answerer->ice.local_ufrag, "ANS", 4);
    answerer->ice.local_ufrag_len = 3;
    memcpy(answerer->ice.local_pwd, "answerer-password-5678", 23);
    answerer->ice.local_pwd_len = 22;
    memcpy(answerer->ice.remote_ufrag, "OFF", 4);
    answerer->ice.remote_ufrag_len = 3;
    memcpy(answerer->ice.remote_pwd, "offerer-password-1234", 22);
    answerer->ice.remote_pwd_len = 21;

    offerer->ice.remote_candidates[0].family = 4;
    offerer->ice.remote_candidates[0].addr[0] = 192;
    offerer->ice.remote_candidates[0].addr[1] = 168;
    offerer->ice.remote_candidates[0].addr[2] = 1;
    offerer->ice.remote_candidates[0].addr[3] = 2;
    offerer->ice.remote_candidates[0].port = 5000;
    offerer->ice.remote_candidate_count = 1;
}

/* ----------------------------------------------------------------
 * Benchmark: full connection (ICE + DTLS + SCTP)
 * ---------------------------------------------------------------- */

static uint64_t bench_one_connection(void)
{
    nanortc_t offerer, answerer;

    nanortc_config_t off_cfg = bench_default_config();
    off_cfg.role = NANORTC_ROLE_CONTROLLING;
    nanortc_init(&offerer, &off_cfg);

    nanortc_config_t ans_cfg = bench_default_config();
    ans_cfg.role = NANORTC_ROLE_CONTROLLED;
    nanortc_init(&answerer, &ans_cfg);

    bench_setup_ice_creds(&offerer, &answerer);

    uint64_t t0 = bench_now_ns();
    uint32_t now_ms = 100;

    /* Drive handshake until both reach DTLS_CONNECTED or beyond */
    for (int round = 0; round < 200; round++) {
        nanortc_handle_input(&offerer, now_ms, NULL, 0, NULL);
        nanortc_handle_input(&answerer, now_ms, NULL, 0, NULL);
        bench_pump(&offerer, &answerer, now_ms, 20);

        if (offerer.state >= NANORTC_STATE_DTLS_CONNECTED &&
            answerer.state >= NANORTC_STATE_DTLS_CONNECTED) {
            break;
        }
        now_ms += 1; /* 1ms steps for fine-grained timing */
    }

    uint64_t elapsed = bench_now_ns() - t0;

    nanortc_destroy(&offerer);
    nanortc_destroy(&answerer);

    return elapsed;
}

#if NANORTC_FEATURE_DATACHANNEL
static uint64_t bench_one_full_connect(void)
{
    nanortc_t offerer, answerer;

    nanortc_config_t off_cfg = bench_default_config();
    off_cfg.role = NANORTC_ROLE_CONTROLLING;
    nanortc_init(&offerer, &off_cfg);

    nanortc_config_t ans_cfg = bench_default_config();
    ans_cfg.role = NANORTC_ROLE_CONTROLLED;
    nanortc_init(&answerer, &ans_cfg);

    bench_setup_ice_creds(&offerer, &answerer);

    uint64_t t0 = bench_now_ns();
    uint32_t now_ms = 100;

    for (int round = 0; round < 200; round++) {
        nanortc_handle_input(&offerer, now_ms, NULL, 0, NULL);
        nanortc_handle_input(&answerer, now_ms, NULL, 0, NULL);
        bench_pump(&offerer, &answerer, now_ms, 20);

        if (offerer.state == NANORTC_STATE_CONNECTED &&
            answerer.state == NANORTC_STATE_CONNECTED) {
            break;
        }
        now_ms += 10;
    }

    uint64_t elapsed = bench_now_ns() - t0;

    nanortc_destroy(&offerer);
    nanortc_destroy(&answerer);

    return elapsed;
}
#endif

/* ----------------------------------------------------------------
 * Benchmark: nanortc_init / nanortc_destroy
 * ---------------------------------------------------------------- */

static void bench_init_destroy(void)
{
    nanortc_t rtc;
    nanortc_config_t cfg = bench_default_config();
    nanortc_init(&rtc, &cfg);
    nanortc_destroy(&rtc);
}

/* ----------------------------------------------------------------
 * Main
 * ---------------------------------------------------------------- */

BENCH_MAIN_BEGIN("bench_connect")

    /* sizeof report */
    bench_print_sizeof_json("nanortc_t", sizeof(nanortc_t));
    fprintf(stderr, "  sizeof(nanortc_t) = %zu bytes (%.1f KB)\n",
            sizeof(nanortc_t), (double)sizeof(nanortc_t) / 1024.0);

    /* Benchmark: init + destroy cycle */
    BENCH_RUN("init_destroy", 1000, bench_init_destroy());

    /* Benchmark: ICE + DTLS connection */
    {
        bench_stats_t st;
        bench_stats_init(&st, "ice_dtls_connect", 100);
        for (int i = 0; i < 100; i++) {
            bench_stats_add(&st, bench_one_connection());
        }
        bench_summary_t r = bench_stats_summarize(&st);
        bench_print_json(&r, "ice_dtls_connect", "ns");
        bench_print_human(&r, "ice_dtls_connect");
        bench_stats_free(&st);
    }

#if NANORTC_FEATURE_DATACHANNEL
    /* Benchmark: ICE + DTLS + SCTP full connection */
    {
        bench_stats_t st;
        bench_stats_init(&st, "full_connect_ice_dtls_sctp", 50);
        for (int i = 0; i < 50; i++) {
            bench_stats_add(&st, bench_one_full_connect());
        }
        bench_summary_t r = bench_stats_summarize(&st);
        bench_print_json(&r, "full_connect_ice_dtls_sctp", "ns");
        bench_print_human(&r, "full_connect_ice_dtls_sctp");
        bench_stats_free(&st);
    }
#endif

BENCH_MAIN_END
