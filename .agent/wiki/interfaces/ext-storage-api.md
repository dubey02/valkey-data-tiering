---
title: ext_storage API
status: active
sources:
  - src/ext_storage.h:1-80
  - src/ext_storage.c:160-189
  - src/server.h:829
updated: 2026-06-08
type: interface
tier: working
claim_count: 7
edges:
  - to: components/engine-integration.md
    kind: refers_to
    source: human
    created: 2026-06-03
  - to: components/state-machine.md
    kind: refers_to
    source: llm_relation
    created: 2026-06-04
  - to: components/eviction-integration.md
    kind: refers_to
    source: llm_relation
    created: 2026-06-04
  - to: components/serialization.md
    kind: refers_to
    source: llm_relation
    created: 2026-06-04
  - to: components/throttle-equilibrium.md
    kind: refers_to
    source: llm_relation
    created: 2026-06-04
  - to: flows/completion-drain.md
    kind: refers_to
    source: llm_relation
    created: 2026-06-04
  - to: interfaces/info-metrics.md
    kind: refers_to
    source: llm_relation
    created: 2026-06-04
  - to: interfaces/config-and-module-args.md
    kind: refers_to
    source: llm_relation
    created: 2026-06-04
  - to: components/memory-accounting.md
    kind: refers_to
    source: llm_relation
    created: 2026-06-08
    note: projected-memory + inflight RAM accounting API
  - to: decisions/known-limitations.md
    kind: refers_to
    source: llm_relation
    created: 2026-06-08
    note: items_spillover_batch_size now vestigial
---

# ext_storage API

> The engine-internal public surface in `src/ext_storage.h`: the `TieringState` enum, the
> global tiering vars, the per-loop entry points, and the state-machine / serialization
> callbacks. Implemented in `ext_storage.c`; consumed across `server.c`, `db.c`, `evict.c`.

## TieringState enum (`ext_storage.h:25-31`)

The five states and their one-line meanings, from the enum itself:

| State | Val | Meaning (`ext_storage.h:26-30`) |
|-------|-----|---------------------------------|
| `TIERING_STATE_ONLY_MEMORY` | 0 | value in RAM (default) |
| `TIERING_STATE_COPYING_TO_FLASH` | 1 | spill in-flight, value still in RAM |
| `TIERING_STATE_ONLY_FLASH` | 2 | value on disk (`encoding == OBJ_ENCODING_TIERED`) |
| `TIERING_STATE_COPYING_TO_MEMORY` | 3 | fetch or delete in-flight from flash |
| `TIERING_STATE_PENDING_EVICT` | 4 | eviction requested during fetch |

The state lives in a 3-bit bitfield on the object: `robj.tiering_state` (`server.h:829`).
`extStorageGetState`/`SetState`/`RemoveState(serverDb *db, sds key)` (`ext_storage.h:56-58`)
resolve the key in the normal keyspace `db->keys` and read/write that bitfield directly
(`ext_storage.c:160-189`); a key absent from `db->keys` reports `ONLY_MEMORY`. There is no
separate state side-table. Full transition and blocking semantics:
[state-machine](../components/state-machine.md).

## Global state (`ext_storage.h:38-44`)

`ext_data_enabled`, `ext_storage_spill_pool_active`, `ext_storage_backend` (name),
`ext_storage_path`, `ext_storage_capacity_mb`, `items_spillover_batch_size`,
`total_items_spilling_to_ext_storage`. Wired to config in
[config-and-module-args](config-and-module-args.md). Note: `items_spillover_batch_size` is still
declared and config-wired but **no longer read** by the cap-less spill controller (vestigial —
see [known-limitations](../decisions/known-limitations.md)).

## Lifecycle & per-loop entry points (`ext_storage.h:46-53`)

| Fn | Role |
|----|------|
| `extStorage_init(void)` | one-time init; also inits bridge + throttle |
| `int preCommandExec(client *c)` | the blocking gate — runs before each command, blocks the client if the key is tiered/in-flight |
| `processCompletedStorageRequestsAndSpillOldItems()` | drain completions + spill if over `maxmemory` (normal) |
| `…Aggressive()` | same, harder spill pass under high pressure |
| `processCompletedStorageRequests(void)` | drain completions only |
| `sds genExternalStorageInfoString(sds)` | builds the `INFO` block ([info-metrics](info-metrics.md)) |

See [engine-integration](../components/engine-integration.md), [completion-drain](../flows/completion-drain.md).

## State-machine API (`ext_storage.h:56-59`)

`extStorageGetState(db,key)`, `extStorageSetState(db,key,state,inflight_op)`,
`extStorageRemoveState(db,key)`, `extStorageEvictFlashKey(db,key)`.

## Eviction, spill-predictor & accounting hooks

- `int extStoragePerformEvictions(int *result)` (`:63`) — eviction override; returns 1 if
  tiering handled the decision (sets `*result` to `EVICT_OK`/`EVICT_FAIL`), 0 to fall back
  to standard eviction. See [eviction-integration](../components/eviction-integration.md).
- `void extStorageOnSpillSubmit(void)` (`:66`) / `void extStorageOnSpillSerialize(size_t bytes)`
  (`:67`) — Smith-predictor hooks (replacing the removed `extStorageUpdateSpillConcurrency`): the
  main thread counts a submit, the IO thread records the serialized footprint into the EMA.
- `void extStorageInflightAddRam(size_t bytes)` (`:78`) / `size_t extStorageProjectedMemory(void)`
  (`:79`) — credit in-flight spill RAM, and query projected memory (the cap-less spill controller's
  gate). See [memory-accounting](../components/memory-accounting.md) and
  [throttle-equilibrium](../components/throttle-equilibrium.md).

## Serialization callbacks (`ext_storage.h:71-76`)

`extStorageSerializeKey`/`Value`, `extStorageDeserializeKey`/`Value`,
`extStorageFreeSerializedKey`/`Value`. Detail: [serialization](../components/serialization.md).

See also: [engine-integration](../components/engine-integration.md), [state-machine](../components/state-machine.md).
