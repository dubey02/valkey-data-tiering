/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */
/* bench_common.h — shared workload generator + stats for fc_bench and rocksdb_bench.
 * Both apps include this so traffic and reporting are identical by construction. */
#ifndef BENCH_COMMON_H
#define BENCH_COMMON_H

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <random>
#include <vector>
#include <string>
#include <algorithm>

/* ---------- clock ---------- */
static inline uint64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
}

/* ---------- workload ---------- */
/* Keys are exactly 16 bytes: "key-" + 12 zero-padded digits. */
#define BENCH_KEY_LEN 16
static inline size_t make_key(char *buf, uint64_t id) {
    snprintf(buf, BENCH_KEY_LEN + 1, "key-%012llu", (unsigned long long)id);
    return BENCH_KEY_LEN;
}

struct Workload {
    uint64_t keyspace;
    int read_pct;              /* 0..100 */
    std::mt19937_64 rng;
    std::uniform_int_distribution<uint64_t> key_dist;
    std::uniform_int_distribution<int> pct_dist;

    Workload(uint64_t ks, int rpct, uint64_t seed)
        : keyspace(ks), read_pct(rpct), rng(seed),
          key_dist(0, ks - 1), pct_dist(0, 99) {}

    bool next_is_read() { return pct_dist(rng) < read_pct; }
    uint64_t next_key() { return key_dist(rng); }
};

/* Fill value buffer with pseudorandom (incompressible) bytes. Deterministic. */
static inline void fill_value(char *buf, size_t len, uint64_t seed) {
    std::mt19937_64 r(seed);
    uint64_t *p = (uint64_t *)buf;
    size_t n = len / 8;
    for (size_t i = 0; i < n; i++) p[i] = r();
    for (size_t i = n * 8; i < len; i++) buf[i] = (char)(r() & 0xff);
}

/* ---------- latency histogram ----------
 * Bucket layout (microseconds):
 *   [0,1ms)    step 10us   -> 100 buckets
 *   [1,10ms)   step 100us  -> 90
 *   [10,100ms) step 1ms    -> 90
 *   [100ms,1s) step 10ms   -> 90
 *   >= 1s      overflow    -> 1
 */
struct LatHist {
    static const int NB = 100 + 90 + 90 + 90 + 1;
    uint64_t buckets[NB];
    uint64_t count, sum_us, max_us;

    LatHist() { reset(); }
    void reset() { memset(buckets, 0, sizeof(buckets)); count = sum_us = max_us = 0; }

    static int idx(uint64_t us) {
        if (us < 1000)    return (int)(us / 10);
        if (us < 10000)   return 100 + (int)((us - 1000) / 100);
        if (us < 100000)  return 190 + (int)((us - 10000) / 1000);
        if (us < 1000000) return 280 + (int)((us - 100000) / 10000);
        return NB - 1;
    }
    /* upper bound of bucket i in us (representative value reported) */
    static uint64_t upper(int i) {
        if (i < 100) return (uint64_t)(i + 1) * 10;
        if (i < 190) return 1000 + (uint64_t)(i - 100 + 1) * 100;
        if (i < 280) return 10000 + (uint64_t)(i - 190 + 1) * 1000;
        if (i < NB - 1) return 100000 + (uint64_t)(i - 280 + 1) * 10000;
        return 1000000;
    }
    void record(uint64_t us) {
        buckets[idx(us)]++;
        count++; sum_us += us;
        if (us > max_us) max_us = us;
    }
    void merge(const LatHist &o) {
        for (int i = 0; i < NB; i++) buckets[i] += o.buckets[i];
        count += o.count; sum_us += o.sum_us;
        if (o.max_us > max_us) max_us = o.max_us;
    }
    uint64_t percentile(double p) const {
        if (count == 0) return 0;
        uint64_t target = (uint64_t)(p / 100.0 * (double)count);
        if (target >= count) target = count - 1;
        uint64_t c = 0;
        for (int i = 0; i < NB; i++) {
            c += buckets[i];
            if (c > target) return (i == NB - 1) ? max_us : upper(i);
        }
        return max_us;
    }
    double avg() const { return count ? (double)sum_us / (double)count : 0.0; }
    /* "p50:123 p90:456 p99:789 p999:1011 p100:1213 avg:99.9 n:12345" */
    void fmt(char *out, size_t outsz) const {
        snprintf(out, outsz,
                 "p50:%llu p90:%llu p99:%llu p999:%llu p100:%llu avg:%.1f n:%llu",
                 (unsigned long long)percentile(50), (unsigned long long)percentile(90),
                 (unsigned long long)percentile(99), (unsigned long long)percentile(99.9),
                 (unsigned long long)max_us, avg(), (unsigned long long)count);
    }
};

/* ---------- per-second stat emitter ----------
 * CSV to a file: wall_sec,phase,read_tps,write_tps,hit,miss,
 *                rd_p50,rd_p99,rd_p999,wr_p50,wr_p99,wr_p999  (interval values)
 * Human line to stdout every emit as well. */
struct StatEmitter {
    FILE *csv;
    uint64_t t0_us, last_us;
    uint64_t last_reads, last_writes, last_hits, last_misses;
    LatHist rd_int, wr_int;    /* interval hists, reset each emit */

    StatEmitter(const char *csv_path) {
        csv = fopen(csv_path, "w");
        if (csv) fprintf(csv, "sec,phase,read_tps,write_tps,hits,misses,"
                              "rd_p50_us,rd_p99_us,rd_p999_us,wr_p50_us,wr_p99_us,wr_p999_us\n");
        t0_us = last_us = now_us();
        last_reads = last_writes = last_hits = last_misses = 0;
    }
    ~StatEmitter() { if (csv) fclose(csv); }

    /* call ~every loop; emits if >=1s since last */
    void maybe_emit(const char *phase, uint64_t reads, uint64_t writes,
                    uint64_t hits, uint64_t misses, bool force = false) {
        uint64_t t = now_us();
        double dt = (double)(t - last_us) / 1e6;
        if (dt < 1.0 && !force) return;
        double rtps = (double)(reads - last_reads) / dt;
        double wtps = (double)(writes - last_writes) / dt;
        uint64_t sec = (t - t0_us) / 1000000ull;
        if (csv) {
            fprintf(csv, "%llu,%s,%.0f,%.0f,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu\n",
                    (unsigned long long)sec, phase, rtps, wtps,
                    (unsigned long long)(hits - last_hits),
                    (unsigned long long)(misses - last_misses),
                    (unsigned long long)rd_int.percentile(50),
                    (unsigned long long)rd_int.percentile(99),
                    (unsigned long long)rd_int.percentile(99.9),
                    (unsigned long long)wr_int.percentile(50),
                    (unsigned long long)wr_int.percentile(99),
                    (unsigned long long)wr_int.percentile(99.9));
            fflush(csv);
        }
        printf("[%6llus %s] rd %.0f/s wr %.0f/s hit %llu miss %llu | rd p50 %lluus p99 %lluus | wr p50 %lluus p99 %lluus\n",
               (unsigned long long)sec, phase, rtps, wtps,
               (unsigned long long)(hits - last_hits), (unsigned long long)(misses - last_misses),
               (unsigned long long)rd_int.percentile(50), (unsigned long long)rd_int.percentile(99),
               (unsigned long long)wr_int.percentile(50), (unsigned long long)wr_int.percentile(99));
        fflush(stdout);
        last_us = t; last_reads = reads; last_writes = writes;
        last_hits = hits; last_misses = misses;
        rd_int.reset(); wr_int.reset();
    }
};

/* ---------- final summary (JSON) ---------- */
static inline void write_summary_json(const char *path, const char *backend,
        uint64_t keyspace, size_t value_size, int read_pct,
        uint64_t reads, uint64_t writes, uint64_t hits, uint64_t misses,
        double measured_secs,
        const LatHist &rd_hit, const LatHist &rd_miss, const LatHist &wr,
        const char *extra_json /* nullable, no braces */) {
    FILE *f = fopen(path, "w");
    if (!f) return;
    char b1[256], b2[256], b3[256];
    rd_hit.fmt(b1, sizeof(b1)); rd_miss.fmt(b2, sizeof(b2)); wr.fmt(b3, sizeof(b3));
    fprintf(f, "{\n"
        "  \"backend\": \"%s\",\n"
        "  \"keyspace\": %llu,\n  \"value_size\": %zu,\n  \"read_pct\": %d,\n"
        "  \"measured_secs\": %.1f,\n"
        "  \"reads\": %llu,\n  \"writes\": %llu,\n  \"hits\": %llu,\n  \"misses\": %llu,\n"
        "  \"read_tps\": %.1f,\n  \"write_tps\": %.1f,\n"
        "  \"hit_rate_pct\": %.2f,\n"
        "  \"read_hit_latency\": \"%s\",\n"
        "  \"read_miss_latency\": \"%s\",\n"
        "  \"write_latency\": \"%s\"%s%s\n"
        "}\n",
        backend, (unsigned long long)keyspace, value_size, read_pct, measured_secs,
        (unsigned long long)reads, (unsigned long long)writes,
        (unsigned long long)hits, (unsigned long long)misses,
        (double)reads / measured_secs, (double)writes / measured_secs,
        reads ? 100.0 * (double)hits / (double)reads : 0.0,
        b1, b2, b3,
        extra_json ? ",\n  " : "", extra_json ? extra_json : "");
    fclose(f);
}

#endif /* BENCH_COMMON_H */
