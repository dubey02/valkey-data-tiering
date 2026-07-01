# Middleware & IO Architecture Decision

## Final Decision: Hybrid (Shared Middleware with Backend Opt-Out)

### What This Means

```
Engine (main thread)
    │
    ├── if backend has put_async → call directly (backend owns IO)
    └── else → submit to shared middleware (engine owns IO)
```

### Deployment Configurations

| Mode | Middleware | Storage | Example |
|------|-----------|---------|---------|
| Native + shared middleware | Engine provides | Backend is sync library | RocksDB, CacheLib |
| Native + own IO | Backend provides | Backend is async library | FlashCache (io_uring) |
| Module + shared middleware | Engine provides | Module implements sync put/get/del | Simple 3rd-party module |
| Module + own IO | Module provides | Module implements async put_async/get_async | Advanced module with custom IO |

### Key Facts That Drove the Decision

1. **flashcacheGetItem does NOT block** — it submits to io_uring and returns immediately with completion via callback. So FlashCache CANNOT use a shared sync middleware — it needs its own async path.
2. **Modules should have choice** — forcing all modules through shared middleware limits what advanced module authors can do (e.g., custom io_uring, SPDK, kernel bypass).
3. **Simple modules shouldn't need to implement threading** — shared middleware gives them free async by just implementing sync functions.
4. **RocksDB/CacheLib are sync libraries** — wrapping them in the shared middleware is natural and adds no overhead.

### Options Considered

#### Option A: Shared Middleware Only
- All backends called synchronously from a shared IO thread pool
- **Rejected because**: FlashCache's read path is async (io_uring, non-blocking). Forcing it through sync middleware would require rewriting FlashCache's IO model or adding a blocking wait, losing the io_uring advantage. Also prevents modules from implementing their own optimized IO.

#### Option B: Per-Backend IO Only
- Each backend manages its own threads and async machinery
- **Rejected because**: Forces every module author to implement thread pools, queues, and completion delivery. Too much boilerplate for simple backends like "store in S3" or "store in a custom file format."

#### Option C: Hybrid (CHOSEN)
- Shared middleware available for sync backends
- Async backends bypass middleware and manage their own IO
- **Chosen because**: Gives FlashCache its optimal io_uring path, gives RocksDB/simple modules free async via middleware, and lets advanced modules own their IO if they want.

### How Other Systems Compare

| System | Pattern | Notes |
|--------|---------|-------|
| Linux block layer | Closest to hybrid | Generic layer + blk-mq bypass for NVMe |
| MySQL/PostgreSQL | Shared only | All engines go through same buffer manager |
| TiKV/CockroachDB | Per-backend only | Each owns IO via async runtime |
| SPDK | Per-backend only | Polling, no shared middleware |

No database does exactly this hybrid pattern. Linux block layer is the closest analogy (generic path + fast bypass for capable hardware).

### Complexity Assessment
- **Code complexity**: Low — one if/else per operation (3 total)
- **Cognitive complexity**: Moderate — two paths to understand
- **Performance impact**: Zero — branch prediction learns immediately, unused path is idle
- **Testing**: Must test both paths

### Interface Implications
```c
typedef struct storageType {
    /* Sync path (called from middleware IO thread) */
    storageStatus (*put)(void *ctx, ...);
    storageStatus (*get)(void *ctx, ...);
    storageStatus (*del)(void *ctx, ...);

    /* Async path (backend owns IO) — NULL means use sync via middleware */
    storageStatus (*put_async)(void *ctx, ..., void *request_ctx);
    storageStatus (*get_async)(void *ctx, ..., void *request_ctx);
    storageStatus (*del_async)(void *ctx, ..., void *request_ctx);
    int (*poll_completions)(void *ctx, int max);

    /* ... lifecycle, iterator, etc ... */
} storageType;
```

### Serialization
- Provided as a **shared utility** (storage_serialize.c)
- Middleware calls it automatically for sync backends
- Async backends call it themselves on their IO thread
- Both paths use the same serialize/deserialize functions
