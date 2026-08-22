---
title: storageType vtable
status: active
sources:
  - src/storage/storage.h:1-186
  - src/storage/storage_dispatch.c:1-119
  - src/storage/storage_middleware.c:1-182
  - src/storage/storage_mock.c:82-418
  - src/storage/storage_flashcache_real.c:23-577
  - src/ext_storage_bridge.c:27-114
  - src/ext_storage.c:267-288
  - src/ext_storage.h:149-151
  - src/module.c:808-841
  - src/config.c:3372
  - src/Makefile:512-515
  - modules/rocksdb-tiering/src/lib.rs:1-194
  - modules/flash-tiering/src/lib.rs:82-201
updated: 2026-07-30
type: interface
tier: working
claim_count: 11
edges:
  - to: components/pluggable-storage-api.md
    kind: refers_to
    source: human
    created: 2026-06-03
  - to: components/backends.md
    kind: refers_to
    source: llm_relation
    created: 2026-06-04
  - to: components/serialization.md
    kind: refers_to
    source: llm_relation
    created: 2026-06-04
  - to: interfaces/bridge-api.md
    kind: refers_to
    source: llm_relation
    created: 2026-06-04
  - to: components/memory-accounting.md
    kind: refers_to
    source: llm_relation
    created: 2026-06-08
    note: storageCompletion.ram_bytes feeds the spill Smith predictor
---

# storageType vtable

> The pluggable-backend interface, `src/storage/storage.h` — the header the build actually
> compiles (`src/Makefile:512-515` lists the `storage/*.o` objects and `src/Makefile:771` builds them
> with the `-I` flag pointing at `storage`, so the in-directory `#include "storage.h"` at
> `src/storage/storage_dispatch.c:7` resolves here; `src/ext_storage_bridge.c:30` and
> `src/module.c:816` include it by the explicit `storage/storage.h` path). One struct serves both
> **sync** backends (engine wraps them via the shared middleware) and **async** backends (backend
> owns its IO). Dispatch rule: **`put_async != NULL` → async path; else the sync `put/get/del` go
> through the middleware** (`src/storage/storage.h:3` top comment + `src/storage/storage.h:104`
> async-ops note; enforced in `src/storage/storage_dispatch.c:32-36` and at each submit,
> `src/storage/storage_dispatch.c:53-54`, `src/storage/storage_dispatch.c:63-64`,
> `src/storage/storage_dispatch.c:72-73`; impl in
> [pluggable-storage-api](../components/pluggable-storage-api.md)).
> `version` must equal `VALKEY_STORAGE_VERSION` = 1 (`src/storage/storage.h:24`).

> ⚠️ CITATION HAZARD: a second, **uncompiled** header `src/storage.h` (247 lines) also exists and
> declares an older, larger variant of this interface. Nothing includes it — every
> `#include "storage.h"` in the tree is from inside `src/storage/`. Always cite this page's header
> by its full path `src/storage/storage.h:NN`; a bare `storage.h:NN` (and a bare `:NN`) resolves to
> the wrong file.

When built inside Valkey (with `STORAGE_USE_ZMALLOC` defined), `storage_malloc` /
`storage_calloc` / `storage_free` map to `zmalloc` (`src/storage/storage.h:11-22`).

## Status codes (`src/storage/storage.h:27-34`)

| Code | Val |
|------|-----|
| `STORAGE_OK` | 0 |
| `STORAGE_NOT_FOUND` | 1 |
| `STORAGE_WOULDBLOCK` | 2 (async op submitted; completion comes later) |
| `STORAGE_ERR_IO` | -1 |
| `STORAGE_ERR_FULL` | -2 |
| `STORAGE_ERR_REJECTED` | -3 (backend at capacity / throttled) |

Op-type constants `STORAGE_OP_PUT/GET/DEL = 0/1/2` (`src/storage/storage.h:37-39`), plus
`STORAGE_OP_BARRIER` = 3 — a drain barrier that performs no IO and only signals completion
(`src/storage/storage.h:40`).

## Completion record (`storageCompletion`, `src/storage/storage.h:43-54`)

Delivered IO-thread → main-thread: `request_ctx` (opaque echo of the submit), `op_type`,
`status`, `db_id`, `key`/`klen`, and for GET the `value`/`vlen` (**caller takes ownership;
malloc'd**) plus `expire_ms`. A `ram_bytes` field (`src/storage/storage.h:53`) carries the spilled
value's in-RAM footprint, computed on the IO thread at serialize and debited by the main thread on
the WRITE completion (the spill controller's Smith-predictor accounting — see
[memory-accounting](../components/memory-accounting.md)). Delivery type `storageCompletionFn` is
`void (*)(storageCompletion *c, void *privdata)` (`src/storage/storage.h:56`) — note the
`privdata`.

## Config & stats

- `storageConfig` (`src/storage/storage.h:59-75`) — core fields `path`, `capacity_bytes`,
  `num_databases`, `io_threads`, `eviction_enabled` (`src/storage/storage.h:64`, `0` = noeviction:
  flash never deletes data), `completion_fn`, `completion_privdata`
  (`src/storage/storage.h:60-66`); then a FlashCache tuning block passed straight through to the
  backend (`src/storage/storage.h:67-74`): `index_size`, `max_allocated_percent` (GC trigger
  threshold), `max_in_flight_reads`, `min_gc_rate`, `max_gc_rate`, `max_buffered_write_size`,
  `buffered_write_flush_threshold`. The bridge fills this in at
  `src/ext_storage_bridge.c:89-98` — note `io_threads` is hardcoded to `1`
  (`src/ext_storage_bridge.c:93`) and `eviction_enabled` is derived from `server.maxmemory_policy`
  (`src/ext_storage_bridge.c:94`).
- `storageStats` (`src/storage/storage.h:78-83`): `total_puts`, `total_gets`, `total_dels`,
  `keys_stored`.

## vtable members (`storageType`, `src/storage/storage.h:86-150`)

| Group | Members | Notes |
|-------|---------|-------|
| identity | `name`, `version` | `src/storage/storage.h:87-88` |
| lifecycle | `open`/`close` | `src/storage/storage.h:91-92` |
| sync KV | `put`/`get`/`del` | called from middleware IO thread; NULL if async-only (`src/storage/storage.h:94-102`) |
| async KV | `put_async`/`get_async`/`del_async` | backend owns IO; NULL = use middleware (`src/storage/storage.h:104-114`) |
| poll | `poll_completions` | required iff async; main thread (`src/storage/storage.h:115`) |
| optional | `cron`, `get_stats` | `src/storage/storage.h:118-119` |
| snapshot | `snapshot_hold`, `snapshot_release`, `gc_pause`, `fork_read` | all four optional; NULL = backend does not support snapshotting (`src/storage/storage.h:121-149`) |

The snapshot group supports fork-based RDB save with tiered values
(`src/storage/storage.h:121-125`): `snapshot_hold` (`src/storage/storage.h:131`) parks the backend IO
thread at a safe point holding no locks, called immediately before `fork()`; `snapshot_release`
(`src/storage/storage.h:134`) releases it; `gc_pause` (`src/storage/storage.h:139`) freezes
on-storage garbage collection so existing item locations stay stable for the child's lifetime;
`fork_read` (`src/storage/storage.h:147-149`) is a synchronous, fork-child-safe read of one item's
stored value bytes that never touches the async IO path (also callable on the parent main thread
while the IO thread is held, for foreground SAVE). A backend leaving them NULL keeps the engine's
fail-loudly snapshot gates (`src/storage/storage.h:123-124`).

Beyond the groups above the vtable is deliberately small — no drain, iterators, flush, config
getters, `exists`, or explicit sync member exists here.

## Engine dispatch API (`src/storage/storage.h:155-174`)

`storageInit(type, cfg)` (`src/storage/storage.h:155`), `storageShutdown()`
(`src/storage/storage.h:156`), `storageSubmitPut/Get/Del` (`src/storage/storage.h:157-163`),
`storagePollCompletions(max)` (`src/storage/storage.h:164`), `storageCron()`
(`src/storage/storage.h:174`) — these encapsulate the async-vs-middleware choice so callers never
branch on backend type: `storageInit` only spins up the middleware when `type->put_async` is NULL,
defaulting to 2 threads when `cfg->io_threads <= 0` (`src/storage/storage_dispatch.c:32-36`;
middleware impl in `src/storage/storage_middleware.c:1-182`). The snapshot hooks are mirrored the same
way: `storageSnapshotSupported()` (`src/storage/storage.h:168`), `storageSnapshotHold()`
(`src/storage/storage.h:169`), `storageSnapshotRelease()` (`src/storage/storage.h:170`),
`storageGcPause(paused)` (`src/storage/storage.h:171`), `storageForkRead(...)`
(`src/storage/storage.h:172-173`) — no-ops / `STORAGE_NOT_FOUND` when the active backend does not
implement them (`src/storage/storage.h:166-167`), and `storageSnapshotSupported` requires
`fork_read`, `snapshot_hold` and `snapshot_release` all present
(`src/storage/storage_dispatch.c:95-97`). The `server_storage` / `server_storage_ctx` globals live
in the dispatch layer (`src/storage/storage_dispatch.c:10-11`); see
[pluggable-storage-api](../components/pluggable-storage-api.md).

## Backend registration (`src/storage/storage.h:176-179`)

The header declares exactly **three** engine-native getters:

| Getter | Backend | Path |
|--------|---------|------|
| `storageGetFlashCacheType` | in-memory hashtable mock, `.name = "flashcache"` (`src/storage/storage_mock.c:397`; getter `src/storage/storage_mock.c:414`) | async (mock implements the `*_async` ops + `poll_completions`, `src/storage/storage_mock.c:402-405`) |
| `storageGetFlashCacheRealType` | real FlashCache, `.name = "flashcache-real"`, links `libflashcache.a` (`src/storage/storage_flashcache_real.c:503`; getter `src/storage/storage_flashcache_real.c:544`) | async |
| `storageGetRocksDBAsyncType` | **alias — returns the same in-memory mock struct** (`src/storage/storage_mock.c:416-418`) | async |

> ⚠️ CORRECTION (2026-07-30): this page previously listed a fourth getter, **storageGetRocksDBType**
> ("RocksDB, sync via middleware"). No such symbol exists anywhere in `src/` or `modules/` — not in
> this header, not in the stale `src/storage.h` (which declares only `storageGetFlashCacheType`,
> `src/storage.h:245`), and not in any backend, test or module. There is **no engine-native RocksDB
> `storageType` and no engine-native sync backend at all**: all three getters above return async
> vtables, so on a stock build the sync `put/get/del` middleware path
> (`src/storage/storage_middleware.c:1-182`) is reachable only via a backend that a module registers.
> `storageGetRocksDBAsyncType` is a testing convenience, not RocksDB — its own comment says the
> `"rocksdb"` config value reuses the mock "to avoid duplicating 300 lines of identical hashtable
> code" (`src/storage/storage_mock.c:416-417`).

Real RocksDB is a **module**, not an engine backend: `modules/rocksdb-tiering` builds a Rust
`storageType` and registers it through `ValkeyModule_RegisterStorageBackend`
(`modules/rocksdb-tiering/src/lib.rs:4`; registration `modules/rocksdb-tiering/src/lib.rs:145-151`;
module name `"rocksdb-tiering"` at `modules/rocksdb-tiering/src/lib.rs:187`).
`modules/flash-tiering` uses the same API (`modules/flash-tiering/src/lib.rs:132-164`) and keeps the
legacy `SubscribeToExternalStorage` path only for reference
(`modules/flash-tiering/src/lib.rs:170-201`).

Selection order, at `src/ext_storage_bridge.c:69-79` — **a module-registered backend always wins**,
otherwise the `ext-storage-backend` string config (`src/config.c:3372`) is matched:

| Condition | vtable used |
|-----------|-------------|
| `moduleHasRegisteredStorageBackend()` | the module's struct via `moduleGetRegisteredStorageBackend()` (`src/ext_storage_bridge.c:70-72`) |
| name `"flashcache"` | `storageGetFlashCacheRealType()` (`src/ext_storage_bridge.c:73-74`) |
| name `"flashcache-mock"` | `storageGetFlashCacheType()` (`src/ext_storage_bridge.c:75-76`) |
| name `"rocksdb"` | `storageGetRocksDBAsyncType()` — i.e. the mock (`src/ext_storage_bridge.c:77-78`) |
| anything else | none; init logs `unknown backend` and returns -1 (`src/ext_storage_bridge.c:81-83`) |

Module registration itself is a single-slot store: `VM_RegisterStorageBackend`
(`src/module.c:820-831`) rejects a second registration, and the bridge reads it via
`moduleHasRegisteredStorageBackend` / `moduleGetRegisteredStorageBackend`
(`src/module.c:834-841`). See [backends](../components/backends.md).

## Serialize utility (`src/storage/storage.h:182-184`)

The header declares `storageSerializeValue`, `storageDeserializeValue` and
`storageFreeSerializedBytes` — but **no definition of any of the three exists in `src/` or
`modules/`**; they are dead declarations, and nothing calls them.

Actual serialization is the `ext_storage` pair, declared at `src/ext_storage.h:149-151` and defined
in `src/ext_storage.c`: `extStorageSerializeValue` (`src/ext_storage.c:267`) and
`extStorageDeserializeValue` (`src/ext_storage.c:288`), RDB DUMP format. Backends pull them in as
`extern` declarations and call them **from the backend IO thread** — mock at
`src/storage/storage_mock.c:82-83` (used at `src/storage/storage_mock.c:129` serialize,
`src/storage/storage_mock.c:189` deserialize), real FlashCache at
`src/storage/storage_flashcache_real.c:35-40` (used at
`src/storage/storage_flashcache_real.c:195` serialize,
`src/storage/storage_flashcache_real.c:146` deserialize). The Rust modules bind the same two symbols
by FFI (`modules/rocksdb-tiering/src/lib.rs:41-42`). See
[serialization](../components/serialization.md).

See also: [pluggable-storage-api](../components/pluggable-storage-api.md), [backends](../components/backends.md), [bridge-api](bridge-api.md).
