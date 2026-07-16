# Data Tiering Storage Module (Non-Key-Spilling)

Valkey external storage module that spills cold values to disk while keeping keys in memory. On access, values are fetched back transparently. Supports pluggable backends with clean compile-time separation.

In non-key-spilling mode:
- Keys always remain in the Valkey dict (EXISTS, TTL, SCAN work without disk access)
- Only serialized values are written to/read from storage
- No bloom filter is needed (key is always findable in memory)

## Architecture

```
src/
├── lib.rs              # Module entry point, config parsing, Valkey FFI
├── callbacks.rs        # FFI callbacks registered with Valkey core
├── dispatcher.rs       # Feature-gated backend dispatch enum
├── common/             # Shared types, traits, utilities
│   ├── types.rs        # StorageRequest/Response, BackendConfig, errors
│   ├── backend_trait.rs # DataTieringBackend trait
│   ├── serialization.rs # Valkey robj serialization callbacks
│   └── pending.rs      # In-flight request tracking
└── backends/
    ├── rocksdb/        # Multi-threaded IO pool, sync get/put
    │   ├── backend.rs  # RocksDB DataTieringBackend impl
    │   └── asio.rs     # Multi-threaded worker pool
    └── flashcache/     # 1 IO thread, async reads, cron ticks
        ├── backend.rs  # FlashCache DataTieringBackend impl
        ├── asio.rs     # Single-threaded async worker
        ├── ffi.rs      # C FFI wrappers
        └── fc_shim.c   # C shim for variadic callbacks + metrics
```

---

## Quick Start (RocksDB — default)

```bash
# Build the module
cd private-valkey/modules/key-spilling
cargo build --release

# Build Valkey (from private-valkey root)
cd ../../..
make -j8

# Run
./src/valkey-server --loadmodule modules/key-spilling/target/release/libkey_spilling_module.so \
    backend=rocksdb \
    db_path=/tmp/tiering-db

# Test
./src/valkey-cli
> CONFIG SET maxmemory 50mb
> CONFIG SET maxmemory-policy allkeys-lru
> SET foo bar
> GET foo
```

## Unit Tests

```bash
cargo test --features enable-system-alloc
```

---

## FlashCache Backend (Internal Testing)

The FlashCache backend is behind the `backend-flashcache` feature flag. It requires building the FlashCache library and providing the `fc_shim.c` file.

### Prerequisites

- FlashCache source tree (typically at `~/workspace/FlashCache`)
- `libaio-dev` installed (`sudo yum install libaio-devel` or `sudo apt install libaio-dev`)
- `cmake` for building FlashCache

### Step 1: Build FlashCache

```bash
cd ~/workspace/FlashCache
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j8
# Produces: ~/workspace/FlashCache/build/libflashcache.a
```

### Step 2: Place fc_shim.c

Copy the FlashCache C shim into the backend directory:

```bash
cp ~/workspace/key-spilling/src/key-spilling-module/src/fc_shim.c \
   ~/workspace/private-valkey/modules/key-spilling/src/backends/flashcache/fc_shim.c
```

This file provides:
- `fc_shim_init()` — wraps `flashcacheInit()` with logger setup
- `fc_shim_noop_eviction()` / `fc_shim_noop_asio_control_msg()` — no-op callbacks
- `fc_shim_print_metrics()` — prints FlashCache internal counters
- `fc_shim_get_metrics_string()` — returns zmalloc'd metrics for INFO output
- `fc_shim_concat_metrics()` — appends ASIO metrics to FC metrics string

### Step 3: Verify build paths

The `build.rs` searches for FlashCache headers and libraries in these locations:

| What | Search path (relative to module dir) |
|------|--------------------------------------|
| Headers | `../../../FlashCache/src` |
| Headers (legacy) | `../flashcache/src` |
| Static lib | `../flashcache/build/libflashcache.a` |

If your FlashCache is elsewhere, either:
- Symlink: `ln -s ~/workspace/FlashCache ~/workspace/private-valkey/modules/flashcache`
- Or edit `build.rs` to add your path

### Step 4: Build with FlashCache

```bash
cd ~/workspace/private-valkey/modules/key-spilling

# FlashCache only
cargo build --release --no-default-features --features backend-flashcache

# Both backends enabled
cargo build --release --features backend-flashcache
```

### Step 5: Run with FlashCache backend

```bash
# Create a pre-allocated file for FlashCache (required)
fallocate -l 1G /tmp/fc-test.dat

# Start Valkey with FlashCache backend
./src/valkey-server --loadmodule modules/key-spilling/target/release/libkey_spilling_module.so \
    backend=flashcache \
    db_path=/tmp/fc-test.dat \
    db_size_bytes=1073741824 \
    num_databases=16 \
    max_in_flight_reads=128 \
    initial_index_size=4096 \
    max_buffered_write_bytes=4194304
```

### FlashCache-specific options

| Key | Default | Description |
|-----|---------|-------------|
| `initial_index_size` | 1024 | Initial hash index slots per database |
| `max_buffered_write_bytes` | 4MB | Staging buffer size before flush to disk |

### Troubleshooting FlashCache builds

**Missing `flashcache_common.h`:**
```
error: flashcache_common.h: No such file or directory
```
→ FlashCache headers not found. Check that `~/workspace/FlashCache/src/include/flashcache_common.h` exists and the path in `build.rs` resolves correctly.

**Undefined reference to `flashcacheInit`:**
```
undefined reference to `flashcacheInit`
```
→ `libflashcache.a` not found or not linked. Verify the build produced it and the search path in `build.rs` is correct.

**Missing `libaio`:**
```
cannot find -laio
```
→ Install: `sudo yum install libaio-devel` (AL2) or `sudo apt install libaio-dev` (Ubuntu).

---

## Configuration Reference

Module load arguments (key=value format):

| Key | Required | Default | Description |
|-----|----------|---------|-------------|
| `backend` | yes | — | `rocksdb` or `flashcache` |
| `db_path` | yes | — | Path to storage directory (RocksDB) or file (FlashCache) |
| `db_size_bytes` | no | 1GB | Max storage size |
| `num_databases` | no | 16 | Valkey logical databases |
| `max_in_flight_reads` | no | 128 | Max concurrent read requests |
| `num_io_threads` | no | 4 | RocksDB: number of IO worker threads |
| `compression_type` | no | none | RocksDB: none/snappy/lz4/zstd |
| `bloom_filter_bits_per_key` | no | 10 | RocksDB: bloom filter bits per key |
| `initial_index_size` | no | 1024 | FlashCache: initial hash index size per DB |
| `max_buffered_write_bytes` | no | 4MB | FlashCache: staging buffer flush threshold |

## Feature Flags

| Feature | Default | Description |
|---------|---------|-------------|
| `backend-rocksdb` | ✓ | RocksDB backend (links rocksdb crate) |
| `backend-flashcache` | — | FlashCache backend (compiles fc_shim.c, links libaio/librt) |
| `enable-system-alloc` | — | Use system allocator for unit tests |

---

## Performance Testing

See `benchmark/README.md` for the benchmark suite. Key scenarios:
- `mixed-rw` with `*-flashcache.env` configs: throughput under mixed read/write with tiering
- `tiering-latency`: pure flash fetch latency measurement
- Remote mode (`--remote`): run on EC2 with NVMe instance store

## Backend Comparison

| | RocksDB | FlashCache |
|---|---------|------------|
| IO threads | Multiple (default 4) | Single |
| Read model | Synchronous (blocking get) | Async (callback on AIO completion) |
| Write model | Synchronous | Synchronous (staging buffer) |
| Storage format | LSM tree (directory) | Log-structured (single file) |
| Bloom filter | Built-in (per-SST) | Bucket occupancy check |
| Dependencies | `rocksdb` crate | `libaio`, `librt`, FlashCache static lib |
| Use case | General purpose, high throughput | Low-latency NVMe, single-device |
