---
title: Data Tiering — Index
status: active
updated: 2026-06-08
---

# Data Tiering Wiki — Index

LLM-maintained wiki for Valkey **data tiering**. In v1, keys always stay in the dict
and only values spill to flash; key spilling may be enabled by config later. Scope and conventions: [WIKI](WIKI.md).
Start at [00-overview](00-overview.md) → [01-architecture](01-architecture.md), then drill into components/interfaces/flows.

> **Reading view:** this index is the source of truth in both senses — for humans, and
> as the manifest the book view parses at runtime. For start-to-finish reading open
> [`docs/index.html`](docs/index.html): a hierarchical ToC, a page per
> chapter, per-page section ToC, Prev/Up/Next. Nothing to regenerate — adding a row to a
> table below is enough for a page to appear there.

> Status legend: `stub` = placeholder · `draft` = written, unverified · `active` = verified vs code.

## Top level
| Page | Summary | Status |
|------|---------|--------|
| [00-overview](00-overview.md) | What data tiering is, key semantics, when it helps | active |
| [01-architecture](01-architecture.md) | System context, threading, event-loop integration | active |

## Components (L2)
| Page | Summary | Status |
|------|---------|--------|
| [engine-integration](components/engine-integration.md) | preCommandExec gate, spill loop, completion drain | active |
| [state-machine](components/state-machine.md) | The 5 tiering states, transitions, blocking matrix | active |
| [pluggable-storage-api](components/pluggable-storage-api.md) | `storageType` vtable: sync/async/middleware dispatch | active |
| [bridge-layer](components/bridge-layer.md) | Adapter between engine and the vtable | active |
| [throttle-equilibrium](components/throttle-equilibrium.md) | Token bucket + dynamic spill concurrency feedback loop | active |
| [eviction-integration](components/eviction-integration.md) | `extStoragePerformEvictions`, spill-pool LRU | active |
| [serialization](components/serialization.md) | Key/value ser/deser, DUMP payload, encodings | active |
| [backends](components/backends.md) | flashcache (mock/real), rocksdb (sync/async), example + Rust modules; registration & selection | active |
| [persistence-replication](components/persistence-replication.md) | Tiered values materialized into standard RDB entries under the fork-snapshot protocol; RDB/AOF-preamble/disk-target full sync covered; non-preamble AOF, slot migration and diskless fork are the gaps | active |
| [memory-accounting](components/memory-accounting.md) | `OBJ_ENCODING_TIERED`, objectComputeSize, overshoot | active |
| [testing](components/testing.md) | Tiering integration tests: data-type spill/fetch validity, lifecycle, Lua/MULTI-EXEC/COPY-APPEND-PERSIST command surface, AOF/RDB crash-safety | active |

## Interfaces (L3)
| Page | Summary | Status |
|------|---------|--------|
| [storagetype-vtable](interfaces/storagetype-vtable.md) | `src/storage/storage.h` — full vtable reference | active |
| [ext-storage-api](interfaces/ext-storage-api.md) | `src/ext_storage.h` — engine public API | active |
| [bridge-api](interfaces/bridge-api.md) | `src/ext_storage_bridge.h` — submit/poll API | active |
| [throttle-api](interfaces/throttle-api.md) | `src/ext_storage_throttle.h` — throttler API | active |
| [info-metrics](interfaces/info-metrics.md) | Exact `INFO external_storage` field names | active |
| [config-and-module-args](interfaces/config-and-module-args.md) | Config directives + module args | active |

## Flows (L3/L4)
| Page | Summary | Status |
|------|---------|--------|
| [spill](flows/spill.md) | Memory → flash spill sequence | active |
| [fetch](flows/fetch.md) | Flash → memory fetch + client block/unblock | active |
| [delete](flows/delete.md) | DEL on a flash key (async delete, no fetch) | active |
| [evict-during-fetch](flows/evict-during-fetch.md) | PENDING_EVICT path | active |
| [completion-drain](flows/completion-drain.md) | beforeSleep completion processing | active |

## Decisions (L4)
| Page | Summary | Status |
|------|---------|--------|
| [adr-index](decisions/adr-index.md) | 9 code-grounded design decisions (ADR-001…009) | active |
| [known-limitations](decisions/known-limitations.md) | 6 code-vs-comment contradictions + 8 functional limits + 4 resolved, all cited | active |

## Diagrams
Sources + `Makefile` in `diagrams/`. Regenerate: `cd .agent/wiki/diagrams && make seq-image && make` (mermaid renders via a container — see [render.Dockerfile](diagrams/render.Dockerfile)).

| PNG | Shows | Source | Renderer |
|-----|-------|--------|----------|
| `diagrams/system-context.png` | engine ⇄ vtable ⇄ backend ⇄ NVMe | `system-context.dot` | dot ✅ |
| `diagrams/components.png` | module graph + engine touch-points | `components.dot` | dot ✅ |
| `diagrams/state-machine.png` | 5-state FSM | `state-machine.dot` | dot ✅ |
| `diagrams/event-loop.png` | event-loop integration | `event-loop.dot` | dot ✅ |
| `diagrams/throttle-feedback.png` | throttle control loop | `throttle-feedback.dot` | dot ✅ |
| `diagrams/controller-model.png` | two decoupled controllers + projected-memory predictor (P1/P2 windows) | `controller-model.dot` | dot ✅ |
| `diagrams/eviction-flow.png` | eviction decision flow | `eviction-flow.dot` | dot ✅ |
| `diagrams/spill-sequence.png` | spill sequence | `spill-sequence.mmd` | mmdc ✅ |
| `diagrams/fetch-sequence.png` | fetch sequence | `fetch-sequence.mmd` | mmdc ✅ |
| `diagrams/delete-sequence.png` | delete sequence | `delete-sequence.mmd` | mmdc ✅ |
| `diagrams/evict-during-fetch-sequence.png` | PENDING_EVICT sequence | `evict-during-fetch-sequence.mmd` | mmdc ✅ |
| `diagrams/completion-drain-sequence.png` | drain cycle | `completion-drain-sequence.mmd` | mmdc ✅ |

All sequence PNGs render at 3× scale via a containerized mermaid-cli ([render.Dockerfile](diagrams/render.Dockerfile)); this host has no native arm64 Chrome. Each flow page embeds its PNG with the mermaid source in a collapsible block.

## KEG graph & tooling

The wiki is a typed-edge graph (KiRoom Entity Graph). Schema + edge kinds: [WIKI](WIKI.md) § Edges & Graph. Cheatsheet: [RETRIEVAL](keg/RETRIEVAL.md).

| Command (from package root) | Purpose |
|---|---|
| `python3 .agent/wiki/keg/keg_lint.py --stats` | graph integrity (errors/warnings/orphans) |
| `python3 .agent/wiki/keg/keg_lint.py --json --out .agent/wiki/keg/viewer/edges.json` | regen graph data |
| `python3 .agent/wiki/keg/query.py related <page> --max-hops 2` | graph-aware retrieval |
| `python3 .agent/wiki/keg/query.py hubs --by inbound` | most-referenced pages |
| `python3 .agent/wiki/keg/extract_relations.py` | validation cross-check (edge proposals) |
| `python3 -m http.server 8000 --bind 127.0.0.1 -d .agent/wiki` | serve both views: `/keg/viewer/index.html` (graph) and `/docs/` (book) |

## Remaining work (handoff)

Current state (2026-06-10): **25 active · 0 draft · 0 stub · 0 stale** — Layer-1 gate clean
(`verify_citations.py` 0 errors, `keg_lint.py --stats` 0 errors/0 warnings/0 orphans). See
[WIKI](WIKI.md) § Verification.

- **2026-06-10 — rebase conflict resolution + line-drift re-ground.** Resolved a stash-pop conflict
  in [known-limitations](decisions/known-limitations.md) by **union**: kept upstream's
  evidence-backed L4 (embedded-values-unspillable + memory-gated throttle collapse, `fullrun-20260608`
  W2 data) and L5 (`spillFillToProjected` livelock, gdb-confirmed) **and** the session's OBJ_MODULE
  NULL-deref (now L6) and Smith-predictor cold-start under-braking (now L7, cross-linked to L5). The
  rebase shifted `ext_storage.c` non-uniformly (e.g. `createDumpPayload` :232→:240,
  `spillItemAsync` :796→:808, `extStoragePerformEvictions` :926→:938, `spillFillToProjected`
  :997→:1009, INFO fn :1066→:1078) **and added new INFO fields** (`spill_submitted_count`,
  `spill_serialized_count`, `mean_spill_ram`). Re-grounded every `ext_storage.c` citation across the
  7 affected pages — known-limitations, engine-integration, eviction-integration, spill,
  completion-drain, serialization, memory-accounting, info-metrics. `throttle-equilibrium`,
  `bridge-layer`, `ext-storage-api`, `storagetype-vtable` needed no change (those source files were
  untouched by the rebase). Empirical L4/L5 benchmark/gdb claims preserved as-authored and flagged
  as not code-derivable. **Layer-2 pending** for the re-grounded synthesis/decision pages.

- **Re-ingested 2026-06-08 (Smith-predictor commit `e4ed4ff3a`, Tranche A — spill/eviction core):**
  [eviction-integration](components/eviction-integration.md), [engine-integration](components/engine-integration.md),
  [spill](flows/spill.md), [memory-accounting](components/memory-accounting.md), plus the
  `diagrams/spill-sequence.{mmd,png}` re-render. Cleared the hard ERROR
  (`SPILL_CONCURRENT_LIMIT` deleted from code). The spill loop is now **cap-less** and
  projected-gated (`spillFillToProjected`, `ext_storage.c:997-1033`); the over-budget eviction
  decision reads `extStorageProjectedMemory()` (two-stage Smith predictor,
  `ext_storage.c:79-88`); throttle/spill concurrency coupling removed. **Two new contradictions**
  (to fold into known-limitations, Tranche D): `items_spillover_batch_size` var + config now dead
  (cap-less loop never reads them); old `SPILL_CONCURRENT_BASE` comment-vs-`#define` contradiction
  is retired (the `#define` no longer exists). **Layer-2 adversarial review still pending** for
  these 4 re-ingested pages.
- **Re-ingested 2026-06-08 (Tranche B — throttle decoupling):** [throttle-equilibrium](components/throttle-equilibrium.md)
  rewritten — throttle now reads **raw `used_memory`**, band moved to `[1.1×, 1.2×]`
  (`ext_storage_throttle.c:221-222`), and the `extStorageUpdateSpillConcurrency` spill coupling was
  **removed** (`:252-254`); spill/throttle are decoupled (implicit via memory). Re-rendered
  `diagrams/throttle-feedback.{dot,png}` to show two decoupled controllers.
  [throttle-api](interfaces/throttle-api.md) needs no change — its header `ext_storage_throttle.h`
  was untouched by the commit. **Layer-2 pending** for throttle-equilibrium.
- **Re-ingested 2026-06-08 (Tranche C — accounting plumbing + interfaces):**
  [serialization](components/serialization.md) (IO-thread serialize now also measures `ram_bytes`
  via `objectComputeSize`, `storage_flashcache_real.c:197`; OBJ_MODULE NULL-key deref flagged),
  [completion-drain](flows/completion-drain.md) (WRITE completion debits `inflight_spill_ram_bytes`
  `:613`; cap-less spill; re-rendered `.mmd/.png`), [storagetype-vtable](interfaces/storagetype-vtable.md)
  (`storageCompletion.ram_bytes` `storage.h:52`, +1 shift), [ext-storage-api](interfaces/ext-storage-api.md)
  (`extStorageUpdateSpillConcurrency` → `extStorageOnSpillSubmit`/`OnSpillSerialize`/`InflightAddRam`/
  `ProjectedMemory`; `items_spillover_batch_size` noted vestigial), [info-metrics](interfaces/info-metrics.md)
  (new `inflight_spill_ram_bytes` + `projected_memory` fields `:1135-1136`; all field ranges re-pinned),
  [bridge-layer](components/bridge-layer.md) (`msg->ram_bytes = c->ram_bytes` `ext_storage_bridge.c:241`).
  [bridge-api](interfaces/bridge-api.md) unchanged — `ext_storage_bridge.h` untouched. **Layer-2 pending.**
- **Re-ingested 2026-06-08 (Tranche D — decisions):** [adr-index](decisions/adr-index.md)
  (ADR-008 throttle band → `[1.1×,1.2×]`; **ADR-009 superseded** by new **ADR-010** cap-less
  projected-gated spill controller / Smith predictor; ADR-004/006 anchors re-pinned),
  [known-limitations](decisions/known-limitations.md) (**C2 retired → R2**; new **C7** dead
  `items_spillover_batch_size` config, **L4** OBJ_MODULE `objectComputeSize(NULL,…)` deref risk,
  **L5** Smith-predictor cold-start under-braking; L2/C3/C4/R1 citations re-pinned). **Layer-2 pending.**
- **Smith-predictor re-ingest (commit `e4ed4ff3a`) is now complete** across Tranches A–D. Tranches
  A, B, C are Layer-1 + Layer-2 clean; Tranche D is Layer-1 clean, Layer-2 pending. Outstanding:
  Layer-2 on Tranche D, Layer-3 (human) on synthesis pages, and the optional
  `controller_model_detailed.png` diagram source (still no `.dot`/`.mmd` source).
- **Diagram follow-up:** the user has a `controller_model_detailed.png` (two-controller model +
  projected-memory windows). Not yet in `diagrams/` and has no `.dot`/`.mmd` source — needs a
  generated source authored before it can be embedded (diagrams must be regenerable).

- **Verified → active 2026-06-05 (this session):** [adr-index](decisions/adr-index.md),
  [known-limitations](decisions/known-limitations.md) — fresh-agent `valkey-dt-reviewer` Layer-2 =
  REVIEW PASS both (1 adjudicated citation fix: hard-cap range `ext_storage.c:880-886`→`:877-884`;
  reviewer's other 2 nits rejected as miscounts vs `awk`). adr-index = 9 code-grounded decisions
  (ADR-001…009). known-limitations collects **6 code-vs-comment/doc contradictions** (C1 val_ptr
  NULL vs empty-SDS; C2 SPILL_CONCURRENT_BASE comment "2" vs 50; C3 metadata no-fetch intent vs
  value-agnostic gate; C4 dead sync rocksdb; C5 unregistered key_may_exist; C6 plain-AOF placeholder)
  + functional limits (L1 durability drop, L2 two OOM paths, L3 single IO thread) + R1 (string-only
  superseded by DUMP, resolved). symmetric `contradicts` edge with serialization preserved.
- **Verified → active 2026-06-05 (this session):** [persistence-replication](components/persistence-replication.md)
  — fresh-agent `valkey-dt-reviewer` Layer-2 = REVIEW PASS, no fixes (1 citation range tightened
  `aof.c:2356-2404`→`:2356-2409`). Thesis verified: every persistence/replication path keys off
  `objectIsTiered` (`server.h:839`). RDB `rdbSaveKeyValuePair` returns 0 → omits whole pair
  (`rdb.c:1190-1195`); `rdbSaveRio` (`rdb.c:1483`) is the shared funnel for RDB file, AOF preamble,
  and full-sync (RDBFLAGS_REPLICATION). `defragKey` skips tiered (`defrag.c:704-712`). `replication.c`
  has zero tiering code.

- **Verified → active 2026-06-05 (this session):** [backends](components/backends.md) — fresh-agent
  `valkey-dt-reviewer` Layer-2 = REVIEW PASS; 2 adjudicated citation nits fixed (`MODULE_NAME`
  :42→:39; flashcache-real serialize range :170-176→:167-176). All in-tree C backends (4
  `storageType` getters) + module backends (C `storage_example`, `storage_flashcache_module`; Rust
  `non-key-spilling`, `rocksdb-tiering`), the two registration paths (native getters vs
  `ValkeyModule_RegisterStorageBackend`, module-takes-precedence), the sync/async dispatch fork, and
  real-backend robj serialization on the IO thread. Two NEW contradictions surfaced (folded target:
  known-limitations):
  1. ~~`storageGetRocksDBType()` sync backend~~ — **RESOLVED** (Jul 2026): file deleted,
     `ext-storage-backend=rocksdb` routes to the same in-memory mock as `flashcache-mock`
     via `storage_mock.c`. The shared middleware path remains available for future sync backends.
  2. Rust backends implement `key_may_exist` but the value-spill module **deliberately leaves it
     unregistered** (`non-key-spilling/src/lib.rs:533-536`: false positives "block clients
     forever") — the dict, not a bloom filter, is the existence oracle while keys stay in memory.
  Also fixed `01-architecture.md` backends-row cell ("rocksdb (sync via middleware)" → async; sync
  variant unwired) to match the bridge.
- **Verified → active 2026-06-05:** [serialization](components/serialization.md),
  [memory-accounting](components/memory-accounting.md) (fresh-agent `valkey-dt-reviewer` Layer-2;
  minor citation nits fixed); and the seed pages [00-overview](00-overview.md),
  [01-architecture](01-architecture.md), [state-machine](components/state-machine.md) (inline
  Layer-2). Two seed-page corrections vs code:
  1. overview's "metadata answered from RAM, no fetch" table was wrong — `preCommandExec` is
     value-agnostic (`server.c:4718` → `keyBlocksClient` `ext_storage.c:288-526`), so
     `EXISTS`/`TYPE`/`TTL`/`EXPIRE` on a tiered key block+fetch; only `SCAN`/`DBSIZE` and
     `DEL`/`UNLINK` avoid it (flagged ⚠️ CONTRADICTION).
  2. architecture wrongly placed serialize/deserialize on the main thread — it runs on the IO thread.
- **Stub, not yet ingested:**
  - [persistence-replication](components/persistence-replication.md) — RDB/AOF/defrag tiering skips
    (`rdb.c`, `aof.c`, `defrag.c`).
  - [adr-index](decisions/adr-index.md), [known-limitations](decisions/known-limitations.md) — collect
    the flagged contradictions: `SPILL_CONCURRENT_BASE` comment ("2" vs 50), tiered `val_ptr` NULL
    vs empty-SDS, string-only vs DUMP-all-types, metadata commands fetch on a tiered key despite the
    no-fetch design intent (value-agnostic `preCommandExec`); the key-in-RAM-floor OOM; **plus the two
    backends contradictions above (unwired sync rocksdb; unregistered key_may_exist)**.
- **Infra:** MeshClaw subagent runner is back up (fresh-agent Layer-2 works again; a dedicated
  `valkey-dt-reviewer` agent exists). NOTE: spawn `cwd` must be under the allowed roots
  (`~/workspace`/`~/workplace`) — pass absolute repo paths in the prompt instead of `cwd` here.
- **Diagrams:** all 5 mermaid `.mmd` sequences now render to PNG at 3× scale via a
  containerized mermaid-cli ([render.Dockerfile](diagrams/render.Dockerfile); `make seq-image`
  then `make seq`) and are embedded in the flow pages; `.dot` PNGs render natively. The host has
  no usable native Chrome (Chrome-for-Testing ships no linux-arm64 build, and glibc is 2.26), so
  mermaid rendering goes through the container.
