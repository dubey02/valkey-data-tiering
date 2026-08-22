# Raw Storage Engine Benchmark: FlashCache vs RocksDB

Purpose: measure the two storage libraries directly (no Valkey engine, no serialization layer)
to quantify the architectural difference between an async log structured cache store
(FlashCache) and a synchronous LSM tree (RocksDB) for the data tiering use case.

Date: 2026-08-06. Harness source listed in the Appendix.

## 1. Executive Summary

FlashCache and RocksDB pay the same price for a single disk read (about 130 to 140
microseconds on this NVMe). Everything else is architecture:

1. FlashCache drives the disk from one thread using async IO (libaio). RocksDB needs one
   blocking thread per in flight read, so its throughput is a function of thread count.
2. RocksDB wins pure read only throughput (266K vs 179K reads/s). FlashCache wins every
   workload that contains writes, growing from 2x at 20% writes to 14.5x at pure writes.
3. The reason is amplification. RocksDB compaction consumes up to 26x write and 30x read
   amplification, saturating the disk with its own traffic. FlashCache holds 1.4 to 1.7x
   write amplification at every ratio.
4. At the 4 to 8 IO threads a real engine integration would grant, RocksDB delivers a
   quarter to a sixth of FlashCache throughput at mixed ratios.

Workload context matters: a tiering backend always has a write stream (spilling is
writing), so the mixed ratio columns are the ones that represent the target use case.

## 2. Test Setup

| Parameter | Value |
|-----------|-------|
| Hosts | 2x r7gd.4xlarge (16 vCPU Graviton3, 128 GB RAM, 885 GB local NVMe), eu-west-1c |
| Disk parity | fio verified: 44.9K vs 44.8K mixed 4K IOPS, within 0.3% |
| Dataset | 200M keys, 16 B keys, 1 KB values, about 200 GB logical, uniform random access |
| Duration | Every run: populate, 300 s settle, 900 s measured |
| Traffic generator | Custom C++ harness linked directly against each library, shared workload generator (same RNG, same key format, same op mix) |
| FlashCache | this repository's FlashCache build, libaio, single driver thread, max 128 in flight reads, GC always on (1 MiB blocks), 1 GB buffered write staging, 600 GB log file, fresh populate every run |
| RocksDB | v11.8.0, leveled compaction, 4 levels, 1 GiB memtable, 8 GiB block cache with pinned index and filter blocks, bloom 10 bits/key, WAL off, direct IO, no compression, 4 background jobs, store reused across runs |
| Metrics | Harness latency histograms (10 us resolution), iostat 1 s, pidstat per thread 1 s, rocksdb.stats and FlashCache counters every 60 s |

Semantics note: FlashCache reads are destructive (this version has no non destructive get).
No re-insert was performed, so FlashCache stores drain during read heavy runs. Misses cost
about 10 us (index lookup only) and inflate the client read number. The "disk served
reads/s" figure is therefore the comparison column used throughout. RocksDB reads never miss.

## 3. Comparison at 4 RocksDB threads

Four threads approximates the IO thread budget a real engine integration grants a storage
backend. FlashCache always runs its native model: one driver thread. This is the most
integration realistic comparison in the study.

### 80:20 read:write

| Metric | FlashCache (1 thread) | RocksDB (4 threads) |
|--------|----------------------|---------------------|
| Client read TPS | 253,034 (65% hit) | 26,226 (100% hit) |
| Disk served reads/s | 164,000 | 26,226 |
| Client write TPS | 63,261 | 6,562 |
| Read latency p50 / p99 (us) | 130 / 350 | 140 / 640 |
| Write latency p50 / p99 (us) | 10 / 10 | 10 / 20 |
| Disk read | 168K r/s, 877 MB/s | 27.7K r/s, 331 MB/s |
| Disk write | 849 w/s, 95 MB/s | 1,770 w/s, 216 MB/s |
| Disk utilization | 99% | 100% |
| CPU cores used (of 16) | 0.9 | 0.9 |

FlashCache serves 6.3x the disk reads and 9.6x the writes at equal CPU. Note the disk
write column: RocksDB is already burning 216 MB/s of compaction bandwidth to absorb only
6.6 MB/s of client writes (about 33x write amplification), and that compaction competes
with client reads for the same device.

### 50:50 read:write

| Metric | FlashCache (1 thread) | RocksDB (4 threads) |
|--------|----------------------|---------------------|
| Client read TPS | 146,114 (81% hit) | 17,935 |
| Disk served reads/s | 119,000 | 17,935 |
| Client write TPS | 146,132 | 17,936 |
| Read latency p50 / p99 (us) | 120 / 420 | 170 / 820 |
| Write latency p50 / p99 (us) | 10 / 10 | 10 / 850 |
| Disk read | 121K r/s, 719 MB/s | 22.5K r/s, 636 MB/s |
| Disk write | 2,202 w/s, 246 MB/s | 4,555 w/s, 558 MB/s |
| Disk utilization | 96% | 100% |
| CPU cores used | 0.9 | 1.1 |

At 50% writes RocksDB is fully compaction bound: 558 MB/s of disk writes to absorb
17.9 MB/s of client writes. FlashCache sustains 8.1x the writes and 6.6x the disk served
reads.

## 4. Comparison at 8 RocksDB threads

### 80:20 read:write

| Metric | FlashCache (1 thread) | RocksDB (8 threads) |
|--------|----------------------|---------------------|
| Client read TPS | 253,034 (65% hit) | 46,793 |
| Disk served reads/s | 164,000 | 46,793 |
| Client write TPS | 63,261 | 11,700 |
| Read latency p50 / p99 (us) | 130 / 350 | 150 / 790 |
| Write latency p50 / p99 (us) | 10 / 10 | 10 / 20 |
| Disk read | 168K r/s, 877 MB/s | 49.6K r/s, 613 MB/s |
| Disk write | 849 w/s, 95 MB/s | 3,353 w/s, 408 MB/s |
| Disk utilization | 99% | 100% |
| CPU cores used | 0.9 | 1.5 |

Doubling RocksDB threads from 4 to 8 delivers 1.8x reads (near linear, reads are still
concurrency starved), but compaction write bandwidth also doubles to 408 MB/s.

### 50:50 read:write

| Metric | FlashCache (1 thread) | RocksDB (8 threads) |
|--------|----------------------|---------------------|
| Client read TPS | 146,114 (81% hit) | 20,113 |
| Client write TPS | 146,132 | 20,110 |
| Read latency p50 / p99 (us) | 120 / 420 | 170 / 830 |
| Write latency p50 / p99 (us) | 10 / 10 | 10 / 1,100 |
| Disk read | 121K r/s, 719 MB/s | 24.6K r/s, 654 MB/s |
| Disk write | 2,202 w/s, 246 MB/s | 4,655 w/s, 568 MB/s |
| Disk utilization | 96% | 100% |
| CPU cores used | 0.9 | 1.3 |

At 50:50 the extra threads buy almost nothing (17.9K to 20.1K). The bottleneck is
compaction bandwidth, not reader concurrency.

## 5. Comparison at 64 RocksDB threads

Sixty four threads is RocksDB at its architectural best on this box, found by calibration
(the largest thread count whose read only p99 stayed under 500 us). No engine integration
would realistically grant this many threads per shard. Included to show RocksDB's ceiling.

### All five ratios, throughput

| Read:Write | FC reads (disk served) | FC writes | Rocks reads | Rocks writes |
|-----------|----------------------:|----------:|------------:|-------------:|
| 100:0 | 179K | 0 | 266K | 0 |
| 95:5 | 179K | 18K | 193K | 10K |
| 80:20 | 164K | 63K | 87K | 22K |
| 50:50 | 119K | 146K | 22K | 22K |
| 0:100 | 0 | 322K | 0 | 22K |

### All five ratios, latency (us, p50 / p99)

| Read:Write | FC read | Rocks read | FC write | Rocks write |
|-----------|---------|-----------|----------|-------------|
| 100:0 | 130 / 250 | 240 / 490 | n/a | n/a |
| 95:5 | 130 / 270 | 250 / 1,800 | 10 / 10 | 10 / 30 |
| 80:20 | 130 / 350 | 220 / 1,300 | 10 / 10 | 2,100 / 5,500 |
| 50:50 | 120 / 420 | 210 / 900 | 10 / 10 | 3,400 / 6,600 |
| 0:100 | n/a | n/a | 10 / 10 (p100 3.5 ms) | 3,400 / 6,700 (p100 3.89 s) |

### All five ratios, disk and CPU

| Read:Write | FC disk r/s, rMB/s | FC disk w/s, wMB/s | FC util | FC cores | Rocks disk r/s, rMB/s | Rocks disk w/s, wMB/s | Rocks util | Rocks cores |
|-----------|-------------------|--------------------|--------:|---------:|----------------------|----------------------|-----------:|------------:|
| 100:0 | 184K, 912 | 18, 2 | 100% | 0.9 | 257K, 1,151 | 1, 0 | 100% | 5.2 |
| 95:5 | 185K, 923 | 225, 25 | 100% | 0.9 | 191K, 1,175 | 2,786, 334 | 100% | 5.4 |
| 80:20 | 168K, 877 | 849, 95 | 99% | 0.9 | 89K, 929 | 4,564, 556 | 100% | 3.5 |
| 50:50 | 121K, 719 | 2,202, 246 | 96% | 0.9 | 26K, 667 | 4,779, 577 | 100% | 2.0 |
| 0:100 | 2.7K, 329 (GC) | 4,035, 460 | 38% | 0.9 | 4.9K, 576 | 4,799, 577 | 100% | 1.2 |

Highlights at 64 threads:

1. Read only: RocksDB wins throughput (266K vs 179K). FlashCache hit its own 128 in
   flight software cap, not the device (RocksDB pushed the disk to 1,151 MB/s vs 912).
   FlashCache still holds 2x better latency (130 vs 240 us p50).
2. The crossover sits at about 5% writes. At 95:5 the two tie on disk reads, but RocksDB
   read p99 already degrades 3.7x (490 to 1,800 us) because compaction has arrived.
3. RocksDB client writes plateau at about 22K/s from 20% writes onward while the disk
   carries a constant 577 MB/s of compaction traffic. The delayed write throttle (measured
   at 16 MB/s in rocksdb.stats) pins client write p50 at 2.1 to 3.4 ms, with a captured
   3.89 second p100 stall at pure writes.
4. FlashCache at pure writes is CPU bound, not disk bound: 322K writes/s from one thread
   at 89% CPU with the disk at only 38% utilization. Its write ceiling on this box is the
   single submit thread, and there is device headroom behind it.

## 6. RocksDB thread scaling summary

| Threads | 80:20 reads | 80:20 writes | 50:50 reads | Read p99 trend (80:20) |
|--------:|-----------:|-------------:|------------:|------------------------|
| 4 | 26.2K | 6.6K | 17.9K | 640 us |
| 8 | 46.8K | 11.7K | 20.1K | 790 us |
| 64 | 86.8K | 21.7K | 22.1K | 1,300 us |
| FC (1) | 164K | 63.3K | 119K | 350 us |

Two structural observations:

1. RocksDB threads are IO concurrency tokens, not compute. Each blocking Get parks a
   thread in the kernel for about 95% of its life. Sixty four threads consume only 2 to 5
   CPU cores. The cost of high thread counts is engineering (context switches, stacks,
   scheduler pressure inside an engine), not CPU.
2. The fast RocksDB writes at 4 to 8 threads (10 us p50) are a symptom of low throughput,
   not headroom. Small pools self throttle offered write load below the compaction
   ceiling. Push real load (64 threads) and the delayed write throttle surfaces at
   millisecond scale. RocksDB writes can be fast or frequent, not both.

## 7. Amplification

Disk bytes moved per logical client byte, measured phase:

| Read:Write | FC Read Amp | FC Write Amp | Rocks Read Amp | Rocks Write Amp |
|-----------|------------:|-------------:|---------------:|----------------:|
| 100:0 | 5.1x | n/a | 4.5x | n/a |
| 95:5 | 5.2x | 1.4x | 6.2x | 34x |
| 80:20 | 5.3x | 1.5x | 10.7x | 25.6x |
| 50:50 | 6.0x | 1.7x | 30x | 26x |
| 0:100 | n/a | 1.4x | 26x (reads for compaction) | 26x |

FlashCache read amplification is page alignment (4K pages to fetch 1 KB items). RocksDB
read amplification is level walks plus compaction reads. Both disks run at 100%
utilization at mixed ratios, but FlashCache converts about twice the fraction of its disk
bandwidth into client bytes.

These numbers reproduce an earlier engine level study (RocksDB write amplification about
28 and read amplification about 38, versus FlashCache about 1 and about 10 on
r5d.2xlarge). The gap is architectural, not integration overhead.

## 8. Findings

1. The write path is not a contest. Log append with GC beats LSM compaction 14.5x at pure
   writes with 340x better median latency and 18x less write amplification. No RocksDB
   tuning closes a structural gap.
2. RocksDB wins pure read only throughput, and the reason is clear on both sides:
   it generated more in flight IO (64 threads) than FlashCache's 128 in flight software
   cap allows, and compaction was silent because nothing was written.
3. Every mixed workload belongs to FlashCache, and tiering is always mixed (spilling is
   writing). The gap grows monotonically with write share: roughly par at 5% writes, 2x at
   20%, 6.6x at 50%, 14.5x at 100%.
4. Latency character differs. FlashCache read latency is flat (p50 120 to 130 us, p99 250
   to 420 us at every ratio). RocksDB latency is workload coupled: excellent when idle,
   millisecond scale tails whenever compaction shares the disk.
5. This is not a feature parity comparison. RocksDB's amplification buys crash recovery,
   snapshots, iterators, range scans, and transactions. FlashCache is a purpose built
   cache log that does not need them. The correct conclusion is that RocksDB pays for
   generality the tiering use case does not use.

## 9. Caveats and limitations

1. FlashCache reads are destructive in this version, and no re-insert was done. Stores
   drain during read heavy runs (hit rate 49% at 100:0, 65% at 80:20, 81% at 50:50).
   Misses are cheap index lookups, which is why the disk served read column is used for
   all comparisons. Because of the drain, full run averages understate steady state read
   throughput by 14 to 45% depending on ratio (at 80:20 the measured steady state was
   about 195K disk served reads/s versus the 164K full run average). All FlashCache read
   cells in this report are therefore conservative lower bounds.
2. FlashCache's read ceiling here is its 128 in flight cap, a software constant, not the
   hardware. A raised cap run was not performed.
3. FlashCache pure write throughput is bounded by its single driver thread (89% CPU, disk
   38% busy). A second submit thread was not tested.
4. RocksDB stores were reused across runs, carrying realistic compaction debt. A fresh
   store would look somewhat better at mixed ratios.
5. The fio device baseline used the psync engine (sync ceiling about 45K IOPS). Both
   stores exceed it with async or multi threaded IO. An async libaio baseline was not
   re-run.
6. Values were incompressible random bytes and RocksDB compression was disabled, matching
   FlashCache which does not compress. Compressible data would shrink RocksDB's disk
   footprint and amplification.
7. Single value size (1 KB) and uniform random access only. Value size sweeps and skewed
   access were out of scope for this round.

## 10. Appendix

RocksDB reader thread calibration (read only, 60 s probes): 4 threads 31K, 8 threads 60K,
16 threads 107K, 24 threads 144K, 32 threads 176K, 48 threads 228K, 64 threads 264K
reads/s, with p99 rising 210 to 480 us. Chosen: 64 (largest under the 500 us p99 rule).

Harness source: `deps/flashcache/bench/raw-harness/` on the `raw-storage-bench` branch of
this repository (shared workload and stats in `bench_common.h`, `fc_bench.cpp`,
`rocksdb_bench.cpp`, `run_bench.sh` collector wrapper). Per run raw artifacts (per second
TPS and latency CSV, iostat, pidstat, rocksdb.stats and FlashCache counter dumps) are
available on request.
