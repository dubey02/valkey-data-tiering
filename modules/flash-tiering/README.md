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

## Running on ezBench (Internal Performance Testing)

ezBench is the internal benchmarking framework that provisions EC2 instances, deploys binaries, and runs workloads automatically. The key-spilling module has a ready-to-use ezbench test configuration.

### Overview

```
Target (r6gd.2xlarge):  Valkey + key-spilling module on NVMe
Clients/SET (c6g.2xlarge × 2):  redis-benchmark writing keys
Clients/GET (c6g.2xlarge × 8):  redis-benchmark reading keys
Controller (m6g.2xlarge):  Orchestrates the test
```

### Prerequisites

1. **ezbench CLI** installed (via Apollo: `ezBench/Beta`)
2. **Midway auth**: `kinit && mwinit -o`
3. **AWS credentials**: turtle auto-rotates for account `833348497722`
4. **ARM binaries** built (r6gd is Graviton/aarch64)

### Step-by-Step

#### 1. Build ARM binaries (on aarch64 dev-dsk or cross-compile)

```bash
# Build Valkey
cd ~/workspace/private-valkey
make -j8

# Build key-spilling module
cd modules/key-spilling
cargo build --release --features backend-flashcache
```

#### 2. Copy binaries to the ezbench files/ directory

```bash
cd ~/workspace/key-spilling/perf/tiering-flashcache

# Copy valkey binaries (renamed to redis-* for ezbench compatibility)
cp ~/workspace/private-valkey/src/valkey-server  files/redis-server
cp ~/workspace/private-valkey/src/valkey-cli     files/redis-cli
cp ~/workspace/private-valkey/src/valkey-benchmark files/redis-benchmark

# Copy the module .so
cp ~/workspace/private-valkey/modules/key-spilling/target/release/libkey_spilling_module.so files/

# Copy libvalkeylua.so (required for server startup)
cp ~/workspace/private-valkey/src/modules/lua/libvalkeylua.so files/

# Verify architecture
file files/redis-server  # Should show: ELF 64-bit LSB ... ARM aarch64
```

#### 3. Configure the test (test.ini)

The `test.ini` in `key-spilling/perf/tiering-flashcache/test.ini` is pre-configured:

```ini
[general]
test_name=key-spilling-flashcache
instance_type=r6gd.2xlarge    # ARM + NVMe

[redis.conf]
maxmemory=1g
maxmemory-policy=allkeys-lru
ext-storage-enabled=yes
loadmodule=/opt/ezbench/files/libkey_spilling_module.so backend=flashcache db_path=/dev/nvme1n1 db_size_bytes=426599997440 num_databases=16 max_in_flight_reads=128 initial_index_size=1048576
```

Key settings to adjust:
- `maxmemory` — controls when eviction/spill starts (1g = aggressive spilling)
- `db_size_bytes` — should match NVMe capacity (~400GB for r6gd.2xlarge)
- `initial_index_size` — FlashCache hash index slots (1M for large keyspaces)

For **RocksDB** backend, change the loadmodule line:
```ini
loadmodule=/opt/ezbench/files/libkey_spilling_module.so backend=rocksdb db_path=/mnt/nvme/ks-db num_databases=16 max_in_flight_reads=128 num_io_threads=4
```
(Note: RocksDB needs a filesystem, so mount NVMe first in `target/setup.sh`)

#### 4. Run the test

```bash
cd ~/workspace/key-spilling/perf/tiering-flashcache

# Option A: One-command run
./run.sh

# Option B: Manual ezbench invocation
ezbench --run --wait --log-level DEBUG
```

#### 5. Monitor and collect results

- **Email**: Results sent to the address in `test.ini [email]` section
- **S3**: Logs uploaded to `s3://ezbench-833348497722/key-spilling-flashcache/logs/<stack>/`
- **SSH**: While running, SSH to target via the IP shown in ezbench output:
  ```bash
  ssh -i ~/.ssh/ezbench-ssh-key ec2-user@<TARGET_IP>
  /opt/ezbench/files/redis-cli INFO all
  /opt/ezbench/files/redis-cli INFO external_storage
  ```

### Test Flow (what happens automatically)

1. **CloudFormation** provisions target + client instances
2. **target/setup.sh** runs on target:
   - Installs `libaio`, tunes kernel (overcommit, THP, TCP buffers)
   - Sets NVMe permissions (`chmod 777 /dev/nvme1n1`)
   - Starts Valkey with the module loaded
   - Starts crash monitor (uploads logs to S3 if server dies)
3. **client/setup.sh** runs on each client
4. **client/pre-test.sh** warms up: fills 1M keys to trigger spilling
5. **client/run-test.sh** runs the benchmark:
   - SET clients: 2 processes × 5M requests each
   - GET clients: 2 processes × 5M requests each
   - 100M key keyspace, 50-byte values
6. **client/post-test.sh** collects final metrics
7. **ezbench** tears down the stack and emails results

### Directory Layout

```
key-spilling/perf/tiering-flashcache/
├── test.ini                 # ezbench configuration
├── test.yml                 # (optional) YAML variant
├── run.sh                   # One-command launcher
├── copy-binaries.sh         # Copies built binaries to files/
├── files/                   # Binaries deployed to /opt/ezbench/files/
│   ├── redis-server         # valkey-server (renamed)
│   ├── redis-cli
│   ├── redis-benchmark
│   ├── libkey_spilling_module.so
│   └── libvalkeylua.so
├── target/
│   └── setup.sh             # Target machine setup + server start
├── client/
│   ├── setup.sh             # Client machine setup
│   ├── pre-test.sh          # Warmup (fill keys)
│   ├── run-test.sh          # Main benchmark workload
│   └── post-test.sh         # Collect results
├── private/
│   └── common.rc            # Shared shell functions
└── reports/                 # Results collected here
```

### Switching to RocksDB Backend on ezBench

To test the RocksDB backend instead of FlashCache:

1. Edit `test.ini` `[redis.conf]` section:
   ```ini
   loadmodule=/opt/ezbench/files/libkey_spilling_module.so backend=rocksdb db_path=/mnt/nvme/ks-db num_databases=16 max_in_flight_reads=128 num_io_threads=4
   ```

2. Add NVMe filesystem mount to `target/setup.sh` (before server start):
   ```bash
   sudo mkfs.ext4 /dev/nvme1n1
   sudo mkdir -p /mnt/nvme
   sudo mount /dev/nvme1n1 /mnt/nvme
   sudo chmod 777 /mnt/nvme
   ```

3. Build without FlashCache feature:
   ```bash
   cargo build --release  # default = backend-rocksdb only
   ```

### Troubleshooting ezBench Runs

| Symptom | Fix |
|---------|-----|
| "Module failed to load" | Check `file redis-server` matches target arch (aarch64 vs x86_64) |
| "libvalkeylua.so not found" | Ensure it's in `files/` and `target/setup.sh` copies it to `/usr/lib64/` |
| Server crashes immediately | SSH to target, check `/opt/ezbench/logs/valkey.log` and `valkey-stderr.log` |
| "Permission denied" on NVMe | `target/setup.sh` should `chmod 777 /dev/nvme1n1` |
| Low throughput | Check `INFO external_storage` for throttling; increase `max_in_flight_reads` |
| Stack stuck in CREATE_IN_PROGRESS | Check CloudFormation console in `eu-west-1`, account `833348497722` |

---

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
