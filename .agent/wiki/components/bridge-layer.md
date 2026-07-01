---
title: Bridge Layer
status: active
sources:
  - src/ext_storage_bridge.c:1-277
updated: 2026-06-08
type: component
tier: working
claim_count: 8
edges:
  - to: interfaces/bridge-api.md
    kind: refers_to
    source: human
    created: 2026-06-03
  - to: components/pluggable-storage-api.md
    kind: depends_on
    source: human
    created: 2026-06-03
  - to: components/engine-integration.md
    kind: refers_to
    source: llm_relation
    created: 2026-06-04
  - to: components/backends.md
    kind: refers_to
    source: llm_relation
    created: 2026-06-04
  - to: components/serialization.md
    kind: refers_to
    source: llm_relation
    created: 2026-06-04
  - to: components/eviction-integration.md
    kind: refers_to
    source: llm_relation
    created: 2026-06-04
  - to: flows/completion-drain.md
    kind: refers_to
    source: llm_relation
    created: 2026-06-04
  - to: interfaces/storagetype-vtable.md
    kind: refers_to
    source: llm_relation
    created: 2026-06-04
  - to: components/memory-accounting.md
    kind: refers_to
    source: llm_relation
    created: 2026-06-08
    note: carries ram_bytes (serialize footprint) on the completion msg
---

# Bridge Layer

> `ext_storage_bridge.c` is the adapter between `ext_storage.c`'s **robj/sds** world and the
> [storageType](../interfaces/storagetype-vtable.md) layer's **opaque-bytes** world. It owns
> backend selection/init, a completion ring buffer (IO thread writes, main thread reads), and
> the translation of `storageCompletion` back into the `ValkeyModuleExternalStorageMsg` the
> engine's [completion drain](../flows/completion-drain.md) consumes. API: [bridge-api](../interfaces/bridge-api.md).

## Backend selection & init (`extStorageBridge_init`, `:61-114`)

Chooses the `storageType` in priority order (`:65-75`): a module-registered backend
(`moduleHasRegisteredStorageBackend`) → else by name — `"flashcache"` →
`storageGetFlashCacheRealType`, `"flashcache-mock"` → mock, `"rocksdb"` →
`storageGetRocksDBAsyncType`. It builds a `storageConfig` with `bridge_on_completion` as the
`completion_fn` and calls `storageInit` (`:96`); if a **real** backend's init fails it falls
back to the mock (`:98-101`). `extStorageBridge_isReady` (`:117-120`) is just
`server_storage && server_storage_ctx`. Backends: [backends](backends.md); dispatch:
[pluggable-storage-api](pluggable-storage-api.md).

## Request context (`bridgeRequestCtx`, `:32-39`)

Each submit allocates a `bridgeRequestCtx` carrying `op_type`, `db_id`, `key`, `value_copy`,
`expire_ms`, and a `key_owned` flag (`:36`): **1** = the bridge `sdsdup`'d the key (free it on
completion), **0** = borrowed from the dict entry.

## Submit paths

| Fn | Key handling | Calls |
|----|--------------|-------|
| `extStorageBridge_submitPut` (`:125-144`) | **borrows** the key sds from the robj (`key_owned=0`, `:131`); `value_copy` = the value robj | `storageSubmitPut` (`:135`) |
| `extStorageBridge_submitGet` (`:146-165`) | `sdsdup`s the key (`key_owned=1`, `:151`) + a `createStringObject` keyobj | `storageSubmitGet` |
| `extStorageBridge_submitDel` (`:167-190`) | `sdsdup`s the key (`key_owned=1`) | `storageSubmitDel` |

A submit returns 0 on `STORAGE_OK`/`STORAGE_WOULDBLOCK`; on reject it frees what it owns and
returns -1 (PUT does **not** free the borrowed key).

## Completion ring (`:41-58`)

A fixed `BRIDGE_COMP_RING` (4096) ring. `bridge_on_completion` (`:49-58`) is the `completion_fn`
the backend/middleware calls **from the IO thread**: it copies the `storageCompletion` under a
mutex and drops it if the ring is full.

## Poll & translate (`extStorageBridge_pollCompletions`, `:193-272`)

Called from the engine's drain. It first calls `storagePollCompletions(max)` (`:197`) to push
backend completions into the ring, then drains the ring:

- **`req_ctx == NULL`** (`:204-221`): a FlashCache-internal GC eviction — synthesize a DELETE
  `ValkeyModuleExternalStorageMsg` whose key comes from `c->value` (`:213`), with
  `ram_bytes = 0` (`:215`), so the engine removes the key from the dict
  ([eviction-integration](eviction-integration.md)).
- otherwise: map `op_type` → `msg_type`, set `status`/`ttl`, copy `msg->ram_bytes = c->ram_bytes`
  (`:241`; the IO-thread serialize footprint, debited on the WRITE completion — see
  [memory-accounting](memory-accounting.md)), wrap the key in a `createStringObject`, and pass the
  value through — for GET, `c->value` is **already a deserialized robj** from the backend's IO
  thread (`:250-251`); for PUT it's the original `value_copy`. Then free the IO-thread key robj and
  the `req_ctx` (`sdsfree` the key iff `key_owned`, `:264`).

This `ValkeyModuleExternalStorageMsg` shape is what `processCompletedStorageRequests` already
expects; serialization/deserialization itself happens on the IO thread, not here
([serialization](serialization.md)).

See also: [bridge-api](../interfaces/bridge-api.md), [pluggable-storage-api](pluggable-storage-api.md), [engine-integration](engine-integration.md).
