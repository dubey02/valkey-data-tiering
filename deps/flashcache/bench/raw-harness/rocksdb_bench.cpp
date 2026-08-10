/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */
/* rocksdb_bench.cpp — raw RocksDB benchmark, twin of fc_bench.cpp.
 * N worker threads each run the shared workload independently (seed+tid),
 * blocking Get()/Put() — RocksDB's natural concurrency model.
 *
 * Config per sign-off: leveled compaction, 4 levels, 1GiB memtable, 8GiB block
 * cache (index+filter pinned), bloom 10 bits, WAL off, direct I/O, 4 bg jobs,
 * no compression (values are incompressible random bytes; FC does not compress).
 *
 * Usage:
 *   rocksdb_bench <db_dir> <keyspace> <value_size> <read_pct> <threads>
 *                 <settle_secs> <run_secs> <seed> <results_dir> [skip_populate]
 */
#include "bench_common.h"

#include <atomic>
#include <thread>

#include <rocksdb/db.h>
#include <rocksdb/options.h>
#include <rocksdb/table.h>
#include <rocksdb/filter_policy.h>
#include <rocksdb/cache.h>
#include <rocksdb/statistics.h>

using ROCKSDB_NAMESPACE::DB;
using ROCKSDB_NAMESPACE::Options;
using ROCKSDB_NAMESPACE::ReadOptions;
using ROCKSDB_NAMESPACE::WriteOptions;
using ROCKSDB_NAMESPACE::Slice;
using ROCKSDB_NAMESPACE::Status;
using ROCKSDB_NAMESPACE::BlockBasedTableOptions;
using ROCKSDB_NAMESPACE::NewLRUCache;
using ROCKSDB_NAMESPACE::NewBloomFilterPolicy;

struct ThreadResult {
    LatHist rd_hit, rd_miss, wr;
    uint64_t reads = 0, writes = 0, hits = 0, misses = 0;
};

static std::atomic<bool> g_stop{false};

static void worker(DB *db, uint64_t keyspace, size_t value_size, int read_pct,
                   uint64_t seed, int tid, const char *value,
                   ThreadResult *res,
                   std::atomic<uint64_t> *a_reads, std::atomic<uint64_t> *a_writes,
                   std::atomic<uint64_t> *a_hits, std::atomic<uint64_t> *a_misses,
                   StatEmitter *emit /* only tid 0 feeds interval hists */) {
    Workload wl(keyspace, read_pct, seed + 1000003ull * (uint64_t)tid);
    ReadOptions ro;
    WriteOptions wo; wo.disableWAL = true;
    char key[BENCH_KEY_LEN + 1];
    std::string got;
    got.reserve(value_size + 64);

    while (!g_stop.load(std::memory_order_relaxed)) {
        size_t klen = make_key(key, wl.next_key());
        if (wl.next_is_read()) {
            uint64_t t = now_us();
            Status s = db->Get(ro, Slice(key, klen), &got);
            uint64_t lat = now_us() - t;
            res->reads++; a_reads->fetch_add(1, std::memory_order_relaxed);
            if (s.ok()) {
                res->hits++; a_hits->fetch_add(1, std::memory_order_relaxed);
                res->rd_hit.record(lat);
                if (tid == 0 && emit) emit->rd_int.record(lat);
            } else {
                res->misses++; a_misses->fetch_add(1, std::memory_order_relaxed);
                res->rd_miss.record(lat);
            }
        } else {
            uint64_t t = now_us();
            Status s = db->Put(wo, Slice(key, klen), Slice(value, value_size));
            uint64_t lat = now_us() - t;
            if (!s.ok()) { fprintf(stderr, "Put failed: %s\n", s.ToString().c_str()); exit(1); }
            res->writes++; a_writes->fetch_add(1, std::memory_order_relaxed);
            res->wr.record(lat);
            if (tid == 0 && emit) emit->wr_int.record(lat);
        }
    }
}

static void dump_rocksdb_stats(DB *db, FILE *f, uint64_t sec) {
    if (!f) return;
    std::string s;
    db->GetProperty("rocksdb.stats", &s);
    fprintf(f, "===== sec %llu =====\n%s\n", (unsigned long long)sec, s.c_str());
    std::string v;
    fprintf(f, "--- properties ---\n");
    const char *props[] = {
        "rocksdb.block-cache-usage", "rocksdb.block-cache-capacity",
        "rocksdb.estimate-live-data-size", "rocksdb.total-sst-files-size",
        "rocksdb.num-files-at-level0", "rocksdb.num-files-at-level1",
        "rocksdb.num-files-at-level2", "rocksdb.num-files-at-level3",
        "rocksdb.cur-size-all-mem-tables", "rocksdb.estimate-num-keys",
        "rocksdb.actual-delayed-write-rate", "rocksdb.is-write-stopped" };
    for (auto p : props) {
        if (db->GetProperty(p, &v)) fprintf(f, "%s = %s\n", p, v.c_str());
    }
    fflush(f);
}

int main(int argc, char **argv) {
    if (argc < 10) {
        fprintf(stderr, "usage: %s <db_dir> <keyspace> <value_size> <read_pct> <threads> "
                        "<settle_secs> <run_secs> <seed> <results_dir> [skip_populate]\n", argv[0]);
        return 1;
    }
    const char *db_dir = argv[1];
    uint64_t keyspace  = (uint64_t)atoll(argv[2]);
    size_t value_size  = (size_t)atoll(argv[3]);
    int read_pct       = atoi(argv[4]);
    int nthreads       = atoi(argv[5]);
    int settle_secs    = atoi(argv[6]);
    int run_secs       = atoi(argv[7]);
    uint64_t seed      = (uint64_t)atoll(argv[8]);
    std::string rdir   = argv[9];
    bool skip_populate = (argc > 10 && atoi(argv[10]) == 1);

    char *value = (char *)malloc(value_size);
    fill_value(value, value_size, 0xF1A5CACE);

    /* ---- options per sign-off ---- */
    Options opt;
    opt.create_if_missing = true;
    opt.num_levels = 4;
    opt.compaction_style = ROCKSDB_NAMESPACE::kCompactionStyleLevel;
    opt.write_buffer_size = 2ull * 1024 * 1024 * 1024;      /* 2 GiB memtable */
    opt.max_write_buffer_number = 2;
    opt.max_background_jobs = 4;
    opt.compression = ROCKSDB_NAMESPACE::kNoCompression;
    opt.use_direct_reads = true;
    opt.use_direct_io_for_flush_and_compaction = true;
    opt.statistics = ROCKSDB_NAMESPACE::CreateDBStatistics();

    BlockBasedTableOptions bbt;
    bbt.block_cache = NewLRUCache(40ull * 1024 * 1024 * 1024); /* 40 GiB */
    bbt.filter_policy.reset(NewBloomFilterPolicy(10, false));
    bbt.cache_index_and_filter_blocks = true;
    bbt.cache_index_and_filter_blocks_with_high_priority = true;
    bbt.pin_l0_filter_and_index_blocks_in_cache = true;
    opt.table_factory.reset(NewBlockBasedTableFactory(bbt));

    std::unique_ptr<DB> db_owner;
    Status st = DB::Open(opt, db_dir, &db_owner);
    if (!st.ok()) { fprintf(stderr, "Open failed: %s\n", st.ToString().c_str()); return 1; }
    DB *db = db_owner.get();

    FILE *fstats = fopen((rdir + "/rocksdb_stats.txt").c_str(), "w");
    StatEmitter emit((rdir + "/persec.csv").c_str());

    /* ---- phase 1: populate (multi-threaded) ---- */
    if (!skip_populate) {
        printf("PHASE populate: %llu keys x %zub, %d threads\n",
               (unsigned long long)keyspace, value_size, nthreads);
        uint64_t t0 = now_us();
        std::atomic<uint64_t> done{0};
        std::vector<std::thread> ths;
        for (int t = 0; t < nthreads; t++) {
            ths.emplace_back([&, t]() {
                WriteOptions wo; wo.disableWAL = true;
                char key[BENCH_KEY_LEN + 1];
                uint64_t chunk = keyspace / (uint64_t)nthreads;
                uint64_t lo = (uint64_t)t * chunk;
                uint64_t hi = (t == nthreads - 1) ? keyspace : lo + chunk;
                for (uint64_t id = lo; id < hi; id++) {
                    size_t klen = make_key(key, id);
                    Status s = db->Put(wo, Slice(key, klen), Slice(value, value_size));
                    if (!s.ok()) { fprintf(stderr, "populate Put: %s\n", s.ToString().c_str()); exit(1); }
                    uint64_t d = done.fetch_add(1) + 1;
                    if (d % 5000000ull == 0) {
                        double el = (double)(now_us() - t0) / 1e6;
                        printf("  populate %lluM/%lluM (%.0f puts/s)\n",
                               (unsigned long long)(d / 1000000),
                               (unsigned long long)(keyspace / 1000000), (double)d / el);
                        fflush(stdout);
                    }
                }
            });
        }
        for (auto &th : ths) th.join();
        printf("PHASE populate done in %.1fs\n", (double)(now_us() - t0) / 1e6);
        dump_rocksdb_stats(db, fstats, 0);
    }

    /* ---- phase 2: settle (let compaction quiesce) ---- */
    printf("PHASE settle: %ds\n", settle_secs);
    std::this_thread::sleep_for(std::chrono::seconds(settle_secs));
    dump_rocksdb_stats(db, fstats, 0);

    /* ---- phase 3: measured ---- */
    printf("PHASE measured: %ds, read_pct=%d, threads=%d\n", run_secs, read_pct, nthreads);
    std::vector<ThreadResult> results(nthreads);
    std::atomic<uint64_t> a_reads{0}, a_writes{0}, a_hits{0}, a_misses{0};
    uint64_t start = now_us();
    std::vector<std::thread> ths;
    for (int t = 0; t < nthreads; t++)
        ths.emplace_back(worker, db, keyspace, value_size, read_pct, seed, t, value,
                         &results[t], &a_reads, &a_writes, &a_hits, &a_misses,
                         t == 0 ? &emit : nullptr);

    uint64_t end = start + (uint64_t)run_secs * 1000000ull;
    uint64_t next_dump = start + 60ull * 1000000ull;
    while (now_us() < end) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        emit.maybe_emit("measured", a_reads.load(), a_writes.load(),
                        a_hits.load(), a_misses.load());
        if (now_us() >= next_dump) {
            dump_rocksdb_stats(db, fstats, (now_us() - start) / 1000000ull);
            next_dump += 60ull * 1000000ull;
        }
    }
    g_stop.store(true);
    for (auto &th : ths) th.join();
    double measured = (double)(now_us() - start) / 1e6;

    /* merge per-thread results */
    LatHist rd_hit, rd_miss, wr;
    uint64_t reads = 0, writes = 0, hits = 0, misses = 0;
    for (auto &r : results) {
        rd_hit.merge(r.rd_hit); rd_miss.merge(r.rd_miss); wr.merge(r.wr);
        reads += r.reads; writes += r.writes; hits += r.hits; misses += r.misses;
    }
    emit.maybe_emit("measured", reads, writes, hits, misses, true);
    dump_rocksdb_stats(db, fstats, (uint64_t)measured);
    if (fstats) fclose(fstats);

    std::string live; db->GetProperty("rocksdb.estimate-live-data-size", &live);
    std::string sst;  db->GetProperty("rocksdb.total-sst-files-size", &sst);
    char extra[256];
    snprintf(extra, sizeof(extra),
             "\"rocksdb_live_data_bytes\": %s, \"rocksdb_sst_total_bytes\": %s, \"threads\": %d",
             live.empty() ? "0" : live.c_str(), sst.empty() ? "0" : sst.c_str(), nthreads);
    write_summary_json((rdir + "/summary.json").c_str(), "rocksdb",
                       keyspace, value_size, read_pct,
                       reads, writes, hits, misses, measured,
                       rd_hit, rd_miss, wr, extra);
    printf("DONE. summary at %s/summary.json\n", rdir.c_str());

    db_owner.reset();
    free(value);
    return 0;
}
