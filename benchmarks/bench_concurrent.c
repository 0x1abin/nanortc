/*
 * nanortc — Concurrent instance scaling benchmark
 *
 * Creates 1/10/50/100 nanortc_t instances to measure:
 *   - Memory linearity (sizeof * N)
 *   - Init/destroy time scaling
 *   - RSS growth (Linux)
 *
 * SPDX-License-Identifier: MIT
 */

#include "bench_common.h"
#include "nanortc.h"
#include "nanortc_crypto.h"
#include "nano_test_config.h"
#include <string.h>

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

static void bench_n_instances(int n)
{
    char label_init[64], label_total[64], label_rss[64];
    snprintf(label_init, sizeof(label_init), "init_%d_instances", n);
    snprintf(label_total, sizeof(label_total), "total_mem_%d_instances", n);
    snprintf(label_rss, sizeof(label_rss), "rss_%d_instances", n);

    nanortc_t *instances = (nanortc_t *)malloc((size_t)n * sizeof(nanortc_t));
    if (!instances) {
        fprintf(stderr, "  SKIP: malloc failed for %d instances\n", n);
        return;
    }

    nanortc_config_t cfg = bench_default_config();

    /* Measure init time */
    uint64_t t0 = bench_now_ns();
    for (int i = 0; i < n; i++) {
        nanortc_init(&instances[i], &cfg);
    }
    uint64_t init_ns = bench_now_ns() - t0;

    /* Report memory */
    size_t total_bytes = (size_t)n * sizeof(nanortc_t);
    long rss = bench_get_rss_kb();

    printf("{\"name\":\"%s\",\"count\":%d,\"elapsed_ns\":%llu,\"per_instance_ns\":%llu}\n",
           label_init, n, (unsigned long long)init_ns,
           (unsigned long long)(init_ns / (uint64_t)n));
    printf("{\"name\":\"%s\",\"count\":%d,\"total_bytes\":%zu,\"per_instance_bytes\":%zu}\n",
           label_total, n, total_bytes, sizeof(nanortc_t));

    if (rss > 0) {
        printf("{\"name\":\"%s\",\"count\":%d,\"rss_kb\":%ld}\n", label_rss, n, rss);
    }

    fprintf(stderr, "  %3d instances: init=%8.2f us/inst  mem=%zu KB total  sizeof=%zu B/inst",
            n, (double)init_ns / (double)n / 1000.0,
            total_bytes / 1024, sizeof(nanortc_t));
    if (rss > 0) fprintf(stderr, "  RSS=%ld KB", rss);
    fprintf(stderr, "\n");

    /* Measure destroy time */
    uint64_t t1 = bench_now_ns();
    for (int i = 0; i < n; i++) {
        nanortc_destroy(&instances[i]);
    }
    uint64_t destroy_ns = bench_now_ns() - t1;
    (void)destroy_ns;

    free(instances);
}

BENCH_MAIN_BEGIN("bench_concurrent")

    bench_print_sizeof_json("nanortc_t", sizeof(nanortc_t));
    fprintf(stderr, "  sizeof(nanortc_t) = %zu bytes (%.1f KB)\n",
            sizeof(nanortc_t), (double)sizeof(nanortc_t) / 1024.0);

    int counts[] = {1, 10, 50, 100};
    for (int i = 0; i < 4; i++) {
        bench_n_instances(counts[i]);
    }

BENCH_MAIN_END
