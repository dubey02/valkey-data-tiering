# Valkey Data Tiering Discussions

This document summarizes the discussions so far around Valkey data tiering, including the earlier Slack thread summary, follow-up feedback, and the latest checkpoint discussion. The intent is to capture the main design topics, current direction, areas of agreement, places where we do **not** yet have consensus, and the data needed to move the discussion forward.

This should be treated as a discussion checkpoint, not a final design.

---

## Workloads: Which workloads benefit most from tiering? Are there any particular workloads we should target?

The broad agreement is that tiering is a good fit for workloads with a bounded and relatively stable hot working set, where the hot set is meaningfully smaller than the total dataset. In this model, DRAM serves the hot working set while colder data can live on cheaper storage.

Workloads that scan or rotate through the full dataset sequentially are likely poor fits because they become bounded by secondary-storage performance. Similarly, workloads where almost all data is hot, or where the hot set changes too quickly, may not benefit much from tiering because data will keep moving between memory and storage.

There was also agreement that we should avoid relying only on intuition or community opinions to classify workloads, because those opinions may be biased. A better direction is to use real workload data, workload tracing, and offline analysis tooling.

Madelyn specifically called out that we need a clearer end-user description of which broad workload categories benefit, such as caching, session storage, rate limiting, streaming, and feature-store-like use cases. The key question is not only whether these workloads exist, but which ones materially benefit from SSD-backed tiering.

**Current direction:**

- Build or upstream a workload tracing / offline analysis tool.
- Use workload shape to estimate whether tiering will help.
- Combine workload characteristics with backend storage characteristics to produce performance and cost expectations.
- Use ElastiCache tiering fleet data where possible to ground decisions in real customer behavior.
- Identify both good-fit and poor-fit workloads explicitly.

**Data needed:**

- Hot-set stability across real workloads.
- Read/write ratio and access skew.
- Value-size distribution.
- TTL distribution.
- Disk read/write rate relative to total command rate.
- Client throttling or latency impact from storage fetches.
- GC overhead for write-heavy or delete-heavy workloads.

**Summary:**

Tiering should target workloads where the hot set is much smaller than the total dataset and remains stable enough that cold data can stay on storage. We need workload tracing and fleet data to clearly describe which workload categories benefit and which do not.

---

## Semantics: Transparent tiering vs native disk access vs backing cache

The strongest signal is to start with transparent tiering for V1.

The reason is adoption: most users want to run an existing Redis/Valkey application on a tiering-enabled configuration without changing application code. Transparent tiering gives the "just works" experience.

There was also agreement that transparent tiering and explicit/native disk access are not mutually exclusive. They are different policy modes that can be phased in over time.

The earlier summary said V1 should likely use read-through behavior with promotion on first access. Madelyn pushed back on presenting this as a decided direction. Her feedback was that "read-through with promotion on first access" is a leading option, but we have not yet shown data that proves it is the right choice.

So the updated framing is: transparent tiering is the likely V1 user experience, but the exact fetch/promotion behavior still needs data.

**Current V1 candidate direction:**

- Start with transparent read-through behavior.
- Existing applications should continue to work without code changes.
- On access, data may be fetched from storage and promoted back into DRAM, but this needs benchmark data before being treated as a decision.
- Preserve normal Valkey command behavior as much as practical.

**Future extension:**

- Add native disk access / no-promote reads.
- Support cases where a one-time read should not evict truly hot data from DRAM.
- Model this as a future promotion policy knob or command/policy mode.

**Data needed:**

- Promotion-on-access vs no-promote read comparison.
- Impact of promotion on hot-key eviction.
- Impact of promotion on memory churn.
- Latency impact for one-time reads, scan-like reads, and repeated reads.
- Workloads where promotion helps vs workloads where it hurts.

**Summary:**

V1 should optimize for transparent tiering, but promotion-on-first-access should be treated as a hypothesis until data supports it. Future versions can add explicit access and no-promotion modes for advanced users.

---

## Tiering policy: Auto vs manual vs custom/user-defined policies

The discussion converged on a policy model where tiering behavior can be split into separate dimensions:

- **Admission policy:** where a key goes when first inserted.
- **Spilling policy:** when a key moves from DRAM to storage.
- **Promotion policy:** when a key moves from storage back to DRAM.
- **Storage eviction policy:** what happens when the storage tier fills.

The proposed basic V1 policy was:

`admit to DRAM → spill using existing Valkey eviction policies → promote on first access`

This keeps V1 easy to understand and minimizes new knobs.

However, based on Madelyn's feedback, this should not be written as consensus yet. The admission/spill/promotion/storage-eviction model is useful, but the actual V1 choices need more data, especially around promotion behavior.

There was caution from Ping that policy knobs can become complex very quickly. The recommendation was to delay knobs until they are justified, possibly even starting with no new user-visible knobs by default.

Madelyn added that data should guide the initial set of knobs, and noted that in ElastiCache, most users do not change the default eviction policy even when it might help them. This is a useful signal that adding knobs does not guarantee users will use them correctly.

**Current direction:**

- Start opinionated and simple.
- Reuse existing eviction policies such as LRU, LFU, and random as candidate spilling policies.
- Avoid exposing too many knobs in V1.
- Keep admission, spilling, promotion, and storage eviction as separate concepts internally.
- Design the policy framework so future knobs can be added incrementally.

**Potential future policies:**

- Admit directly to storage.
- No-promote reads.
- Promote only after repeated accesses.
- Custom promotion rules.
- Storage-tier eviction policies.
- Explicit/manual tiering controls.
- Key-level or pattern-level tiering hints.

**Data needed:**

- Whether existing eviction policies work well as spill policies.
- How LFU/LRU/random behave under tiering.
- Whether promotion-on-access causes unnecessary churn.
- Whether users need manual controls in common workloads.
- What happens when storage fills.

**Summary:**

V1 should likely be automatic and transparent with minimal knobs. The policy model should separate admission, spilling, promotion, and storage eviction, but the exact V1 behavior needs data before being treated as decided.

---

## Pluggable storage: How should we think about different storage backends?

There was agreement that storage should be pluggable in principle, but V1 should not try to support many backends immediately.

Ping suggested starting OSS with one storage option: a locally attached block device, possibly emulated. Hyperscalers or downstream vendors can do more specialized backends.

The discussion also called out that different storage backends will have different interfaces, latency profiles, IOPS behavior, batching behavior, durability guarantees, and semantic limitations. Therefore, users need a way to estimate what experience they will get before deploying.

A storage backend is not just an implementation detail. Its capabilities influence which Valkey semantics can be preserved efficiently.

**Current direction:**

- Start with one local block-device-oriented backend for OSS.
- Keep the design open for pluggable storage drivers.
- Classify/model storage devices by interface and performance characteristics.
- Tie backend choice to workload-analysis tooling.
- Let the analysis tool recommend whether a workload/backend combination is a good fit.

**Storage properties to evaluate:**

- Read IOPS.
- Write IOPS.
- Read latency distribution.
- Write latency distribution.
- Queue depth behavior.
- Batching behavior.
- GC overhead.
- Durability behavior.
- Crash recovery model.
- Raw block device vs filesystem-backed storage.

**Summary:**

V1 should start with one simple local storage backend, while the module/driver boundary should leave room for future backend-specific implementations.

---

## Module vs core: Should this live as a module or part of core?

The direction is not purely module or purely core. The emerging view is both:

- Valkey core needs changes to understand tiered key states, client blocking, async fetch, command re-execution, eviction/spilling hooks, replication/AOF/RDB interactions, and command semantics.
- The storage implementation should live behind a module-style or driver-style interface so different storage engines can implement the interface differently.

Qu mentioned an existing POC with changes in core and the storage layer implemented separately as a module, with a thin interface in between.

Ping's caution was that the interface will not be perfect in one shot. It should evolve over time as questions around eviction behavior, tiering granularity, replication, AOF, and snapshotting become clearer.

The earlier summary said we should start with a narrow V1 interface and evolve it. Madelyn pushed back that people do not yet understand what a narrow V1 interface would look like. That means the interface itself is one of the key open design areas.

**Current direction:**

- Make necessary Valkey core changes.
- Keep storage backend implementation outside core behind a module/driver interface.
- Start with an opinionated interface that supports the V1 behavior.
- Do not claim the interface is already clear; make it concrete through a design doc and prototype.
- Evolve the interface over time rather than trying to design the perfect abstraction upfront.

**Likely interface responsibilities:**

- Spill/store a value asynchronously.
- Fetch a value asynchronously.
- Delete a value from storage.
- Handle overwrite/update behavior.
- Report completion/failure to core.
- Support backpressure and storage-full behavior.
- Provide storage metrics.
- Possibly support snapshot/restore hooks.
- Possibly expose backend capability flags.

**Open questions:**

- What is the narrowest useful V1 interface?
- Does the interface accept serialized blobs or typed Valkey objects?
- Who owns serialization/deserialization?
- How are expiry, version, DB ID, key metadata, and object type represented?
- How are in-flight spill/fetch races handled?
- How does the interface interact with RDB, AOF, and replication?

**Summary:**

Core needs tiering awareness, but storage should be modular/pluggable. The core/module interface is not yet well understood and should be one of the first concrete design documents and POCs.

---

## Key handling: Key spilling vs keeping keys fully in memory

This was one of the more important scoping discussions.

V1 direction appears to be moving toward keeping key metadata in memory rather than full key spilling.

**Reasoning:**

- Full key spilling can improve cost savings for some workloads.
- But it adds significant complexity around lookup, metadata, SCAN, RDB/AOF, replication, recovery, and command semantics.
- The benefit may not justify the complexity for V1.
- Keeping keys in memory allows Valkey to preserve more existing keyspace behavior.
- If keys remain in memory, commands like EXISTS, TYPE, TTL, and SCAN can potentially work using metadata without always going to storage.

For small string/integer values, keeping key metadata in DRAM may consume enough memory that spilling the tiny value gives little or no benefit.

Viktor suggested excluding small string/integer values from tiering initially if key metadata remains in memory.

Your prototype work on key spilling also seems to be reducing confidence that key spilling is worth doing immediately, due to complexity vs benefit tradeoff.

Madelyn's feedback was that if keys stay in memory, we should describe this as a more complete semantic model, not only as a memory optimization. Keeping keys in memory lets V1 preserve keyspace semantics more naturally.

**Current direction:**

- Start with values tiered to storage while key metadata remains in memory.
- Consider excluding very small values from tiering in V1.
- Design with a path toward key spilling later, but do not make it a V1 requirement.
- Use prototype results to decide whether key spilling is worth pursuing.

**Metadata likely needed in memory:**

- Key name.
- Object type.
- Expiry metadata.
- State: in memory, on storage, in-flight spill, in-flight fetch.
- Storage handle or pointer.
- Version/generation for race handling.
- Enough metadata to support keyspace operations.

**Data needed:**

- Per-key memory overhead when value is tiered.
- Break-even value size for meaningful savings.
- Memory savings by value size distribution.
- Complexity and latency overhead of full key spilling.
- Impact of excluding small values.

**Summary:**

V1 should likely avoid full key spilling and keep key metadata in memory. This simplifies semantics and reduces implementation risk. Key spilling remains a possible future optimization, but only if data shows the savings justify the added complexity.

---

## Priorities: Speed vs cost vs system complexity

Ping framed this as a tradeoff rather than a strict priority order. Users are effectively choosing among performance, cost, and complexity depending on workload and backend.

The desired experience is that this tradeoff should be tied to the chosen backend and workload shape, rather than being deeply coupled to Valkey core complexity.

The main value proposition of tiering is cost reduction: users should be able to store much larger datasets without proportionally increasing DRAM cost. However, that comes with potential tradeoffs in latency, CPU, storage I/O, operational complexity, and failure modes.

**Current thinking:**

- Do not try to maximize speed, cost savings, and simplicity all at once.
- Make the tradeoff understandable to users.
- Use tooling to estimate expected behavior.
- Keep V1 simple enough to be reliable and understandable.
- Let advanced backends or future policies expose different tradeoff points.

**Summary:**

The goal is not to pick speed, cost, or simplicity globally. The goal is to let users trade performance for cost while staying within the Valkey ecosystem, with complexity hidden as much as possible.

---

## Feature parity: Should storage match Valkey semantics fully?

There was an important clarification: feature parity does not mean imitating other databases. It means preserving Valkey semantics even when data is on storage.

**Example questions:**

- If keys are spilled, should SCAN include both DRAM and storage keys?
- If keys remain in memory, does SCAN naturally include tiered keys?
- Can some storage backends support SCAN while others do not?
- Is it acceptable to have separate commands for scanning memory vs storage?
- Should disk-resident keys behave exactly like memory-resident keys for all commands?
- Which commands can be served from metadata only?
- Which commands require fetching the value?

Dante's leaning was that maintaining Valkey semantics gives the best user experience, but some constraints can significantly increase complexity. Therefore, each command/feature should be evaluated explicitly.

Ping suggested capturing these questions in a design doc because the surface area is too large for Slack.

A possible V1 simplification discussed earlier was that SCAN may initially return only in-memory keys. However, if V1 keeps keys in memory, then SCAN can potentially preserve normal behavior more naturally by returning all keys, including keys whose values are tiered.

**Current direction:**

- Preserve Valkey semantics where feasible.
- Use "keys stay in memory" to preserve more complete keyspace semantics in V1.
- Explicitly document any semantic gaps or V1 limitations.
- Evaluate command parity by complexity and user demand.
- Avoid trying to solve the full surface area in Slack.

**Need to define:**

- Metadata-only commands.
- Fetch-required commands.
- Unsupported or limited commands.
- Behavior for scripts/functions/transactions.
- Behavior for keyspace notifications.

**Summary:**

The desired end state is strong Valkey semantic compatibility. V1 may still need documented limitations, but each semantic gap should be evaluated deliberately rather than assumed.

---

## Persistence: RDB/AOF/snapshot compatibility

Persistence came up as a key prerequisite, especially in the module/core discussion.

Open questions include:

- How should RDB represent tiered keys?
- Should RDB include storage-resident values directly?
- Should RDB include references to storage records?
- How does restore work if values are on external storage?
- How should AOF represent spills and promotions?
- Are spill/promotion operations logical state changes or internal implementation details?
- During AOF rewrite, should tiered values be materialized?
- How does replication interact with tiered data?
- Does the replica also tier independently, or does it follow primary tiering state?

No final decision was reached in the Slack discussion, but the consensus is that these must be addressed in the design spec before the interface can be considered complete.

Madelyn agreed that persistence and replication need to be part of the design and should not be afterthoughts.

**Current direction:**

- Treat RDB, AOF, and replication as core design dependencies.
- Do not hide these behind the storage module without defining the contract.
- Start with an opinionated V1 behavior and evolve over time.
- Decide whether spill/fetch is an internal implementation detail or part of persisted/replicated state.

**Likely design questions:**

- Does the primary replicate logical writes only?
- Should replicas make independent tiering decisions?
- What happens during failover if primary and replica have different tiering layouts?
- How does crash recovery reconcile core metadata and storage records?
- Are orphaned storage records garbage-collected on restart?

**Summary:**

Persistence and replication are not optional details. They are core constraints that will shape the module interface and semantic model.

---

## Partial tiering: Large hashes, sets, and lists

Viktor raised partial tiering of large aggregate data structures as a missing topic.

The question is whether tiering operates only at full-key granularity or whether parts of a large hash/list/set can live on storage while other parts remain in memory.

No final decision was reached, but based on the rest of the conversation, partial tiering seems likely to be out of scope for V1 because the group is already leaning toward a simpler full-key tiering model first.

Partial tiering could matter for large aggregate values where commands touch only a small part of the object. For example:

- A hash with many fields where most operations access one field.
- A list where operations mostly touch head or tail.
- A sorted set where range queries touch a small subset.
- A stream with large history but active recent entries.

However, partial tiering would require much deeper data-structure-specific design and could significantly increase complexity.

**Current likely direction:**

- Start with full-key/value tiering.
- Do not attempt partial object tiering in V1.
- Revisit partial tiering later for large hashes/lists/sets/zsets/streams if workload data shows strong demand.

**Summary:**

Partial tiering is important but probably too complex for V1. Full-key/value tiering should be the initial scope.

---

## Data type coverage: Which Valkey data types should V1 support?

The discussion has mostly focused on tiering semantics at the key/value level, but V1 still needs to define which data types are supported.

The simplest starting point is strings, because full-value spill/fetch is easiest to reason about. Supporting all core types through whole-object serialization may be possible, but it creates more command-specific and serialization-specific concerns.

**Data type considerations:**

- **Strings:** likely easiest V1 target.
- **Hashes:** full-hash tiering is simpler, but inefficient for single-field operations on large hashes.
- **Lists:** full-list tiering may be expensive for head/tail/range operations on large lists.
- **Sets:** full-set fetch may be expensive for membership checks or set operations.
- **Sorted sets:** ordering/indexing makes partial tiering complex.
- **Streams:** likely complex because of append, trim, range reads, consumer groups, and pending entries.
- **Modules:** may need explicit serialization support or may be excluded from V1.

**Current direction:**

- Start with string or full-object tiering.
- Avoid partial tiering in V1.
- Build a command/data-type compatibility matrix.
- Decide whether V1 means "strings only" or "all core types as whole serialized objects."

**Summary:**

V1 needs an explicit data type support statement. Strings are the cleanest starting point, while complex data types may initially be supported only through whole-value fetch or deferred to later phases.

---

## Fetch, blocking, and command re-execution

Transparent tiering requires Valkey to fetch values from storage when a command touches a tiered value. This introduces async behavior into a system where command execution usually expects data to be available in memory.

The current POC direction includes:

- Core spill-to-external-storage logic.
- Client blocking while a value is fetched.
- Async key/value fetch from storage.
- Command re-execution once data is available.

This area is central to the core changes needed for tiering.

**Important states:**

- In memory.
- On storage / on disk.
- In-flight spill.
- In-flight fetch.
- Possibly deleted/tombstone/error states.

**Race cases to handle:**

- Key deleted while spill is in flight.
- Key updated while spill is in flight.
- Key expires while fetch is in flight.
- Multiple clients fetch the same key concurrently.
- Fetch completes after key was modified.
- Storage write succeeds but core metadata update fails.
- Core crashes while spill/fetch is in progress.

**Current direction:**

- Keep state machine explicit.
- Coalesce concurrent fetches where possible.
- Use version/generation checks to avoid stale fetch/spill results.
- Treat command re-execution as a core responsibility.

**Summary:**

Async fetch, client blocking, and command re-execution are core pieces of transparent tiering. A clear state machine and race-handling model are required before the design can be considered complete.

---

## Storage engine requirements and performance expectations

For tiering to be useful, the storage engine needs to provide high random-read performance with predictable latency and manageable CPU overhead.

The current engine direction uses a hash-based index on SSD/NVMe, async I/O, and an ASIO/message-passing layer. Earlier experiments showed large differences between storage approaches, including raw block-device-backed designs and filesystem/RocksDB-like designs.

Important observation from prior experiments: disk utilization may be low while CPU is high, especially if serialization/deserialization and message passing are expensive. This means improving disk IOPS alone may not improve end-to-end tiering performance if CPU becomes the bottleneck.

**Storage backend requirements:**

- Async reads and writes.
- High random-read IOPS.
- Efficient write batching.
- Low read amplification.
- Low CPU overhead.
- Efficient garbage collection.
- Crash recovery behavior.
- Backpressure and storage-full signaling.
- Metrics for latency, IOPS, queue depth, GC, and errors.
- Ability to run on local NVMe / raw block devices.

**Data needed:**

- Storage-only IOPS and latency.
- End-to-end Valkey tiering latency.
- CPU cost of serialization/deserialization.
- CPU cost of message passing.
- Raw block vs filesystem comparison.
- GC overhead under overwrite/delete-heavy workloads.

**Summary:**

The storage layer must be optimized for tiering access patterns, not just generic persistence. End-to-end performance must separate disk bottlenecks from CPU, serialization, and messaging overhead.

---

## Other systems: What should we learn from existing systems?

Ping's point was that we should reuse proven technologies, but not imitate other databases just to be "on par."

The core value proposition should stay Valkey-specific: allow users to trade performance for cost without leaving the Valkey ecosystem.

This means we can learn from other systems, especially around storage engines, caching/tiering policies, operational knobs, and workload fit, but we should not let feature parity with other databases drive the design.

Relevant systems to learn from include:

- Redis Enterprise / Redis on Flash-style tiering.
- Aerospike hybrid memory / storage architecture.
- RocksDB-backed cache/database designs.
- KeyDB or other Redis-compatible storage experiments.
- Analytics systems that separate hot/cold tiers.

**Summary:**

Use proven storage/tiering ideas where helpful, but optimize for Valkey's user experience and semantics rather than copying another database.

---

## GitHub/process: Where should this discussion live?

There was agreement that important discussion should move to GitHub because Slack history may disappear after 3 months.

Possible locations discussed:

- RFC repo: likely inactive/dead right now.
- Existing issue: already present but dormant and more pitch-focused.
- Design spec in repo: better once the design has gelled.
- New GitHub issue: likely best for collecting the design discussion and linking to older artifacts.

Since the last checkpoint, we created a public fork for this work:

https://github.com/dubey02/valkey-data-tiering

The plan is to use this fork for initial code, design documents, experiments, and benchmark data. Existing Valkey data tiering issues will be updated to point contributors to this fork for ongoing discussion and progress tracking. Later, once the data tiering work becomes more concrete, the relevant changes can be consolidated into a separate Valkey branch.

**Current direction:**

- Continue Slack discussion briefly while ideas are still forming.
- Post iterative summaries in Slack.
- Use the public fork as the durable working area.
- Update existing issues to point to the fork.
- Create focused GitHub issues/design docs for specific topics.
- Keep the GitHub discussion alive with regular updates and data.

**Summary:**

Slack can be used for early iteration, but durable decisions, design summaries, POC results, and benchmark data should move to GitHub soon.

---

## Follow-up from Madelyn on the previous checkpoint

After the previous checkpoint summary was posted, Madelyn responded with several important corrections and clarifications.

Her main feedback was:

- More data is needed before claiming that read-through with promotion on first access is the right V1 behavior.
- Some points in the summary were stated more strongly than what the Slack thread actually supported.
- The "narrow V1 interface" is not yet clear to contributors; people do not yet share an understanding of what it looks like.
- If keys stay in memory, the semantics should be described more completely.
- Persistence and replication need to be part of the design.
- Partial tiering should not be the starting point.
- The process should move toward GitHub/design documents for durable discussion.

The response was that we will add more data for the topics called out and use the new public fork as the place for initial code, design documents, and benchmark results.

**Impact on this summary:**

- Promotion-on-access is now described as a leading candidate, not a settled decision.
- The V1 module/core interface is described as an open design problem, not a solved abstraction.
- "Keys stay in memory" is described as a semantic simplification, not only a memory tradeoff.
- Persistence, replication, and recovery are elevated as core design topics.
- Partial tiering remains future work unless data shows it must be prioritized earlier.

**Summary:**

Madelyn's feedback mainly pushes the discussion toward being more data-driven and more precise about what is consensus vs what is only a candidate direction.

---

## Overall emerging V1 direction

A reasonable summary of the emerging V1 design is:

Build transparent full-key/value tiering for Valkey, with key metadata kept in memory, values spilled to a local block-device-backed storage module, spilling likely driven by existing Valkey eviction policies, and fetched asynchronously when accessed. Keep the user experience simple and mostly knob-free, while designing the core/module interface so future policies, storage backends, native disk access, no-promote reads, key spilling, and partial tiering can be added later.

However, this should be presented as a **working hypothesis**, not final consensus.

More specifically:

- Transparent tiering first.
- No application changes required.
- Read-through behavior is likely, but promotion-on-access needs data.
- Existing eviction policies are candidate spilling policies.
- Admission, spilling, promotion, and storage eviction should be separate concepts internally.
- Few or no new knobs in V1.
- Storage implementation behind a module/driver interface.
- Core changes are expected.
- Start with locally attached block-device-oriented storage.
- Keep key metadata in memory.
- Exclude or deprioritize very small values initially.
- Full-key/value tiering first; partial tiering later.
- Preserve Valkey semantics where feasible; document V1 gaps.
- Treat persistence, replication, and recovery as core design constraints.
- Use workload tracing and real data to justify future knobs and scope.

---

## Open decision points

The main open decision points are:

1. Which workloads should V1 explicitly target?
2. What workload categories are good fits vs poor fits?
3. What value-size threshold makes tiering worthwhile?
4. Should reads from storage promote values back to memory?
5. Should no-promote reads exist in V1 or later?
6. Can existing eviction policies act as spilling policies?
7. What should happen when storage fills?
8. What metadata must remain in memory?
9. Should small values be excluded from tiering?
10. What is the minimum useful core/module API?
11. Who owns serialization/deserialization?
12. Which data types are supported in V1?
13. Which commands require special handling?
14. How should SCAN behave?
15. How should scripts, functions, and transactions interact with async fetch?
16. How should RDB represent tiered values?
17. How should AOF represent or ignore spill/fetch operations?
18. Should replicas tier independently?
19. What are the crash consistency guarantees?
20. What storage backend should OSS V1 use?
21. What metrics are required for users to operate tiering safely?
22. What is explicitly out of scope for V1?
