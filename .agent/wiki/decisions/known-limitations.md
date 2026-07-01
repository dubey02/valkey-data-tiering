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
  - src/config.c:3426
  - src/rdb.c:1190-1195
  - src/aof.c:2356-2409
  - src/storage/storage.h:130
  - src/storage/storage_flashcache_real.c:191-201
  - modules/non-key-spilling/src/lib.rs:533-536
  - DATA-TIERING.md
updated: 2026-06-10
type: decision
tier: wisdom
claim_count: 15
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
    note: L1 regression marker is the failing AOF-reload test
---

# Known Limitations

> Code-grounded contradictions and functional constraints in the NKS tiering POC.
> Every entry cites `file:line` in **this** repo; where a comment or legacy doc disagrees
> with the code, the **code is authoritative**.

## Contradictions (code vs comment / design doc)

| # | What | Code is… | Comment/doc claims… | Cite |
|---|------|----------|---------------------|------|
| C1 | tiered `val_ptr` | **empty-SDS placeholder** (freed via `sdsfree`, sized via `sdsAllocSize`) | `val_ptr is NULL` | `server.h:838` vs `object.c:632-634`, `object.c:1208-1211` |
| C3 | metadata on a tiered key | gate is **value-agnostic** → any named-key cmd blocks+fetches | EXISTS/TYPE/TTL answered from RAM, no fetch | `server.c:4718` + `ext_storage.c:359-434` vs `DATA-TIERING.md` |
| C4 | sync `rocksdb` backend | **RESOLVED** (Jul 2026) — file deleted. `ext-storage-backend=rocksdb` routes to mock via `storage_mock.c` | N/A | `storage_mock.c` (see [backends](../components/backends.md)) |
| C5 | `key_may_exist` | implemented in both Rust backends but **left unregistered** in NKS | (bloom probe available) | `non-key-spilling/src/lib.rs:533-536` (see [backends](../components/backends.md)) |
| C6 | plain-AOF rewrite of tiered keys | **no skip** — emits the empty placeholder value | (RDB/preamble path skips cleanly) | `aof.c:2356-2409` (see [persistence-replication](../components/persistence-replication.md)) |
| C7 | `items_spillover_batch_size` config | **dead** — the cap-less spill controller never reads it | config directive still settable (1–100) | `ext_storage.c:95`, `config.c:3426` (see [eviction-integration](../components/eviction-integration.md)) |

> Note: former **C2** (`SPILL_CONCURRENT_BASE` comment "2" vs `#define` 50) is **retired** — the
> Smith-predictor commit deleted the macro and the dynamic-concurrency cap entirely. See R2 below.

### C1 — `val_ptr` NULL vs empty-SDS placeholder
`server.h:838` documents `OBJ_ENCODING_TIERED` as *"val_ptr is NULL (value on disk)"*, but the
code keeps an **empty SDS** as the placeholder: `decrRefCount` frees it with `sdsfree`
(`object.c:632-634`) and `objectComputeSize` adds `sdsAllocSize(placeholder)`
(`object.c:1208-1211`). A reader trusting the comment would mishandle the placeholder. See
[memory-accounting](../components/memory-accounting.md).

### C7 — `items_spillover_batch_size` is dead config
The variable (`ext_storage.c:95`) and its `ext-storage-items-spillover-batch-size` config directive
(`config.c:3426`, range 1–100, default 10) are still declared and settable, but the cap-less
`spillFillToProjected` controller never reads them — queue depth is now gated purely on projected
memory. Setting the directive has **no effect**. See [eviction-integration](../components/eviction-integration.md).

### C3 — metadata "no fetch" design intent vs value-agnostic gate
`DATA-TIERING.md` intends metadata commands (EXISTS/TYPE/TTL/EXPIRE) to be answered from
RAM without a fetch. But `preCommandExec` (`server.c:4718`) has **no command-type guard** and
`keyBlocksClient` blocks+fetches any *named key* in `ONLY_FLASH` (`ext_storage.c:359-434`, state
cases at `:398`). So those metadata commands **do** block and fetch the value; only `SCAN`/`DBSIZE`
(no named key) and `DEL`/`UNLINK` (async delete) avoid the fetch. (`server.c:4721-4735` even adds a
`TIERED_SAFETY` re-check that re-runs `preCommandExec` if a key is tiered post-gate.) See
[00-overview](../00-overview.md), [engine-integration](../components/engine-integration.md).

> C4, C5, C6 are documented in full on [backends](../components/backends.md) and
> [persistence-replication](../components/persistence-replication.md); summarized here for the index.

## Functional limitations

### L1 — Tiered cold data is not persisted or replicated
`rdbSaveKeyValuePair` returns `0` for tiered values (`rdb.c:1195`), omitting the **whole** pair
from every RDB consumer (RDB file, AOF preamble, replication full-sync). On restart or on a fresh
replica full-sync, already-tiered keys are **absent** unless the storage module restores them from
its own persistent store (`rdb.c:1191-1194`). Tiering state is node-local and not replicated. See
[persistence-replication](../components/persistence-replication.md). The forward-looking
regression marker for this limitation (the intentionally-failing "Tiered keys persist across AOF
reload" test) lives in [testing](../components/testing.md).

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
Two NKS constraints compound on key-heavy / tiny-value workloads:

1. **Embedded values never spill.** `isEmbeddedObject` (`ext_storage.c:341`) treats
   `OBJ_ENCODING_EMBSTR` (string ≤44B) and `OBJ_ENCODING_INT` as embedded, and `spillItemAsync`
   rejects them (`ext_storage.c:814`, `return -1`). The value lives in the object header itself,
   so there is nothing to move to flash. Combined with the per-key metadata floor (keys never
   leave RAM in NKS — see L2), a workload of many small values pins `used_memory` **above
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
  `if (!total_keys) break` (`evict.c:418`), never true in NKS — **spins at 100% CPU on the main
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

## Resolved / historical

### R1 — "String values only" → all types via DUMP
An earlier limitation restricted spilling to `OBJ_STRING`. The current serializer uses
`createDumpPayload` (`ext_storage.c:240`) / `rdbLoadObject`, which handles **every** object type
(comment at `ext_storage.c:827`). The string-only constraint is **superseded** (symmetric
`contradicts` edge with [serialization](../components/serialization.md)).

### R2 — `SPILL_CONCURRENT_BASE` / dynamic-concurrency cap removed (was C2)
The former C2 contradiction (a stale *"only 2 concurrent spills"* comment vs the `#define
SPILL_CONCURRENT_BASE 50`) is **resolved by deletion**: the Smith-predictor commit removed the
`SPILL_CONCURRENT_BASE`/`LIMIT` macros, `max_num_concurrent_items_spilled`, and the
throttle-driven `extStorageUpdateSpillConcurrency` entirely (removal comment `ext_storage.c:100-104`).
Spill concurrency is no longer capped or throttle-driven — it is the emergent output of the
projected-memory controller (ADR-010). See [eviction-integration](../components/eviction-integration.md),
[adr-index](adr-index.md).

## See also

[adr-index](adr-index.md) · [serialization](../components/serialization.md) ·
[backends](../components/backends.md) · [persistence-replication](../components/persistence-replication.md) ·
[eviction-integration](../components/eviction-integration.md) · [throttle-equilibrium](../components/throttle-equilibrium.md)
