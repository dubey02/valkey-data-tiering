---
title: Serialization
status: active
sources:
  - src/ext_storage.c:226-269
  - src/storage/storage_flashcache_real.c:130-205
  - src/server.h:4170
updated: 2026-06-10
type: component
tier: working
claim_count: 7
edges:
  - to: decisions/known-limitations.md
    kind: refers_to
    source: human
    created: 2026-06-03
  - to: decisions/known-limitations.md
    kind: contradicts
    source: human
    created: 2026-06-03
    note: DUMP-format spill supports all types vs string-only limit
  - to: interfaces/storagetype-vtable.md
    kind: refers_to
    source: llm_relation
    created: 2026-06-04
  - to: interfaces/ext-storage-api.md
    kind: refers_to
    source: llm_relation
    created: 2026-06-04
  - to: flows/spill.md
    kind: refers_to
    source: llm_relation
    created: 2026-06-04
  - to: flows/fetch.md
    kind: refers_to
    source: llm_relation
    created: 2026-06-04
  - to: components/bridge-layer.md
    kind: refers_to
    source: llm_relation
    created: 2026-06-04
  - to: components/memory-accounting.md
    kind: refers_to
    source: llm_relation
    created: 2026-06-08
    note: IO-thread serialize measures ram_bytes for the Smith predictor
---

# Serialization

> Values cross the engine↔backend boundary in **RDB DUMP** format, so every object type spills
> and fetches correctly. Serialization runs on the **backend IO thread**, not the main thread.
> Callbacks live in `ext_storage.c`; the [ext_storage API](../interfaces/ext-storage-api.md)
> declares them.

## Value: RDB DUMP format

`extStorageSerializeValue` (`:238-243`) calls `createDumpPayload(&payload, value, NULL, -1)`
(`:240`; declared `server.h:4170`), producing the DUMP/RESTORE encoding
`<type byte><rdb object><2B version><8B CRC64>`. Because it routes through `rdbSaveObject`, it
handles **string, list, set, hash, zset, and stream** — the spill path passes a borrowed robj
carrying the entry's real `type`/`encoding` ([spill](../flows/spill.md)).

> ⚠️ CONTRADICTION (resolved): this DUMP-based path means spilling is **not** string-only.
> The historical "string values only" limitation in [known-limitations](../decisions/known-limitations.md)
> is superseded for the value-serialization layer; remaining type-specific issues are workload
> bugs, not a serialize limit.

`extStorageDeserializeValue` (`:251-261`) reverses it: `rioInitWithBuffer` → `rdbLoadType` →
`rdbLoadObject`, returning a ready robj (or NULL on a malformed payload, `type == -1`). This is
what the [fetch](../flows/fetch.md) READ completion receives.

## Key: zero-copy

`extStorageSerializeKey` (`:226-231`) returns the key's sds pointer **directly** (no copy),
length `sdslen`; `extStorageFreeSerializedKey` (`:263-265`) is therefore a no-op (the key isn't
owned). `extStorageDeserializeKey` (`:245-247`) is `sdsnewlen(key, length)`.

## Ownership

`extStorageFreeSerializedValue` (`:267-269`) `sdsfree`s the heap DUMP buffer. The value buffer
is owned by the producer until freed; the key buffer is borrowed.

## Where it runs (IO thread)

In the real FlashCache backend, the IO worker serializes on submit
(`storage_flashcache_real.c:172`) and frees the DUMP buffer after the put
(`:183`), and deserializes on a GET completion (`:135`) — all off the main thread. The
[bridge](bridge-layer.md) passes robj pointers through. Serialization is handled by
`extStorageSerializeValue`/`extStorageDeserializeValue` in `ext_storage.c`, called from the IO thread.

## Footprint measurement (Smith predictor)

The same IO-thread serialize step also measures the value's in-RAM footprint for the spill
controller's Smith predictor: right after `extStorageSerializeValue`, the worker computes
`comp.ram_bytes = objectComputeSize(NULL, value_robj, 5, db_id)` (`storage_flashcache_real.c:197`),
credits it to in-flight spill bytes (`extStorageInflightAddRam`, `:198`), and folds it into the
EMA (`extStorageOnSpillSerialize`, `:201`). Doing this on the IO thread keeps the (potentially
O(n)) size walk off the main thread; the value is carried back on the completion as `ram_bytes`
and debited on the main thread when the spill completes. See
[memory-accounting](memory-accounting.md).

> ⚠️ CONTRADICTION: `objectComputeSize` is called with a NULL key
> (`storage_flashcache_real.c:197`). That is safe for every spillable type today, but the
> `OBJ_MODULE` size path dereferences the key — a module-typed spill candidate would segfault.
> `spillItemAsync` does not currently exclude `OBJ_MODULE`. Folded into
> [known-limitations](../decisions/known-limitations.md).

See also: [storageType vtable](../interfaces/storagetype-vtable.md), [spill](../flows/spill.md), [fetch](../flows/fetch.md), [known-limitations](../decisions/known-limitations.md).
