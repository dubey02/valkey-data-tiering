# FlashCache — Storage Performance Summary

## Hardware

| Component | Spec |
|-----------|------|
| Instance | r7gd.4xlarge (AWS Graviton3, ARM Neoverse V1) |
| CPU | 16 vCPUs, 2.6 GHz |
| NVMe | Amazon EC2 NVMe Instance Storage, 884.8 GB |
| Filesystem | ext4, mounted at /mnt/nvme |
| Region | eu-west-1 |

---

## 1. Raw NVMe Baseline (fio)

All fio tests run with `direct=1` (bypasses kernel page cache) on the bare NVMe device after full precondition (307 GB written). Read tests use `ioengine=libaio` with specified IO depth; write tests at depth=1 use `ioengine=sync`.

### Random I/O (4K block size)

| Test | IO Depth | IOPS | Bandwidth | Mean Latency | p99 Latency |
|------|----------|------|-----------|--------------|-------------|
| Random Read | 1 | 13,247 | 52 MB/s | 75 µs | 82 µs |
| Random Read | 32 | 243,181 | 950 MB/s | 131 µs | 228 µs |
| Random Read | 128 | 279,521 | 1,092 MB/s | 458 µs | 537 µs |
| Random Write | 1 | 41,321 | 161 MB/s | 24 µs | 24 µs |
| Random Write | 32 | 137,684 | 538 MB/s | 232 µs | 247 µs |

### Sequential I/O (4K)

| Test | IOPS | Bandwidth | Mean Latency |
|------|------|-----------|--------------|
| Sequential Read (d=32) | 274,203 | 1,071 MB/s | 116 µs |
| Sequential Write (d=32) | 136,416 | 533 MB/s | 234 µs |

### Mixed 70/30 Read/Write (4K, d=32)

| Direction | IOPS | Bandwidth |
|-----------|------|-----------|
| Read | 168,428 | 658 MB/s |
| Write | 72,170 | 282 MB/s |
| **Total** | **240,598** | **940 MB/s** |

### Block Size Sweep (randread: libaio d=32; randwrite: sync d=1)

| Block Size | Read IOPS | Read BW | Read Lat (mean) | Write IOPS | Write BW | Write Lat (mean) |
|-----------|-----------|---------|-----------------|------------|----------|-----------------|
| 128 B | 640,933 | 78 MB/s | 50 µs | — (invalid) | — | — |
| 512 B | 331,194 | 162 MB/s | 96 µs | 29,890 | 15 MB/s | 33 µs |
| 1 KB | 317,023 | 310 MB/s | 101 µs | 39,927 | 39 MB/s | 24 µs |
| 4 KB | 237,794 | 929 MB/s | 134 µs | 37,456 | 146 MB/s | 26 µs |
| 100 KB | 12,929 | 1,263 MB/s | 2,474 µs | 6,123 | 598 MB/s | 163 µs |
| 512 KB | 2,481 | 1,241 MB/s | 12,895 µs | 1,199 | 600 MB/s | 833 µs |
| 1 MB | 1,241 | 1,241 MB/s | 25,788 µs | 600 | 600 MB/s | 1,667 µs |
| 5 MB | 248 | 1,241 MB/s | 128,870 µs | 120 | 600 MB/s | 8,336 µs |

**Device limits:** ~1,241 MB/s sequential read bandwidth ceiling, ~600 MB/s sequential write bandwidth ceiling. Random read IOPS peaks at ~331K (512B block). Random write IOPS peaks at ~41K (sync d=1). 128B writes fail (`Invalid argument` — below device minimum block alignment).

### Post-Fill Performance (after FlashCache filled disk)

| Test | IO Depth | IOPS | Bandwidth | Mean Latency | p99 Latency |
|------|----------|------|-----------|--------------|-------------|
| Random Read | 1 | 12,988 | 51 MB/s | 76 µs | 234 µs |
| Random Read | 32 | 225,908 | 882 MB/s | 141 µs | 259 µs |
| Random Read | 128 | 278,701 | 1,089 MB/s | 459 µs | 569 µs |
| Random Write | 1 | 41,411 | 162 MB/s | 24 µs | 24 µs |
| Random Write | 32 | 140,562 | 549 MB/s | 227 µs | 247 µs |
| Mixed 70/30 (d=32) | — | 236,506 | 924 MB/s | — | R: 334 µs / W: 104 µs |

Post-fill shows minimal degradation (~7% read IOPS drop at d=32, negligible write impact). The NVMe has no significant performance cliff after heavy writes.

---

## 2. FlashCache Standalone Benchmark

FlashCache's built-in benchmark tool (`fc_bench`) exercises the storage engine directly — no Valkey overhead. It populates 4M items (512B values), then runs a mixed 80/20 read/write workload.

### Configuration

| Parameter | Value |
|-----------|-------|
| DB size | 8 GB |
| Max items | 4,000,000 |
| Value size | 512 bytes (constant) |
| Read:Write ratio | 4:1 (80/20) |
| Reading order | prefer_old_item (worst-case: maximizes disk reads) |
| Snapshot | bgsave v2 |

### Results

| Phase | Read TPS | Write TPS | Disk Read | Disk Write | GC Read | GC Write | Write Amp | Active DB | Memory |
|-------|----------|-----------|-----------|------------|---------|----------|-----------|-----------|--------|
| **Populate** (0→2M items) | 0 | 1,187,283 | 126 MB/s | 770 MB/s | 126 MB/s | 125 MB/s | 1.19× | 1.06 GB | 35 MB |
| **Populate** (2M→4M items) | 0 | 868,941 | 183 MB/s | 653 MB/s | 183 MB/s | 180 MB/s | 1.38× | 2.12 GB | 68 MB |
| **Mixed read** (4M→2.8M items) | 152,116 | 38,032 | 715 MB/s | 55 MB/s | 39 MB/s | 35 MB/s | 2.66× | 2.29 GB | 51 MB |
| **Mixed steady** (2.8M→1.6M) | 163,591 | 40,897 | 740 MB/s | 22 MB/s | 13 MB/s | 0 | 1.00× | 2.37 GB | 33 MB |
| **Mixed steady** (1.6M→400K) | 164,923 | 41,230 | 745 MB/s | 22 MB/s | 13 MB/s | 0 | 1.00× | 2.46 GB | 15 MB |

### Key Metrics

| Metric | Value |
|--------|-------|
| **Peak read throughput** | 164,923 TPS (745 MB/s disk read) |
| **Peak write throughput** | 1,187,283 TPS (770 MB/s disk write, log-buffered) |
| **Sustained mixed (80/20)** | ~206K TPS (165K read + 41K write) |
| **Write latency** | >99.9% under 1 µs (buffered log append) |
| **Write amplification** | 1.0× at steady state (no GC pressure), 1.2–2.7× during fill |
| **Memory usage** | 15–68 MB (index only, values on disk) |
| **Read throttled ops** | 3,750–5,941 (brief bursts, <0.004% of total) |

### Read Latency Distribution (steady state, from fc_bench)

The fc_bench tool reports read latency as a 10-bucket histogram (% of reads in each range). Steady-state values at 165K read TPS:

```
5.41%, 16.24%, 16.55%, 16.46%, 16.17%, 15.69%, 9.10%, 3.81%, 0.34%, 0.23%
```

FlashCache's internal FIO layer defines disk-level latency buckets in microseconds: `[0–100], [100–200], [200–400], [400–1000], [1000–2000], [2000–10000], [10000–50000], [50000–100000], [100000+]`. The fc_bench API-level histogram may use different boundaries, but the shape shows ~94% of reads complete within the first 7 buckets with a flat distribution across the mid-range — consistent with NVMe random read latency for ~5 KB requests.

Write latency: >99.9% of writes complete in the fastest bucket (sub-µs, buffered log append).

---

## 3. FlashCache vs Raw NVMe Efficiency

Comparing FlashCache throughput against the raw fio baseline:

| Metric | Raw NVMe (fio) | FlashCache | Notes |
|--------|---------------|------------|-------|
| Read IOPS (4K, d=32) | 243,181 | 164,923 read TPS | **68% efficiency** — index lookup + 5KB avg read size overhead |
| Read Bandwidth | 950 MB/s (4K random) | 745 MB/s | **78%** — larger request size partly compensates |
| Sequential Write BW | 533 MB/s | 770 MB/s (populate) | FC log-append matches sequential throughput |
| Random Write IOPS (4K, d=1) | 41,321 | N/A | Not comparable — FC buffers writes in log, doesn't do random writes |

**Read efficiency (68%):** Each FC read involves: in-memory index hash lookup → compute log segment offset → issue single NVMe read (~5 KB avg) → copy to user buffer. The 32% gap vs raw 4K reads comes from the larger request size (5KB vs 4KB) and index traversal overhead.

**Write model:** FlashCache writes are fundamentally different from fio random writes. FC appends to an in-memory log buffer (sub-µs per fc_put), then flushes full segments sequentially to NVMe. The 1.2M "write TPS" is the API-level throughput; actual NVMe writes are large sequential I/Os (~114 KB) at ~770 MB/s — comparable to fio sequential write bandwidth (533 MB/s at d=32). The difference is FC batches many small values into large sequential writes.

---

## 4. Disk I/O Profile During Valkey Integration

When FlashCache is used as the Valkey data tiering backend (uniform 80/20 workload, 200 clients):

| Phase | Read IOPS | Read BW | Write IOPS | Write BW | Disk Util | Queue Depth |
|-------|-----------|---------|------------|----------|-----------|-------------|
| **Populate** (filling memory) | 0 | 0 | 0 | 0 | 0% | 0 |
| **Spill burst** (memory full → flash) | 990 | 111 MB/s | 4,102 | 457 MB/s | 23% | 5.7 |
| **Steady state** (read-heavy) | 81,816 | 414 MB/s | 477 | 53 MB/s | 65–74% | 12 |
| **Peak** (GC compaction overlap) | 146,793 | 726 MB/s | 1,094 | 126 MB/s | 100% | 112 |

### Why the gap vs standalone FC bench?

| Factor | Standalone FC | Valkey Integration | Impact |
|--------|--------------|-------------------|--------|
| Serialization | None (raw bytes in/out) | createDumpPayload() + rdbLoad per value | Adds CPU cost on the ASIO thread per op |
| Request path | Direct fc_get() in tight loop | queue → dequeue → serialize/deserialize → fc call → completion → unblock | Added latency per hop |
| Caller overhead | Benchmark does nothing between issuing reads | Main thread must process command, block client, manage state | Throughput limited by main-thread command rate |
| GC interference | Benchmark fills then reads (GC settles) | Continuous spill+fetch creates ongoing GC pressure | GC compaction competes for NVMe bandwidth |

The Valkey integration achieves **~50%** of FlashCache's standalone read throughput (81K vs 165K read ops/s). The gap comes from serialization overhead (createDumpPayload/rdbLoad on ASIO thread) and main-thread command processing cost. This is the primary optimization target for future work (multi-ASIO-thread, or bypassing RDB serialization for simple types).

---

## 5. FlashCache Design Properties

| Property | Behavior |
|----------|----------|
| Write path | Log-structured append (`O_DIRECT`). No in-place updates. |
| Read path | Index → log offset → single NVMe read (`O_DIRECT`) |
| GC | Reads live entries from old segments, rewrites to new. LRU eviction when full. |
| Write amplification | 1.0× at steady state (no GC), 1.2–2.7× during fill/churn |
| Index | In-memory hash map. ~15–68 MB for 400K–4M items |
| Crash safety | Snapshot-based (bgsave). Index rebuilt from log on restart. |
| Concurrency | Internal parallelism for reads. Single-writer for log append. |
| Value size | No inherent limit. Tested 512B–5MB. Larger values = lower IOPS (bandwidth-bound). |

---

## 6. Summary

| Dimension | Performance |
|-----------|-------------|
| **Raw device** | 243K read IOPS / 138K write IOPS (4K, d=32) |
| **FlashCache standalone** | 165K read TPS / 1.2M write TPS (512B values, 80/20) |
| **FlashCache via Valkey (uniform)** | 81K disk read IOPS / ~85K Valkey TPS total |
| **FlashCache via Valkey (zipfian)** | 27K disk read IOPS (most requests served from DRAM) |
| **Read latency (FC standalone)** | ~94% within first 7 of 10 histogram buckets (flat mid-range distribution) |
| **Read latency (Valkey end-to-end)** | p50: 1.2ms idle, 2.8ms under load |
| **Write amplification** | 1.0× steady state, 1.2–2.7× during fill/GC |
| **Memory overhead** | 15–68 MB (index only) |

The storage layer itself is not the bottleneck in the Valkey integration — the single ASIO thread and main-thread client blocking are. FlashCache has 2–3× headroom beyond what the current architecture utilizes.
