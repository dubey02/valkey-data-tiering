# Data Tiering — Non-Key-Spilling

This document covers the non-key-spilling data tiering feature for Valkey. Unlike key-spilling (where entire keys are evicted to disk), this approach keeps keys in memory and only spills values to external storage. This preserves keyspace semantics — EXISTS, TTL, SCAN, DBSIZE all work without disk access.

---

## How It Works

```
┌─────────────────────────────────────────────────────────────────┐
│                        Valkey Server                             │
│                                                                 │
│  Memory (maxmemory)          External Storage Module API        │
│  ┌──────────────┐           ┌──────────────────────────┐       │
│  │ Key → Value  │──spill──▶│ request_callback (WRITE)  │       │
│  │ Key → TIERED │◀─fetch───│ response_callback (READ)  │       │
│  │ (key stays!) │           └────────────┬─────────────┘       │
│  └──────────────┘                        │                      │
└──────────────────────────────────────────┼─────────────────────┘
                                           │
                    ┌──────────────────────┼──────────────────────┐
                    │     Storage Module (Rust .so)               │
                    │                      │                      │
                    │  ┌───────────────────▼────────────────┐     │
                    │  │         ASIO Layer                 │     │
                    │  │  (IO worker threads + queues)      │     │
                    │  └───────────┬───────────┬────────────┘     │
                    │              │           │                   │
                    │  ┌───────────▼──┐  ┌────▼──────────┐       │
                    │  │   RocksDB    │  │  FlashCache   │       │
                    │  │  (default)   │  │  (optional)   │       │
                    │  └──────────────┘  └───────────────┘       │
                    └─────────────────────────────────────────────┘
                                           │
                                    ┌──────▼──────┐
                                    │  NVMe Disk  │
                                    └─────────────┘
```

**Key difference from key-spilling**: Keys always remain in the dict. Only the serialized value is written to disk. On spill completion, the value robj is replaced with a lightweight "tiered marker" (encoding = OBJ_ENCODING_TIERED, val_ptr = empty SDS placeholder).

---

## Key Semantics

| Operation | Behavior on Tiered Key |
|-----------|----------------------|
| EXISTS | Returns 1 (key is in dict) — no disk access |
| TTL/PTTL | Returns correct TTL (metadata in memory) — no disk access |
| TYPE | Returns correct type (preserved in robj) — no disk access |
| SCAN | Includes tiered keys — no disk access |
| DBSIZE | Counts tiered keys — no disk access |
| GET/SET/etc. | Blocks client, fetches value from disk, then executes command |
| DEL | Blocks, fetches, then deletes normally |
| EXPIRE/PERSIST | Works on metadata — no disk access |

---

## State Machine

```
ONLY_MEMORY ──spill──▶ COPYING_TO_FLASH ──write_done──▶ ONLY_FLASH
     ▲                  (keys_spilling set)              (encoding=TIERED)
     │                                                        │
     │                                                        │ client accesses key
     └──fetch_done── COPYING_TO_MEMORY ◀──────────────────────┘
                     (keys_fetching set)
```

- `ONLY_MEMORY`: Normal robj with real value
- `COPYING_TO_FLASH`: Value copy sent to module, original still in memory, key in `keys_spilling`
- `ONLY_FLASH`: encoding = OBJ_ENCODING_TIERED, val_ptr = empty SDS
- `COPYING_TO_MEMORY`: Fetch in progress, key in `keys_fetching`, client blocked

---

## Memory Savings

Per tiered key, we keep in memory:
- Key SDS string (~key_len + 9 bytes)
- Dict entry (~48 bytes)
- Tiered marker robj (~16 bytes + empty SDS ~9 bytes)
- Expire metadata (if any)

Total overhead per tiered key: ~82 + key_len bytes
Savings: entire value freed from memory

Break-even: value_size > ~82 bytes (almost always true).

---

## Repository Layout

```
private-valkey-non-key-spilling/private-valkey/
├── src/                          # Valkey core
│   ├── server.h                  # OBJ_ENCODING_TIERED (15), objectIsTiered() macro
│   ├── ext_storage.c             # Spill/fetch logic, preCommandExec blocking
│   ├── ext_storage_throttle.c    # Token bucket throttling
│   ├── object.c                  # objectComputeSize, decrRefCount (tiered-aware)
│   ├── defrag.c                  # Skips tiered entries
│   └── rdb.c                     # Skips tiered entries during save
├── modules/
│   └── key-spilling/             # Storage module (Rust)
│       ├── src/
│       │   ├── common/           # Shared types, traits, serialization
│       │   ├── backends/
│       │   │   ├── rocksdb/      # Multi-threaded sync backend
│       │   │   └── flashcache/   # Single-threaded async backend
│       │   ├── dispatcher.rs     # Feature-gated backend routing
│       │   ├── callbacks.rs      # Valkey FFI callbacks (no bloom filter)
│       │   └── lib.rs            # Module entry point
│       └── Cargo.toml
└── DATA-TIERING.md               # ← You are here
```

---

## Quick Start

### 1. Build Valkey

```bash
cd private-valkey-non-key-spilling/private-valkey
make -j8
```

### 2. Build the Module

```bash
cd modules/key-spilling
cargo build --release
```

### 3. Run

```bash
./src/valkey-server \
    --loadmodule modules/key-spilling/target/release/libkey_spilling_module.so \
        backend=rocksdb db_path=/tmp/tiering-db \
    --maxmemory 100mb \
    --maxmemory-policy allkeys-lru
```

### 4. Test

```bash
./src/valkey-cli

# Fill memory to trigger spilling
> DEBUG POPULATE 500000 key: 200

# Keys stay in memory (non-key-spilling!)
> DBSIZE
(integer) 500000

# EXISTS works without disk access
> EXISTS key:000000000001
(integer) 1

# GET fetches value from disk transparently
> GET key:000000000001
"..."
```

---

## Differences from Key-Spilling

| | Key-Spilling | Non-Key-Spilling |
|---|---|---|
| Key in memory after spill | No (deleted from dict) | Yes (always in dict) |
| EXISTS on spilled key | Requires bloom filter check | Always returns 1 |
| TTL on spilled key | Requires disk fetch | Works from memory |
| SCAN includes spilled keys | No | Yes |
| DBSIZE counts spilled keys | No | Yes |
| Bloom filter needed | Yes (to check if key is on disk) | No |
| Memory per spilled key | 0 (key gone) | ~82 + key_len bytes |
| Best for | Large keys, low hit rate | Small keys, metadata-heavy workloads |

---

## Configuration

```conf
ext-storage-enabled yes
maxmemory 1gb
maxmemory-policy allkeys-lru

loadmodule /path/to/libkey_spilling_module.so \
    backend=rocksdb \
    db_path=/mnt/nvme/tiering-db \
    num_databases=16 \
    max_in_flight_reads=128 \
    num_io_threads=4
```

### Module Arguments

| Key | Required | Default | Description |
|-----|----------|---------|-------------|
| `backend` | yes | — | `rocksdb` or `flashcache` |
| `db_path` | yes | — | Storage path |
| `db_size_bytes` | no | 1GB | Max storage size |
| `num_databases` | no | 16 | Valkey logical databases |
| `max_in_flight_reads` | no | 128 | Max concurrent reads before throttling |
| `num_io_threads` | no | 4 | RocksDB worker threads |
| `compression_type` | no | none | none/snappy/lz4/zstd |

---

## Monitoring

```bash
valkey-cli INFO external_storage
```

Key metrics:
- `total_num_items_spilled_to_ext_storage` — values written to disk
- `total_num_items_fetched_from_ext_storage` — values read back
- `num_items_spilling_to_ext_storage` — in-flight writes
- `num_items_fetching_from_ext_storage` — in-flight reads
- `kbc_in_memory` — commands that found value in memory (fast path)
- `kbc_key_may_exist_true` — commands that triggered a disk fetch
- `throttle_current_rate` — current throttle level (0.0 = no throttle)

---

## RDB/AOF Behavior

- **RDB save**: Tiered keys are skipped (value is on external storage). On restart, tiered keys are lost unless the module restores them from its persistent store.
- **AOF**: Commands are logged normally. Tiering is transparent to the AOF — SET/GET/DEL are recorded as usual.
- **Replication**: Tiered keys are not replicated via RDB. The replica must have its own storage module instance.

---

## Known Limitations

1. Tiered keys are lost on restart (RDB doesn't include them)
2. DEL on tiered keys fetches the value first (could be optimized to delete without fetch)
3. No garbage collection for orphaned data on disk after DEL
4. Only string values are currently supported for spilling
5. Embedded objects (EMBSTR, INT encoding) cannot be spilled

---

## Branch Info

- **Branch**: `data-tiering-non-key-spilling`
- **Base**: Forked from `data-tiering-key-spilling`
- **Key changes from key-spilling branch**:
  - `ext_storage.c`: WRITE keeps key in dict, READ restores to existing entry
  - `server.h`: Added OBJ_ENCODING_TIERED (15), objectIsTiered() macro
  - `object.c`: decrRefCount/objectComputeSize handle tiered entries
  - `defrag.c`: Skips tiered entries
  - `rdb.c`: Skips tiered entries
  - Module: Bloom filter removed (key_may_exist always returns 0)
