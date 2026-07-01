# Pluggable Storage Design Decisions

## Decision: Interface Style
**Chosen**: C struct of function pointers (like SQLite VFS, Linux block layer, CockroachDB Engine)
**Rejected**: Valkey Module API (6.7μs/request overhead, 2x TPS gap), C++ virtual classes, Rust traits with FFI

## Decision: Pluggability Model
**Chosen**: Compile-time selection (`make STORAGE=rocksdb`)
**Not needed**: Runtime hot-swap, dlopen at startup
**Rationale**: User said "doesn't need to be runtime pluggable, just swappable per deployment"

## Decision: Abstraction Level
**Chosen**: KV-level (get/put/del by key)
**Not**: File-level (too low, like SQLite VFS) or Row-level (too high, like MySQL handler)
**Rationale**: Engine spills/fetches opaque serialized blobs by key — that's the contract

## Decision: Module API Role
**Chosen**: Cold-path only (eviction hooks, admin commands, metrics exporters)
**Not for**: Storage backend ops (hot path)
**Rationale**: Perf data proves module dispatch costs 6.7μs/request on main thread

## Decision: Benchmark Experiment
**Plan**: Same interface, two implementations (native vs module-backed), same RocksDB underneath
**Goal**: Isolate exact cost of module dispatch for storage ops vs other architectural differences
**Prediction**: Module path will be 30-50% slower (not full 2x, since prior gap included Rust FFI + bloom filter)

## Precedents Studied
| System | Interface | Plug Method | Level |
|--------|-----------|-------------|-------|
| TiKV | Rust trait KvEngine | Compile-time generic | KV |
| CockroachDB | Go Engine interface | Build-time swap | KV |
| FoundationDB | C++ IKeyValueStore | Config knob at startup | KV |
| etcd | Go Backend interface | Build-time | KV |
| SQLite | C struct (VFS) | Register at init | File |
| Linux | C struct (block_device_ops) | obj-y or obj-m | Block |
| PostgreSQL | C struct (TableAmRoutine) | Extension .so | Tuple |
| MySQL | C++ virtual (handler) | Compile or INSTALL PLUGIN | Row |

## Interface Requirements (from FlashCache API analysis)
1. Async reads (callback-based) — FlashCache uses `flashcacheGetItem` with completion callback
2. Sync writes — `flashcachePutItem` is synchronous
3. Per-DB isolation — `dbid` parameter on all ops
4. Cron/background work — GC, processing completions (`flashcacheRunCronTasks`)
5. Snapshot/replication — file-based and stream-based save/load
6. Metrics — count-based and histogram
7. Flush/teardown lifecycle

## Resolved Questions

### Middleware/IO model: Hybrid (shared middleware with backend opt-out)
**Decision**: Interface supports both sync and async. If backend implements `put_async/get_async/poll_completions`, engine calls directly (backend owns IO). If only sync `put/get/del` provided, engine's shared middleware wraps them in a thread pool.
**Rationale**: FlashCache's `flashcacheGetItem` is non-blocking (io_uring, returns immediately). Can't force it through sync middleware. But RocksDB/CacheLib are sync — they benefit from free async wrapping. Modules get choice: simple modules implement sync only (free async), advanced modules implement async (own IO).
**Precedent**: Linux block layer (generic path + blk-mq bypass for NVMe) is closest analogy.

### Snapshot: iterator in storageType, snapshot ops separate
**Decision**: `iterator_create/next/destroy` is in storageType for replication/RDB. Full snapshot (FlashCache's FDB file) is backend-specific and not in the interface.
**Rationale**: Every backend can iterate its keys. But FlashCache's file-based snapshot is unique to its log-structured design. RocksDB would use its own checkpoint mechanism.

### Cron: optional in interface
**Decision**: `cron()` is in the interface but nullable. Called from beforeSleep. Returns 1 if more work needed.
**Rationale**: FlashCache needs it (GC, completion processing). RocksDB handles compaction internally. Making it optional covers both.

### Module support: shim adapter
**Decision**: A `storageGetModuleShimType()` returns a storageType where each fn ptr dispatches through the Module API. Same interface, different dispatch path.
**Rationale**: Enables the benchmark experiment (native vs module, same backend) and supports future third-party backends via modules.
