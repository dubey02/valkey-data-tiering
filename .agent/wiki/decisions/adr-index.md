---
title: Architecture Decision Records
status: active
sources:
  - src/server.h:779-839
  - src/storage/storage.h:75-130
  - src/storage/storage_dispatch.c:26
  - src/ext_storage.c:79-88
  - src/ext_storage.c:232
  - src/ext_storage.c:815
  - src/ext_storage.c:940-944
  - src/ext_storage.c:997-1033
  - src/ext_storage_throttle.c:221-222
  - src/ext_storage_bridge.c:65-104
  - src/rdb.c:1190-1195
  - .agent/knowledge/pluggable-storage-design.md
  - .agent/knowledge/middleware-architecture-decision.md
updated: 2026-06-08
type: decision
tier: wisdom
claim_count: 9
edges:
  - to: 01-architecture.md
    kind: refers_to
    source: human
    created: 2026-06-03
  - to: 00-overview.md
    kind: refers_to
    source: llm_relation
    created: 2026-06-05
  - to: components/state-machine.md
    kind: refers_to
    source: llm_relation
    created: 2026-06-05
  - to: components/pluggable-storage-api.md
    kind: refers_to
    source: llm_relation
    created: 2026-06-05
  - to: interfaces/storagetype-vtable.md
    kind: refers_to
    source: llm_relation
    created: 2026-06-05
  - to: components/serialization.md
    kind: refers_to
    source: llm_relation
    created: 2026-06-05
  - to: components/backends.md
    kind: refers_to
    source: llm_relation
    created: 2026-06-05
  - to: components/bridge-layer.md
    kind: refers_to
    source: llm_relation
    created: 2026-06-05
  - to: components/persistence-replication.md
    kind: refers_to
    source: llm_relation
    created: 2026-06-05
  - to: components/throttle-equilibrium.md
    kind: refers_to
    source: llm_relation
    created: 2026-06-05
  - to: components/eviction-integration.md
    kind: refers_to
    source: llm_relation
    created: 2026-06-08
    note: ADR-010 cap-less projected-gated spill controller
  - to: components/memory-accounting.md
    kind: refers_to
    source: llm_relation
    created: 2026-06-08
    note: ADR-010 Smith predictor / projected memory
  - to: decisions/known-limitations.md
    kind: refers_to
    source: llm_relation
    created: 2026-06-05
---

# Architecture Decision Records

> Index of the load-bearing NKS tiering design decisions, each distilled from **this repo's
> code** and the page that documents it. Legacy `.agent/knowledge` notes are background; where
> they disagree with the code, the code wins (see [known-limitations](known-limitations.md)).

| ADR | Decision | Code anchor | Page | Status |
|-----|----------|-------------|------|--------|
| 001 | **Non-key-spilling**: keys always stay in the dict; only *values* spill to flash | `OBJ_ENCODING_TIERED` `server.h:779`; `objectIsTiered` `server.h:839` | [00-overview](../00-overview.md) | active |
| 002 | Per-object tiering state lives in a **3-bit `robj` bitfield**, not a side table | `tiering_state:3` `server.h:829` | [state-machine](../components/state-machine.md) | active |
| 003 | **One `storageType` vtable, two dispatch paths** — async if `put_async!=NULL`, else shared middleware | `storage_dispatch.c:26` | [pluggable-storage-api](../components/pluggable-storage-api.md), [storageType vtable](../interfaces/storagetype-vtable.md) | active |
| 004 | **robj-at-boundary serialization** — the backend serializes via engine FFI callbacks on its own IO thread | `ext_storage.c:232` | [serialization](../components/serialization.md), [backends](../components/backends.md) | active |
| 005 | **Module backend takes precedence** over native; native init failure falls back to mock | `ext_storage_bridge.c:65-104` | [bridge-layer](../components/bridge-layer.md), [backends](../components/backends.md) | active |
| 006 | **DUMP-based serialization for all types** (supersedes string-only) | `ext_storage.c:232`, `:815` | [serialization](../components/serialization.md) | active |
| 007 | **Persistence skips tiered values; the backend owns durability** | `rdb.c:1195` | [persistence-replication](../components/persistence-replication.md) | active |
| 008 | **Memory-gated throttle** — connection-level admission control reading *raw* `used_memory`; throttle band **1.1×–1.2×**, hard reject at 1.2× | `ext_storage_throttle.c:221-222`; hard cap `ext_storage.c:940-944` | [throttle-equilibrium](../components/throttle-equilibrium.md) | active |
| 009 | ~~**Dynamic spill concurrency feedback** — `throttle_rate` scales concurrency from base to limit~~ | removed (`extStorageUpdateSpillConcurrency`, commit `e4ed4ff3a`) | [throttle-equilibrium](../components/throttle-equilibrium.md) | **superseded by ADR-010** |
| 010 | **Cap-less, projected-gated spill controller** — `spillFillToProjected` submits while `extStorageProjectedMemory() > maxmemory`; queue depth is emergent, no fixed cap; spill/throttle decoupled (implicit via memory) | `ext_storage.c:997-1033`; predictor `:79-88` | [eviction-integration](../components/eviction-integration.md), [memory-accounting](../components/memory-accounting.md) | active |

## Notes

- **ADR-001/002 (NKS core).** Because the key never leaves the dict, the engine's state machine —
  not a bloom filter — is the existence oracle; this is why the Rust backends' `key_may_exist` is
  left unregistered ([known-limitations](known-limitations.md) C5).
- **ADR-003/004 (one vtable, robj boundary).** The vtable serves both sync backends (via the shared
  middleware pool) and async backends (own IO thread). Serializing the `robj` *behind* the vtable
  (backend-side, over FFI) is the deliberate divergence from a bytes-at-boundary store; it is
  zero-copy on the key but couples the backend to engine serialization symbols
  (`extStorageSerializeKey`/`Value`).
- **ADR-006 (DUMP).** `createDumpPayload`/`rdbSaveObject` made compound types spillable, retiring the
  original string-only restriction (now a resolved item in [known-limitations](known-limitations.md) R1).
- **ADR-007 (durability).** Skipping tiered keys in RDB/AOF-preamble/full-sync is a deliberate
  decision that pushes durability onto the storage backend — and is the source of the headline
  durability limitation ([known-limitations](known-limitations.md) L1).
- **ADR-008/010 (control).** The throttle is a connection-level admission gate reading *raw*
  `used_memory` over the `[1.1×, 1.2×]` band; the spill controller (ADR-010) is **cap-less** and
  gates on *projected* memory (`used_memory` minus committed-but-not-freed spill RAM, a two-stage
  Smith predictor). The two are **decoupled** — they share no signal and couple only implicitly
  through memory. This supersedes ADR-009's `throttle_rate`-driven dynamic concurrency, which the
  Smith-predictor commit removed. Operating bands and equilibria:
  [throttle-equilibrium](../components/throttle-equilibrium.md), [memory-accounting](../components/memory-accounting.md).

## See also

[01-architecture](../01-architecture.md) · [known-limitations](known-limitations.md)
