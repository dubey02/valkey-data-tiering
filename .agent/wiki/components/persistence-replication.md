---
title: Persistence & Replication
status: active
sources:
  - src/rdb.c:1190-1195
  - src/rdb.c:1447
  - src/rdb.c:1483-1545
  - src/rdb.c:1636
  - src/aof.c:1446
  - src/aof.c:2356-2409
  - src/aof.c:2447-2496
  - src/aof.c:2530-2532
  - src/defrag.c:704-712
  - src/object.c:1205-1211
  - src/server.h:779-839
  - src/replication.c:1018-1021
updated: 2026-06-05
type: component
tier: working
claim_count: 12
edges:
  - to: components/state-machine.md
    kind: refers_to
    source: llm_relation
    created: 2026-06-05
    note: the tiered-state predicate objectIsTiered drives every skip
  - to: components/serialization.md
    kind: refers_to
    source: llm_relation
    created: 2026-06-05
    note: spill uses RDB DUMP; persistence here SKIPS instead of serialising
  - to: components/memory-accounting.md
    kind: refers_to
    source: llm_relation
    created: 2026-06-05
    note: tiered val is an empty-SDS placeholder, relevant to AOF-plain emit
  - to: components/engine-integration.md
    kind: refers_to
    source: llm_relation
    created: 2026-06-05
  - to: decisions/known-limitations.md
    kind: refers_to
    source: human
    created: 2026-06-03
    note: tiered keys dropped from RDB/full-sync; AOF-plain placeholder emit
  - to: components/backends.md
    kind: refers_to
    source: llm_relation
    created: 2026-06-05
    note: each node owns a local backend; tiering state not replicated
  - to: components/bridge-layer.md
    kind: refers_to
    source: llm_relation
    created: 2026-06-05
---

# Persistence & Replication

> RDB save, AOF rewrite, defrag, and replication all key off one predicate —
> `objectIsTiered(o)` — to skip values that live on external storage. Tiered cold data is
> **not** captured by RDB/full-sync; the storage backend owns its own (node-local) durability.

The single marker is `objectIsTiered(o)` (`server.h:839`) = `encoding == OBJ_ENCODING_TIERED`
(`server.h:779`); the per-object state lives in the `tiering_state` bitfield (`server.h:829`).
For a tiered object the in-memory value is only an **empty-SDS placeholder** — the real bytes
are on flash (`object.c:1205-1211`; see [memory-accounting](memory-accounting.md)).

## Where tiering is handled

| Path | Function | Tiered handling | Cite |
|---|---|---|---|
| RDB save | `rdbSaveKeyValuePair` | **Skip** whole pair (`return 0`) | `rdb.c:1190-1195` |
| RDB iterate | `rdbSaveRio` loop | `res==0` ⇒ key omitted, not an error | `rdb.c:1447` |
| AOF base (preamble, default) | `rewriteAppendOnlyFile` → `rdbSaveRio` | inherits RDB **skip** | `aof.c:2530-2532` |
| AOF base (plain) | `rewriteObjectRio` | **No guard** ⇒ emits placeholder ⚠️ | `aof.c:2356-2404` |
| AOF tail | `feedAppendOnlyFile` | logs the write *command*, not the value | `aof.c:1446` |
| Active defrag | `defragKey` | **Skip** (`return`) | `defrag.c:704-712` |
| Replication full-sync | `rdbSaveRio` (RDBFLAGS_REPLICATION) | inherits RDB **skip** | `rdb.c:1536-1545` |

## RDB

`rdbSaveKeyValuePair` (`rdb.c:1190`) returns early for a tiered value before writing anything
(`rdb.c:1195`). The return value `0` means *"key was not saved"* (vs `1` saved, `-1` error), so
the caller loop in `rdbSaveRio` (`rdb.c:1447`, `res < 0` is the only error branch) simply
**omits the entire key-value pair** — both key and value disappear from the RDB. The inline
comment is explicit (`rdb.c:1191-1194`): the storage module handles its own persistence, and on
restart tiered keys are **not** present — *"cold data is lost unless the module restores them
from its own persistent store."*

`rdbSaveRio` (`rdb.c:1483`) is the single funnel for every RDB consumer: the on-disk RDB file
(`rdbSave`, `rdb.c:1636`), the AOF preamble base (below), and replication full-sync (below) —
so the skip applies uniformly to all three.

## AOF

AOF has two independent surfaces:

- **Live tail** — `feedAppendOnlyFile` (`aof.c:1446`) appends the *executed write command*, not
  the value. A write to a key is logged at execution time, independent of whether that key is
  later spilled, so the AOF tail stays correct regardless of tiering state.
- **Base/rewrite** — depends on `aof_use_rdb_preamble`:
  - **Preamble (default `yes`)**: `rewriteAppendOnlyFile` calls `rdbSaveRio` with
    `RDBFLAGS_AOF_PREAMBLE` (`aof.c:2530-2532`) ⇒ inherits the RDB tiered **skip**. Tiered keys
    are omitted from the base, exactly like RDB.
  - **Plain (`no`)**: `rewriteAppendOnlyFileRio` (`aof.c:2447`) iterates the keyspace
    (`aof.c:2472-2489`) and calls `rewriteObjectRio` (`aof.c:2356`) per key.

> ⚠️ CONTRADICTION (asymmetry): `rewriteObjectRio` (`aof.c:2356-2409`) dispatches purely on
> `o->type` with **no `objectIsTiered` guard**. A tiered object keeps its real `o->type` but its
> `o->ptr` is the empty-SDS placeholder (`object.c:1205-1211`), so plain-AOF rewrite serializes a
> tiered key from its **placeholder** — e.g. `SET key ""` for a string — rather than skipping it
> as the RDB/preamble path does. The default preamble mode avoids this; plain-AOF mode silently
> emits empty/incomplete values for already-tiered keys. Candidate for
> [known-limitations](../decisions/known-limitations.md).

## Active defragmentation

`defragKey` (`defrag.c:704`) — invoked per key from the active-defrag scan — returns early for a
tiered object (`defrag.c:712`): *"value is on external storage, nothing to defrag."* Because the
guard sits at the top, before `activeDefragStringOb` and any `defragLater`/`scanLater*`
deferral, **all** deferred defrag work is skipped for tiered objects too — there is no in-memory
value graph to relocate.

## Replication

`replication.c` contains **no** tiering-specific code (zero `objectIsTiered`/`OBJ_ENCODING_TIERED`
references). Tiering interacts with replication only through the two mechanisms above:

- **Full sync (RDB transfer):** the primary's snapshot goes through `rdbSaveRio` with
  `RDBFLAGS_REPLICATION` (`rdb.c:1536-1545`; driven from `replication.c:1018-1021` via
  `rdbSaveToReplicasSockets` / `rdbSaveBackground`). The tiered skip applies ⇒ a freshly-synced
  replica **does not receive already-tiered keys/values**.
- **Steady-state stream:** write commands propagate to replicas at execution time (the same
  command-propagation path that feeds the AOF tail), so writes *after* the sync replicate
  normally and land in the replica's own memory.

Consequence: **tiering state is not replicated.** Each node spills/fetches independently under
its own memory pressure against its own node-local backend (see [backends](backends.md),
[bridge-layer](bridge-layer.md)); a replica's tiered set is whatever its own eviction produced,
not a copy of the primary's.

## See also

[state-machine](state-machine.md) · [serialization](serialization.md) ·
[memory-accounting](memory-accounting.md) · [engine-integration](engine-integration.md) ·
[known-limitations](../decisions/known-limitations.md)
