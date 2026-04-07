/*
 * nanortc — Benchmark measurement framework
 *
 * Provides timing, statistics, and JSON Lines output for all benchmarks.
 * Uses CLOCK_MONOTONIC on Linux/macOS for high-resolution timing.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef NANORTC_BENCH_COMMON_H_
#define NANORTC_BENCH_COMMON_H_

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ----------------------------------------------------------------
 * High-resolution timer
 * ---------------------------------------------------------------- */

static inline uint64_t bench_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* ----------------------------------------------------------------
 * Statistics collector
 * ---------------------------------------------------------------- */

#define BENCH_MAX_SAMPLES 100000

typedef struct bench_stats {
    const char *name;
    uint64_t *samples;   /* heap-allocated sample array */
    int count;
    int capacity;
} bench_stats_t;

static inline void bench_stats_init(bench_stats_t *s, const char *name, int capacity)
{
    s->name = name;
    s->capacity = capacity < BENCH_MAX_SAMPLES ? capacity : BENCH_MAX_SAMPLES;
    s->count = 0;
    s->samples = (uint64_t *)malloc((size_t)s->capacity * sizeof(uint64_t));
}

static inline void bench_stats_free(bench_stats_t *s)
{
    free(s->samples);
    s->samples = NULL;
    s->count = 0;
}

static inline void bench_stats_add(bench_stats_t *s, uint64_t sample_ns)
{
    if (s->count < s->capacity) {
        s->samples[s->count++] = sample_ns;
    }
}

/* qsort comparator for uint64_t */
static int bench_cmp_u64(const void *a, const void *b)
{
    uint64_t va = *(const uint64_t *)a;
    uint64_t vb = *(const uint64_t *)b;
    if (va < vb) return -1;
    if (va > vb) return 1;
    return 0;
}

typedef struct bench_summary {
    uint64_t min_ns;
    uint64_t max_ns;
    uint64_t avg_ns;
    uint64_t median_ns;
    uint64_t p95_ns;
    uint64_t p99_ns;
    uint64_t total_ns;
    int count;
} bench_summary_t;

static inline bench_summary_t bench_stats_summarize(bench_stats_t *s)
{
    bench_summary_t r;
    memset(&r, 0, sizeof(r));

    if (s->count == 0) return r;

    qsort(s->samples, (size_t)s->count, sizeof(uint64_t), bench_cmp_u64);

    r.count = s->count;
    r.min_ns = s->samples[0];
    r.max_ns = s->samples[s->count - 1];
    r.median_ns = s->samples[s->count / 2];
    r.p95_ns = s->samples[(int)((double)s->count * 0.95)];
    r.p99_ns = s->samples[(int)((double)s->count * 0.99)];

    uint64_t sum = 0;
    for (int i = 0; i < s->count; i++) {
        sum += s->samples[i];
        /* Overflow protection: if sum wraps, cap it */
        if (sum < s->samples[i]) { sum = UINT64_MAX; break; }
    }
    r.total_ns = sum;
    r.avg_ns = sum / (uint64_t)s->count;

    return r;
}

/* ----------------------------------------------------------------
 * JSON Lines output (machine-parseable)
 * ---------------------------------------------------------------- */

static inline void bench_print_json(const bench_summary_t *r, const char *name, const char *unit)
{
    printf("{\"name\":\"%s\",\"unit\":\"%s\",\"count\":%d,"
           "\"min_ns\":%llu,\"max_ns\":%llu,\"avg_ns\":%llu,"
           "\"median_ns\":%llu,\"p95_ns\":%llu,\"p99_ns\":%llu,"
           "\"total_ns\":%llu}\n",
           name, unit, r->count,
           (unsigned long long)r->min_ns, (unsigned long long)r->max_ns,
           (unsigned long long)r->avg_ns, (unsigned long long)r->median_ns,
           (unsigned long long)r->p95_ns, (unsigned long long)r->p99_ns,
           (unsigned long long)r->total_ns);
}

/* Human-readable summary to stderr */
static inline void bench_print_human(const bench_summary_t *r, const char *name)
{
    fprintf(stderr, "  %-35s  n=%-6d  avg=%8.2f us  p50=%8.2f us  p95=%8.2f us  p99=%8.2f us\n",
            name, r->count,
            (double)r->avg_ns / 1000.0,
            (double)r->median_ns / 1000.0,
            (double)r->p95_ns / 1000.0,
            (double)r->p99_ns / 1000.0);
}

/* ----------------------------------------------------------------
 * Throughput reporting
 * ---------------------------------------------------------------- */

typedef struct bench_throughput {
    const char *name;
    uint64_t total_bytes;
    uint64_t total_msgs;
    uint64_t elapsed_ns;
} bench_throughput_t;

static inline void bench_print_throughput_json(const bench_throughput_t *t)
{
    double elapsed_s = (double)t->elapsed_ns / 1e9;
    double mb_per_s = (elapsed_s > 0) ? (double)t->total_bytes / (1024.0 * 1024.0) / elapsed_s : 0;
    double msg_per_s = (elapsed_s > 0) ? (double)t->total_msgs / elapsed_s : 0;

    printf("{\"name\":\"%s\",\"total_bytes\":%llu,\"total_msgs\":%llu,"
           "\"elapsed_ns\":%llu,\"mb_per_s\":%.2f,\"msg_per_s\":%.0f}\n",
           t->name,
           (unsigned long long)t->total_bytes, (unsigned long long)t->total_msgs,
           (unsigned long long)t->elapsed_ns, mb_per_s, msg_per_s);
}

static inline void bench_print_throughput_human(const bench_throughput_t *t)
{
    double elapsed_s = (double)t->elapsed_ns / 1e9;
    double mb_per_s = (elapsed_s > 0) ? (double)t->total_bytes / (1024.0 * 1024.0) / elapsed_s : 0;
    double msg_per_s = (elapsed_s > 0) ? (double)t->total_msgs / elapsed_s : 0;

    fprintf(stderr, "  %-35s  %.2f MB/s  %.0f msg/s  (%llu bytes in %.3f s)\n",
            t->name, mb_per_s, msg_per_s,
            (unsigned long long)t->total_bytes, elapsed_s);
}

/* ----------------------------------------------------------------
 * Convenience macros
 * ---------------------------------------------------------------- */

/* Run a benchmark: collect N samples of expr, report stats */
#define BENCH_RUN(stat_name, n_iters, expr) do { \
    bench_stats_t _st; \
    bench_stats_init(&_st, stat_name, (n_iters)); \
    for (int _i = 0; _i < (n_iters); _i++) { \
        uint64_t _t0 = bench_now_ns(); \
        { expr; } \
        bench_stats_add(&_st, bench_now_ns() - _t0); \
    } \
    bench_summary_t _r = bench_stats_summarize(&_st); \
    bench_print_json(&_r, stat_name, "ns"); \
    bench_print_human(&_r, stat_name); \
    bench_stats_free(&_st); \
} while (0)

/* Measure wall-clock throughput of a loop */
#define BENCH_THROUGHPUT_BEGIN(var_name) \
    uint64_t var_name##_t0 = bench_now_ns(); \
    uint64_t var_name##_bytes = 0; \
    uint64_t var_name##_msgs = 0

#define BENCH_THROUGHPUT_ADD(var_name, bytes, msgs) do { \
    var_name##_bytes += (bytes); \
    var_name##_msgs += (msgs); \
} while (0)

#define BENCH_THROUGHPUT_END(var_name, label) do { \
    bench_throughput_t _tp = { \
        .name = (label), \
        .total_bytes = var_name##_bytes, \
        .total_msgs = var_name##_msgs, \
        .elapsed_ns = bench_now_ns() - var_name##_t0, \
    }; \
    bench_print_throughput_json(&_tp); \
    bench_print_throughput_human(&_tp); \
} while (0)

/* ----------------------------------------------------------------
 * Memory reporting (Linux /proc/self/status, sizeof)
 * ---------------------------------------------------------------- */

static inline long bench_get_rss_kb(void)
{
#ifdef __linux__
    FILE *f = fopen("/proc/self/status", "r");
    if (!f) return -1;
    char line[256];
    long rss = -1;
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == 'V' && line[1] == 'm' && line[2] == 'R' &&
            line[3] == 'S' && line[4] == 'S') {
            /* VmRSS: <number> kB */
            const char *p = line + 5;
            while (*p == ' ' || *p == ':' || *p == '\t') p++;
            rss = strtol(p, NULL, 10);
            break;
        }
    }
    fclose(f);
    return rss;
#else
    /* macOS: not easily available via /proc, return -1 */
    return -1;
#endif
}

static inline void bench_print_sizeof_json(const char *name, size_t bytes)
{
    printf("{\"name\":\"%s\",\"sizeof_bytes\":%zu}\n", name, bytes);
}

/* ----------------------------------------------------------------
 * Benchmark main boilerplate
 * ---------------------------------------------------------------- */

#define BENCH_MAIN_BEGIN(suite_name) \
    int main(void) { \
        fprintf(stderr, "=== %s (DC=%d AUDIO=%d VIDEO=%d) ===\n", suite_name, \
                NANORTC_FEATURE_DATACHANNEL, NANORTC_FEATURE_AUDIO, NANORTC_FEATURE_VIDEO); \
        int _bench_failures = 0; \
        (void)_bench_failures;

#define BENCH_MAIN_END \
        fprintf(stderr, "=== done ===\n"); \
        return _bench_failures; \
    }

#endif /* NANORTC_BENCH_COMMON_H_ */
