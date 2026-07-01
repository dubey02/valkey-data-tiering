# Pluggable Storage Backend — Design Document

## 1. Overview

The Valkey Data Tiering pluggable storage interface allows multiple storage backends (FlashCache, RocksDB, future engines) to be used interchangeably through a single C struct function-pointer API (`storageType`). The design supports **two deployment modes**:

- **Core (native)**: Backend compiled directly into the Valkey binary, selected via `--ext-storage-backend` config
- **Module**: Backend loaded dynamically as a Valkey module, registers via `ValkeyModule_RegisterStorageBackend()`

Both modes use the identical `storageType` interface with zero per-request dispatch overhead.

## 2. Architecture

```
┌─────────────────────────────────────────────────────────────────────┐
│                         Valkey Engine (main thread)                   │
│                                                                       │
│  ext_storage.c                    ext_storage_bridge.c                │
│  ┌─────────────────┐             ┌─────────────────────┐            │
│  │ Key State Machine│────────────▶│ Bridge Layer         │            │
│  │ (spill/fetch/    │             │ (translates robj*    │            │
│  │  completion)     │             │  to storageType API) │            │
│  └─────────────────┘             └──────────┬──────────┘            │
└─────────────────────────────────────────────┼────────────────────────┘
                                              │
                              ┌────────────────┼────────────────┐
                              ▼                ▼                ▼
                 ┌──────────────────┐ ┌──────────────┐ ┌──────────────────┐
                 │  Native Backend  │ │ Module Backend│ │ Shared Middleware │
                 │  (FlashCache)    │ │ (flash-tiering│ │ (for sync-only   │
                 │                  │ │  or rocksdb-  │ │  backends)        │
                 │  Own IO thread   │ │  tiering)     │ │                  │
                 │  Lock-free MPSC  │ │              │ │  IO thread pool  │
                 │  io_uring async  │ │  Own IO thread│ │  Serialization   │
                 └──────────────────┘ │  crossbeam    │ └──────────────────┘
                                      └──────────────┘
```

## 3. The `storageType` Interface

```c
typedef struct storageType {
    const char *name;
    int version;

    /* Lifecycle */
    void *(*open)(storageConfig *cfg);
    void (*close)(void *ctx);

    /* Sync KV ops — called from middleware IO thread (NULL if async-only) */
    storageStatus (*put)(void *ctx, uint32_t db_id, const void *key, size_t klen,
                         const void *value, size_t vlen, int64_t expire_ms);
    storageStatus (*get)(void *ctx, uint32_t db_id, const void *key, size_t klen,
                         void **value, size_t *vlen, int64_t *expire_ms);
    storageStatus (*del)(void *ctx, uint32_t db_id, const void *key, size_t klen);

    /* Async KV ops — backend owns its IO (NULL = use middleware with sync ops) */
    storageStatus (*put_async)(void *ctx, uint32_t db_id, const void *key, size_t klen,
                               const void *value, size_t vlen, int64_t expire_ms,
                               void *request_ctx);
    storageStatus (*get_async)(void *ctx, uint32_t db_id, const void *key, size_t klen,
                               void *request_ctx);
    storageStatus (*del_async)(void *ctx, uint32_t db_id, const void *key, size_t klen,
                               void *request_ctx);

    /* Completion delivery — main thread polls this */
    int (*poll_completions)(void *ctx, int max);

    /* Optional periodic work */
    int (*cron)(void *ctx);
    void (*get_stats)(void *ctx, storageStats *out);
} storageType;
```

**Design decisions:**
- Dual sync/async: Simple backends implement only `put/get/del` and get a free IO thread pool from the shared middleware. Advanced backends implement `put_async/get_async/del_async` and manage their own IO.
- `request_ctx`: Opaque pointer round-tripped through the backend for completion correlation.
- `poll_completions`: Calls a registered `completion_fn` callback for each completed operation — no allocations on the hot path.

## 4. Core (Native) Path

### Configuration
```bash
valkey-server --ext-storage-enabled yes --ext-storage-backend flashcache
```

### Initialization Sequence
1. `main()` → `initServer()` → (modules not loaded yet)
2. `main()` → `moduleLoadFromQueue()` → (modules register if present)
3. `main()` → `extStorage_init()` → `extStorageBridge_init()`
4. Bridge checks `moduleHasRegisteredStorageBackend()`:
   - If YES → uses module-registered `storageType`
   - If NO → selects native backend based on `--ext-storage-backend` config
5. `storageInit(type, &cfg)` → calls `type->open()` → backend starts IO thread

### Native FlashCache Backend (`storage_flashcache_real.c`)
- **IO thread**: pthread with lock-free MPSC ring buffer (C11 atomics)
- **Request queue**: Fixed SPSC ring, 4096 slots, `memory_order_release/acquire`
- **Completion queue**: Fixed SPSC ring, 8192 slots
- **Wakeup**: pipe fd (1-byte write on submit, non-blocking read on IO thread)
- **Idle**: 50μs nanosleep when no pending work
- **Processing**: Batch dequeue all requests → serialize → call FlashCache → push completions → `flashcacheRunCronTasks()`
- **Serialization**: `extStorageSerializeKey/Value()` called ON the IO thread (not main thread)
- **Zero-copy spill**: Borrowed robj wraps original sds — no value memcpy

### Available Native Backends
| Backend | Config value | Type | Notes |
|---------|-------------|------|-------|
| FlashCache (real) | `flashcache` | Async | Production — io_uring, own IO thread |
| FlashCache (mock) | `flashcache-mock` | Async | Testing — in-memory hash map |
| RocksDB (sync) | `rocksdb` | Sync | Uses shared middleware IO pool |
| RocksDB (async) | `rocksdb-async` | Async | Own IO thread, own serialization |

## 5. Module Path

### Available Module Backends
| Module | Directory | Backend |
|--------|-----------|---------|
| `flash-tiering` | `modules/flash-tiering/` | FlashCache via Rust crossbeam + FFI |
| `rocksdb-tiering` | `modules/rocksdb-tiering/` | RocksDB via Rust bindings |

### Configuration
```bash
valkey-server --ext-storage-enabled yes \
  --loadmodule modules/flash-tiering/target/release/libflash_tiering.so \
  backend=flashcache db_path=/tmp/valkey-flash.db
```

### Registration Flow
1. Module `OnLoad` → calls `ValkeyModule_RegisterStorageBackend(ctx, &storage_type_struct)`
2. Engine stores the pointer in `module_registered_storage_type`
3. When `extStorage_init()` runs (after module load), bridge detects `moduleHasRegisteredStorageBackend() == true`
4. Bridge calls `storageInit()` on the module's `storageType` — module's `open()` stores the `completion_fn` callback
5. All subsequent `storageSubmitPut/Get/Del` calls go through module's function pointers

### Module `storageType` Implementation (`storage_type_bridge.rs`)
```rust
// C-compatible struct matching storage.h layout exactly
pub struct StorageType {
    pub name: *const c_char,
    pub version: c_int,
    pub open: Option<unsafe extern "C" fn(*mut StorageConfig) -> *mut c_void>,
    pub close: Option<unsafe extern "C" fn(*mut c_void)>,
    pub put: Option<...>,     // NULL — async-only
    pub get: Option<...>,     // NULL — async-only
    pub del: Option<...>,     // NULL — async-only
    pub put_async: Option<unsafe extern "C" fn(...)>,
    pub get_async: Option<unsafe extern "C" fn(...)>,
    pub del_async: Option<unsafe extern "C" fn(...)>,
    pub poll_completions: Option<unsafe extern "C" fn(*mut c_void, c_int) -> c_int>,
    pub cron: Option<...>,
}
```

The module's `nks_put_async`/`nks_get_async` push requests to a crossbeam `SegQueue`, and the IO worker thread drains them, serializes, calls FlashCache FFI, and pushes completions. `nks_poll_completions` drains the completion queue and invokes the engine's `completion_fn`.

### Module IO Architecture
- **Request queue**: crossbeam `SegQueue` (unbounded, lock-free MPMC)
- **Completion queue**: crossbeam `SegQueue`
- **IO worker**: Rust `std::thread` with batch processing (`while let Some(req) = queue.pop()`)
- **Serialization**: Same `extStorageSerializeKey/Value` via `extern "C"` FFI calls

## 6. Dispatch Layer (`storage_dispatch.c`)

The dispatch layer is the single point where the engine submits operations:

```c
storageStatus storageSubmitPut(uint32_t db_id, const void *key, size_t klen,
                               const void *value, size_t vlen,
                               int64_t expire_ms, void *request_ctx) {
    if (server_storage->put_async) {
        // Direct call to backend's async function pointer
        return server_storage->put_async(server_storage_ctx, db_id,
                                         key, klen, value, vlen,
                                         expire_ms, request_ctx);
    }
    // Fallback: submit to shared middleware (for sync-only backends)
    return storageMiddlewareSubmit(STORAGE_OP_PUT, ...);
}
```

**Key property**: When a backend implements `put_async`, the dispatch is a single function pointer call — zero overhead vs calling the backend directly.

## 7. Shared Middleware (`storage_middleware.c`)

For backends that only implement sync `put/get/del`:
- Provides a thread pool (configurable `io_threads`)
- Dequeues requests from a shared queue
- Calls the backend's sync functions on IO threads
- Pushes completions via the registered `completion_fn`

This allows simple backends (e.g., a basic RocksDB wrapper) to get async behavior for free without managing their own threads.

## 8. Bridge Layer (`ext_storage_bridge.c`)

Translates between the engine's robj/sds world and the storageType's opaque bytes world:

- **Submit**: Creates `bridgeRequestCtx` (key sds dup + metadata), wraps robj* as void* for the backend
- **Completions**: Polls backend via `storagePollCompletions()`, translates `storageCompletion` structs into `ValkeyModuleExternalStorageMsg` for the state machine in `ext_storage.c`
- **Module detection**: Checks `moduleHasRegisteredStorageBackend()` at init time to select module vs native

## 9. Key Design Principles

1. **Zero dispatch overhead**: Function pointer call, not vtable/interface dispatch
2. **Engine never serializes**: robj references passed through; IO thread converts to bytes
3. **Rehash paused during in-flight**: Prevents sds pointer invalidation while IO thread holds references
4. **FlashCache single-threaded**: All FlashCache API calls on one IO thread (not thread-safe)
5. **Module and native independently functional**: Either can be used at any time via config
6. **Completion-driven**: No polling from main thread — completions delivered via callback registered at `open()` time

## 10. Performance Characteristics

| Path | SET/s (pressure) | GET/s (pressure) | Queue |
|------|-----------------|-----------------|-------|
| Native (C lock-free) | 47,000 | 30,000 | SPSC ring 4096/8192 |
| Module (Rust crossbeam) | 50,000 | 35,000 | Unbounded SegQueue |

- Pre-pressure throughput: ~52K SET/s (both paths identical)
- Module ~17% faster under sustained pressure due to unbounded queue (no ring overflow stalls)
- Both pass 10M-operation stress tests with zero crashes

## 11. Adding a New Backend

### Option A: Sync-only (simplest)
Implement `put/get/del`, leave `put_async/get_async/del_async` as NULL. The middleware provides IO threads.

### Option B: Async with own IO (production)
Implement `put_async/get_async/del_async/poll_completions`. Manage your own IO thread and call the `completion_fn` registered via `open(cfg)`.

### Option C: As a loadable module
1. Create a Rust/C module implementing the `StorageType` struct
2. Call `ValkeyModule_RegisterStorageBackend()` during `OnLoad`
3. Implement `put_async/get_async/del_async/poll_completions`
4. Load with `--loadmodule path/to/module.so [args]`

## 12. File Map

| File | Role |
|------|------|
| `src/storage/storage.h` | Interface definition (`storageType` struct + engine API) |
| `src/storage/storage_dispatch.c` | Dispatch layer — routes to backend or middleware |
| `src/storage/storage_middleware.c` | Shared IO thread pool for sync backends |
| `src/storage/storage_flashcache_real.c` | Native FlashCache backend (lock-free, io_uring) |
| `src/storage/storage_flashcache.c` | Mock FlashCache (in-memory, for testing) |
| `src/storage/storage_rocksdb.c` | Sync RocksDB backend |
| `src/storage/storage_rocksdb_async.c` | Async RocksDB backend (own IO thread) |
| `src/ext_storage_bridge.c` | Bridge: robj world ↔ storageType bytes world |
| `src/ext_storage.c` | Engine integration (state machine, spill/fetch logic) |
| `src/module.c` | `ValkeyModule_RegisterStorageBackend()` implementation |
| `modules/flash-tiering/` | Rust module: FlashCache via crossbeam + FFI |
| `modules/rocksdb-tiering/` | Rust module: RocksDB via Rust bindings |
