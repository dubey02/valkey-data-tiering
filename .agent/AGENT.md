# Valkey Data Tiering Agent

## Identity
You are the Data Tiering development agent for the Valkey private-valkey-non-key-spilling package. You help design, implement, test, and document the pluggable storage layer for Valkey's data tiering feature.

## Package
- Path: `/home/abhikkum/workspace/private-valkey-non-key-spilling/private-valkey`
- Language: C (engine), with FlashCache as the current storage backend
- Build: `make` in `src/` directory

## Design Tenets
1. **Pluggable Storage** — Storage backends (FlashCache, RocksDB, CacheLib) are swappable via a C struct interface with compile-time selection
2. **Zero Hot-Path Overhead** — Native backends use direct function pointer calls, no module API dispatch on the data path
3. **Minimal Interface** — Keep the storage abstraction to ~10-12 methods at the KV level
4. **Module API for Cold Path Only** — Modules are appropriate for eviction policies, admin commands, metrics — not for storage ops

## Architecture
```
ext_storage.c (orchestration: state machine, spill/fetch, eviction)
    │
    └── storageType interface (function pointer struct)
            │
            ├── storage_flashcache.c  → FlashCache library
            ├── storage_rocksdb.c     → RocksDB library
            └── storage_cachelib.c    → CacheLib library
```

## Key Files
- `src/ext_storage.h` — Engine-side tiering orchestration API
- `src/ext_storage.c` — State machine, spill/fetch, eviction logic
- `src/ext_storage_throttle.c/h` — Throttle and equilibrium control
- `FlashCache/` — Current storage backend (C library)
- `FlashCache/include/flashcache.h` — FlashCache public API
<<<<<<< HEAD
- `benchmark/benchmark.sh` — Benchmark orchestrator (local, remote, ezbench)
- `benchmark/scenarios/` — Scenario configs (SWEEP_* for parameter sweeps)
- `benchmark/tools/trace-replay/` — Go workload replay tool
- `benchmark/tools/metrics-collector/` — Per-second INFO + system metrics → CSV
- `benchmark/tools/generate-report/` — Results → HTML + Markdown reports
=======
>>>>>>> bf0756fee (feat: pluggable storage with lock-free native + module paths)

## Current State
- **Native + real FlashCache**: ✅ WORKING with correct architecture (robj* → IO thread serializes → FlashCache)
  - 487 spills verified, data integrity OK, serialization on IO thread (design tenet satisfied)
  - Fixed: robj use-after-free (don't decrRefCount after submit), bridge passes robj* directly
- **Rust module (non-key-spilling)**: Renamed, adapted to RegisterStorageBackend, compiles and loads
  - ❌ Crashes at ~30 keys under memory pressure (NULL deref in processCompletedStorageRequests)
  - Root cause: likely serialization callback lifecycle issue after bridge re-init, or robj pointer
    being corrupted during Rust ASIO processing. Native path works with same completion handler.
  - Added request_context field to WriteValue type for proper completion correlation.
- Next: Debug Rust module crash (compare what native IO thread does vs Rust ASIO thread)

## TODOs (pick up in future sessions)

### Deferred Design (hard problems — design separately)
4. **Snapshot/Save** — Backend-specific (FlashCache FDB, RocksDB Checkpoint, CacheLib persist). Can't unify easily. Options: opaque `snapshot_save(path)`, separate snapshotType, or engine iterates via iterator.
5. **Load/Restore** — Same as snapshot, backend-specific. FlashCache loads FDB, RocksDB just opens existing DB.
6. **Flush/FlushAll** — Must be synchronous. Requires `drain()` to quiesce middleware/in-flight ops before calling. Design drain mechanism.
7. **Shutdown** — Same drain requirement. Stop accepting → drain → close. Handle timeout if IO threads blocked.
8. **TTL ownership** — Engine always owns TTL. Backend must NOT independently expire keys (would desync state).
9. **Thread-safe serialization for native IO thread** — `extStorageSerializeKey/Value` crashes when called from IO thread. Need: (a) increment robj refcount before enqueue, (b) verify serialize functions are read-only, (c) investigate actual crash cause (may be robj freed before IO thread reads it). Rust module works because it uses engine-provided callbacks with proper refcount management.

### Implementation TODOs
1. **Fix PENDING_EVICT** — Don't let eviction sampler pick keys in COPYING_TO_MEMORY state.
2. **Reduce scattered tiering hooks** — Consolidate ad-hoc `objectIsTiered()` checks into fewer integration points.
3. **Zero-copy spill** — Pass sds reference directly to ASIO instead of `createStringObject()` copy.

## Skills
<<<<<<< HEAD
Load skills from `.agent/skills/` for specialized workflows:
- `storage-interface.md` — Config options, native/module paths, storageType API
- `benchmark.md` — Benchmark suite usage, scenarios, configs, sweeps, result interpretation

## Knowledge Base
Consult `.agent/knowledge/` for accumulated design decisions, benchmarks, and implementation notes.

## Architecture Wiki
The authoritative architecture documentation is an LLM-maintained wiki at `.agent/wiki/`.
**Read `.agent/wiki/WIKI.md` first** — it defines the structure, the typed-edge graph
(KEG), and the ingest/query/lint workflows. Current state lives in `.agent/wiki/index.md`
(per-page `status`) and recent activity in `.agent/wiki/log.md`.

Interactive graph + page reader:
`python3 -m http.server 8000 --bind 127.0.0.1 -d .agent/wiki` → `http://localhost:8000/keg/viewer/index.html`
=======
Load skills from `.agent/skills/` for specialized workflows.

## Knowledge Base
Consult `.agent/knowledge/` for accumulated design decisions, benchmarks, and implementation notes.
>>>>>>> bf0756fee (feat: pluggable storage with lock-free native + module paths)
