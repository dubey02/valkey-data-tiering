# Benchmarking Strategy

## Overview

Two complementary benchmark systems measure data-tiering performance at different scales:

1. **trace-replay** — Single-instance, controlled workload from real Twitter cache traces. Fast iteration (10-20s per run). Tests correctness and relative performance.
2. **ezbench** — Distributed, multi-client, production-like load on dedicated EC2 hardware. Tests absolute throughput, memory stability, and tail latency under sustained pressure.

---

## Trace-Replay Scenarios

**Instance:** r7gd.xlarge (or r7gd.8xlarge for NVMe)  
**Trace:** cluster52 Twitter cache trace (1M, 10M, or 100M ops)  
**Tool:** `benchmark/run-remote.sh` + `trace-replay` binary

### Cold-Start Replay

DB starts empty. Keys created on first access via `-set-on-miss`. Memory pressure builds organically. Spill/eviction policies have real LRU history.

**What it measures:** End-to-end throughput including key creation, spill policy quality, fetch rate.

| Scenario | maxmemory | Module | Promotion | Notes |
|----------|-----------|--------|-----------|-------|
| baseline | 32mb | none | — | Evicts keys when full (data loss) |
| tiering-always | 32mb | tiering module | always | Spills to NVMe, promotes on fetch |
| tiering-never | 32mb | tiering module | never | Spills to NVMe, transient serve only |
| inmemory | 32mb | tiering_mem.so | always | RAM backend (measures interface overhead) |

### Warm-Start Replay

All keys populated first (maxmemory 0). Then maxmemory lowered to force spilling. Replay is pure GETs — measures fetch overhead vs all-in-RAM ceiling.

**What it measures:** Pure read-path fetch cost. Baseline is the throughput ceiling (100% RAM hits).

| Scenario | maxmemory (populate) | maxmemory (replay) | Module | Promotion |
|----------|---------------------|-------------------|--------|-----------|
| baseline | 0 | 0 | none | — |
| tiering-always | 0 | 32mb | tiering module | always |
| tiering-never | 0 | 32mb | tiering module | never |

### Key Configuration

```
maxmemory-policy allkeys-lru
tiering-spill-policy allkeys-truelru
tiering-spill-batch-size 2
tiering-spill-threshold 100
tiering-key-spill-enabled yes
```

### Metrics Collected

- Throughput (ops/s) with checkpoints every 50-250K ops
- Latency percentiles (p50, p99, p99.9)
- Hit ratio
- used_memory / used_memory_rss
- tiered_spills, tiered_fetches_requested, tiered_fetches_promoted
- tiered_respills, tiered_write_through
- tiering_keys_on_disk
- Disk IOPS (read/write) via iostat
- Keys retained in RAM (db0:keys)
- evicted_keys (should be 0 for tiering scenarios)

---

## ezbench Scenarios

**Instance:** r6gd.2xlarge (NVMe local disk)  
**Workload:** Synthetic hot/cold Zipfian (80/20 split)  
**Tool:** ezbench framework (ElmoHydra)

### Configuration

- 10M keyspace, 500B values
- 2M hot keys (80% traffic), 8M cold keys (20% traffic)
- 2 SET machines × (2 cold + 8 hot connections)
- 8 GET machines × (2 cold + 8 hot connections)
- 5M requests per process
- Warmup: 3M keys
- maxmemory: 2GB

### Scenarios

| Scenario | Directory | Module | Key Config |
|----------|-----------|--------|-----------|
| baseline | perf/baseline/ | none | maxmemory-policy allkeys-lru |
| tiering-always | perf/storage/ | tiering module | promotion-policy always |
| tiering-never | perf/tiering-never/ | tiering module | promotion-policy never |
| storage-preload | perf/storage-preload/ | tiering module | cold-start trace replay |

### What ezbench Measures

- Sustained TPS over 20-60 minutes
- Memory stability (used_memory drift)
- Tail latency (p99, p100) under load
- Miss rate convergence
- Disk I/O patterns (read/write IOPS, throughput)

---

## Promotion Policy Matrix

| Policy | Behavior | Best For |
|--------|----------|----------|
| always | Promote to RAM on every flash hit | Zipfian workloads (hot keys stay hot) |
| never | Serve from transient dict, never promote | Write-heavy, scan-heavy, or memory-constrained |

### Expected Performance Characteristics

**Cold-Start (Zipfian trace, 32MB):**
- always-promote: ~55K ops/s (high spill:fetch ratio, hot keys stay in RAM)
- never-promote: Should approach always-promote (hot keys in RAM, cold served transiently)

**Warm-Start (all keys on flash):**
- always-promote: ~85K ops/s on NVMe (promotes hot keys, stabilizes)
- never-promote: IO-thread-bound (every access requires flash read)

---

## Hardware Requirements

| Component | trace-replay | ezbench |
|-----------|-------------|---------|
| Instance | r7gd.xlarge+ | r6gd.2xlarge |
| NVMe | Required for storage backend | Required |
| RAM | 8GB+ | 32GB+ |
| Disk | Local NVMe (NOT EBS) | Local NVMe |

**Critical:** storage backend MUST run on local NVMe instance store (the "d" suffix: r7g**d**, r6g**d**, i3). EBS gives 5-11ms read latency → 3K reads/s → 75% of clients blocked. Local NVMe gives ~100µs → 30-50K reads/s.

---

## Running Benchmarks

### trace-replay (quick iteration)

```bash
# Build
make -j$(nproc) -C src
gcc -shared -fPIC -O2 -D_GNU_SOURCE -Istorage backend/src -Isrc \
    -o tiering module src/tiering_storage_module.c \
    -Lstorage backend/build -lstorage -l:libaio.so.1 -lpthread

# Deploy + run
bash benchmark/deploy.sh
bash benchmark/run-remote.sh cluster52_1m.csv 200 32mb coldstart
```

### ezbench (sustained load)

```bash
export PATH="/local/apollo/package/local_1/AL2_aarch64/ElmoHydra/ElmoHydra-1064.0-0/bin:$PATH"
export AWS_SHARED_CREDENTIALS_FILE=/apollo/var/turtle/ezbench/credentials

cd perf/storage
sed -i '/^stack_name/d' test.ini
ezbench --run --wait --log-level DEBUG > /tmp/ezbench.log 2>&1 &
```

---

## Reference Results

### trace-replay: r7gd.8xlarge, 1M trace, 32MB, 200 clients, Cold-Start

| Metric | Baseline | FC Always | FC Never |
|--------|----------|-----------|----------|
| Throughput | 133K | 55K | TBD (fix in progress) |
| % of Baseline | 100% | 41% | — |
| Keys in RAM | 255K | 42K | — |
| Evicted | 0 | 0 | 0 |
| Spills | 0 | 261K | — |
| Keys on disk | 0 | 261K | — |

### ezbench: r6gd.2xlarge, 10M keys, 2GB, storage backend

| Metric | Baseline | storage backend |
|--------|----------|-----------|
| Best TPS | 142K | 55K |
| Avg TPS | ~143K | ~44K |
| p50 | 0.16ms | 0.79ms |
| p99 | 0.96ms | 3.0ms |
| Memory | stable | stable at 2GB |
| Miss rate | 5.9% | 31.7% |

---

## Benchmark Scenario Matrix

### Fetch Microbenchmarks

| # | Scenario | Method | Dimensions | Measures |
|---|----------|--------|-----------|----------|
| F2 | Worst-case fetch throughput | Flood server with GETs for keys guaranteed on flash (sequential spill then sequential GET) | value size, value type, promotion on/off, at-maxmemory vs below | Max sustained fetch rate, p99/p99.9 under saturation |
| F3 | Multi-type command latency | LRANGE/HGETALL/ZRANGEBYSCORE on spilled complex objects | object size (100–10K elements), type, promoted vs transient | Deserialization cost, head-of-line blocking in IO thread |

### Spill Microbenchmarks

| # | Scenario | Method | Dimensions | Measures |
|---|----------|--------|-----------|----------|
| S2 | Max spill rate | Continuous spills via forced eviction or direct backend driver | value size, batch size (1/2/16), backend type | Spills/s ceiling, serialization cost |
| S3 | Memory convergence speed | Burst 10x keyspace from 50% full steady state | spill-batch-size, active-spill frequency, backend write throughput | Time-to-stable, overshoot above maxmemory |

### Workload Scenarios

| # | Scenario | Method | Dimensions | Measures |
|---|----------|--------|-----------|----------|
| W1 | Cold-start replay | trace-replay with -set-on-miss on empty DB | trace (Twitter 1M/10M/100M, synthetic Zipfian), maxmemory, clients | Throughput ramp, spill:fetch ratio, hit ratio |
| W2 | Warm-start replay | Populate all keys, lower maxmemory, replay GETs | same as W1 | Pure fetch overhead vs RAM ceiling |
| W3 | Mixed read/write under pressure | Steady state at maxmemory, continuous SETs + GETs | SET:GET ratio (10:90, 50:50, 90:10), promotion policy, value sizes | Write/read IO queue priority, spill starvation |

### Scaling & Saturation

| # | Scenario | Method | Dimensions | Measures |
|---|----------|--------|-----------|----------|
| C1 | Client scaling | Fixed 50% hit-rate workload, vary clients 1→1000 | client count, promotion policy | Throughput curve, latency percentiles, IO thread saturation point |

### Correctness & Durability

| # | Scenario | Method | Dimensions | Measures |
|---|----------|--------|-----------|----------|
| D1 | Crash recovery | Spill 100K keys, kill -9, restart | RDB on/off, AOF on/off | Keys recoverable, startup time, consistency |
| D2 | TTL interaction | Keys with TTL expire while on flash | TTL range, lazy vs active expiry | Correct nil on access, flash cleanup latency |
| D3 | Eviction fairness | Compare LRU idle-time distribution of spilled keys with/without tiering | sample size, policy | Whether hot keys are incorrectly spilled |
| D4 | Replication lag | Primary spilling aggressively with replica connected | spill rate, replication buffer size | Replication lag, whether spill/fetch generates repl traffic |

### Dimension Reference

| Dimension | Values |
|-----------|--------|
| Value size | 64B, 256B, 1KB, 10KB, 100KB, 1MB |
| Value type | string, hash (10/100/1000 fields), list, zset, stream |
| Promotion policy | always, never |
| Spill mode | value-spill, key-spill |
| Backend | storage backend, in-memory mock |
| Clients | 1, 10, 50, 200, 500, 1000 |
| maxmemory | 32MB, 300MB, 2GB |
| Trace | cluster52_1m, cluster52_10m, cluster52_100m, synthetic_zipfian, synthetic_uniform |

---

## Tool Categories

The benchmark suite uses three complementary tools, each suited to different measurement needs:

| Tool | Strengths | Scenarios |
|------|-----------|-----------|
| **trace-replay** | Real workload patterns, configurable clients, set-on-miss mode | W1, W2, W3 |
| **valkey-benchmark** | High client counts, simple throughput ceiling, built-in latency histograms | C1, C2 |


- Measures individual fetch/spill latency with nanosecond resolution
- Controls exactly when spilling occurs (via CONFIG SET maxmemory)
- Sweeps value sizes, object types, promotion policies
- Outputs per-operation CSV for statistical analysis

### trace-replay

Replays real Twitter cache traces with configurable parallelism:
- `-set-on-miss`: Creates keys on first access (cold-start mode)
- `-c N`: Client count (1-1000)
- Reports TPS, p50/p99/p99.9 latency, hit ratio
- Trace files: cluster52_1m.csv, cluster52_10m.csv, cluster52_100m.csv

### valkey-benchmark

Built-in Valkey benchmark for raw throughput measurement:
- Best for client scaling (C1) — native support for high connection counts
- Best for saturation testing (C2) — can flood with pure GETs or SETs
- Less suitable for mixed workloads or realistic access patterns

---

## Scenario-to-Tool Mapping

| Scenario | Primary Tool | Config | Key Flags |
|----------|-------------|--------|-----------|
| F2 (fetch throughput) | trace-replay | tiering-always | `-c 200` (all keys pre-spilled) |
| W1 (cold-start) | trace-replay | tiering-always/never | `-set-on-miss -c 200` |
| W2 (pre-loaded) | trace-replay | tiering-always/never | pre-populate then `-c 200` |
| W3 (mixed r/w) | trace-replay | tiering-always | `-c 200` at steady state |
| C1 (client scaling) | valkey-benchmark | tiering-always | `scenarios/client-scaling.sh` |
| C2 (backend saturation) | valkey-benchmark | tiering-always | pure SET flood then pure GET flood |

---

## benchmark.sh Orchestrator Outline

The main orchestrator (`benchmark/benchmark.sh`) follows this execution flow:

```
benchmark.sh <category|scenario> [--config FILE] [--tag NAME] [--no-metrics]
    │
    ├── Parse args, expand category → scenario IDs
    │
    └── For each scenario ID:
        ├── Select config (baseline.conf / tiering-always.conf / tiering-never.conf)
        ├── Create results directory: results/<tag>/<scenario_id>/
        ├── Stop any running server
        ├── Start valkey-server with selected config
        ├── Start metrics-collector.sh (background, 1s interval → metrics.csv)
        ├── Dispatch scenario:
        │   ├── Inline function (F1, F2, F3, S1, S2, W2, W3, C2)
        ├── Stop metrics-collector
        └── Stop server
```

### Server Lifecycle

- Server starts fresh for each scenario (no state leakage)
- Config determines module loading, promotion policy, maxmemory
- Server log captured to `results/<tag>/<id>/valkey.log`
- Graceful shutdown via `SHUTDOWN NOSAVE`

### Metrics Integration

- `metrics-collector.sh` starts AFTER server is confirmed responsive (PING)
- Collects 19 fields per second: memory, tiering stats, CPU, disk IO
- Stops BEFORE server shutdown (clean CSV, no connection errors)
- Skipped with `--no-metrics` for quick iteration

### Results Layout

```
results/<tag>/
├── F1/
│   ├── metrics.csv
│   ├── results.csv
│   └── valkey.log
├── W1/
│   ├── metrics.csv
│   ├── snapshots.csv
│   └── valkey.log
└── ...
```
