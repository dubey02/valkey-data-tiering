# Valkey Data Tiering: Community Alignment Meeting

## 1. Purpose of the meeting

Align the community and stakeholders on the data tiering approach prior to fully
developing the feature. This document covers the tenets, scope, high-level design, and
open alignment points. The agenda for this meeting is to walk through the scope and
high-level working of data tiering, then reach agreement on the open alignment points in
Section 6 below.

## 2. Tenets

1. **Transparent by default.** Existing applications work unmodified. Any limitation is
   explicit, never silent. No new commands required to use tiering.
2. **Main thread never waits on storage.** All IO and serialization happen on IO threads.
   The one documented exception is scripts, where a single bounded read is allowed.
3. **Near-zero cost when disabled.** The residency check is a bit-test in the object
   header. When tiering is off, there is no measurable overhead.
4. **Separate mechanism from policy.** The state machine, async movement, and memory
   accounting are core mechanism. Spill and promotion policy is replaceable.
5. **Predictable under pressure.** Every degraded state (memory full, storage full,
   storage slow, queue buildup) has defined, bounded behavior. No unbounded queues, no
   silent drops.
6. **Engine guarantees extend to tiered data.** Persistence, replication, expiry, and
   eviction all work correctly with tiered values.
7. **Scope to win, design to evolve.** V1 targets workloads where tiering clearly pays.
   The architecture does not foreclose key-spilling, partial-object tiering, or
   additional storage engines in the future.

## 3. Scope of the launch

**Proposal:** The bar for V1 inclusion is "required to run tiering in production". The
bar for deferral is "adds significant complexity without being required for a useful
version of the feature, and benefits from real V1 usage data before the design is
finalized".

| Capability | V1 | Deferred (Not in Scope) | Why deferred |
|------------|----|-------------------------|--------------|
| Tiering model | Transparent whole-value tiering. Commands behave identically for DRAM and flash values. Keys, expiry, type and metadata stay in DRAM, only values move to flash. | Key spilling (keys moved out of DRAM) | We benchmarked both architectures head-to-head on identical hardware with matched throttle settings. Key-spilling gives +35-44% throughput on metadata-heavy workloads where keys are a large fraction of total bytes. On read-heavy workloads with skewed access and values in the 500B-5KB range (session stores, catalogs, TTL-heavy caches), the gain drops to +6-23%. The complexity cost is very high: bloom filters for lookup ambiguity (25-29% of GETs hit the bloom-miss path), SCAN/DBSIZE/active-expiry redesign, and roughly 200 lookup-callsite audits. The architecture is designed so key-spilling can be added on top of non-key-spilling as a follow-up. Nothing shipped in V1 is thrown away. |
| Data types | All types (strings, hashes, lists, sets, sorted sets, streams, module bloom, module Json), tiered as whole values, serialized on IO threads | Partial fetch/update of large collections like Hashes, Sets, Lists. Search module. | Partial item fetch requires per-type on-disk formats and sub-key indexing. This adds lots of complexity. It is best to prioritize it once V1 shows which types and access patterns actually need it. Search maintains its own secondary indexes that reference values directly, bypassing the lookup path where tiering intercepts. Correct behavior needs index-aware integration designed with the Search maintainers. JSON and Bloom are in V1 because they access values through the module key-open path, which tiering already handles. Search is the first fast-follow, not architecturally blocked. |
| Persistence & replication | RDB, AOF, restore, full-sync, replication, failover, migration in both directions (tiered to non-tiered and vice versa) without data loss | | |
| Policies | Spill candidates from existing LRU/LFU signals, spill before evict, default promote-on-access promotion and size-gated admission (large-object guardrail) on an extensible framework. Supported eviction policies: allkeys-lru, allkeys-lfu, noeviction | User-defined tiering policies, manual tier/untier commands | V1 delivers a broadly applicable default (always-promote or never-promote on access). The framework supports additional strategies (conditional promotion, no-promote reads, SSD-only mode) that will be delivered incrementally in follow-up releases. User-defined policies and manual tier/untier commands follow once real usage data shows what users need, so we grow it incrementally against demand. |
| Storage backends | Pluggable backend interface, ships FlashCache as the built-in default storage engine. FlashCache also doubles as the reference module storage engine (the same storage engine loaded via the module path demonstrates the pluggable interface) | | |
| Operational safety & observability | Async I/O (main thread never waits on storage), throttling/backpressure with defined behavior for memory-full/storage-full/storage-slow, metrics for hit rates, spill/fetch rates, items on flash, latency impact | | |

## 4. Where we are today

- We have discussed and collected feedback from multiple stakeholders on the tenets,
  requirements, and the various moving pieces of data tiering.
- We have a working proof-of-concept on the public fork
  ([dubey02/valkey-data-tiering](https://github.com/dubey02/valkey-data-tiering),
  `unstable` branch). It covers all data types, TTL expiry, Lua/MULTI-EXEC, eviction
  integration, and async spill/fetch with adaptive throttling, with 204 integration tests
  passing.
- We have open-sourced the FlashCache storage engine.
- We have pluggable storage engine capabilities tested with FlashCache both as a built-in
  core backend and through the module interface.
- We have published performance benchmarks for both the Valkey data tiering POC and the
  FlashCache storage engine independently. We have also published a benchmarking tool for
  data tiering and a way to monitor it at runtime.
- We are currently designing and prototyping snapshotting, replication, and slot
  migration.
- We are at a point where we would like to start the actual development of data tiering
  in a branch of valkey-io, once we are aligned on the approach.

## 5. Current Data Tiering Approach

Data tiering keeps every key, its type, and its metadata in DRAM and transparently moves
cold values to local NVMe. Commands behave identically whether a value is in memory or on
flash: a memory-resident key pays only a bit-test in the object header, while a command
touching a flash-resident value blocks just that one client, fetches asynchronously, and
re-executes. The main thread never waits on storage, and all IO and serialization run on
dedicated IO threads. Under memory pressure the engine spills cold values (selected by
the existing LRU/LFU sampling) before evicting, throttling writes within a bounded band.
The tiering logic lives in the engine core; the storage backend sits behind a single
pluggable interface, provided either by the built-in FlashCache backend or by a loadable
module through the identical call path.

Interactive explainer (click components to see exact source code):
https://dubey02.github.io/valkey-data-tiering/design-docs/data-tiering/dt-flows.html

## 6. Alignment Points

### AP1. Where tiering lives: engine in core, built-in default storage engine, module APIs for custom storage engine (experimental in V1)

**Proposal:**

1. **The tiering engine lives in core, with a built-in default storage engine.** The
   per-key state machine, command interception, spill controller, eviction integration,
   and throttling are core engine code. Valkey ships with a built-in default storage
   engine, so tiering works out of the box with a config option. There is no module to
   download, load, version, or operate.
2. **One storage engine interface.** Storage engines will differ based on the workload
   types they are targeting, so they sit behind a single interface (`storageType`:
   open/close, async put/get/del, poll_completions, snapshot hooks).
3. **Modular internal APIs for custom storage engines.** The same interface is exposed
   through `ValkeyModule_RegisterStorageBackend`, so custom storage engines load as
   modules, driven through the identical call path as the built-in engine. This works
   today. In V1 these APIs are internal: they are expected to evolve as tiering matures,
   which is why they are not yet stable. A stable public module API follows once the
   interface hardens with real usage.

**Why in core, with modular internal APIs:**

1. **The tiering logic needs core execution paths.** Blocking a client mid-command,
   eviction integration, object-header state, and persistence hooks are all core paths
   that no module can reach today.
2. **The part that will differ is pluggable.** Storage engines will differ based on the
   workload types customers target, so they sit behind one interface. Two working
   providers exist today (built-in and module) through an identical call path.
3. **In core, tiering is tested with every engine change.** A breaking change fails a
   named test on the PR instead of surfacing downstream months later.
4. **Lowest barrier to adoption.** Tiering enables with a config option. Nothing to
   download, load, version, or operate.

> **Concern 1:** "Every future engine change now has to consider tiering. That is a permanent tax on all contributors."

**Response:** The surface is small and CI-enforced.

- Residency checks sit at a handful of choke points: lookup, add and delete, eviction,
  expiry, persistence. Commands, data types, scripting, and networking never touch
  tiering state.
- The tax exists wherever tiering lives. Out of core, the same engine changes still break
  it, just silently and months later.

> **Concern 2:** "Why can't the whole feature be a module, with no core changes at all?"

**Response:** The hooks tiering needs do not exist in the module API, and adding them
would be at least as invasive as adding the tiering logic itself.

- Exposing those hooks means freezing a stable public ABI over core execution paths, for
  one consumer.
- The part that varies (the storage engine) is already behind a module boundary.

> **Concern 3:** "Are the interfaces ready for future modes like key-spilling and partial tiering, or will they need rework?"

**Response:** The modular internal tiering APIs are expected to evolve as tiering
matures, which is why they are internal.

- Key-spilling is designed, benchmarked, and additive to the storage engine contract. It
  is left as a follow-up feature behind a planned config mode.
- Storage engine designs for future workload types will benefit from real usage feedback,
  so they are designed after V1 rather than guessed now.

> **Concern 4:** "An experimental, undocumented module API is a private API in practice. Why not stabilize it from day one?"

**Response:** Usable from day one. Internal only means the signatures can still change.

- The interface header and the sample module are in the fork. Anyone can build a storage
  engine today, knowing the interface evolves until it stabilizes.
- Stabilizing now means freezing first-guess signatures. The interface should harden
  against real usage: more storage engines, more workloads, more operational experience.
- Stabilization is a planned step, not an open-ended maybe. Once V1 usage shows the
  interface has stopped changing, we document it and make the compatibility promise.

> **Concern 5:** "What does this cost users who never enable tiering?"

**Response:** Zero measurable performance impact when disabled. Benchmark number
presented at the meeting.

- Off by default behind an immutable config. The residency check is a few bits in the
  object header, not a function call.
- A build without tiering carries no storage-library dependency and compiles everywhere
  core does.

### AP2. Key Spilling vs Non Key Spilling: values-only tiering first

**Proposal:** V1 ships non-key-spilling (values-only) tiering: keys, expiry, type, and
metadata stay in DRAM and only values move to flash, so SCAN, DBSIZE, RANDOMKEY, active
expiry, and LRU/LFU semantics stay exactly intact. Key-spilling (moving cold keys to
flash as well) is left as a follow-up feature. The alignment we ask for: values-only
first is the right sequencing.

**Why Non Key Spilling:**

1. **The gain-to-complexity ratio decides it.** On a real production trace, values-only
   tiering cut total cost 65% versus all-DRAM with zero semantic changes. Key-spilling
   adds another 6 to 23% on the value-dominated workloads V1 targets (session stores,
   catalogs, values 500B to 5KB), at the cost of bloom filters for lookup ambiguity, a
   redesign of SCAN, DBSIZE and active expiry, roughly 200 lookup call site audits, and a
   16B per spilled key index floor.
2. **Every Valkey semantic stays exactly intact.** SCAN, DBSIZE, RANDOMKEY, active
   expiry, and LRU/LFU are unchanged because every key stays in the hashtable. For a
   first release the community must trust, exact semantics beat faster benchmarks.
3. **Nothing is thrown away.** The state machine, async IO, storage engine interface,
   throttle, eviction integration, and persistence are all required by key-spilling
   unchanged. This is a sequencing decision, not an architecture rejection.
4. **Key-spilling wins where it suits, which is exactly why it is the follow-up.** Our
   benchmarking shows it pulls ahead on skewed workloads with larger key fractions (up to
   +35 to 44% on metadata-heavy patterns). It lands later as a config mode on the same
   foundation.

> **Concern 1:** "That's not real tiering. Every key still lives in DRAM, so dataset size stays bounded by key count."

**Response:** We know from experience that many workloads see significant benefit from
values-only tiering, and the limit is quantified.

- The irreclaimable share equals the key fraction of the data: roughly 20% for small
  keys, up to half when keys match values. On the production trace it was 21% of bytes,
  and the cost cut was still 65%.
- Key-spilling is the designed way to reclaim that share later.

> **Concern 2:** "If moving keys to disk too is the better architecture, why not start there?"

**Response:** On value-dominated workloads the extra performance costs far more
engineering than it returns.

- The costs are structural, not incremental: every lookup call site where a hashtable
  miss stops meaning the key does not exist has to be audited, and SCAN, DBSIZE, active
  expiry, and LRU/LFU need redesigns.
- Starting simpler ships value sooner and derisks the complex mode. The community
  evaluates the foundation on real workloads first, then key-spilling lands as a focused
  addition instead of an all-at-once bet.

> **Concern 3:** "Keeping every key in memory sounds like a limitation you could engineer around cheaply. Is key-spilling really that hard?"

**Response:** Measured, not estimated.

- In our key-spilling build, a quarter to a third of reads landed on the missing-key
  ambiguity path.
- Key-spilling never frees all of the key memory. The bloom filter and storage index keep
  per-key state in DRAM, which erodes roughly a third of the ideal savings under
  conservative index-size assumptions.
- Worth paying eventually, not worth gating the first release.

> **Concern 4:** "Workloads with small values get nothing from this."

**Response:** Conceded. Small values are a declared anti-target, quantified and
published.

- Per-key overhead dominates at around 100B values, the lowest TPS we measured. Neither
  architecture rescues that pattern.

### AP3. Promotion policy: V1 default and the incremental roadmap

**Proposal:** V1 ships two promotion strategies on an extensible framework:
always-promote (on access, a tiered value moves back to DRAM) and never-promote (it is
served from flash and stays there). Always-promote is the default. Additional strategies
(conditional promotion, no-promote reads, SSD-only mode) land incrementally in follow-up
releases on the same framework with no config break. User-defined policies and manual
tier and untier commands follow once real usage shows what users need.

**The alignment we ask for:** always-promote as the V1 default, never-promote as the
supported alternative, everything else as follow-up.

**Why this roadmap:**

1. **One well-understood default.** Most deployments never touch the config.
   Always-promote matches cache intuition: an accessed value is hot, repeated access is
   served from DRAM after the first touch. That fits the read-heavy skewed workloads V1
   targets (session stores, catalogs).
2. **Never-promote covers the patterns the default handles poorly.** Scans and one-shot
   reads churn DRAM under always-promote. Operators with those patterns switch to
   never-promote and keep DRAM contents predictable.
3. **The framework ships in V1, strategies land incrementally.** New strategies arrive on
   the same hook with no config break, each validated against eviction, throttling, and
   memory accounting before release.
4. **Demand decides the long tail.** User-defined policies and manual commands wait for
   real usage data, so the policy surface grows against demand rather than speculation.

**Promotion Policy table:**

| Default | Strengths | Weaknesses |
|---------|-----------|------------|
| Always-promote | Matches cache intuition (access means hot). Repeated access is served from DRAM after the first touch. Best fit for the skewed read-heavy workloads V1 targets. | Scans and one-shot reads churn DRAM and the spill pipeline. Each cold read implies a future spill write when the value cools again. |
| Never-promote | No churn on scans and one-shot reads. DRAM contents stay predictable. Cold reads stay cheap and repeatable on flash. | Keys turning hot keep paying flash latency until rewritten. Hit rate does not improve as access patterns shift. |

> **Concern 1:** "Always-promote thrashes on scans and one-shot reads."

**Response:** True for those patterns, and it is exactly why never-promote is in the
tradeoff table.

- Scan-heavy patterns are declared anti-targets, but they exist in real fleets. The room
  picks the default with that on the table.

> **Concern 2:** "Why make users wait for the other strategies? Some of them sound simple."

**Response:** None of them is as simple as it sounds.

- Every strategy interacts with eviction, throttling, and memory accounting, and needs
  its own benchmarks and metrics before operators depend on it.

> **Concern 3:** "Without manual tier and untier commands, operators cannot correct the policy when it gets it wrong."

**Response:** V1 gives operators visibility and guardrails instead.

- Runtime metrics (hit rates, spill and fetch rates, items on flash) show policy fit
  live, and size-gated admission bounds the worst mistakes.

### AP4. Default Storage Engine selection

**Proposal:** FlashCache ships as the built-in default storage engine: log-structured,
cache-native GC, async completion model, open-sourcing approved. The same engine loaded
through the module path doubles as the reference module backend, proving the pluggable
interface with real code. We benchmarked FlashCache against RocksDB twice: once
integrated behind the engine's storage interface, and once as raw libraries head-to-head,
iterating the RocksDB integration across six optimization rounds. Storage engine choice
remains a build and config decision.

**Why FlashCache as the default:**

1. **Measured performance in two settings.** Integrated behind the engine (r6gd.2xlarge,
   1GB max memory): FlashCache sustained about 145K TPS at 2.1ms P99, RocksDB reached 45
   to 71K TPS at 5 to 12ms P99. Raw library head-to-head (r7gd.4xlarge, 200M keys, 1KB
   values) on the 80:20 read:write mix tiering targets: FlashCache served 164K reads plus
   63K writes per second, RocksDB at its best configuration (64 threads) served 87K reads
   plus 22K writes. Roughly 2x the throughput, with read p99 at 350us versus 1,300us and
   write p99 at 10us versus 5,500us. Full benchmarking result:
   [raw-storage-benchmark-flashcache-vs-rocksdb.md](raw-storage-benchmark-flashcache-vs-rocksdb.md)
2. **The gap is architectural, not tuning.** FlashCache submits reads asynchronously, so
   one IO thread drives 57 to 84K in-flight read IOPS. Synchronous reads need a thread
   per outstanding IO, so the thread pool becomes the ceiling before the NVMe does.
   RocksDB needed 64 threads to reach half of FlashCache's single-thread throughput.
3. **An LSM costs for what a cache never uses.** Measured write amplification was near 1
   for the log-structured design versus 18 to 28 for RocksDB, read amplification about 10
   versus 26 to 40, with compaction producing multi-second write stalls at p100. Ordered
   iteration, the thing an LSM buys with that cost, is something the engine never asks of
   storage.
4. **Proven at scale.** On 64GB nodes: 150M+ key datasets at 2 to 2.5x memory expansion,
   109K TPS under uniform access and 230K+ with a hot working set. The same architecture
   lineage has backed ElastiCache data tiering in production since 2021.
5. **No lock-in, extensible by design.** FlashCache is being open-sourced alongside the
   fork. The RocksDB integration works today through the identical call path, and anyone
   can build a different storage engine against the interface, natively or as a module,
   without forking the engine. Switching is configuration.

> **Concern 1:** "A vendor-built default backend looks like lock-in."

**Response:** The interface is the commitment. The default is not the definition.

- Nothing FlashCache-specific sits above the storage interface. Switching engines is
  configuration, not a fork.

> **Concern 2:** "Why not a battle-tested store like RocksDB as the default? Everyone knows it and trusts it."

**Response:** We tried it seriously. The remaining gap is architectural.

- Six optimization rounds (dispatcher and thread pools, serialization offload to workers,
  deserialization lock removal, WAL off, direct IO, bloom filters, batched writes) took
  the integration from 3K to about 70K TPS. FlashCache held about 145K on the same box
  with a third of the tail latency.
- Where RocksDB wins, we say so: it leads on pure read-only workloads by 13 to 17%, a
  format efficiency edge. On any mixed workload, including the 80:20 mix tiering targets,
  FlashCache leads.
- RocksDB stays fully buildable behind the same interface, and the methodology and
  configs are published so anyone can rerun the comparison.

> **Concern 3:** "A purpose-built new storage engine is less proven than a decade of RocksDB production."

**Response:** The design has production lineage, and every claim is reproducible.

- The same architecture has backed ElastiCache data tiering in production since 2021.
- Every number ships with configs in the fork. Trust the reruns, not the vendor.

## 7. Alignment Record (filled during the meeting)

| # | Decision | Position (agree / object / follow-up) | Owner / notes |
|---|----------|---------------------------------------|---------------|
| AP1 | Tiering engine in core, module APIs for custom backends (experimental in V1) | | |
| AP2 | Non key spilling (values-only) first, key spilling as a follow-up mode | | |
| AP3 | Promotion policy: V1 default (always-promote vs never-promote) + incremental roadmap | | |
| AP4 | FlashCache as default backend + what a stock build ships | | |

## 8. References

- Public fork (code, `unstable`): https://github.com/dubey02/valkey-data-tiering
- Overview doc (problem, goals, requirements R1-R16, design topics + issues):
  https://github.com/dubey02/valkey-data-tiering/blob/unstable/design-docs/data-tiering/valkey-data-tiering-overview.md
- Interactive: storage interfaces and call stack (click-through to source):
  https://dubey02.github.io/valkey-data-tiering/design-docs/data-tiering/dt-interfaces-compare.html
- Benchmarks and methodology:
  https://github.com/dubey02/valkey-data-tiering/tree/unstable/perf
- Raw storage engine benchmark (FlashCache vs RocksDB):
  [raw-storage-benchmark-flashcache-vs-rocksdb.md](raw-storage-benchmark-flashcache-vs-rocksdb.md)
- TSC alignment doc:
  https://github.com/dubey02/valkey-data-tiering/blob/unstable/design-docs/data-tiering/valkey-data-tiering-tsc-alignment.md
- Upstream discussion: https://github.com/valkey-io/valkey/issues/83
