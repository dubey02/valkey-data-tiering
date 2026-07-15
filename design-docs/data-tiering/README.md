# Valkey Data Tiering — Non-Key-Spilling POC

## Overview

This POC implements transparent value tiering to NVMe flash storage. **Keys always stay in DRAM** — only values are spilled to flash when memory pressure is detected. All keyspace operations (EXISTS, TTL, SCAN, DBSIZE, RANDOMKEY) work without disk access.

```
┌─────────────────────────────────────────────────────────────────┐
│                        Valkey Server                             │
│                                                                 │
│  Memory (maxmemory)          Pluggable Storage Interface        │
│  ┌──────────────┐           ┌──────────────────────────┐       │
│  │ Key → Value  │──spill──▶│ storageType vtable        │       │
│  │ Key → TIERED │◀─fetch───│ (FlashCache / RocksDB)    │       │
│  │ (key stays!) │           └────────────┬─────────────┘       │
│  └──────────────┘                        │                      │
└──────────────────────────────────────────┼─────────────────────┘
                                           │
                              ┌─────────────▼─────────────┐
                              │   NVMe Flash / SSD         │
                              │   (FlashCache 8GB+)        │
                              └───────────────────────────┘
```

---

## How to Try It

### Prerequisites
- Linux (aarch64 or x86_64)
- `cmake3` (for FlashCache build)
- `libaio-dev` / `libaio` (for async I/O)
- NVMe or SSD storage (or use `/tmp` for testing)

### Quick Start (Native FlashCache)

```bash
# Build
make -j$(nproc)

# Create flash backing file
fallocate -l 8G /mnt/nvme/flashcache.db   # NVMe recommended
# Or for testing: fallocate -l 256M /tmp/flashcache.db

# Start server with data tiering
./src/valkey-server \
  --ext-storage-enabled yes \
  --ext-storage-backend flashcache \
  --ext-storage-path /mnt/nvme/flashcache.db \
  --ext-storage-capacity-mb 8192 \
  --maxmemory 1gb \
  --maxmemory-policy allkeys-lru \
  --enable-debug-command yes

# Test spill/fetch
./src/valkey-cli SET mykey $(python3 -c "print('x'*500)")
./src/valkey-cli DEBUG SPILL mykey    # Force spill to flash
./src/valkey-cli GET mykey            # Fetches from flash transparently
./src/valkey-cli INFO ALL | grep spilled
```

### Quick Start (Module FlashCache)

```bash
# Build module
cd modules/flash-tiering && cargo build --release && cd ../..

# Create flash file
fallocate -l 8G /mnt/nvme/flashcache.db

# Start with module
./src/valkey-server \
  --ext-storage-enabled yes \
  --maxmemory 1gb \
  --maxmemory-policy allkeys-lru \
  --loadmodule modules/flash-tiering/target/release/libflash_tiering_module.so \
    backend=flashcache db_path=/mnt/nvme/flashcache.db db_size_bytes=8589934592
```

### Running Benchmarks

```bash
# Local benchmark (no EC2 needed)
fallocate -l 8G /tmp/flashcache.db
./benchmark/benchmark.sh --config zipfian-1gb mixed-rw

# Remote benchmark (EC2 with NVMe)
# 1. Configure benchmark/benchmark.env with your EC2 host
# 2. Run:
./benchmark/benchmark.sh --remote --config zipfian-1gb --tag my-test mixed-rw

# View results
cat benchmark/results/my-test/mixed-rw/zipfian-1gb/get_output.txt
```

### Benchmark Dashboard

Open `benchmark_dashboard/index.html` in a browser to view interactive charts of benchmark results (TPS, latency, memory hit rates across workloads).

---

## Architecture & How It Works

### Tiering State Machine (5 States)

Each key's tiering state is stored in `robj->tiering_state` (3 bits) — zero per-key memory overhead.

```
          ┌─────────────────────────────────────────────────┐
          │                                                   │
          ▼                                                   │
    ONLY_MEMORY ─────spill────▶ COPYING_TO_FLASH             │
         ▲                          │       │                 │
         │                     OK   │       │ FAIL            │
         │                     ▼    │       ▼                 │
         │               ONLY_FLASH │   ONLY_MEMORY           │
         │                     │    │                         │
         │              fetch/del   │                         │
         │                     ▼    │                         │
         │            COPYING_TO_MEMORY                       │
         │                     │                              │
         │               OK    │                              │
         └─────────────────────┘                              │
                                                              │
                         eviction during IO ──▶ PENDING_EVICT─┘
```

| State | Value | Meaning |
|-------|-------|---------|
| `ONLY_MEMORY` | 0 | Value in RAM (default) |
| `COPYING_TO_FLASH` | 1 | Spill in-flight, value still readable |
| `ONLY_FLASH` | 2 | Value on disk, encoding = `OBJ_ENCODING_TIERED` |
| `COPYING_TO_MEMORY` | 3 | Fetch in-flight from flash |
| `PENDING_EVICT` | 4 | Eviction requested while IO in-flight |

### Command Processing (`preCommandExec`)

Before every command executes, `preCommandExec()` checks if any accessed key is in `ONLY_FLASH` or `COPYING_TO_MEMORY` state. If so, the client is **blocked** (like BLPOP) until the fetch completes. Reads from `ONLY_MEMORY` and `COPYING_TO_FLASH` proceed without blocking.

### Spill Controller (Smith Predictor, V2)

When `used_memory > maxmemory`, the spill controller activates:

1. **Candidate selection**: Reuses Valkey's LRU/LFU eviction pool sampling (`evictionPoolPopulate`) to find cold keys
2. **Projected memory**: Uses a Smith predictor to estimate future memory after in-flight spills complete (prevents over-spilling)
3. **Self-terminating**: Stops when projected memory ≤ maxmemory (no fixed batch size)
4. **Serialization on IO thread**: Engine passes `robj*` reference; the IO thread serializes via RDB DUMP format

### Write Throttle (Token Bucket)

When spilling can't keep up (memory between 1.1x–1.2x maxmemory), writes are throttled:
- Token bucket with TPS-based refill
- Queued clients released via 1ms timer
- At 1.2x maxmemory: hard OOM reject (`-OOM` error)
- Memory-only reads are NEVER throttled

### Eviction Override

The engine **never deletes keys** to free DRAM. Instead:
- `performEvictions()` delegates to `extStoragePerformEvictions()` which spills values to flash
- If flash is full, FlashCache GC internally evicts old flash items (data loss for those keys)
- Keys only deleted from dict when explicitly DEL'd or expired

### Expiry

- **Passive expiry**: On GET, if key is expired + tiered, returns empty (defers deletion if IO in-flight)
- **Active expiry**: Cron samples keys; tiered expired keys get async-deleted from flash
- **FLUSHDB/FLUSHALL**: Drains all in-flight IO, then clears flash index

### Defragmentation

Active defrag skips keys in `COPYING_TO_FLASH` or `COPYING_TO_MEMORY` states to avoid UAF.

---

## Storage Backend: FlashCache

FlashCache is a high-performance NVMe key-value cache library designed for data tiering:

- **Log-structured writes**: Sequential NVMe writes (no random write amplification)
- **In-memory index**: O(1) key→offset lookup
- **Background GC**: Reclaims space from deleted/overwritten items
- **Configurable capacity**: Index size, GC rate, buffered write thresholds all tunable at runtime

### How FlashCache is Wired

**Native path** (`src/storage/storage_flashcache_real.c`):
- Lock-free MPSC request queue (C11 atomics)
- Dedicated IO thread: dequeues requests, serializes robj→bytes, calls FlashCache PUT/GET/DEL
- Lock-free SPSC completion ring: IO thread → main thread
- Main thread polls completions in `beforeSleep` + 1ms timer

**Module path** (`modules/flash-tiering/`):
- Same FlashCache library via Rust FFI (`fc_shim.c`)
- ASIO worker thread with crossbeam channels
- Registers a `storageType` struct via `ValkeyModule_RegisterStorageBackend`

---

## Pluggable Storage Interface

Any storage backend can be integrated by implementing the `storageType` vtable (`src/storage.h`):

```c
typedef struct storageType {
    const char *name;       // "flashcache", "rocksdb", "cachelib"
    int version;

    // Lifecycle
    void *(*open)(storageConfig *cfg);
    void (*close)(void *ctx);

    // Async KV operations (backend owns IO)
    storageStatus (*put_async)(void *ctx, uint32_t db_id, ...);
    storageStatus (*get_async)(void *ctx, uint32_t db_id, ...);
    storageStatus (*del_async)(void *ctx, uint32_t db_id, ...);
    int (*poll_completions)(void *ctx, int max);

    // OR: Sync KV operations (engine-managed IO thread wraps these)
    storageStatus (*put)(void *ctx, uint32_t db_id, ...);
    storageStatus (*get)(void *ctx, uint32_t db_id, ...);
    storageStatus (*del)(void *ctx, uint32_t db_id, ...);

    // Optional
    storageStatus (*flush_db)(void *ctx, uint32_t db_id);
    void (*get_stats)(void *ctx, storageStats *out);
    int (*cron)(void *ctx);   // Periodic maintenance
} storageType;
```

### Integration modes:
1. **Native (compiled in)**: Implement `storageType`, add to `ext_storage_bridge.c` selection logic
2. **Module (dynamic)**: Build as .so, call `ValkeyModule_RegisterStorageBackend(ctx, &my_type)` in OnLoad

### Adding a new backend (e.g., CacheLib):
1. Create `src/storage/storage_cachelib.c` implementing `storageType`
2. Add `storageGetCacheLibType()` function
3. Add selection in `ext_storage_bridge.c`: `if (strcmp(name, "cachelib") == 0) type = storageGetCacheLibType();`
4. Link library in `src/Makefile`

---

## Configuration

| Config | Default | Description |
|--------|---------|-------------|
| `ext-storage-enabled` | `no` | Enable/disable data tiering (immutable) |
| `ext-storage-backend` | `""` | Backend: `flashcache`, `flashcache-mock`, `rocksdb` |
| `ext-storage-path` | `""` | Flash file path |
| `ext-storage-capacity-mb` | `1024` | Flash capacity |
| `ext-storage-max-spill-size` | `128MB` | Max value size to spill (0=unlimited) |
| `ext-storage-items-spillover-batch-size` | `10` | Spill batch size (V1 strategy) |
| `ext-storage-throttling-strategy` | `v2` | `v1` (legacy) or `v2` (decoupled) |
| `ext-storage-spilling-strategy` | `v2` | `v1` (batch+cap) or `v2` (Smith predictor) |
| `ext-storage-throttle-band-start` | `100` | Throttle start (% of maxmemory) |
| `ext-storage-throttle-band-end` | `120` | OOM reject (% of maxmemory) |

### FlashCache tuning (modifiable at runtime via CONFIG SET):
| Config | Default | Description |
|--------|---------|-------------|
| `ext-storage-index-size` | `1048576` | Index hash table entries |
| `ext-storage-max-allocated-percent` | `90` | GC trigger threshold |
| `ext-storage-min-gc-rate` | `4096` | Minimum GC bytes/sec |
| `ext-storage-max-gc-rate` | `30MB` | Maximum GC bytes/sec |
| `ext-storage-max-buffered-write-size` | `4MB` | Write buffer capacity |
| `ext-storage-max-in-flight-reads` | `128` | Concurrent read limit |

---

## Test Suite

12 TCL test suites covering all functionality:

```bash
# Run all native tests
./runtest --single unit/data-tiering/ext-storage-data-types \
  --single unit/data-tiering/ext-storage-eviction \
  --single unit/data-tiering/ext-storage-expiry \
  --single unit/data-tiering/ext-storage-defrag \
  --single unit/data-tiering/ext-storage-embstr-spill \
  --single unit/data-tiering/ext-storage-fc-configs \
  --single unit/data-tiering/ext-storage-max-spill-size \
  --single unit/data-tiering/ext-storage-persistence

# Run module tests (requires: cd modules/flash-tiering && cargo build --release)
./runtest --single unit/data-tiering/ext-storage-module-configs \
  --single unit/data-tiering/ext-storage-module-spill-fetch \
  --single unit/data-tiering/ext-storage-module-expiry-eviction

# Run Lua pressure test (requires pre-created flash file)
fallocate -l 256M /tmp/valkey-flash-pressure.db
./runtest --single unit/lua-pressure
```

---

## Benchmark Results Summary

| Workload | TPS | Memory Hit | Disk Hit | Throttle |
|----------|-----|-----------|----------|----------|
| Zipfian (80/20 R/W, 512B, 1GB) | **128K** | 82% | 18% | 0% |
| Uniform (80/20 R/W, 512B, 1GB) | 82K | 55% | 45% | 0% |
| Write-heavy (50/50, 512B) | 66K | — | — | 0% |

All benchmarks: 4M keys, 512B values, 1GB maxmemory, 8GB FlashCache, allkeys-lru, 200 clients.

---

## Monitoring Metrics

Data tiering exposes a dedicated `INFO ext_storage` section with metrics for monitoring tiering effectiveness. These are the same metrics captured by our benchmarking tool (`benchmark/tools/metrics-collector/metrics-collector.sh`) at 1-second granularity.

### Hit Rate Metrics

| Metric | Source | Description |
|--------|--------|-------------|
| `dram_value_hits` | INFO ext_storage | Commands served from DRAM (includes re-executions after fetch) |
| `completion_read_ok` | INFO ext_storage | Values fetched from flash (disk hits) |
| `completion_read_miss` | INFO ext_storage | Flash lookups that returned nothing (key was GC'd from flash) |
| `keyspace_hits` | INFO stats | Standard keyspace hits |
| `keyspace_misses` | INFO stats | Standard keyspace misses |

**Derived hit rates** (used in benchmarks):
- **Memory hit %** = `(dram_value_hits - completion_read_ok) / dram_value_hits × 100`
- **Disk hit %** = `completion_read_ok / dram_value_hits × 100`
- Both sum to 100% among value-accessing commands

Higher memory hit % = hot data staying in DRAM effectively. Rising disk hit % = working set exceeds available DRAM.

### Throughput & RPS Metrics

| Metric | Source | Description |
|--------|--------|-------------|
| `instantaneous_ops_per_sec` | INFO stats | Overall request throughput |
| `total_commands_processed` | INFO stats | Lifetime command count (compute delta for rate) |
| `throttle_allowed_tps` | INFO ext_storage | Current allowed ops/sec under memory pressure |
| `throttle_current_rate` | INFO ext_storage | Throttle ratio (0.0 = no throttle, 1.0 = fully throttled) |
| `throttle_total_throttled` | INFO ext_storage | Cumulative throttled client count |
| `throttle_queued_clients` | INFO ext_storage | Clients currently queued by throttler |

If `throttle_current_rate > 0`, the engine is slowing writes to prevent OOM.

### Latency Metrics

| Metric | Source | Description |
|--------|--------|-------------|
| `latency_percentiles_usec_<cmd>` | INFO latencystats | Per-command p50/p99/p99.9 latency (µs) |
| `kbc_fetching_block` | INFO ext_storage | Commands that blocked waiting for async flash read |
| `kbc_spilling_block` | INFO ext_storage | Commands that blocked because key was mid-spill |
| `completion_read_retry` | INFO ext_storage | Transient read rejections re-issued |

Per-command latency histograms naturally reflect tiering impact — commands hitting flash show higher tail latency (p99) compared to pure DRAM hits.

### Memory Metrics

| Metric | Source | Description |
|--------|--------|-------------|
| `used_memory` | INFO memory | Logical memory used by engine (bytes) |
| `used_memory_rss` | INFO memory | Resident set size (physical memory) |
| `maxmemory` | INFO memory | Configured memory limit |
| `mem_fragmentation_ratio` | INFO memory | RSS / used_memory (jemalloc fragmentation) |
| `mean_spill_ram` | INFO ext_storage | EMA of in-flight spill memory footprint |
| `inflight_spill_ram_bytes` | INFO ext_storage | Current bytes in spill pipeline |

### Disk I/O Metrics

Captured from `/sys/block/*/stat` (NVMe device):

| Metric | Description |
|--------|-------------|
| `disk_read_iops` / `disk_write_iops` | Read/Write IOPS per second |
| `disk_read_mb` / `disk_write_mb` | Read/Write throughput (MB/s) |
| `disk_r_await_ms` / `disk_w_await_ms` | Average read/write latency (ms per I/O) |
| `disk_aqu_sz` | Average queue depth |
| `disk_util_pct` | Disk utilization % (capped at 100) |
| `disk_in_flight` | I/Os currently in flight |

### CPU Metrics

| Metric | Source | Description |
|--------|--------|-------------|
| `cpu_user` / `cpu_sys` | /proc/stat | System-wide CPU % |
| `valkey_cpu_user` / `valkey_cpu_sys` | INFO cpu | Main thread CPU % |
| `asio_cpu_pct` | /proc/PID/task | FlashCache IO worker thread CPU % (`fc_io_worker`) |

### Spill Pipeline Metrics

| Metric | Source | Description |
|--------|--------|-------------|
| `total_num_items_spilled_to_ext_storage` | INFO ext_storage | Lifetime items spilled to flash |
| `total_num_items_fetched_from_ext_storage` | INFO ext_storage | Lifetime items fetched from flash |
| `num_items_on_flash` | INFO ext_storage | Current tiered item count |
| `num_items_spilling_to_ext_storage` | INFO ext_storage | Items currently in-flight spilling |
| `spill_submitted_count` | INFO ext_storage | Spills submitted to IO thread |
| `spill_serialized_count` | INFO ext_storage | Spills serialized on IO thread |
| `blocked_clients` | INFO clients | Clients currently blocked (includes fetch waits) |

### FlashCache Backend Metrics

| Metric | Source | Description |
|--------|--------|-------------|
| `fc_num_disk_reads` / `fc_num_disk_writes` | INFO ext_storage | Flash I/O operation count |
| `fc_total_disk_read_bytes` / `fc_total_disk_write_bytes` | INFO ext_storage | Flash I/O volume |
| `fc_num_items_evicted` | INFO ext_storage | Items evicted from flash by GC |
| `fc_active_memory_bytes` | INFO ext_storage | FlashCache internal memory usage |
| `fc_num_retryable_disk_errors` | INFO ext_storage | Disk error count (health signal) |

### Quick Example

```bash
# Check tiering effectiveness
./src/valkey-cli INFO ext_storage | grep -E "dram_value_hits|completion_read_ok|throttle_current_rate|num_items_on_flash"

# Per-command latency with tiering
./src/valkey-cli INFO latencystats | grep -E "get|set"
```

---

## What's Not Yet Covered (In Progress / Design Phase)

### Persistence (P1)
- **RDB**: Currently tiered keys are skipped during BGSAVE. Need: fetch-during-save or backend-level snapshot export.
- **AOF**: AOF rewrite asserts if tiered key encountered. Need: synchronous fetch before rewrite, or append spill/fetch as commands.

### Replication (P1)
- **Full sync**: Must include tiered values in RDB stream to replicas. Options: fetch all to RAM before fork, or iterator-based streaming from flash.
- **Partial sync**: Replication backlog captures all writes (including SETs that were later spilled). No issue for incremental sync.
- **Asymmetric tiering**: Primary has flash, replica may not. Replica receives full values — no flash awareness needed on replica side.

### Module Interaction (P1)
- **ValkeyModule_OpenKey** on tiered key: Currently crashes (NULL deref on placeholder SDS). Fix: synchronous fetch before returning key handle, or return BUSY error.
- **JSON/Search modules**: Any module using `ValkeyModule_StringDMA` on a tiered key will crash. Same fix needed.

### DT ↔ Non-DT Migration
- **Enable tiering on existing instance**: Need drain-to-flash migration (spill all values on startup). Not yet implemented — requires `ext-storage-enabled` to become mutable.
- **Disable tiering**: Need fetch-all-from-flash to restore all values to RAM. Requires drain mechanism.
- **Scaling/resharding**: Slot migration must handle tiered keys — fetch value from flash before transferring to target node.

### Other Design Gaps
- **CONFIG SET ext-storage-enabled**: Currently immutable (restart required)
- **Configurable promotion/admission policy**: Designed but not implemented (4 modes: cache, hot-cold, cold-storage, large-values)
- **Bytes-denominated throttle**: For mixed value sizes (small + large values in same workload)
- **CLIENT NO-TOUCH**: Prevents LRU/LFU warmup on access (useful for cold-storage mode)

---

## File Map

```
src/
├── ext_storage.c/h          # State machine, spill/fetch orchestration (1437 LOC)
├── ext_storage_throttle.c/h # Token bucket write throttle
├── ext_storage_bridge.c/h   # Adapter between engine and storageType vtable
├── storage.h                # Pluggable storage interface definition
└── storage/
    ├── storage.h            # Internal storage header
    ├── storage_dispatch.c   # Sync/async routing
    ├── storage_middleware.c # IO thread wrapper for sync backends
    ├── storage_flashcache_real.c  # Real FlashCache (lock-free MPSC/SPSC)
    └── storage_mock.c      # In-memory mock for testing

deps/flashcache/             # FlashCache library source (C, cmake)
modules/flash-tiering/       # Rust module with FlashCache + RocksDB backends
modules/rocksdb-tiering/     # Standalone RocksDB module
modules/valkeymodule-rs/     # Rust bindings for Valkey Module API

tests/unit/data-tiering/     # 12 TCL test suites
benchmark/                   # Benchmark suite (16 configs, metrics collector, report generator)
benchmark_dashboard/         # Interactive HTML dashboard for results
```

---

## Performance Characteristics

- **Throughput bottleneck**: Main thread CPU (not disk I/O) at moderate disk utilization
- **Latency**: GETs from memory = ~0.1ms; GETs from flash = ~1-2ms (includes async fetch)
- **Memory stability**: Used memory stays at maxmemory ±5% under sustained writes
- **No eviction storms**: Spill controller prevents memory pressure from cascading
- **Zero overhead when not tiering**: Below maxmemory, ext_storage adds only a single `if (ext_data_enabled)` check per command
