# Valkey Data Tiering — Benchmark Summary

## Overview

This document presents benchmark results for the Non-Key-Spilling data tiering implementation. All tests run on a single r7gd.4xlarge EC2 instance (AWS Graviton3, 16 vCPUs, 128 GiB RAM, 1x 1.9TB NVMe SSD) in eu-west-1.

**Architecture:** Values spill to NVMe-backed FlashCache while keys remain in DRAM. Async IO thread handles serialization and disk operations. Client commands that access tiered keys block transparently until the value is fetched.

**Engine:** Valkey (unstable branch + data tiering, aarch64)

---

## Test Configuration

| Parameter | Value |
|-----------|-------|
| Instance | r7gd.4xlarge (Graviton3) |
| maxmemory | 1 GB |
| maxmemory-policy | allkeys-lru |
| Keyspace | 4,000,000 keys |
| Key size | 100 bytes |
| Value size | 512 bytes (unless noted) |
| Dataset | ~2.4 GB (exceeds 1GB maxmem → ~60% on disk) |
| FlashCache capacity | 8 GB (NVMe) |
| Clients | 200 (160 GET + 40 SET for 80/20) |
| Operations | 50,000,000 per run |
| Access pattern | Zipfian (α=1.0) unless noted |
| IO threads | 1 (dedicated async spill/fetch) |
| Active defrag | enabled |

---

## 1. Throughput — Access Pattern Comparison

All tests: 4M keys × 512B values, 1GB maxmemory, 200 clients, 50M ops.

| Scenario | GET TPS | SET TPS | Total TPS | GET p50 | GET p99 | Notes |
|----------|---------|---------|-----------|---------|---------|-------|
| **Baseline (no tiering)** | 126,961 | 49,129 | 176,090 | 0.74 ms | 1.34 ms | All data in memory, maxmemory=0 |
| **Zipfian 80/20** | 103,372 | 30,808 | 134,180 | 1.3 ms | 2.4 ms | Hot set fits in memory |
| **Uniform 80/20** | 64,924 | 21,510 | 86,434 | 2.0 ms | 4.1 ms | No locality — worst case |
| **Balanced 50/50** | 65,127 | 65,127 | 130,254 | 1.3 ms | 2.7 ms | Equal read/write pressure |
| **Zipfian + TTL=120s** | 121,156 | 37,615 | 158,771 | 1.0 ms | 1.9 ms | Expiry reduces disk pressure |

**Key observations:**
- **Tiering overhead: ~24% TPS reduction** vs baseline (176K → 134K for Zipfian). The cost comes from ~18% of GETs hitting flash (async fetch latency) and spill IO competing for CPU.
- Zipfian workloads achieve ~134K TPS with 82% DRAM hit rate (hot keys stay in memory via LRU)
- Uniform is ~35% slower due to no locality — every key equally likely to be on disk
- TTL workload is fastest because expired keys reduce the active dataset, improving memory hit rate
- All workloads maintain stable memory at maxmemory throughout the run (no OOM, no stall)
- GET latency: 0.74ms baseline → 1.3ms with tiering (the average includes the ~18% of GETs that wait for flash fetch)

---

## 2. Value Size Sensitivity

Zipfian 80/20, 1GB maxmemory, varying value sizes. Keyspace derived to keep dataset >> maxmemory.

| Value Size | GET TPS | SET TPS | GET p50 | GET p99 | Bottleneck |
|-----------|---------|---------|---------|---------|------------|
| **100 B** | 60,101 | 17,288 | 2.2 ms | 7.3 ms | Main thread CPU (small values = high overhead ratio) |
| **500 B** | 90,826 | 28,595 | 1.6 ms | 3.0 ms | Main thread CPU |
| **5 KB** | 77,922 | 25,377 | 1.9 ms | 3.6 ms | Main thread CPU |
| **500 KB** | 1,623 | — | 64 ms | 79 ms | NVMe bandwidth (~800 MB/s read) |
| **5 MB** | 1,602 | — | 99 ms | 111 ms | NVMe bandwidth |

**Key observations:**
- Sweet spot is 500B–5KB: throughput is CPU-bound (main thread saturated at 100%)
- 100B values have lower TPS because per-key metadata overhead dominates (134B overhead per key)
- 500KB+ values shift the bottleneck to disk bandwidth — TPS drops to ~1.6K but remains stable (no stalls)
- System gracefully degrades with increasing value size — no cliff behavior

---

## 3. Disk Fetch Latency

Measures the raw cost of reading a tiered value from NVMe.

| Scenario | p50 | p99 | p99.9 | p100 |
|----------|-----|-----|-------|------|
| **Idle STRING** (512B, 1 RPS, tiered) | 1.21 ms | 1.31 ms | 1.31 ms | 1.31 ms |
| Idle STRING (baseline, no tiering) | 0.12 ms | 0.14 ms | 0.14 ms | 0.14 ms |
| **Idle HASH** (~1MB, 1 RPS, tiered) | 4.26 ms | 4.63 ms | 4.63 ms | 4.63 ms |
| Idle HASH (baseline, no tiering) | 0.12 ms | 0.14 ms | 0.14 ms | 0.14 ms |
| **Slam** (512B, 200 clients, tiered) | 2.84 ms | 6.21 ms | 7.88 ms | 11.64 ms |
| Slam (baseline, no tiering) | 0.96 ms | 1.91 ms | 1.98 ms | 2.92 ms |

**Disk fetch overhead:**
- Small values (512B): **+1.1 ms** at idle, **+1.9 ms p50 / +4.3 ms p99** under load
- Large values (1MB hash): **+4.1 ms** at idle (dominated by NVMe read + deserialization)
- Under load, queuing adds ~1-4 ms to the base fetch time

---

## 4. Data Type Support

All 7 Valkey data types work correctly with tiering. Compound workload: 50MB maxmemory, 100K keys × 10 items × 100B, zipfian access, 200 clients.

| Data Type | Operations | Spilled | Fetched | DRAM Hit Rate |
|-----------|-----------|---------|---------|---------------|
| HASH | 2,000,000 | 745,787 | 678,373 | 95% |
| LIST | 2,000,000 | 723,706 | 658,372 | 95% |
| SET | 2,000,000 | 742,516 | 675,411 | 95% |
| ZSET | 2,000,000 | 906,149 | 823,962 | 95% |
| STREAM | 2,000,000 | 806,650 | 733,358 | 95% |

**Key observations:**
- All types complete 2M operations with active spill/fetch cycling
- ZSET has highest spill count (larger serialized size due to scores)
- 95% DRAM hit rate across all types (zipfian hot set stays in memory)
- Memory remains stable at ~49MB (under 50MB cap) throughout

---

## 5. Memory Behavior

The system maintains memory at maxmemory using a Smith-predictor spill controller:

```
Memory zones:
  ≤ 1.0× maxmemory : Normal operation (no spill, no throttle)
  1.0×–1.2× maxmemory : Throttle band (write TPS progressively reduced, spilling active)
  > 1.2× maxmemory : Hard OOM reject (writes rejected immediately)
```

- **No eviction**: The engine never deletes keys. Only outcomes are: spill to disk, throttle writes, or reject at hard cap.
- **Projected memory**: Spill decisions use projected memory (used − in-flight spill bytes) to avoid over-spilling.
- **Stable state**: During steady-state benchmark, memory stays pinned at exactly maxmemory ± 0.1%.

---

## 6. Hardware Utilization (Zipfian 80/20 steady state)

| Resource | Utilization | Notes |
|----------|-------------|-------|
| Main thread CPU | ~100% | Bottleneck for small values |
| IO thread CPU | ~60% | Serialization + FC calls |
| NVMe read | ~130 MB/s | GC compaction reads |
| NVMe write | ~22 MB/s | Spill writes + GC rewrites |
| NVMe IOPS | ~200 write, ~150 read | Low IOPS — large sequential IO |
| Disk utilization | ~55% | Not saturated |
| Memory fragmentation | 1.2× | Normal for active workload |

---

## Test Environment

- **Instance**: r7gd.4xlarge (AWS Graviton3, eu-west-1)
- **OS**: Amazon Linux 2023 (aarch64)
- **NVMe**: 1× 1.9TB local SSD (formatted ext4, mounted at /mnt/nvme)
- **Benchmark tool**: valkey-benchmark (built from same source tree, supports --zipfian)
- **FlashCache**: Log-structured KV store with internal GC (linked statically)

---

## Reproducing

```bash
# Build
make -j$(nproc)

# Run benchmark (deploys to EC2 via SSH)
cd benchmark/
cp benchmark.env.example benchmark.env  # configure EC2_HOST, EC2_USER, EC2_KEYPATH
./benchmark.sh --remote --config zipfian-1gb mixed-rw

# Local run (requires NVMe at /mnt/nvme)
./benchmark.sh --config zipfian-1gb mixed-rw
```

Available configs: `zipfian-1gb`, `uniform-flashcache`, `balanced-flashcache`, `zipfian-1gb-ttl`, `size-sweep-fc`, `compound-flashcache`

Latency tests: `./benchmark.sh --remote --config idle tiering-latency`
