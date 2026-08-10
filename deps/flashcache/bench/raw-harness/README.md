# Raw Storage Engine Benchmark Harness: FlashCache vs RocksDB

This is the harness behind the FlashCache vs RocksDB raw storage comparison. It benchmarks
the two storage libraries directly, with no Valkey engine and no serialization layer, so the
numbers isolate the storage architecture: an async log structured cache store (FlashCache)
against a synchronous LSM tree (RocksDB).

Both binaries share `bench_common.h` (workload generator, latency histograms, per second
stat emitter), so traffic and reporting are identical by construction.

## Files

| File | What it is |
|---|---|
| `bench_common.h` | Shared uniform random workload generator, HDR style latency histograms, per second CSV emitter, summary.json writer |
| `fc_bench.cpp` | FlashCache benchmark. Single driver thread, async reads via completion callback, sync puts with throttle retry, cron pumped every iteration |
| `rocksdb_bench.cpp` | RocksDB benchmark. N worker threads, blocking Get/Put, WAL off, direct IO, leveled compaction, no compression |
| `run_bench.sh` | Host side wrapper: starts iostat, pidstat and memory collectors, runs the binary, stores everything under a per run results dir |
| `driver_fc_idx256m.sh` | Example driver: full read/write ratio matrix (100:0, 95:5, 80:20, 50:50, 0:100) for FC |
| `driver_rocks_c40g.sh` | Example driver: same matrix for RocksDB |

## Building

FlashCache is vendored in this repo at `deps/flashcache`. Build it first:

```bash
cd deps/flashcache/build && cmake .. && make -j
```

Then build the FC bench (from the repo root):

```bash
g++ -O2 -std=c++20 -o fc_bench deps/flashcache/bench/raw-harness/fc_bench.cpp \
    -I deps/flashcache -I deps/flashcache/src \
    -L deps/flashcache/build -lflashcache \
    -ljemalloc -laio -lpthread -ldl
```

RocksDB is not vendored. Build v11.x from source (`make static_lib`), then:

```bash
g++ -O2 -std=c++20 -o rocksdb_bench deps/flashcache/bench/raw-harness/rocksdb_bench.cpp \
    -I <rocksdb>/include -L <rocksdb> -lrocksdb \
    -lz -lbz2 -llz4 -lzstd -lsnappy -lpthread -ldl
```

## Running

Both binaries take positional args. Run through `run_bench.sh` to get collectors for free:

```bash
# FC: <db_file> <db_size_gb> <keyspace> <value_size> <read_pct> <settle_s> <run_s> <seed> <results_dir> [skip_populate] [queue_depth]
./run_bench.sh my_fc_run nvme1n1 ./fc_bench /mnt/nvme/fc.db 600 200000000 1024 80 300 900 42 /mnt/nvme/results/my_fc_run 0 128

# RocksDB: <db_dir> <keyspace> <value_size> <read_pct> <threads> <settle_s> <run_s> <seed> <results_dir> [skip_populate]
./run_bench.sh my_rocks_run nvme1n1 ./rocksdb_bench /mnt/nvme/rocks.db 200000000 1024 80 64 300 900 42 /mnt/nvme/results/my_rocks_run 0
```

Each run dir gets: `summary.json` (final latency histograms and throughput), `persec.csv`
(1 s TPS and interval latency), `iostat.log.gz`, `pidstat.log.gz` (per thread CPU),
`mem.log`, `run_info.txt`, plus `fc_metrics.jsonl` or `rocksdb_stats.txt` (engine
internals every 60 s).

## Reference setup used in the published study

- Instance: r7gd.4xlarge (16 vCPU Graviton3, 123 GB RAM, ~885 GB local NVMe), one instance per engine
- Dataset: 200M keys x 1 KB (about 210 GB), uniform random access, seed 42
- FC: 600 GB log file, index pre sized to 256M buckets, 2 GiB staging buffer, queue depth 128, fresh populate per run
- RocksDB: 40 GiB block cache (index and filter pinned, bloom 10 bits), 2 GiB memtable, 64 threads, DB reused across ratios
- Phases per run: populate, settle 300 s, measured 900 s

## Known caveats

- FC reads are destructive in this harness (FC_READ deletes the item, no re-put). Read
  heavy runs drain the keyspace during the measured window, so count disk served reads
  (hits) rather than submitted reads. Misses complete in about 10 us at the index.
- The FC internal in flight read capacity must stay at 128 or higher (an internal snapshot
  assertion requires at least 126). Client visible queue depth is gated in the submit loop
  via the `queue_depth` arg instead.
- fc_bench is single threaded by design (FC's usage model inside the engine). Its pure
  write throughput is bounded by that one thread, not the disk.
- Values are incompressible random bytes and RocksDB compression is disabled, matching FC
  which does not compress.
