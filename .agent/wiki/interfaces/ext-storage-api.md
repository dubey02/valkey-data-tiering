---
title: ext_storage API
status: active
sources:
  - src/ext_storage.h:1-157
  - src/ext_storage.c:40-90
  - src/ext_storage.c:130-140
  - src/ext_storage.c:175-207
  - src/ext_storage.c:320-360
  - src/ext_storage.c:1029-1061
  - src/ext_storage.c:1063-1158
  - src/ext_storage.c:1420-1440
  - src/ext_storage.c:1690-1829
  - src/server.h:824
  - src/db.c:95
  - src/db.c:864
  - src/db.c:1931
  - src/expire.c:80
  - src/config.c:2620-2635
  - src/debug.c:1088-1096
  - src/rdb.c:1195-1215
  - src/aof.c:2650-2660
  - tests/unit/data-tiering/ext-storage-swapdb.tcl
  - tests/unit/data-tiering/ext-storage-sync-fetch.tcl
updated: 2026-07-30
type: interface
tier: working
claim_count: 11
edges:
  - to: components/engine-integration.md
    kind: refers_to
    source: human
    created: 2026-06-03
  - to: components/state-machine.md
    kind: refers_to
    source: llm_relation
    created: 2026-06-04
    note: owns the 6-state transition + blocking narrative
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
  - to: components/persistence-replication.md
    kind: refers_to
    source: llm_relation
    created: 2026-07-30
    note: consumer of the snapshot group + extStorageMaterializeTiered
---

# ext_storage API

> The engine-internal public surface in `src/ext_storage.h` (157 lines): the `TieringState`
> enum, the config-wired globals, the per-loop entry points, and the state-machine,
> serialization, snapshot and SWAPDB db-id groups. Implemented in `ext_storage.c`; consumed
> across `server.c`, `db.c`, `evict.c`, `expire.c`, `rdb.c`, `aof.c`, `config.c`, `debug.c`.

The sections below follow the header's own grouping order.

## Preamble (`src/ext_storage.h:12-16`)

`CMD_FILTER_ACCEPT` (0) / `CMD_FILTER_REJECT` (1) — module command-filter status codes.
`typedef struct serverObject dbEntry` — the keyspace entry type used by the tiering paths
(`src/ext_storage.h:16`); e.g. `dbFind()` results in the sync-fetch loop.

## TieringState enum (`src/ext_storage.h:25-35`)

**Six** states, from the enum itself:

| State | Val | Meaning (`src/ext_storage.h:26-34`) |
|-------|-----|-------------------------------------|
| `TIERING_STATE_ONLY_MEMORY` | 0 | value in RAM (default) |
| `TIERING_STATE_COPYING_TO_FLASH` | 1 | spill in-flight, value still in RAM |
| `TIERING_STATE_ONLY_FLASH` | 2 | value on disk (`encoding == OBJ_ENCODING_TIERED`) |
| `TIERING_STATE_COPYING_TO_MEMORY` | 3 | fetch or delete in-flight from flash |
| `TIERING_STATE_PENDING_EVICT` | 4 | eviction requested during fetch |
| `TIERING_STATE_PENDING_DELETION` | 5 | flash copy deleted for a client `DEL`; the entry is retained so the re-executed `DEL` removes it with full command-layer side effects |

The state lives in a 3-bit bitfield on the object: `robj.tiering_state` (`src/server.h:824`,
commented "TieringState (0-5)"). There is no separate state side-table — the header notes the
`tieringStateEntry` struct was removed (`src/ext_storage.h:37`) and `server.h:913` records the
removal of the `keys_tiering_state` hashtable. Full transition, blocking and DEL semantics:
[state-machine](../components/state-machine.md) — this page does not duplicate that narrative.

> ⚠️ Stale comment in the code (flagged, not fixed — sources are immutable): the block comment
> at `src/ext_storage.h:21-23` still says "Each key can be in one of 5 states" and still
> describes the removed `keys_tiering_state` hashtable, both contradicted by the enum below it
> (6 values) and by `src/ext_storage.h:37`.

## Globals and config-wired vars

| Group | Symbols | Cite |
|-------|---------|------|
| Enable + counters | `ext_data_enabled`, `num_items_on_flash` | `src/ext_storage.h:42`, `:44` |
| Debug hook | `ext_storage_debug_pause_completions` | `src/ext_storage.h:43` |
| Backend selection / capacity | `ext_storage_spill_pool_active`, `ext_storage_backend`, `ext_storage_path`, `ext_storage_capacity_mb`, `ext_storage_max_spill_size`, `items_spillover_batch_size` | `src/ext_storage.h:71-76` |
| FlashCache tuning | `ext_storage_index_size`, `ext_storage_max_allocated_percent`, `ext_storage_max_in_flight_reads`, `ext_storage_min_gc_rate`, `ext_storage_max_gc_rate`, `ext_storage_max_buffered_write_size`, `ext_storage_buffered_write_flush_threshold` | `src/ext_storage.h:77-84` |
| In-flight counter | `total_items_spilling_to_ext_storage` | `src/ext_storage.h:85` |

`num_items_on_flash` is the count of values *currently resident on external storage* (not
in-flight); defined at `src/ext_storage.c:135` and maintained by the completion handler. It is
also the fail-closed gate for persistence: `src/rdb.c:4031` / `src/aof.c:2653` refuse to proceed
when `num_items_on_flash > 0` and the backend cannot snapshot.

Config wiring: [config-and-module-args](config-and-module-args.md). `items_spillover_batch_size`
is still declared and config-wired but **no longer read** by the cap-less spill controller
(vestigial — see [known-limitations](../decisions/known-limitations.md)).

### Strategy selectors (`src/ext_storage.h:87-114`)

Two orthogonal A/B switches, each a versioned pair (v2 = default):
`ExtStorageThrottlingStrategy` = {`THROTTLING_STRATEGY_V2` (0, `adjustRateV2`, decoupled
1.1x–1.2x band), `THROTTLING_STRATEGY_V1` (1, legacy coupled 1.0x–1.1x that also drives the
spill-concurrency cap)}; `ExtStorageSpillingStrategy` = {`SPILLING_STRATEGY_V2` (0,
`spillFillToProjected`, cap-less Smith predictor), `SPILLING_STRATEGY_V1` (1, legacy batch +
concurrency cap)}. Selected by `ext_storage_throttling_strategy` /
`ext_storage_spilling_strategy`, band edges by `ext_storage_throttle_band_start` /
`ext_storage_throttle_band_end` (`src/ext_storage.h:111-114`). See
[throttle-equilibrium](../components/throttle-equilibrium.md).

## Lifecycle and per-loop entry points (`src/ext_storage.h:116-123`, `:52`)

| Fn | Cite | Role |
|----|------|------|
| `void extStorage_init(void)` | `:116` (impl `src/ext_storage.c:366`) | one-time init; also inits bridge + throttle and builds the db-id maps |
| `int extStorageIsInitialized(void)` | `:52` (impl `src/ext_storage.c:338-340`) | true once `extStorage_init` completed (post config load); implemented as "db-id maps allocated" |
| `int preCommandExec(client *c)` | `:118` (impl `src/ext_storage.c:573`) | the blocking gate — runs before each command, blocks the client if the key is tiered/in-flight |
| `int processCompletedStorageRequestsAndSpillOldItems(void)` | `:120` (impl `src/ext_storage.c:1522`) | drain completions + spill if over `maxmemory` (normal) |
| `int processCompletedStorageRequestsAndSpillOldItemsAggressive(void)` | `:121` (impl `src/ext_storage.c:1534`) | same, harder spill pass under high pressure |
| `void processCompletedStorageRequests(void)` | `:122` (impl `src/ext_storage.c:1035`) | drain completions only |
| `sds genExternalStorageInfoString(sds)` | `:123` (impl `src/ext_storage.c:1548`) | builds the `INFO` block ([info-metrics](info-metrics.md)) |

See [engine-integration](../components/engine-integration.md),
[completion-drain](../flows/completion-drain.md).

## State-machine API (`src/ext_storage.h:125-129`)

`TieringState extStorageGetState(serverDb *db, sds key)`,
`void extStorageSetState(serverDb *db, sds key, TieringState state, int inflight_op)`,
`void extStorageRemoveState(serverDb *db, sds key)`,
`int extStorageEvictFlashKey(serverDb *db, sds key)` (impl `src/ext_storage.c:1239`).

The three accessors resolve the key in the normal keyspace via
`kvstoreHashtableFind(db->keys, …)` and read/write the `tiering_state` bitfield directly
(`src/ext_storage.c:179-207`); a key absent from `db->keys` reports `ONLY_MEMORY`.
`inflight_op` is accepted but unused (`src/ext_storage.c:191`), and `extStorageSetState`
silently no-ops on a missing key (empty `else` at `src/ext_storage.c:195-196`).

## Mid-execution synchronous fetch (`src/ext_storage.h:46-50`)

`void extStorageSyncFetch(serverDb *db, sds key)` — implemented at
`src/ext_storage.c:1078-1158`, design intent in `.agent/knowledge/sync-fetch-design.md`
(header comment `src/ext_storage.h:46-49`; function comment `src/ext_storage.c:1063-1077`).

- **Why:** called from `lookupKey()` (`src/db.c:95`) when a non-resident key is accessed in a
  context that cannot block-and-re-execute — Lua / `EXEC`-inner commands, `SORT BY`/`GET`
  pattern resolution, module `OpenKey`.
- **What:** stalls the main thread until *this* key's IO resolves. It submits the read if the
  key is `ONLY_FLASH`, then loops on a *selective* drain: completions for the target key are
  processed, every other completion is pushed onto `deferred_completions` and counted in
  `sync_fetch_deferred_count` (`src/ext_storage.c:1112-1130`). Keyspace isolation for the
  running command is the reason — a spill completion processed mid-command could free memory
  the command still references.
- **Ordering rule:** deferred completions run **first** on the next
  `processCompletedStorageRequests()` drain, before anything still queued in the bridge
  (`src/ext_storage.c:1040-1050`).
- **Terminal states:** returns with the key either resident or absent — the caller must
  re-find the entry. `PENDING_DELETION` and a read miss both resolve as absent
  (`src/ext_storage.c:1096`, `src/ext_storage.c:1131`).
- **Never times out** — callers cannot roll back partial execution (a Lua script may already
  have applied writes). Instead it logs a stall warning every 5s
  (`src/ext_storage.c:1140-1147`). Backoff is 50µs steps capped at 200µs
  (`src/ext_storage.c:1136`).
- Metrics `sync_fetch_count` / `sync_fetch_miss_count` / `sync_fetch_wait_us_total` /
  `sync_fetch_wait_us_max` are updated on exit (`src/ext_storage.c:1150-1157`); surfaced via
  [info-metrics](info-metrics.md). Tests: tests/unit/data-tiering/ext-storage-sync-fetch.tcl.

## Eviction, spill-predictor and accounting hooks

- `int extStoragePerformEvictions(int *result)` (`src/ext_storage.h:133`, impl
  `src/ext_storage.c:1297`) — eviction override; returns 1 if tiering handled the decision
  (sets `*result` to `EVICT_OK`/`EVICT_FAIL`), 0 to fall back to standard eviction. See
  [eviction-integration](../components/eviction-integration.md).
- `void extStorageOnSpillSubmit(void)` (`src/ext_storage.h:136`, impl
  `src/ext_storage.c:52-54`) / `void extStorageOnSpillSerialize(size_t bytes)`
  (`src/ext_storage.h:137`, impl `src/ext_storage.c:61-71`) — the two-stage Smith-predictor
  hooks: the main thread counts a submit (shrinking window 1), the IO thread records the exact
  serialized footprint into the EMA (`alpha = 1/16`, initialised on the first real sample).
- `void extStorageUpdateSpillConcurrency(double throttle_rate)` (`src/ext_storage.h:143`, impl
  `src/ext_storage.c:1430`) — **still present**, not removed: the legacy coupled-throttle
  actuator that sets the cap read only by `SPILLING_STRATEGY_V1`. A no-op in effect under the
  default v2 spill strategy, which ignores the cap.
- `void extStorageInflightAddRam(size_t bytes)` (`src/ext_storage.h:154`, impl
  `src/ext_storage.c:46-48`) / `size_t extStorageProjectedMemory(void)`
  (`src/ext_storage.h:155`, impl `src/ext_storage.c:79-90`) — credit in-flight spill RAM, and
  query projected memory (the cap-less spill controller's gate). See
  [memory-accounting](../components/memory-accounting.md) and
  [throttle-equilibrium](../components/throttle-equilibrium.md).

> ⚠️ Misplaced comment in the code (flagged, not fixed): `src/ext_storage.h:135` reads "Called
> from throttle layer to update dynamic spill concurrency" but sits above
> `extStorageOnSpillSubmit`/`extStorageOnSpillSerialize`, which are the Smith-predictor hooks.
> The concurrency actuator it describes is `extStorageUpdateSpillConcurrency` at
> `src/ext_storage.h:143`.

## Serialization callbacks (`src/ext_storage.h:147-153`)

`int extStorageSerializeKey(void *key, char **serialized_key)`,
`int extStorageSerializeValue(void *value, char **serialized_value)`,
`void *extStorageDeserializeKey(char *key, int length)`,
`void *extStorageDeserializeValue(char *value, int length)`,
`void extStorageFreeSerializedKey(void *key)`,
`void extStorageFreeSerializedValue(void *value)`. Detail:
[serialization](../components/serialization.md).

## Snapshot support (`src/ext_storage.h:54-66`)

The fork-based RDB/AOF-rewrite protocol; implementation and the protocol comment live at
`src/ext_storage.c:1690-1829`.

| Fn | Cite | Role |
|----|------|------|
| `int extStorageSnapshotSupported(void)` | `:57` (impl `src/ext_storage.c:1721-1723`) | `ext_data_enabled` **and** the backend advertises snapshot support |
| `int extStorageSnapshotActive(void)` | `:58` (impl `src/ext_storage.c:1725-1727`) | prepare done, `Done` still pending |
| `int extStorageSnapshotPrepare(void)` | `:59` (impl `src/ext_storage.c:1729`) | main thread, before fork/save: bounded settle drain (200 passes of `extStorageBridge_drainOnly()` + completion processing) until no in-flight IO, then park the IO thread and pause GC. Returns `C_ERR` if the backend cannot snapshot |
| `void extStorageSnapshotResume(void)` | `:60` (impl `src/ext_storage.c:1763`) | parent, right after fork — unpark the IO thread; GC stays paused |
| `void extStorageSnapshotDone(void)` | `:61` (impl `src/ext_storage.c:1768`) | child reaped / foreground save done — unpause GC |
| `int extStorageMaterializeTiered(int dbid, robj *key, robj *val, char **payload, size_t *plen)` | `:65` (impl `src/ext_storage.c:1784`) | fork-child (or held-worker main-thread) read of a tiered value. Returns 1 with `*payload`/`*plen` set to `[type byte][rdbSaveObject bytes]` (no version+CRC footer), caller `zfree()`s; 0 = skip this key |
| `sds genExternalStorageSnapshotInfoString(sds info)` | `:66` (impl `src/ext_storage.c:1816`) | `snapshot_supported` / `snapshot_active` / `snapshot_saves` / `snapshot_tiered_values_saved` / `snapshot_tiered_values_skipped` |

`extStorageMaterializeTiered` skips (returns 0, bumping `snapshot_tiered_skipped`) in two cases:
the value is `TIERING_STATE_PENDING_DELETION` — logically gone — or
`extStorageBridge_forkRead()` cannot find it because GC evicted it before the freeze
(`src/ext_storage.c:1787-1801`). Consumers: `src/rdb.c:1210` (inside `rdbSaveKeyValuePair`),
`src/rdb.c:1680`/`src/rdb.c:1752` and `src/aof.c:2654` (prepare/resume/done bracketing). Narrative:
[persistence-replication](../components/persistence-replication.md).

## SWAPDB db-id indirection (`src/ext_storage.h:67-70`)

Flash records and in-flight IO messages are addressed by a **physical** db id that follows a
keyspace across `SWAPDB`; the `server.db[]` index used in command context is the **logical**
id. Identity-mapped at init, so logical N == physical N until the first swap
(`src/ext_storage.c:320-360`).

| Fn | Cite | Role |
|----|------|------|
| `int extStoragePhysicalDbId(int logical_id)` | `:68` (impl `src/ext_storage.c:342-345`) | logical → physical, for every new submission; identity if maps are unallocated |
| `int extStorageLogicalDbId(int physical_id)` | `:69` (impl `src/ext_storage.c:347-350`) | physical → logical, to route a completion back to whichever logical db now owns the keyspace |
| `void extStorageSwapDbIds(int id1, int id2)` | `:70` (impl `src/ext_storage.c:352-360`) | swap both mapping entries; called from `SWAPDB` (`src/db.c:1931`) |

Correctness for IO in flight across a swap comes from `dbSwapDatabases()` moving the entries —
and their tiering-state bits — together with the keyspace
(`src/ext_storage.c:326-333`). Callers of the mapping outside `ext_storage.c`:
`src/db.c:864` (`extStorageBridge_flushDB` on `FLUSHDB`), `src/expire.c:80`
(`extStorageBridge_submitDel` on expiry), `src/debug.c:1133`/`src/debug.c:1189` (`DEBUG SPILL`), and
`extStorageMaterializeTiered` itself (`src/ext_storage.c:1797`). Tests:
tests/unit/data-tiering/ext-storage-swapdb.tcl.

## Runtime guard consuming this API

`updateMaxmemoryPolicy()` (`src/config.c:2620-2635`) rejects a runtime `CONFIG SET
maxmemory-policy` to anything other than `allkeys-lru`, `allkeys-lfu` or `noeviction` while
tiering is active — gated on `ext_data_enabled && extStorageIsInitialized()`, so it does not
fire during config load before init. The same rule is enforced at init (where it disables
tiering instead).

## Debug hook

`extern int ext_storage_debug_pause_completions` (`src/ext_storage.h:43`, defined
`src/ext_storage.c:1033`) — **tests only**. Set via `DEBUG EXT-STORAGE-PAUSE-COMPLETIONS <0|1>`
(`src/debug.c:1088-1096`). When set, `processCompletedStorageRequests()` returns immediately
(`src/ext_storage.c:1038`), holding submitted flash operations — and the clients blocked on
them — in flight, so tests can deterministically exercise in-flight windows such as the SWAPDB
pending-DEL guard.

See also: [engine-integration](../components/engine-integration.md),
[state-machine](../components/state-machine.md),
[persistence-replication](../components/persistence-replication.md).
