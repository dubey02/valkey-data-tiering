# Valkey Data Tiering: Overview

> _This document is the entry point for the Valkey data tiering overview. It summarizes the high-level problem, goals, design tenets, candidate V1 scope and major design topics. Each major topic should have a separate focused design document linked from this page._
>
> _This document is not intended to capture every detail or settle every open question. The goal is to provide a shared structure for discussion and make it clear where deeper design work is needed._

## Overview

**What problem are we solving?**
Valkey stores all data in DRAM, constraining the application data set size to the available memory capacity and forcing storage costs to scale linearly regardless of whether a subset of the keyspace is accessed more frequently. Applications must either accept an upper data set size scalability bound or implement methods to repopulate evicted data in the cache when it's needed again. Customers must also pay the cost of keeping all cached data in the most expensive storage type (DRAM) rather than storing data in locations with cost profiles better aligned to the data's access frequency. As users continue to move toward using Valkey as a primary database, these scalability and cost limitations make Valkey a more painful choice for larger workloads with uneven keyspace access patterns.

**Who is the customer?**
Customers with workloads that have larger data sets where a smaller portion of their overall dataset is more frequently accessed like session stores and product catalogs who do not want to tolerate the complexity and additional latency of fetching infrequently accessed data from another store when it's needed encounter this problem. These customers also tend to be cost conscious and do not want to pay the higher costs of store infrequently accessed data in DRAM.

**What is the proposed solution?**
We will add a new data tiering capability to Valkey that leverages existing eviction mechanisms to automatically move infrequently accessed data to NVMe/SSD storage instead of removing it from the cached dataset entirely. When data that has been tiered out of DRAM is requested, Valkey will retrieve the data from NVMe storage, store it again in DRAM, and return the data in the request. This approach automatically keeps the most frequently accessed data in DRAM

**Why now?**
Data tiering is the second actively most discussed and second most reacted-to [issue](https://github.com/valkey-io/valkey/issues/83) in the Valkey repository, clearly showing strong community interest. Redis supports a form of data tiering with the Auto Tiering feature. The AWS ElastiCache service offers a data tiering feature that is popular with customers with workloads that fit its profiles, and we know from experience that modern NVMe hardware provides the latency and cost profiles needed to make data tiering practical and cost-effective.

## Goal

The overall goal of data tiering is to expand Valkey's scalability while reducing the cost of caching suitable workloads without making unacceptable compromises in compatibility or performance.

Our specific design goals include:

* **Reduce workload cost.** Reduce the cost for suitable workloads by allowing the logical dataset to exceed the memory-resident dataset, enabling operators to store more data per node without provisioning additional DRAM.
* **Deliver consistent, predictable latency and behavior.** Memory-only operations should see minimal overhead. Memory-miss latency should be bounded. Under memory pressure, the system should degrade gracefully with defined fallback behavior rather than uncontrolled tail-latency spikes.
* **Provide operator control through configuration and guardrails**. Users should be able to enable/disable tiering, configure capacity and thresholds.
* **Reduce adoption friction.** Users should be able to evaluate workload suitability **before** enabling tiering through proactive tooling (workload tracing, MRC curves), easily observe runtime behavior through metrics after enabling data tiering, and constrain the impact of disabling data tiering without impact beyond data availability.
* **Keep the initial implementation simple.** While we know some workload types would benefit from additional storage types beyond NVMe/SSD, our initial implementation should demonstrate real cost savings and compatibility while avoiding unnecessary complexity.

## Design Tenets

* **Transparent by default.** Data tiering is invisible to clients. Existing applications work without modification. Any limitation is explicit, never silent.
* **Main thread does not wait on storage.** All storage I/O is asynchronous. Memory-resident commands pay near-zero cost for tiering's existence.
* **Separate mechanism from policy.** The core mechanism should handle state, async movement, accounting, and command execution; policy should decide when and what to move.
* **Predictable under pressure.** Every degraded state (memory full, storage full, storage slow, queue buildup) has defined, bounded behavior. No surprise failure modes.
* **Scope to win; design to evolve.** V1 optimizes for workloads where tiering clearly pays off. The architecture must not foreclose alternative backends, key spilling, sub-object tiering or advanced policies.

## V1 Scope & Requirements

The following captures the current V1 direction based on discussions so far. This is intended to define the starting scope, not close all design details.

### V1 Requirements

#### Compatibility
* **R1. Full Valkey API compatibility.** Existing clients work without query-path changes.
* **R2. OSS-compatible persistence:** RDB snapshot, restore, and full-sync produce standard-format files readable by non-tiered Valkey.
* **R3. Safe adoption and rollback.** Operators can enable tiering and revert to non-tiered without data loss or complex migration.

#### Performance
* **R4. Near-zero overhead for memory-resident operations.** Commands served from memory must not regress measurably due to tiering being enabled.
* **R5. Bounded miss latency.** Storage-miss P99 latency must be bounded (target: <500us for small values on NVMe at low load; to be validated with benchmarks).
* **R6. No hidden cost regressions.** Per-key metadata overhead, I/O amplification, serialization cost, and small-value economics must be quantified and acceptable.

#### Operational Safety
* **R7. Backpressure and throttling.** Defined behavior for memory full, storage full, storage slow, and queue buildup. No unbounded queue growth or OOM.
* **R8. Configuration-driven enablement.** Tiering is enabled, disabled, and tuned through configuration without code changes.
* **R9. Large-object guardrails:** Object-size threshold to avoid spilling values where full-object I/O causes excessive latency or amplification.

#### Observability
* **R10. Runtime metrics.** Expose memory/disk hit rate, miss rate, spill/fetch rate, storage pressure, items on flash, and latency impact.
* **R11. Proactive workload suitability tooling.** Tools to predict DT performance on existing workloads before enabling (tracing, MRC curves).

#### Architecture
* **R12. Pluggable storage backend interface.** V1 ships with one optimized backend plus one reference backend demonstrating extensibility.
* **R13. Asynchronous storage operations.** Spill, fetch, delete, and cleanup handled off main thread with completion-based flow.
* **R14. Keys and metadata remain in memory.** V1 spills values only; key lookup stays memory-resident.
* **R15. Spilling Policy based on existing Valkey signals.** V1 uses existing sampling/LRU/LFU-style eviction signals for spill candidate selection.
* **R16. Simple promotion policy with extensible framework.** V1 delivers a broadly applicable default (always-promote or never-promote on access). The framework supports additional strategies (conditional promotion, no-promote reads, SSD-only mode) that can be delivered incrementally.

### Deferred Requirements (Designed For, Not Delivered)

The following capabilities are part of the longer-term roadmap. V1 is designed to accommodate them incrementally but does not deliver them initially.

* **Full key spilling:** Keys and lookup metadata are not moved to secondary storage in V1.
* **Partial object tiering:** V1 does not split large hashes, sets, lists, sorted sets, or streams across memory and storage.
* **Arbitrary pluggable storage backends:** V1 does not aim to support every storage system from day one.
* **Advanced policy framework:** V1 does not start with complex user-defined admission, spill, promotion, or storage-eviction policies.
* **Exact access-time tracking:** V1 does not require precise per-key access timestamp tracking unless data shows it is necessary.
* **Optimizing for all workloads:** V1 does not aim to make full scans, highly rotating workloads, or small-value-heavy datasets efficient through tiering.

## Design Topics

Each topic below should have its own focused document. This high-level document only captures the decision areas.

| # | Topic | Summary | Related Documents |
|---|-------|---------|-------------------|
| 1 | Workload Suitability & Success Criteria | Which workloads should V1 target or avoid, and what benchmarks prove V1 is good enough. Covers target access patterns, value-size economics, and latency bar. | [#6](https://github.com/dubey02/valkey-data-tiering/issues/6), [#14](https://github.com/dubey02/valkey-data-tiering/issues/14) |
| 2 | Tiering Semantics & Data Placement | Defines the user-visible tiering model: what stays in memory vs storage, per-key metadata overhead, maxmemory interaction, and storage capacity accounting. | [#3](https://github.com/dubey02/valkey-data-tiering/issues/3), [#7](https://github.com/dubey02/valkey-data-tiering/issues/7) |
| 3 | Spill & Promotion Policy | When does Valkey spill, and what happens when a tiered value is accessed. Covers candidate selection, LRU/LFU interaction, promote-on-read vs conditional vs no-promote. | [#10](https://github.com/dubey02/valkey-data-tiering/issues/10), [#9](https://github.com/dubey02/valkey-data-tiering/issues/9) |
| 4 | Key State Machine & Async I/O | Valid states (in-memory, on-storage, spilling, fetching, deleting), transitions, blocked clients, command re-execution, and completion handling. | [#4](https://github.com/dubey02/valkey-data-tiering/issues/4) |
| 5 | Storage Backend Interface & Engine Selection | The API contract between Valkey core and storage backends. Evaluates candidates (FlashCache, RocksDB, filesystem) and defines put/get/delete, completion model, and backpressure hooks. | [#12](https://github.com/dubey02/valkey-data-tiering/issues/12) |
| 6 | Core vs Module Boundary | Which logic belongs in Valkey core vs pluggable module/backend. How modules register and what guarantees core provides. | [#11](https://github.com/dubey02/valkey-data-tiering/issues/11) |
| 7 | Pressure Handling & Eviction | What happens when memory, storage, or I/O queues are under pressure. Covers throttling, maxmemory-policy interaction, and final eviction semantics. | [#15](https://github.com/dubey02/valkey-data-tiering/issues/15) |
| 8 | Command Semantics & Multi-Key Operations | Which commands require fetch, metadata-only commands, SCAN/bigkeys behavior, and multi-key blocking/atomicity. | [#8](https://github.com/dubey02/valkey-data-tiering/issues/8) |
| 9 | Persistence, Replication & Lifecycle | How RDB/AOF, restart, replica sync, failover, and expiry work with tiered data. Covers snapshot consolidation and TTL enforcement for spilled values. | [#13](https://github.com/dubey02/valkey-data-tiering/issues/13) |
| 10 | Observability, Tooling & Configuration | Runtime metrics, MRC curves, workload tracing, suitability prediction, and operator controls (enable/disable, thresholds). | *To be added* |
