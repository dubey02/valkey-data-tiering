# Valkey Data Tiering: TSC Alignment

> **Purpose:**
> State the decisions we have made for Valkey data tiering, the three points needing TSC alignment. Sources:
> [design overview](https://github.com/dubey02/valkey-data-tiering/blob/unstable/design-docs/data-tiering/valkey-data-tiering-overview.md),
> [POC](https://github.com/dubey02/valkey-data-tiering/tree/unstable),
> [benchmarks](https://github.com/dubey02/valkey-data-tiering/blob/unstable/design-docs/data-tiering/data-tiering-benchmark-summary.md),
> [discussion issues](https://github.com/dubey02/valkey-data-tiering/issues).

## Points Needing TSC Alignment

### 1. Data tiering lives in Valkey core with a built-in default backend, and custom backends via modules

**Proposal:** The data tiering engine is implemented in Valkey core and ships with a built-in default storage backend, so tiering works out of the box as Valkey's default tiering offering. The backend interface is designed as a flexible framework so that other storage engines can be implemented as modules. In V1 this module path is experimental and exercised by a sample module but undocumented, with no API-stability or performance guarantees. A stable public module API is deferred until the interface hardens with real usage.

**Why:**

* **Lowest barrier to adoption:** Users enable tiering with a config option. No module to download, load, version, or operate.
* **Inseparable from the engine:** Tiering needs client blocking, memory-pressure accounting, and keyspace state transitions which are all core execution paths. Core changes are required regardless. A module implementation would need them too, plus a larger module API.
* **Flexible backend framework:** The in-core framework is designed to make modular implementations of other storage engines easy, prioritizing flexibility over an early rigid contract.
* **First-class citizen:** In core, data tiering is built and tested with every engine change, so new features must stay compatible with it and the chances of silently breaking tiering are far lower than for an out-of-tree module.

**Concerns from previous discussions and our responses:**

> **Core bloat, "should be a module"**
>
> **Response:** Tiering is disabled by default, isolated behind a storage dispatch layer, and adds no measurable overhead when off. The engine-side hooks it needs cannot live in a module without a far more invasive module API expansion. For e.g., fetching tiered values inside the full-sync/RDB stream, blocking a client mid-dispatch and re-executing native commands on fetch completion would still require engine hooks.

> **New storage library dependency in the core build**
>
> **Response:** The engine talks only to a compile-time or pluggable backend interface. No core code references FlashCache APIs directly. In the POC the FlashCache backend is vendored in `deps/` (like `jemalloc` and `lua`) and always built — for the upstream contribution, backend selection will be a build-time option (e.g., `make STORAGE_BACKEND=flashcache|none`), so a build without tiering carries no storage-library or `libaio` dependency.

### 2. V1 launch scope

**Proposal:** V1 is a complete first release, not a preview. The bar for V1 inclusion is "required to run tiering in production". The bar for deferral is "adds significant complexity without being required for a useful version of the feature and benefits from real V1 usage data before the design is finalized".

| # | Capability | V1 | Deferred (Not in Scope) | Why deferred |
|---|---|---|---|---|
| 1 | **Tiering model** | Transparent whole-value tiering. Commands behave identically for DRAM and flash values. Keys, expiry, type and metadata stay in DRAM, only values move to flash. | Key spilling (keys moved out of DRAM) | We benchmarked both architectures head-to-head (KS vs NKS, matched throttle, identical hardware). KS is the better architecture for throughput — up to +35–44% on metadata-heavy workloads — but only +6–23% on the workloads V1 targets, at a large complexity cost: bloom filters for lookup ambiguity (~25–29% of GETs on the bloom-miss path), SCAN/DBSIZE/active-expiry redesign, ~200 lookup-callsite audits. Low ROI for the first release; the architecture is designed to take KS as a phased follow-up, and the data tells us exactly which workloads justify it. |
| 2 | **Data types** | All types (strings, hashes, lists, sets, sorted sets, streams, module bloom, module Json), tiered as whole values, serialized on IO threads | Partial fetch/update of large collections like Hashes, Sets, Lists etc | Requires per-type on-disk formats and sub-key indexing. Best designed once V1 shows which types and access patterns actually need it. |
| 3 | **Persistence & replication** | RDB, AOF, restore, full-sync, replication, failover; migration in both directions (tiered ↔ non-tiered) without data loss | | Not deferrable: a feature that cannot be backed up, replicated, failed over, or rolled back is not adoptable. Table stakes, not enhancements. |
| 4 | **Policies** | Spill candidates from existing LRU/LFU signals, spill before evict, default promote-on-access promotion and size-gated admission (large-object guardrail) on an extensible framework. Supported eviction policies: allkeys-lru, allkeys-lfu, noeviction | User-defined tiering policies, manual tier/untier commands | V1 delivers a broadly applicable default (always-promote or never-promote on access). The framework supports additional strategies (conditional promotion, no-promote reads, SSD-only mode) that will be delivered incrementally in follow-up releases. User-defined policies and manual tier/untier commands follow once real usage data shows what users need, so we grow it incrementally against demand. |
| 5 | **Storage backends** | Pluggable backend interface, ships FlashCache as the built-in default backend. FlashCache also doubles as the reference module backend - the same backend loaded via the module path demonstrates the pluggable interface. | | One backend exercised through both paths (built-in default and module plug-in) proves the interface is real. Further backends are community-additive against a stable API - nothing in V1 blocks them |
| 6 | **Operational safety & observability** | Async I/O (main thread never waits on storage), throttling/backpressure with defined behavior for memory-full/storage-full/storage-slow, metrics for hit rates, spill/fetch rates, items on flash, latency impact | | Required for production operation from day one. |

### 3. What tiering is optimized for — and what is not

**Proposal:** V1 explicitly targets a workload profile rather than trying to make tiering good for everything.

**What we want from TSC:** Agreement that poor-fit workloads are explicit non-goals — V1 acceptance criteria and benchmarks are judged against the target profile, and the release is not blocked on performance for non-target workloads.

| # | Category | Workloads | Evidence |
|---|---|---|---|
| 1 | **Optimized for** | Read-heavy, skewed (Zipfian) access with a cold tail; values 500 B–5 KB; datasets larger than DRAM; session stores, product catalogs; TTL-heavy workloads | 134K TPS at 82% DRAM hit, ~24% overhead vs non-tiered; TTL workloads fastest (159K TPS) |
| 2 | **Works, with reduced performance** | Uniform access (no locality); very large values (500 KB+); balanced read/write | Uniform: 86K TPS; 500 KB+: disk-bandwidth-bound (~1.6K TPS) but stable, no cliff |
| 3 | **Poor fit — not targeted** | Tiny values (~100 B, per-key metadata overhead dominates); always-hot datasets (nothing cold to tier — pure overhead); full-scan and highly-rotating workloads; write-heavy at sustained high rate | 100 B values: lowest TPS of all sizes; scans defeat LRU/LFU locality |
