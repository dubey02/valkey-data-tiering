---
title: storageType vtable
status: active
sources:
  - src/storage/storage.h:1-136
updated: 2026-06-08
type: interface
tier: working
claim_count: 8
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
> compiles (every `storage/*.o` includes it via `-Istorage`; `ext_storage_bridge.c` and
> `module.c` include `storage/storage.h`). One struct serves both **sync** backends (engine
> wraps them via the shared middleware) and **async** backends (backend owns its IO). Dispatch
> rule: **`put_async != NULL` → async path; else the sync `put/get/del` go through the
> middleware** (`storage.h:3` top comment + `storage.h:94` async-ops note; impl in [pluggable-storage-api](../components/pluggable-storage-api.md)).
> `version` must equal `VALKEY_STORAGE_VERSION` = 1 (`storage.h:24`).

When built inside Valkey (with `STORAGE_USE_ZMALLOC` defined), `storage_malloc` /
`storage_calloc` / `storage_free` map to `zmalloc` (`storage.h:11-22`).

## Status codes (`storage.h:27-34`)

| Code | Val |
|------|-----|
| `STORAGE_OK` | 0 |
| `STORAGE_NOT_FOUND` | 1 |
| `STORAGE_WOULDBLOCK` | 2 (async op submitted; completion comes later) |
| `STORAGE_ERR_IO` | -1 |
| `STORAGE_ERR_FULL` | -2 |
| `STORAGE_ERR_REJECTED` | -3 (backend at capacity / throttled) |

Op-type constants `STORAGE_OP_PUT/GET/DEL = 0/1/2` (`storage.h:37-39`).

## Completion record (`storageCompletion`, `storage.h:42-53`)

Delivered IO-thread → main-thread: `request_ctx` (opaque echo of the submit), `op_type`,
`status`, `db_id`, `key`/`klen`, and for GET the `value`/`vlen` (**caller takes ownership;
malloc'd**) plus `expire_ms`. A `ram_bytes` field (`storage.h:52`) carries the spilled value's
in-RAM footprint, computed on the IO thread at serialize and debited by the main thread on the
WRITE completion (the spill controller's Smith-predictor accounting — see
[memory-accounting](../components/memory-accounting.md)). Delivery type `storageCompletionFn` is
`void (*)(storageCompletion *c, void *privdata)` (`storage.h:55`) — note the `privdata`.

## Config & stats

- `storageConfig` (`storage.h:58-65`): `path`, `capacity_bytes`, `num_databases`,
  `io_threads`, `completion_fn`, `completion_privdata`.
- `storageStats` (`storage.h:68-73`): `total_puts`, `total_gets`, `total_dels`, `keys_stored`.

## vtable members (`storageType`, `storage.h:76-110`)

| Group | Members | Notes |
|-------|---------|-------|
| identity | `name`, `version` | `:77-78` |
| lifecycle | `open`/`close` | `:81-82` |
| sync KV | `put`/`get`/`del` | called from middleware IO thread; NULL if async-only (`:85-92`) |
| async KV | `put_async`/`get_async`/`del_async` | backend owns IO; NULL = use middleware (`:95-104`) |
| poll | `poll_completions` | required iff async; main thread (`:105`) |
| optional | `cron`, `get_stats` | `:108-109` |

This vtable is deliberately small — none of the richer members found in larger storage specs
exist here (no drain, iterators, flush, snapshot, config getters, exists, or sync). Only the
groups above are present.

## Engine dispatch API (`storage.h:115-125`)

`storageInit(type, cfg)`, `storageShutdown()`, `storageSubmitPut/Get/Del`,
`storagePollCompletions(max)`, `storageCron()` — these encapsulate the async-vs-middleware
choice so callers never branch on backend type. The `server_storage` / `server_storage_ctx`
globals live in the dispatch layer; see [pluggable-storage-api](../components/pluggable-storage-api.md).

## Backend getters (`storage.h:128-131`)

| Getter | Backend | Path |
|--------|---------|------|
| `storageGetFlashCacheType` | mock, in-memory async | testing |
| `storageGetFlashCacheRealType` | real FlashCache (links `libflashcache.a`) | async |
| `storageGetRocksDBType` | RocksDB | sync (via middleware) |
| `storageGetRocksDBAsyncType` | RocksDB | async (owns IO) |

See [backends](../components/backends.md).

## Serialize utility (`storage.h:134`)

Serialization uses RDB DUMP format via `extStorageSerializeValue`/`extStorageDeserializeValue`
in `ext_storage.c`, called from the backend IO thread. See [serialization](../components/serialization.md).

See also: [pluggable-storage-api](../components/pluggable-storage-api.md), [backends](../components/backends.md), [bridge-api](bridge-api.md).
