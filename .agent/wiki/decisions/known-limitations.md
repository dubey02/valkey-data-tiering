---
title: Known Limitations
status: active
sources:
  - src/server.h:779-839
  - src/object.c:632-634
  - src/object.c:1208-1211
  - src/ext_storage.c:79-88
  - src/ext_storage.c:95
  - src/ext_storage.c:100-104
  - src/ext_storage.c:240
  - src/ext_storage.c:341
  - src/ext_storage.c:359-434
  - src/ext_storage.c:444-593
  - src/ext_storage.c:814
  - src/ext_storage.c:827
  - src/ext_storage.c:938-983
  - src/ext_storage.c:1009-1046
  - src/ext_storage_throttle.c:220-247
  - src/evict.c:378-470
  - src/ext_storage_bridge.c:68-104
  - src/server.c:4718-4735
  - src/config.c:3332
  - src/config.c:3339
  - src/config.c:3426
  - src/rdb.c:1190-1195
  - src/rdb.c:1191-1212
  - src/rdb.c:1487
  - src/rdb.c:1675-1691
  - src/rdb.c:1736
  - src/rdb.c:1751-1761
  - src/rdb.c:3845-3947
  - src/rdb.c:4028-4036
  - src/rdb.c:4079-4085
  - src/aof.c:2356-2409
  - src/aof.c:2414-2451
  - src/aof.c:2458-2510
  - src/aof.c:2551-2558
  - src/aof.c:2653-2663
  - src/ext_storage.c:1694-1719
  - src/ext_storage.c:1721-1723
  - src/ext_storage.c:1784-1814
  - src/ext_storage_bridge.c:310-319
  - src/defrag.c:705-721
  - src/replication.c:1017-1022
  - src/cluster_migrateslots.c:1571-1572
  - tests/unit/data-tiering/ext-storage-snapshot.tcl:1-14
  - tests/unit/data-tiering/ext-storage-persistence.tcl:82-108
  - src/storage/storage.h:130
  - src/storage/storage.h:177-179
  - src/storage/storage_mock.c:414-418
  - src/storage/storage_flashcache_real.c:191-201
  - src/module.c:934-946
  - modules/flash-tiering/src/lib.rs:552-559
  - modules/flash-tiering/src/backends/flashcache/backend.rs:101-103
  - modules/flash-tiering/src/backends/rocksdb/backend.rs:218-224
  - DATA-TIERING.md
updated: 2026-07-30
type: decision
tier: wisdom
claim_count: 18
edges:
  - to: decisions/adr-index.md
    kind: refers_to
    source: human
    created: 2026-06-03
  - to: components/serialization.md
    kind: contradicts
    source: human
    created: 2026-06-03
    note: string-only limit superseded by createDumpPayload (resolved)
  - to: components/backends.md
    kind: refers_to
    source: llm_relation
    created: 2026-06-05
  - to: components/persistence-replication.md
    kind: refers_to
    source: llm_relation
    created: 2026-06-05
  - to: components/eviction-integration.md
    kind: refers_to
    source: llm_relation
    created: 2026-06-05
  - to: components/throttle-equilibrium.md
    kind: refers_to
    source: llm_relation
    created: 2026-06-05
  - to: components/engine-integration.md
    kind: refers_to
    source: llm_relation
    created: 2026-06-05
  - to: components/memory-accounting.md
    kind: refers_to
    source: llm_relation
    created: 2026-06-05
  - to: components/bridge-layer.md
    kind: refers_to
    source: llm_relation
    created: 2026-06-05
  - to: 00-overview.md
    kind: refers_to
    source: llm_relation
    created: 2026-06-05
  - to: components/testing.md
    kind: refers_to
    source: human
    created: 2026-06-17
    note: L1 persistence-gap coverage; the AOF-reload test now asserts tiered keys survive
---

# Known Limitations

> Code-grounded contradictions and functional constraints in the data tiering POC.
> Every entry cites `file:line` in **this** repo; where a comment or legacy doc disagrees
> with the code, the **code is authoritative**.

## Contradictions (code vs comment / design doc)

| # | What | Code is… | Comment/doc claims… | Cite |
|---|------|----------|---------------------|------|
| C1 | tiered `val_ptr` | **empty-SDS placeholder** (freed via `sdsfree`, sized via `sdsAllocSize`) | `val_ptr is NULL` | `server.h:838` vs `object.c:632-634`, `object.c:1208-1211` |
| C3 | metadata on a tiered key | gate is **value-agnostic** → any named-key cmd blocks+fetches | EXISTS/TYPE/TTL answered from RAM, no fetch | `server.c:4718` + `ext_storage.c:359-434` vs `DATA-TIERING.md` |
| C4 | sync `rocksdb` backend | **RESOLVED** (Jul 2026) — file deleted. `ext-storage-backend=rocksdb` routes to the in-memory mock | N/A | `storage_mock.c:418`, `src/storage/storage.h:177-179` (see [backends](../components/backends.md)) |
| C5 | `key_may_exist` | implemented for **both** flash-tiering backends, registration **commented out** → engine falls back to "may exist" | (bloom probe available) | `flash-tiering/src/lib.rs:552-559`, `module.c:944-946` (see [backends](../components/backends.md)) |
| C6 | plain-AOF rewrite of tiered keys | **RESOLVED** (Jul 2026) — both non-preamble loops now skip tiered objects with a warning instead of emitting the placeholder | N/A | `aof.c:2446-2450`, `aof.c:2504-2508` (see R4 below) |
| C7 | `items_spillover_batch_size` config | **strategy-gated, not dead** — the default v2 (PROJECTED) controller ignores it; the v1 (ITEM_COUNT) controller reads it | comment/wiki previously called it dead config | `ext_storage.c:95`, `:1462`, `config.c:3482`, `:3493` (see [eviction-integration](../components/eviction-integration.md)) |
| C8 | the two non-preamble skip log messages | **transposed** — the slot-migration loop logs *"AOF rewrite (non-preamble)"*; the non-preamble AOF loop logs *"Slot snapshot"* | each message names the path it is in | `aof.c:2414` + `:2447` vs `aof.c:2458` + `:2505` |
| C9 | `extStorageUpdateSpillConcurrency` | **still live** — defined `ext_storage.c:1430`, called `ext_storage_throttle.c:257` | two comments say it *"was removed"* | `ext_storage.c:1274`, `ext_storage_throttle.c:326` vs `:1430`, `:257` |

> Note: former **C2** (`SPILL_CONCURRENT_BASE` comment "2" vs `#define` 50) is **retired** — the
> Smith-predictor commit deleted the macro and the dynamic-concurrency cap entirely. See R2 below.

### C1 — `val_ptr` NULL vs empty-SDS placeholder
`server.h:838` documents `OBJ_ENCODING_TIERED` as *"val_ptr is NULL (value on disk)"*, but the
code keeps an **empty SDS** as the placeholder: `decrRefCount` frees it with `sdsfree`
(`object.c:632-634`) and `objectComputeSize` adds `sdsAllocSize(placeholder)`
(`object.c:1208-1211`). A reader trusting the comment would mishandle the placeholder. See
[memory-accounting](../components/memory-accounting.md).

### C7 — `items_spillover_batch_size` is strategy-gated, not dead config
The variable (`ext_storage.c:95`) and its `ext-storage-items-spillover-batch-size` directive
(`config.c:3482`, range 1–100, default 10) are read by the **legacy ITEM_COUNT spill loop**
(`ext_storage.c:1462`: `while (num_items_spilling < items_spillover_batch_size)`), which is
selected by `ext-storage-spilling-strategy=v1` (`config.c:3493`, enum at `config.c:80`). Under the
**default** `v2` PROJECTED (Smith-predictor) controller the directive genuinely has no effect —
queue depth is gated purely on projected memory. So the directive is inert only for the default
strategy, not globally.

> ⚠️ CORRECTED (2026-07-30): this entry previously said the config was **dead**, on the belief that
> the cap-less controller had replaced the batch loop outright. Both controllers are present and
> selectable — the code calls them "A/B benchmark switches" whose "defaults preserve the current
> HEAD behavior" (`ext_storage.c:109-112`). See
> [eviction-integration](../components/eviction-integration.md).

### C3 — metadata "no fetch" design intent vs value-agnostic gate
`DATA-TIERING.md` intends metadata commands (EXISTS/TYPE/TTL/EXPIRE) to be answered from
RAM without a fetch. But `preCommandExec` (`server.c:4718`) has **no command-type guard** and
`keyBlocksClient` blocks+fetches any *named key* in `ONLY_FLASH` (`ext_storage.c:359-434`, state
cases at `:398`). So those metadata commands **do** block and fetch the value; only `SCAN`/`DBSIZE`
(no named key) and `DEL`/`UNLINK` (async delete) avoid the fetch. (`server.c:4721-4735` even adds a
`TIERED_SAFETY` re-check that re-runs `preCommandExec` if a key is tiered post-gate.) See
[00-overview](../00-overview.md), [engine-integration](../components/engine-integration.md).

### C5 — `key_may_exist` implemented but deliberately unregistered
Both flash-tiering backends implement the probe — `fc_key_exists` for FlashCache
(`flash-tiering/src/backends/flashcache/backend.rs:101-103`) and `key_may_exist_cf` for RocksDB
(`flash-tiering/src/backends/rocksdb/backend.rs:218-224`) — and the C-ABI callback plus its FFI
setter both exist. The registration call is **commented out** in module init, with the reason in
the code itself: *"Registering it causes false positives that block clients forever."*
(`flash-tiering/src/lib.rs:552-559`). With no subscriber callback,
`moduleExternalStorageKeyMayExist` falls through to a conservative `return 1`
(`module.c:944-946`), so `keyBlocksClient` never receives a negative answer and the probe saves
no fetches. Re-verified against the current tree on 2026-07-30 (the code moved from the retired
non-key-spilling module to flash-tiering; the claim and the stated reason are unchanged). The
standalone rocksdb-tiering module implements no probe at all.

### C8 — the two non-preamble skip warnings name each other's path
Both non-preamble serialization loops skip tiered objects, but their log strings are swapped.
`rewriteSlotToAppendOnlyFileRio` (`aof.c:2414`) is reached only from slot migration
(`cluster_migrateslots.c:1571-1572`), yet its skip logs *"AOF rewrite (non-preamble): skipping
tiered key (enable aof-use-rdb-preamble for full coverage)"* (`aof.c:2447-2448`).
`rewriteAppendOnlyFileRio` (`aof.c:2458`) is reached only from the non-preamble AOF rewrite
(`aof.c:2551-2558`), yet its skip logs *"Slot snapshot: skipping tiered key (tiered slot
migration not yet supported)"* (`aof.c:2505-2506`). The **behaviour** is identical on both paths
(skip + `LL_WARNING`); only the operator-facing messages mislead — an operator running with
`aof-use-rdb-preamble no` sees "Slot snapshot" warnings with no slot migration in progress, and
vice versa. Cosmetic, but it inverts the diagnostic signal for exactly the two limitations in L1.

> C4 is documented in full on [backends](../components/backends.md); the persistence paths
> (C6, C8, L1, L8, R3, R4) are the subject of
> [persistence-replication](../components/persistence-replication.md).
> ⚠️ As of this pass (2026-07-30) that page has been re-grounded onto the materialization
> account in the same branch, so the two now agree. Where any residual disagreement remains,
> **this page and the code win**; the citations below were opened against the current `unstable`
> tree.

### C9 — comments assert `extStorageUpdateSpillConcurrency` "was removed"; it is still live
Two comments state the dynamic spill-concurrency actuator is gone —
`ext_storage.c:1274` and `ext_storage_throttle.c:326` both read
*"extStorageUpdateSpillConcurrency was removed"*. It is not: the function is defined at
`ext_storage.c:1430` (labelled there as the "legacy dynamic spill-concurrency actuator"),
declared at `ext_storage.h:143`, and still called at `ext_storage_throttle.c:257`. The accurate
statement is the one at `ext_storage.c:117-121`: the cap is read **only** by the legacy
ITEM_COUNT spill strategy, and the default PROJECTED strategy ignores it. The "removed" comments
describe the *default* path as if it were the only path.

This is the contradiction that caused this wiki to record R2 as resolved-by-deletion; see R2.

## Functional limitations

### L1 — Persistence coverage is not universal: non-preamble AOF, slot migration, diskless full-sync; and tiering placement is never replicated
**Scope correction (2026-07-30):** the previous form of this entry — *"tiered cold data is not
persisted or replicated"*, on the claim that `rdbSaveKeyValuePair` returns `0` for tiered values —
is **false against the current tree**. That path now materializes the on-flash payload into a
standard RDB entry; see **R3** below for the proof and the citations. What remains are four
narrower, individually-verified gaps.

**1. The non-preamble AOF rewrite drops tiered keys.** `rewriteAppendOnlyFileRio`
(`aof.c:2458`) is reached only when `server.aof_use_rdb_preamble` is off (`aof.c:2551-2558`), and
it `continue`s past every tiered object after a `LL_WARNING` (`aof.c:2504-2508`).
`aof-use-rdb-preamble` defaults to **yes** (`config.c:3339`), so this is the non-default path —
but a node configured with it off writes a rewritten AOF that is missing every flash-resident
key, with only a log line to say so. (Note the misleading message text — C8.)

**2. Cluster slot migration is unsupported with tiering.** `rewriteSlotToAppendOnlyFileRio`
(`aof.c:2414`), whose only caller is the slot-migration snapshot
(`cluster_migrateslots.c:1571-1572`), skips tiered objects the same way (`aof.c:2446-2450`). The
in-code statement of the limitation — *"Slot migration is not yet supported with tiering"* — sits
at `aof.c:2503`, i.e. attached to the **other** loop (that is precisely the transposition in C8);
the reason it gives is real and applies here: the RESTORE-based emit needed to serialize a tiered
value into command form is unimplemented. A migrated slot therefore arrives at the target missing
exactly the keys that were on flash. Unlike gap 1 there is **no** configuration that avoids it.

**3. Diskless full-sync forks without the snapshot protocol.** `rdbSaveToReplicasSockets`
(`rdb.c:3845`) is the default replication path — `repl-diskless-sync` defaults to **yes**
(`config.c:3332`) and `replication.c:1017-1022` dispatches to it whenever `socket_target` holds.
It forks (`rdb.c:3925`) and the child serializes through `rdbSaveRioWithEOFMark`
(`rdb.c:3947`) → `rdbSaveKeyValuePair` → `extStorageMaterializeTiered`, i.e. it *does* read
flash in the child — but it is the **only** fork/save entry point that never calls
`extStorageSnapshotPrepare()`. Every other one does: foreground `rdbSave` (`rdb.c:1675-1691`),
`rdbSaveBackground` (`rdb.c:1751-1761`), AOF-rewrite background (`aof.c:2653-2663`). So the
child can fork with keys still in a `COPYING_*` state, with the backend IO thread unparked, and
with on-flash GC running — the three conditions the protocol comment
(`ext_storage.c:1694-1714`) exists to exclude. It also lacks the fail-closed guard those sites
carry (L8), so a backend that cannot snapshot is not refused here. Disk-target full sync is
unaffected: it routes through `rdbSaveBackground` (`replication.c:1021`) and is covered.
> ⚠️ **Flagged, not demonstrated.** This is a code-level asymmetry — three prepare sites, one
> without. No test exercises a replica full-sync with tiered values (see
> [testing](../components/testing.md) § What is not covered), and no corrupt-sync or torn-payload
> failure has been reproduced. `extStorageBridge_forkRead` (`ext_storage_bridge.c:310-319`)
> returns `-1` on a miss rather than faulting, so the likely symptom is silently skipped keys
> (counted in `snapshot_tiered_values_skipped`, `ext_storage.c:1719`) rather than a crash — but
> that is inference, not evidence. Tracked as issue #20. Needs a maintainer decision on whether the omission is
> deliberate.

**4. Which keys are tiered is node-local and never replicated.** The RDB entry a tiered value
produces is deliberately indistinguishable from an in-memory one — *"loadable by any node, tiered
or not (on load the value starts in memory and re-tiers under memory pressure)"*
(`rdb.c:1196-1198`). So a replica or a restarted node receives **values**, never placement: it
re-derives its own flash residency from its own memory pressure against its own backend. Nothing
in `ext_storage.c` consults primary/replica role, and no spill, fetch or eviction decision is
propagated. Consequences: a replica's `num_items_on_flash` and INFO tiering counters are its own,
a failover changes the flash working set, and a replica sized with less DRAM than its primary
will spill more (and serve more reads through flash) for the identical dataset. This is by design
per the replication design note (`ext_storage.c:1695`), not a defect — but it is a real operating
constraint and is the part of the old L1 that survives intact. See
[persistence-replication](../components/persistence-replication.md).

### L2 — Out-of-capacity writes are rejected (two OOM paths)
`extStoragePerformEvictions` (`ext_storage.c:938`) rejects writes (`C_ERR`) in two cases:
- **Hard cap:** `used_memory > 1.2× maxmemory` (`ext_storage.c:952`) — the spill controller owns
  the 1.0–1.1× band on *projected* memory and the throttle the 1.1–1.2× band on *raw* memory; this is
  the hard reject at 1.2×.
- **Key floor:** when `total_keys <= num_items_on_flash` (`ext_storage.c:973`) — everything
  spillable is already on flash and nothing is in flight, so there is nothing left to evict. This
  bites the key-heavy / tiny-value regime where per-key metadata in RAM dominates and cannot be
  spilled. See [eviction-integration](../components/eviction-integration.md).

### L3 — Single storage IO thread (head-of-line blocking)
The bridge initializes the backend with `io_threads = 1` (`ext_storage_bridge.c:89`). A large-value
fetch blocks smaller fetches queued behind it; small-object tail latency in mixed-size workloads is
bounded by the largest in-flight fetch, not by NVMe random-read latency. See
[backends](../components/backends.md), [bridge-layer](../components/bridge-layer.md).

### L4 — Small/embedded values are un-spillable; memory-gated throttle then collapses throughput
Two constraints compound on key-heavy / tiny-value workloads:

1. **Embedded values never spill.** `isEmbeddedObject` (`ext_storage.c:341`) treats
   `OBJ_ENCODING_EMBSTR` (string ≤44B) and `OBJ_ENCODING_INT` as embedded, and `spillItemAsync`
   rejects them (`ext_storage.c:814`, `return -1`). The value lives in the object header itself,
   so there is nothing to move to flash. Combined with the per-key metadata floor (keys never
   leave RAM — see L2), a workload of many small values pins `used_memory` **above
   `maxmemory`** no matter how well the spill controller runs. The spill loop still *attempts*
   every LRU victim, but only the RAW-encoded (>44B) minority can submit.

2. **The throttle reads raw memory and misreads un-spillable bytes as disk backlog.**
   `extStorageThrottle_adjustRate` gates on raw `zmalloc_used_memory()` (`ext_storage_throttle.c:220`),
   engaging at 1.1× (`:221`) and maxing at 1.2× (`:222`). Its design assumes
   `used_memory − maxmemory` is *in-flight spill backlog* (a disk-saturation proxy). When the excess
   is **un-spillable embedded content**, that assumption is false: memory sits permanently ≥1.2×, so
   the throttle pins `allowed_tps` to `max_tps × (1 − rate·0.99)` with a hard 1000-TPS floor
   (`:246-247`) — for the entire run, even though the disk is idle. This is an inverted control
   hierarchy: the throttle should engage on disk saturation, not on memory it can never drain.

**Empirical** (benchmark results fullrun-20260608, W2/flashcache, 255,333 keys, cluster52 trace
values 1–249 B): `used_memory` pinned 44–55 MB vs `maxmemory` 32 MB; `throttle_current_rate=1.0`,
`throttle_allowed_tps=2000` the whole run; disk utilization 1–3 %; `spill_attempts=5,218,910` vs
`spill_submitted=102,348` (~2 %, the RAW-encoded minority). Result: ~2 K TPS vs ~126 K on the prior
(pre-memory-gated-throttle) run — a regression isolated to W1/W2. W3/W4 use 400-byte values (RAW,
spillable) so memory holds near `maxmemory`, the throttle never pins, and they are unaffected
(97 K / 164 K). (Benchmark figures are empirical, not derivable from code.) See
[throttle-equilibrium](../components/throttle-equilibrium.md),
[eviction-integration](../components/eviction-integration.md) (L2 key floor),
[memory-accounting](../components/memory-accounting.md).

### L5 — Spill controller livelocks on a large instantaneous memory overshoot
The cap-less spill controller `spillFillToProjected` (`ext_storage.c:1009`) loops
`while (extStorageProjectedMemory() > maxmemory)` (`ext_storage.c:1024`) submitting spills.
`projected` only falls via *in-flight* credit — `inflight_spill_ram_bytes` (window 2) plus
`submit_depth * mean_spill_ram` (window 1) — and in-flight is bounded by the FC submit
ring (~4096 items). RAM is only truly reclaimed on **completion**, which is drained in
`beforeSleep` / the periodic timer — i.e. only *after the loop returns to the event loop*.

So when `used_memory` starts far above `maxmemory` (overshoot ≫ ring × mean_value, e.g.
~250MB resident vs a 100MB cap → ~150MB overshoot vs a ~2MB in-flight credit ceiling),
`projected` can **never** reach `maxmemory` from inside the loop. The loop submits without
bound, draining the entire resident `ONLY_MEMORY` set into the in-flight queue, and then:
- if the IO thread keeps the ring drained, candidates exhaust and `findBestEvictionCandidate`
  (`evict.c:378`) — whose `while (bestkey == NULL)` loop (`evict.c:386`) exits only via
  `if (!total_keys) break` (`evict.c:418`), never true here — **spins at 100% CPU on the main
  thread** inside `evictionPoolPopulate` (`evict.c:409`): the event loop never runs, completions
  never drain, RAM never frees — a hard livelock (server stops answering PING/INFO);
- if the IO thread can't keep up, the ring fills and `spillItemAsync` returns −1 → `break`
  (the only graceful exit).

Confirmed by gdb on a wedged server (a W4-equivalent mixed-rw zipfian-flashcache run, 500K×400B,
maxmemory dropped from ∞→32MB after populate): main thread spinning in
`hashtableSampleEntries ← evictionPoolPopulate ← findBestEvictionCandidate ←
spillFillToProjected ← beforeSleep`.

**Mitigation status (partial, mechanism not fully confirmed):** the harness no longer drops
`maxmemory` after an uncapped populate; the mixed-rw scenario applies `maxmemory` **before**
populate so spilling is incremental. This was sufficient for uniform-flashcache (100MB cap):
`used` held at the cap, no livelock, ~81K TPS. It was **NOT** sufficient for
zipfian-flashcache (32MB cap), which still wedges **during populate** — the identical
sequential populate runs fine at 100MB but hangs at 32MB. Observed at the wedge (32MB):
`used` pinned at the cap, spilling ~115K/s, in-flight ≈ 0, **disk_util 99%** (vs 34–70% at
100MB). So the failure correlates with **disk saturation under the tight cap** (tiny resident
headroom forces almost every write to spill immediately), not with overshoot magnitude — in
fact the 100MB cap has the *larger* absolute overshoot at 1.2×, so "overshoot size" does not
explain the cap dependence. The precise spin trigger for the during-populate wedge is **not
yet confirmed** (the earlier gdb backtrace was captured from a different trigger, the
post-populate cap-drop). A fresh gdb backtrace at the 32MB during-populate wedge is needed to
pin it down. **Deferred fix (the real one):** bound per-pass submission and yield to drain
completions (gate on `inflight < bound`, not only `projected > maxmemory`), and make
`findBestEvictionCandidate` return NULL on a no-candidate sample pass as a backstop.
(Benchmark/gdb evidence is empirical, not derivable from code.) See
[eviction-integration](../components/eviction-integration.md),
[memory-accounting](../components/memory-accounting.md),
[throttle-equilibrium](../components/throttle-equilibrium.md).

### L6 — `objectComputeSize(NULL, …)` would deref-crash on a module-typed spill
The IO-thread footprint measurement passes a **NULL key** to `objectComputeSize`
(`storage_flashcache_real.c:197`). That is safe for every currently-spillable type (string/list/
set/zset/hash/stream ignore the key arg), but the `OBJ_MODULE` size path dereferences the key — a
NULL deref / segfault. `spillItemAsync` does **not** currently exclude `OBJ_MODULE` candidates
(the `TODO(safety)` note in the IO worker, `storage_flashcache_real.c:191-201`). Latent crash if a
module-typed value ever becomes a spill candidate. See [serialization](../components/serialization.md),
[memory-accounting](../components/memory-accounting.md).

### L7 — Smith-predictor cold-start / stale-mean under-braking
A milder, transient form of the **L5** mechanism. The window-1 credit in `extStorageProjectedMemory`
(`ext_storage.c:79-88`) is `submit_depth × mean_spill_ram`, but `mean_spill_ram` is `0` until the
first serialize completes and `used_memory` does not drop within a single fill pass (RAM frees only
on completion). So on the **first** overshoot the predictor does not brake `spillFillToProjected` —
it stops only on candidate exhaustion or `FC_REQ_RING` submit backpressure (~4096 submits). The same
under-braking recurs when the EMA lags actual size on heavy-tailed / mixed-size workloads. Documented
as a `TODO(perf)` in the controller (`ext_storage.c:1015`); benign for the same-size target workload
(converges in one pass), but the acute version under a tight cap is the L5 livelock. See
[memory-accounting](../components/memory-accounting.md), [eviction-integration](../components/eviction-integration.md).

### L8 — With a non-snapshotting backend, tiered data disables persistence entirely (fail-closed)
Snapshot capability is a **backend property**: `extStorageSnapshotSupported()`
(`ext_storage.c:1721-1723`) is `ext_data_enabled && extStorageBridge_snapshotSupported()`. When it
is false and any value is on flash (`num_items_on_flash > 0`), every save path refuses rather than
writing a lossy artifact:

| Path | Refuses at | Observable |
|------|-----------|------------|
| foreground `rdbSave` (SAVE, DEBUG RELOAD, SHUTDOWN save, cron auto-save) | `rdb.c:1679-1691` | `errno = EPERM`, `C_ERR`, rate-limited *"Refusing RDB save"* every 60 s |
| `rdbSaveBackground` (BGSAVE **and** disk-target full sync) | `rdb.c:1751-1761` | `lastbgsave_status = C_ERR`, *"Refusing background save"* |
| background AOF rewrite | `aof.c:2653-2663` | `aof_lastbgrewrite_status = C_ERR`, *"Refusing AOF rewrite"* |
| `SAVE` command | `rdb.c:4028-4036` | client error, before any work |
| `BGSAVE` command | `rdb.c:4079-4085` | client error, before any work |

Refusing is the right call — the alternative is a snapshot silently missing its cold set — and the
code says so: module-registered backends without snapshot support *"still refuse, loudly"*
(`rdb.c:1675-1676`). The **operational** limitation is the blast radius: for as long as a single
value sits on flash, such a node has no RDB, no AOF rewrite, and no disk-target replica bootstrap,
and it recovers only by draining flash back to RAM. Note the asymmetry with L1 gap 3 — diskless
full-sync carries no such guard, so the one path that is *not* refused is also the one that does
not prepare. See [backends](../components/backends.md) for which backends support snapshotting.

> Note (scanned this pass, **not** limitations): `defragKey` (`defrag.c:705`) returns early for
> tiered objects (`defrag.c:712-713`) — there is no in-memory value allocation left to relocate —
> and also for keys in `COPYING_TO_FLASH` / `COPYING_TO_MEMORY` / `PENDING_EVICT`
> (`defrag.c:715-721`), where the storage IO thread holds references. `dismissObject` is likewise
> skipped for tiered objects in the RDB fork child (`rdb.c:1487`). All three are deliberate
> correctness guards, not gaps. ⚠️ The in-flight skip does mean RAM held by a key with tiering IO
> outstanding is not defragmented on that pass; transient by construction, and the magnitude is
> unmeasured — flagged rather than claimed.

## Resolved / historical

### R1 — "String values only" → all types via DUMP
An earlier limitation restricted spilling to `OBJ_STRING`. The current serializer uses
`createDumpPayload` (`ext_storage.c:240`) / `rdbLoadObject`, which handles **every** object type
(comment at `ext_storage.c:827`). The string-only constraint is **superseded** (symmetric
`contradicts` edge with [serialization](../components/serialization.md)).

### R2 — `SPILL_CONCURRENT_BASE` / dynamic-concurrency cap is strategy-gated (was C2)
The former C2 contradiction — a stale *"only 2 concurrent spills"* comment against
`#define SPILL_CONCURRENT_BASE 50` — is resolved in the sense that the cap no longer governs the
**default** path: the v2 PROJECTED (Smith-predictor) controller is cap-less and ignores it
entirely.

> ⚠️ CORRECTED (2026-07-30): this entry previously claimed resolution **by deletion** — that the
> macros, `max_num_concurrent_items_spilled` and `extStorageUpdateSpillConcurrency` had been
> removed outright. They are all still in the tree, gated to the legacy strategy:
> `SPILL_CONCURRENT_BASE`/`SPILL_CONCURRENT_LIMIT` (`ext_storage.c:120-121`),
> `max_num_concurrent_items_spilled` (`:122`, reset at `:1432`), and
> `extStorageUpdateSpillConcurrency` (`:1430`, still called from
> `ext_storage_throttle.c:257`). The code's own comment is explicit: the cap is "used ONLY by the
> legacy ITEM_COUNT spill strategy" and the PROJECTED strategy "ignores this entirely"
> (`ext_storage.c:117-121`).

See C9 for the source comments that assert the removal, [eviction-integration](../components/eviction-integration.md), [adr-index](adr-index.md).

### R3 — Tiered values ARE persisted: RDB, AOF preamble, disk-target full sync (was most of L1)
The old L1 rested on `rdbSaveKeyValuePair` returning `0` for tiered values. That is no longer what
the function does. It now materializes the payload and emits a normal entry
(`rdb.c:1191-1212`); `return 0` survives only as the *skip* case for a key that is logically gone —
`PENDING_DELETION` (a client DEL in flight) or GC-evicted before the freeze (`rdb.c:1211`,
`ext_storage.c:1784-1814`). Proof, all opened against the current tree:

| Claim | Citation |
|-------|----------|
| tiered value → **standard** RDB entry, *"loadable by any node, tiered or not"* | `rdb.c:1192-1198` |
| synchronous, fork-child-safe read; on-flash DUMP payload minus its 10-byte footer is byte-identical to what `rdbSaveObject` would emit | `ext_storage.c:1784-1814`, `ext_storage_bridge.c:310-319` |
| fork-based snapshot protocol: settle to no `COPYING_*` key (the disjointness invariant) → park the backend IO thread → pause GC → `fork()` → resume → done | `ext_storage.c:1694-1714` |
| prepare wired into foreground SAVE / BGSAVE / AOF-rewrite background | `rdb.c:1679-1691`, `rdb.c:1751-1761`, `aof.c:2653-2663` |
| INFO surface for it: `snapshot_saves`, `snapshot_tiered_values_saved`, `snapshot_tiered_values_skipped` | `ext_storage.c:1717-1719` |
| AOF-preamble base of a rewrite goes through the same RDB path (`aof-use-rdb-preamble` default yes) | `aof.c:2551-2558`, `config.c:3339` |
| disk-target replica full sync goes through `rdbSaveBackground`, so it is covered too | `replication.c:1017-1022`, `rdb.c:1736` |

**Test evidence.** ext-storage-persistence.tcl:82-108, *"AOF rewrite (RDB preamble) preserves
tiered keys"*, is the test the old L1 pointed at as an *intentionally-failing regression marker*.
That framing is dead: the test now **passes and asserts the opposite** — three spilled keys
(string, hash, list) survive BGREWRITEAOF + DEBUG LOADAOF with values intact — and its in-file
comment states that tiered keys now survive an AOF reload, while noting the non-preamble path
still skips them (L1 gap 1). Eight further tests in ext-storage-snapshot.tcl:1-14 cover SAVE,
BGSAVE, DEBUG RELOAD, TTL survival, writes concurrent with the fork, and the
prepare/hold/GC-pause lifecycle. Per-test detail: [testing](../components/testing.md).

What did **not** get resolved is carried in **L1** (non-preamble AOF, slot migration, diskless
full-sync prepare, node-local placement) and **L8** (non-snapshotting backends refuse to save).

### R4 — plain-AOF rewrite no longer emits the placeholder (was C6)
The former C6 contradiction — `rewriteObjectRio` (`aof.c:2356-2409`) has no tiered guard, so a
non-preamble rewrite emits the **empty SDS placeholder** as if it were the value — is **resolved
at the callers**. `rewriteObjectRio` still has no guard of its own (unchanged), but it is now
unreachable for a tiered object: both loops that feed it skip tiered keys first — the non-preamble
AOF rewrite at `aof.c:2504-2508` and the slot-migration snapshot at `aof.c:2446-2450`. A
non-preamble rewrite therefore **omits** the key (loud, lossy, L1 gap 1) instead of writing a
`SET key ""` that would silently corrupt the value on reload. The residue of C6 is cosmetic and is
tracked as C8 (the two warnings name each other's path).

## See also

[adr-index](adr-index.md) · [serialization](../components/serialization.md) ·
[backends](../components/backends.md) · [persistence-replication](../components/persistence-replication.md) ·
[eviction-integration](../components/eviction-integration.md) · [throttle-equilibrium](../components/throttle-equilibrium.md) ·
[testing](../components/testing.md)
