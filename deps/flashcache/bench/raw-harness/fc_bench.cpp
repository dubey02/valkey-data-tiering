/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */
/* fc_bench.cpp — raw FlashCache benchmark with uniform-random fixed-keyspace workload.
 * Single driver thread (FC's usage model): async reads via completion callback
 * (<=128 in-flight), sync puts with throttle-retry, cron pumped every iteration.
 *
 * Usage:
 *   fc_bench <db_file> <db_size_gb> <keyspace> <value_size> <read_pct>
 *            <settle_secs> <run_secs> <seed> <results_dir> [skip_populate]
 *
 * NOTE: FC reads are destructive (FC_READ deletes the item). No re-put is done.
 * Misses (value==NULL) are counted separately with their own latency histogram.
 */
#include "bench_common.h"

#include <cstdarg>
#include <fcntl.h>
#include <unistd.h>

extern "C" {
#include "include/flashcache.h"
}

#define MAX_INFLIGHT_READS 128
#define DBID 0

/* runtime-configurable in-flight read cap (arg 12), default MAX_INFLIGHT_READS */
static size_t g_max_inflight = MAX_INFLIGHT_READS;

/* ---- globals shared with callbacks ---- */
static size_t g_inflight = 0;
static uint64_t g_reads_done = 0, g_hits = 0, g_misses = 0;
static LatHist g_rd_hit, g_rd_miss;          /* cumulative (measured phase) */
static StatEmitter *g_emit = nullptr;
static bool g_measured_phase = false;

struct ReadCtx { uint64_t start_us; };

static void get_cb(void *ctx_v, char *value, size_t value_len, int add_to_rdb) {
    (void)add_to_rdb;
    ReadCtx *ctx = (ReadCtx *)ctx_v;
    uint64_t lat = now_us() - ctx->start_us;
    g_inflight--;
    g_reads_done++;
    bool hit = (value != NULL && value_len > 0);
    if (hit) g_hits++; else g_misses++;
    if (g_measured_phase) {
        if (hit) g_rd_hit.record(lat); else g_rd_miss.record(lat);
        if (g_emit) { if (hit) g_emit->rd_int.record(lat); }
    }
    delete ctx;
}

static void evict_cb(void *ctx, uint32_t dbid, char *key, size_t key_len) {
    (void)ctx; (void)dbid; (void)key; (void)key_len;
}

static void fc_logger(int level, const char *fmt, ...) {
    if (level < FC_LL_WARNING) return;   /* warnings and worse only */
    /* FC warns per read-miss; sample to avoid GB of logs in drain scenarios */
    static uint64_t n = 0;
    n++;
    if (n > 20 && (n % 1000000ull) != 0) return;
    va_list ap; va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, " [warn #%llu]\n", (unsigned long long)n);
    va_end(ap);
}

static int g_asio_ctx = 0;
static void asio_cb(void *ctx) { (void)ctx; }

/* dump FC counter metrics as a json-ish snapshot line */
static void dump_fc_metrics(FILE *f, uint64_t sec) {
    if (!f) return;
    fprintf(f,
        "{\"sec\":%llu,\"num_items\":%zu,\"disk_read_bytes\":%zu,\"disk_write_bytes\":%zu,"
        "\"gc_read_bytes\":%zu,\"gc_write_bytes\":%zu,\"gc_rate_bps\":%zu,"
        "\"active_mem_bytes\":%zu,\"active_db_bytes\":%zu,\"allocated_db_bytes\":%zu,"
        "\"items_evicted\":%zu,\"read_reqs\":%zu,\"delete_reqs\":%zu}\n",
        (unsigned long long)sec,
        flashcacheGetCountBasedMetric(FC_NUM_ITEMS),
        flashcacheGetCountBasedMetric(FC_TOTAL_DISK_READ_BYTES),
        flashcacheGetCountBasedMetric(FC_TOTAL_DISK_WRITE_BYTES),
        flashcacheGetCountBasedMetric(FC_GARBAGE_COLLECTION_READ_BYTES),
        flashcacheGetCountBasedMetric(FC_GARBAGE_COLLECTION_WRITE_BYTES),
        flashcacheGetCountBasedMetric(FC_GARBAGE_COLLECTION_CURR_RATE_BYTES_PER_SECOND),
        flashcacheGetCountBasedMetric(FC_ACTIVE_MEMORY_SIZE),
        flashcacheGetCountBasedMetric(FC_ACTIVE_DB_SIZE_BYTES),
        flashcacheGetCountBasedMetric(FC_ALLOCATED_DB_SIZE_BYTES),
        flashcacheGetCountBasedMetric(FC_NUM_ITEMS_EVICTED),
        flashcacheGetCountBasedMetric(FC_NUM_READ_REQUEST),
        flashcacheGetCountBasedMetric(FC_NUM_DELETE_REQUEST));
    fflush(f);
}

int main(int argc, char **argv) {
    if (argc < 10) {
        fprintf(stderr, "usage: %s <db_file> <db_size_gb> <keyspace> <value_size> "
                        "<read_pct> <settle_secs> <run_secs> <seed> <results_dir> [skip_populate] [queue_depth]\n",
                argv[0]);
        return 1;
    }
    const char *db_file    = argv[1];
    size_t db_size         = (size_t)atoll(argv[2]) * 1024ull * 1024 * 1024;
    uint64_t keyspace      = (uint64_t)atoll(argv[3]);
    size_t value_size      = (size_t)atoll(argv[4]);
    int read_pct           = atoi(argv[5]);
    int settle_secs        = atoi(argv[6]);
    int run_secs           = atoi(argv[7]);
    uint64_t seed          = (uint64_t)atoll(argv[8]);
    std::string rdir       = argv[9];
    bool skip_populate     = (argc > 10 && atoi(argv[10]) == 1);
    if (argc > 11) g_max_inflight = (size_t)atoll(argv[11]);
    printf("queue depth (max inflight reads): %zu\n", g_max_inflight);

    char *value = (char *)malloc(value_size);
    fill_value(value, value_size, 0xF1A5CACE);

    /* ---- pre-allocate the DB file (FC asserts file size >= db_size) ---- */
    {
        int fd = open(db_file, O_RDWR | O_CREAT, 0644);
        if (fd < 0) { perror("open db_file"); return 1; }
        if (posix_fallocate(fd, 0, (off_t)db_size) != 0) {
            fprintf(stderr, "posix_fallocate(%zu) failed\n", db_size);
            return 1;
        }
        close(fd);
    }

    /* ---- init FC (same params as stress_test_app) ---- */
    static int ev_ctx = 0;
    flashcacheEvictionDetails ev = {};
    ev.context = (void *)&ev_ctx;
    ev.callback = evict_cb;
    flashcacheAsioControlMsgCallbackDetails asio_details = { (void *)&g_asio_ctx, asio_cb };
    /* index pre-sized to 256M buckets (2 GiB array): load factor 0.78 at 200M keys,
     * no growth-rehash during populate/run (also avoids growth+GC collision bug) */
    if (flashcacheInit(db_file, db_size, 256ull * 1024 * 1024, 1,
                       /*max_allocated_db_size_percent=*/50,
                       MAX_INFLIGHT_READS, /* FC internal capacity: must be >=126 (snapshot fio assert); client concurrency gated by g_max_inflight in the submit loop */
                       /*min_gc_rate=*/4096,
                       /*evict_under_max_logsize_time_limit=*/100 * 1000,
                       /*optimized_delete=*/1,
                       now_us, &ev, fc_logger, &asio_details) != FC_OK) {
        fprintf(stderr, "flashcacheInit failed\n");
        return 1;
    }
    /* write-staging parity knob: 2 GiB buffered writes (= RocksDB memtable) */
    flashcacheConfig cfg;
    cfg.key = FC_CONFIG_KEY_MAX_BUFFERED_WRITE_SIZE_BYTES;
    cfg.numeric_value = 2ll * 1024 * 1024 * 1024;
    flashcacheSetConfig(&cfg);

    FILE *fstats = fopen((rdir + "/fc_metrics.jsonl").c_str(), "w");
    StatEmitter emit((rdir + "/persec.csv").c_str());
    g_emit = &emit;

    char key[BENCH_KEY_LEN + 1];

    /* ---- phase 1: populate ---- */
    if (!skip_populate) {
        printf("PHASE populate: %llu keys x %zub\n", (unsigned long long)keyspace, value_size);
        uint64_t t0 = now_us();
        for (uint64_t id = 0; id < keyspace; id++) {
            size_t klen = make_key(key, id);
            while (flashcachePutItem(DBID, key, klen, value, value_size) != FC_OK)
                flashcacheRunCronTasks();
            if ((id & 0xFFF) == 0) flashcacheRunCronTasks();
            if (id && (id % 5000000ull) == 0) {
                double el = (double)(now_us() - t0) / 1e6;
                printf("  populate %lluM/%lluM (%.0f puts/s)\n",
                       (unsigned long long)(id / 1000000), (unsigned long long)(keyspace / 1000000),
                       (double)id / el);
                fflush(stdout);
            }
        }
        printf("PHASE populate done in %.1fs, items=%zu\n",
               (double)(now_us() - t0) / 1e6, flashcacheGetCountBasedMetric(FC_NUM_ITEMS));
        dump_fc_metrics(fstats, 0);
    }

    /* ---- phase 2: settle ---- */
    printf("PHASE settle: %ds\n", settle_secs);
    uint64_t settle_end = now_us() + (uint64_t)settle_secs * 1000000ull;
    while (now_us() < settle_end) flashcacheRunCronTasks();
    dump_fc_metrics(fstats, 0);

    /* ---- phase 3: measured ---- */
    printf("PHASE measured: %ds, read_pct=%d\n", run_secs, read_pct);
    Workload wl(keyspace, read_pct, seed);
    LatHist wr_hist;
    uint64_t writes_done = 0;
    uint64_t start = now_us();
    uint64_t end = start + (uint64_t)run_secs * 1000000ull;
    uint64_t next_metric_dump = start + 60ull * 1000000ull;
    g_measured_phase = true;

    while (now_us() < end) {
        if (wl.next_is_read()) {
            while (g_inflight >= g_max_inflight) flashcacheRunCronTasks();
            size_t klen = make_key(key, wl.next_key());
            ReadCtx *ctx = new ReadCtx{ now_us() };
            g_inflight++;
            if (flashcacheGetItem(DBID, key, klen, FC_READ, ctx, get_cb) != FC_OK) {
                /* throttled at submit: retry after cron */
                g_inflight--;
                delete ctx;
                flashcacheRunCronTasks();
                continue;
            }
        } else {
            size_t klen = make_key(key, wl.next_key());
            uint64_t t = now_us();
            while (flashcachePutItem(DBID, key, klen, value, value_size) != FC_OK)
                flashcacheRunCronTasks();
            uint64_t lat = now_us() - t;
            wr_hist.record(lat);
            emit.wr_int.record(lat);
            writes_done++;
        }
        flashcacheRunCronTasks();
        emit.maybe_emit("measured", g_reads_done, writes_done, g_hits, g_misses);
        uint64_t t = now_us();
        if (t >= next_metric_dump) {
            dump_fc_metrics(fstats, (t - start) / 1000000ull);
            next_metric_dump += 60ull * 1000000ull;
        }
    }
    /* drain */
    while (g_inflight > 0) flashcacheRunCronTasks();
    double measured = (double)(now_us() - start) / 1e6;
    g_measured_phase = false;

    emit.maybe_emit("measured", g_reads_done, writes_done, g_hits, g_misses, true);
    dump_fc_metrics(fstats, (uint64_t)measured);
    if (fstats) fclose(fstats);

    /* ---- final summary ---- */
    char extra[512];
    snprintf(extra, sizeof(extra),
             "\"fc_final_items\": %zu, \"fc_disk_read_bytes\": %zu, \"fc_disk_write_bytes\": %zu, "
             "\"fc_gc_write_bytes\": %zu, \"fc_active_mem_bytes\": %zu",
             flashcacheGetCountBasedMetric(FC_NUM_ITEMS),
             flashcacheGetCountBasedMetric(FC_TOTAL_DISK_READ_BYTES),
             flashcacheGetCountBasedMetric(FC_TOTAL_DISK_WRITE_BYTES),
             flashcacheGetCountBasedMetric(FC_GARBAGE_COLLECTION_WRITE_BYTES),
             flashcacheGetCountBasedMetric(FC_ACTIVE_MEMORY_SIZE));
    write_summary_json((rdir + "/summary.json").c_str(), "flashcache",
                       keyspace, value_size, read_pct,
                       g_reads_done, writes_done, g_hits, g_misses, measured,
                       g_rd_hit, g_rd_miss, wr_hist, extra);
    printf("DONE. summary at %s/summary.json\n", rdir.c_str());

    flashcacheTearDown();
    free(value);
    return 0;
}
