---
title: Eviction Integration
status: active
sources:
  - src/evict.c:97-135
  - src/evict.c:378-430
  - src/evict.c:505-545
  - src/evict.c:553-580
  - src/ext_storage.c:76-88
  - src/ext_storage.c:93-155
  - src/ext_storage.c:366-381
  - src/ext_storage.c:405-407
  - src/ext_storage.c:1229-1271
  - src/ext_storage.c:1273-1352
  - src/ext_storage.c:1361-1420
  - src/ext_storage.c:1422-1512
  - src/ext_storage.c:1514-1542
  - src/ext_storage.h:129
  - src/config.c:467
  - src/config.c:776-800
  - src/config.c:920-935
  - src/config.c:2620-2635
  - src/config.c:3415
  - src/config.c:3482-3493
  - .agent/knowledge/config-compatibility.md
updated: 2026-07-30
type: component
tier: working
claim_count: 12
edges:
  - to: flows/spill.md
    kind: refers_to
    source: human
    created: 2026-06-03
  - to: components/engine-integration.md
    kind: refers_to
    source: llm_relation
    created: 2026-06-04
  - to: flows/delete.md
    kind: refers_to
    source: llm_relation
    created: 2026-06-04
  - to: components/state-machine.md
    kind: refers_to
    source: llm_relation
    created: 2026-06-04
  - to: components/throttle-equilibrium.md
    kind: refers_to
    source: llm_relation
    created: 2026-06-04
  - to: interfaces/ext-storage-api.md
    kind: refers_to
    source: llm_relation
    created: 2026-06-04
  - to: decisions/known-limitations.md
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
    note: spill controller gates on projected memory (Smith predictor)
  - to: interfaces/config-and-module-args.md
    kind: configures
    source: human
    created: 2026-07-30
    note: maxmemory-policy guard, ext-storage-spilling-strategy, items-spillover-batch-size
---

# Eviction Integration

> When over `maxmemory`, `performEvictions` **short-circuits into the tiering path and never
> runs the destructive normal eviction loop** — here, freeing memory means spilling to flash
> and waiting for the write completion, not deleting keys. Three pieces: a policy gate
> (which `maxmemory-policy` values tiering will run under), a spill pool (LRU sampling of
> *spillable* keys), and an eviction override (decides OK vs OOM).

![Eviction flow](../diagrams/eviction-flow.png)

## Policy gate: tiering only runs under allkeys-lru / allkeys-lfu / noeviction

Spill victim selection reuses the engine's LRU/LFU sampler over the **main keyspace**, so
`volatile-*` (which must sample the expires index, including for keys already on flash) and
`allkeys-random` are unsupported. The rule is enforced twice, with different consequences:

| When | Site | Effect |
|------|------|--------|
| **Startup** (`extStorage_init`, after config load) | `src/ext_storage.c:373-381` | `serverLog(LL_WARNING, …)` and `ext_data_enabled = 0` — the server starts with **tiering silently disabled** |
| **Runtime** `CONFIG SET maxmemory-policy` | `src/config.c:2620-2635` (`updateMaxmemoryPolicy`, wired as the apply fn at `src/config.c:3415`) | `CONFIG SET` **fails**, value rolls back, tiering keeps running |

Refused set at runtime: `volatile-lru`, `volatile-lfu`, `volatile-random`, `volatile-ttl`,
`allkeys-random`. Accepted: `allkeys-lru`, `allkeys-lfu`, `noeviction`. The guard is
conditioned on `ext_data_enabled && extStorageIsInitialized()` (`src/config.c:2626`), so a
server with `ext-storage-enabled yes` whose init bailed out (bad policy at startup, or backend
open failure) does **not** enforce it — `volatile-*` is freely settable there.

What the client sees, since apply functions run *after* the new value has been written and a
failure triggers `restoreBackupConfig` (`src/config.c:924-933`, defined at
`src/config.c:776-800`):

```
-ERR CONFIG SET failed (possibly related to argument 'maxmemory-policy') -
    maxmemory-policy must be allkeys-lru, allkeys-lfu, or noeviction
    while data tiering (ext-storage-enabled) is active
```

The rollback is whole-invocation: a multi-directive `CONFIG SET` containing a refused policy
restores **every** directive in that call and re-runs their apply fns. Startup is *not* covered
by the apply fn — `loadServerConfigFromString` (`src/config.c:467`) never invokes apply
functions, which is exactly why the separate init guard exists. Config surface and directive
table: [config-and-module-args](../interfaces/config-and-module-args.md).

### noeviction is accepted but makes tiering inert

`performEvictions` runs the tiering branch **before** the `noeviction` check
(`src/evict.c:528-540`), and the in-code comment there claims "with tiering + noeviction, we
still spill (data-preserving)".

> ⚠️ CONTRADICTION: that comment is wrong. All three spill entry points return immediately
> under `MAXMEMORY_NO_EVICTION` — `spillFillToProjected` (`src/ext_storage.c:1379`),
> `spillItemCountBeforeSleep` (`src/ext_storage.c:1455`), `spillItemCountAggressive`
> (`src/ext_storage.c:1490`). Nothing spills. Code wins: `noeviction` + tiering = zero
> spilling.

> ⚠️ CONTRADICTION: `.agent/knowledge/config-compatibility.md` says under `noeviction`
> "writes OOM exactly like vanilla". They do not. Vanilla returns `EVICT_FAIL` at 1.0×
> `maxmemory` (`src/evict.c:542-545`); with `ext_data_enabled` that check is never reached, and
> `extStoragePerformEvictions` reports `C_OK` for everything below the 1.2× hard cap. So
> tiering + `noeviction` lets memory grow to **1.2× maxmemory** with no spilling before writes
> are rejected. Flagged, not fixed — the user-facing guidance ("tiering requires an eviction
> policy to spill") is still right, the OOM-threshold claim is not.

## The performEvictions hook (`src/evict.c:512-540`)

If `ext_data_enabled`, `performEvictions` calls
`processCompletedStorageRequestsAndSpillOldItemsAggressive()` (`src/evict.c:532`) then
`extStoragePerformEvictions(&ts_result)` (`src/evict.c:534`), maps `C_OK`/`C_ERR` to
`EVICT_OK`/`EVICT_FAIL` (`src/evict.c:535`), and `goto update_metrics` (`src/evict.c:539`) —
skipping both the `noeviction` check and the normal loop. The normal loop
(`findBestEvictionCandidate(EvictionPoolLRU,…)` at `src/evict.c:557` + `dbGenericDelete` at
`src/evict.c:576`) runs **only** when tiering is disabled. Consequence: with tiering on,
`dbGenericDelete` is never reached from the eviction path, so **no key — tiered or resident —
is ever evicted** (re-verified 2026-07-30). See [engine-integration](engine-integration.md) and
the [spill flow](../flows/spill.md).

## Spill pool & candidate selection

The spill loops draw candidates from `spillPoolLRU` (allocated in `extStorage_init`,
`src/ext_storage.c:405-407`) via `findBestEvictionCandidate(spillPoolLRU,…)`. That function
(`src/evict.c:378`) is **shared** with normal eviction — the normal path passes
`EvictionPoolLRU` (`src/evict.c:557`), the spill path passes `spillPoolLRU`. It samples keys
across all DBs into the pool with `evictionPoolPopulate` (`src/evict.c:105`, called at
`src/evict.c:409`), ordered by idle/score.

**Spillable-only gate** (`src/evict.c:120`): while `ext_storage_spill_pool_active`
(`src/ext_storage.c:141`), `evictionPoolPopulate` skips any sampled key whose
`tiering_state != TIERING_STATE_ONLY_MEMORY`, so the spill pool only collects keys that can
actually be spilled (not in-flight/tiered ones).

**Empty-pool escape** (`src/evict.c:420-425`): if the filter is active and the pool is still
empty after sampling, `findBestEvictionCandidate` breaks out rather than resampling forever —
the "all keys non-spillable" case.

## Eviction override (`extStoragePerformEvictions`, `src/ext_storage.c:1297-1352`)

Returns 1 (handled) and sets `*result`. Evaluation order matters and **changed**: the raw
hard cap is now checked *first*, deliberately, because the projected (Smith-predictor) credit
can be inflated by stalled/dropped WRITE completions and would otherwise pin `projected`
below `maxmemory` while raw `used_memory` runs away (comment at
`src/ext_storage.c:1302-1309`).

| # | Condition | Result |
|---|-----------|--------|
| 0 | `!ext_data_enabled` | returns **0** — not handled, normal eviction proceeds (`src/ext_storage.c:1298`) |
| 1 | `used_memory > maxmemory + maxmemory/5` (1.2× hard cap) | `C_ERR` + `memory_hard_cap_exceeded_count++` + `oom_reject_write_count++` (`src/ext_storage.c:1310-1315`) |
| 2 | `extStorageProjectedMemory() <= maxmemory` | `C_OK` (`src/ext_storage.c:1320-1324`) |
| 3 | over projected setpoint, under hard cap | `C_OK` **always** (`src/ext_storage.c:1349-1351`); if `total_items_spilling_to_ext_storage == 0` and `total_keys - num_items_on_flash <= 0`, bumps `no_spillable_items_count` as a **metric only** (`src/ext_storage.c:1336-1347`) |

Row 2 is the load-bearing predictor: gating on projected memory
([memory-accounting](memory-accounting.md), `extStorageProjectedMemory` at
`src/ext_storage.c:79-88`) lets the override report `C_OK` as soon as enough drain has been
*committed* (submitted/in-flight), without waiting for the asynchronous completions to actually
free RAM.

> ⚠️ CORRECTED 2026-07-30: this page previously described a five-row table in which
> "nothing left to spill" returned `C_ERR` — the *key-in-RAM-floor OOM*. That path no longer
> exists. The exhausted-spillable case is now purely observational
> (`no_spillable_items_count`, `src/ext_storage.c:1345`) and the write is admitted; the
> throttle is the sole back-pressure below the hard cap
> ([throttle-equilibrium](throttle-equilibrium.md)). **The only write rejection from this
> function is the 1.2× hard cap.** See [known-limitations](../decisions/known-limitations.md).

## Evicting a key already on flash (`extStorageEvictFlashKey`, `src/ext_storage.c:1239-1271`)

Async DELETE with **no fetch**: `ONLY_FLASH → COPYING_TO_MEMORY` (delete in-flight, submitted
against `extStoragePhysicalDbId(db->id)` — the SWAPDB indirection, `src/ext_storage.c:1245`);
if a fetch is already in-flight it becomes `PENDING_EVICT` (`src/ext_storage.c:1258`);
`COPYING_TO_FLASH`, `PENDING_EVICT`, `ONLY_MEMORY` and anything else return -1. Same path as
the [delete flow](../flows/delete.md). FlashCache's own internal GC eviction surfaces
separately, through the [bridge](bridge-layer.md)'s `req_ctx == NULL` branch, as a synthetic
DELETE.

> ⚠️ FLAGGED: `extStorageEvictFlashKey` has **no caller in the tree** — the only references
> are its declaration (`src/ext_storage.h:129`) and its definition. Nothing in `src/`,
> `modules/` or `tests/` invokes it. It is currently dead code; the "ONLY_FLASH → async
> delete" branch of the diagram above is not reachable from the eviction path. Tracked as
> issue #21. Flagged, not
> fixed.
>
> It also does **not** handle `TIERING_STATE_PENDING_DELETION` (state 5, added by commit
> 6976634d2) explicitly — that falls into `default:` and returns -1, which is the safe answer.

## Two spill controllers, selected by config

`ext-storage-spilling-strategy` (`src/config.c:3493`, default `v2`) picks the controller; both
pumps dispatch on it — `processCompletedStorageRequestsAndSpillOldItems` (beforeSleep/timer,
`src/ext_storage.c:1522-1530`) and `processCompletedStorageRequestsAndSpillOldItemsAggressive`
(per-command, from `src/evict.c:532`; `src/ext_storage.c:1534-1542`). Both early-return when
`!ext_data_enabled` or `maxmemory == 0`.

### v2 — `spillFillToProjected` (default, cap-less, projected-gated) `src/ext_storage.c:1378-1420`

There is **no fixed concurrency cap**. The loop runs
`while (extStorageProjectedMemory() > server.maxmemory)` (`src/ext_storage.c:1393`), drawing
candidates from `findBestEvictionCandidate(spillPoolLRU,…)` (`src/ext_storage.c:1396`). Queue
depth is an **emergent output** of the projected-memory signal, not a prescribed constant. It
deliberately has **no memory-headroom clamp** — halting on memory pressure would starve the
disk and *raise* memory (only completions free RAM); disk saturation is handled implicitly by
the [throttle](throttle-equilibrium.md), which reads raw `used_memory`.

Stop conditions (six, not three):

| Stop | Site |
|------|------|
| policy is `noeviction` (never even starts) | `src/ext_storage.c:1379` |
| setpoint reached — `projected <= maxmemory` | `src/ext_storage.c:1380`, `src/ext_storage.c:1393`, re-check `src/ext_storage.c:1401` |
| no spillable candidate (`spill_skipped_null`) | `src/ext_storage.c:1397` |
| `EVPOOL_SIZE` consecutive non-spillable candidates (TOCTOU re-validation budget) | `src/ext_storage.c:1406-1410` |
| submit-queue backpressure (`spillItemAsync` returns -1) | `src/ext_storage.c:1412` |
| cold-start brake `spill_submitted >= 64` | `src/ext_storage.c:1416` |

> ⚠️ FLAGGED (suspected defect): the cold-start brake at `src/ext_storage.c:1416` tests
> `spill_submitted`, which is a **process-lifetime cumulative counter**
> (`src/ext_storage.c:149`, reported in `INFO`), not a per-pass counter. Once 64 spills have
> ever been submitted the condition is permanently true, so the loop breaks after its **first**
> submit on every subsequent pass — capping the "cap-less" controller at ~1 submit per
> event-loop pass for the rest of the process's life. Either the counter or the brake is wrong.
> Flagged, not fixed; report to the code owner.

The `TODO(perf)` at `src/ext_storage.c:1384-1392` documents the known cold-start/stale-mean
under-braking: `mean_spill_ram` is 0 until the first serialize, so the predictor does not brake
the first overshoot.

### v1 — legacy ITEM_COUNT strategy `src/ext_storage.c:1441-1512`

Restored verbatim from pre-Smith HEAD (commit b0f5bb9e^) for A/B comparison, with one
intentional fix: the aggressive path now sets `ext_storage_spill_pool_active` around candidate
selection (`src/ext_storage.c:1494`), which the original omitted (pool pollution).

- `spillItemCountBeforeSleep` (`src/ext_storage.c:1454`): spills at **1.0×** `maxmemory`
  (`src/ext_storage.c:1458`), bounded per tick by `items_spillover_batch_size`
  (`src/ext_storage.c:1462`) **and** the dynamic `max_num_concurrent_items_spilled` cap
  (`src/ext_storage.c:1463`).
- `spillItemCountAggressive` (`src/ext_storage.c:1489`): spills at **1.1×** `maxmemory`
  (`src/ext_storage.c:1492`), bounded only by the cap (`src/ext_storage.c:1496`).
- The cap itself is `SPILL_CONCURRENT_BASE` = 50 … `SPILL_CONCURRENT_LIMIT` = 200
  (`src/ext_storage.c:120-122`), ramped by `extStorageUpdateSpillConcurrency(throttle_rate)`
  (`src/ext_storage.c:1430-1439`) — which is only ever called by the **v1 (coupled)** throttle.
  With v1 spilling and v2 throttling the cap is never updated and stays at BASE
  (`src/ext_storage.c:116-119`).

> ⚠️ CORRECTED 2026-07-30: this page previously stated that
> `extStorageUpdateSpillConcurrency` and the `SPILL_CONCURRENT_BASE`/`SPILL_CONCURRENT_LIMIT`
> cap were **removed**, and that `items_spillover_batch_size` (`src/ext_storage.c:95`) plus its
> `ext-storage-items-spillover-batch-size` directive (`src/config.c:3482`) were **dead
> config**. Both claims are false against the current tree: all of it is live under
> `ext-storage-spilling-strategy` = `v1`. The removal comment still sitting at
> `src/ext_storage.c:1273-1278` ("extStorageUpdateSpillConcurrency was removed") is itself
> stale — the function is defined 152 lines below it at `src/ext_storage.c:1430`. The wiki had
> believed the comment over the code. Only under the **default v2** strategy are the batch size
> and the cap ignored.

See also: [spill](../flows/spill.md), [engine-integration](engine-integration.md), [memory-accounting](memory-accounting.md), [ext-storage-api](../interfaces/ext-storage-api.md), [config-and-module-args](../interfaces/config-and-module-args.md).
