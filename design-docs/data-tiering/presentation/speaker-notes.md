# Valkey Data Tiering -- Speaker Notes

Companion to `slides.html`. ~25 min talk + 5 min demo.

---

## Slide 1: Title
- Simple intro. "I'm Abhishek, working on adding data tiering to Valkey."

## Slide 2: The Problem
- Start with the pain: "Today, Valkey stores everything in DRAM. When memory is full, data is evicted and lost."
- The cost contrast is the hook: DRAM costs ~100x more than NVMe per GB. Most workloads have uneven access. Hot data is maybe 10-20% of the keyspace. The rest is paying $8/GB rent to sit idle.
- "As Valkey becomes a primary database, not just a cache, this ceiling is a real barrier."

## Slide 3: The Solution
- One sentence: "Keys stay in DRAM, cold values spill to NVMe, and come back transparently when requested."
- Walk through the architecture boxes left to right: client sends a normal command, main thread handles tiering logic (decides what to spill), hands off to an IO thread (serialization + async write), which talks to NVMe via FlashCache.
- Emphasize the three zeros: no new commands (drop-in), no main thread waits (engine stays fast), no evicted keys (data survives).

## Slide 4: Design Tenets
- Don't read them all. Pick 2-3 to emphasize based on the audience.
- For operators: "Transparent by default" and "Engine guarantees extend to tiered data" (replication, failover, persistence all work).
- For engine developers: "Main thread never waits" is the hard constraint. Serialization on the IO thread, not main thread. Client is paused (read handler disarmed), not the engine.
- For storage people: "Pluggable storage backend" -- the vtable has put/get/del + async variants. FlashCache is the default. Module API lets you bring your own.

## Slide 5: Write Path
- Walk through the 5 steps. Emphasize: "Step 1 is completely normal. SET writes to DRAM. Tiering happens later."
- Step 2 happens in beforeSleep() -- the spill controller uses LRU/LFU sampling (same eviction clock Valkey already has) to pick cold values.
- Step 3: "We hand off the robj pointer directly. No memcpy, no serialization on the main thread."
- Step 4: "The IO thread serializes using createDumpPayload, the same RDB DUMP format used for replication. Then FlashCache appends to its staging buffer -- that write is sub-microsecond."
- Step 5: "On completion, the DRAM value is released. The key stays in memory with encoding=TIERED."

## Slide 6: Read Path
- "This is the interesting part. What happens when a client requests a value that is on flash?"
- Step 2: "There is one generic gate -- extStoragePreCommandFilter -- that checks if any of the command's keys are flash-resident. It uses a 3-bit residency check in the object header. This means most commands need zero per-command hooks."
- Step 3: "The client is blocked. We disarm its read handler and re-queue it. The engine keeps serving other clients normally."
- Step 4: "The IO thread reads from FlashCache and deserializes back into a Valkey object."
- Step 5: "The client is unblocked and the command re-executes, now against the value that is back in memory."
- Key audience question to preempt: "What about multi-key commands?" Answer: the gate checks all keys. If any is on flash, all are fetched before the command runs.

## Slide 7: Key State Machine
- "Every key has a 3-bit residency state. The state machine has 6 states."
- Two stable states: ONLY_MEMORY (green) and ONLY_FLASH (orange).
- Two transient states: COPYING_TO_FLASH and COPYING_TO_MEMORY (purple). These are in-flight.
- Two edge states: PENDING_EVICT (GC evicts from flash while a fetch is in-flight) and PENDING_DELETE (DEL arrives mid-flight).
- "Every transition has a named trigger, and every state has a defined command behavior table. If you GET a key that is COPYING_TO_FLASH, we serve it from the still-in-memory value. If you DEL a key that is COPYING_TO_MEMORY, we let the fetch complete and then delete."
- Point them to dt-flows.html for the interactive version with source code modals for each state.

## Slide 8: Why FlashCache
- "We evaluated RocksDB and FlashCache head-to-head on identical hardware."
- The headline: "At 80/20 read/write, FlashCache delivers 1.9x the read throughput and 2.9x the write throughput."
- "But the real story is write amplification. FlashCache has 1.5x write amplification. RocksDB has 25.6x. That is 17x more write wear on your NVMe for the same work."
- "FlashCache write latency is flat at 10 microseconds regardless of workload mix. RocksDB degrades from 10 microseconds at low load to 3.4 milliseconds p50 under write pressure. At p100, RocksDB hits 3.89 seconds -- that is a compaction stall."
- "FlashCache is log-structured with 1 thread. RocksDB needs 4-64 threads for compaction. For a Valkey integration where the engine owns 1 dedicated IO thread, this matters."

## Slide 9: End-to-End Performance
- "This is the full-stack result. Valkey, FlashCache, NVMe, running a realistic workload."
- The anchor: 176K TPS baseline (all DRAM). Tiered Zipfian: 134K TPS (76% of baseline), but with 2.4x the data capacity. No keys evicted.
- "The cost: +0.56ms on GET p50 (0.74 -> 1.3ms). For a workload where 82% of reads hit DRAM anyway, this is the tail, not the typical case."
- Sweet spot: 500B-5KB values. Below 100B the serialization overhead per byte is high. Above 500KB you are NVMe bandwidth-bound.
- "The storage layer has 2-3x headroom. The bottleneck is main thread CPU. Multi-IO-thread support is the next performance target."
- If anyone asks about TTL: Zipfian + TTL=120s gets you 158K TPS (90% of baseline) because expiring keys naturally keep the hot set in DRAM.

## Slide 10: V1 Scope
- Quick pass. "V1 ships whole-value tiering for all native types plus JSON and Bloom modules. Full persistence and replication support."
- Deferred: "Key-spilling -- storing keys on flash too -- is architecturally possible but we have shown the gains do not justify the complexity for V1 target workloads."
- "The architecture does not foreclose anything deferred. The vtable and state machine support evolution."

## Slide 11: Status and Contribute
- "We have started building. M1 (the storage interface scaffolding) is merged. M2 (the FlashCache IO thread) is in review. Next up is M3 (wiring FlashCache end-to-end)."
- "We are a team of 4, on 11 two-week sprints, targeting code-complete in February."
- "Google is reviewing. We are jointly evaluating key-spilling with Yandex Cloud."
- "Everything is on the public fork. Design docs, benchmarks, interactive visualizers, the full POC -- all in the repo. Contributions welcome."

## Slide 12: Demo
- Pre-stage: have a terminal ready with Valkey built on the unstable branch with FlashCache.
- Run the commands live. The key moments:
  1. After loading 200K keys into 50MB maxmemory: `INFO keyspace` shows all 200K keys. "Without tiering, half of these would be evicted."
  2. `INFO ext_storage` shows spilled items count. "These values are on the NVMe."
  3. `GET key:123456` returns the value. "This key was on flash. Fetched back transparently."
- If time: `DEBUG SLEEP 0` then show INFO memory -- used_memory is at 50MB, but you have 100MB worth of data accessible.
- Kill line: "Without tiering, 100K of these keys would be gone. With tiering, they are all here, served from a $0.08/GB NVMe instead of $8/GB DRAM."

## Slide 13: Thank You
- Point to the repo. Mention dt-flows.html specifically -- "it is an interactive visualizer where you can step through every operation and click to see the source code."

---

## Audience Q&A Prep

**Q: What about consistency? Is tiered data second-class?**
A: No. RDB, AOF, replication, failover, slot migration all work. A tiered key appears in an RDB the same as an in-memory key. The snapshot code fetches values back before serializing.

**Q: What happens if NVMe is full?**
A: Bounded behavior is a design tenet. If storage is full, we stop spilling and fall back to normal eviction. No new failure mode.

**Q: How does this compare to Redis tiering?**
A: Different approach. Redis tiering used RocksDB internally. We use FlashCache (log-structured, no compaction, 1.5x write amp vs 25x). Our design is pluggable, so you could use RocksDB if you wanted.

**Q: Why not key-spilling?**
A: Our benchmarks show +6-23% gain on V1 target workloads vs high complexity (bloom filters, 200 callsite audits, SCAN/DBSIZE redesign). Not worth it for V1. Architecture supports it later.

**Q: What is the overhead when tiering is disabled?**
A: Near zero. A single bit-test in the object header. The entire tiering code is compile-time gated (BUILD_EXT_STORAGE=yes/no). A built-out binary is behaviorally identical to upstream.

**Q: Can I use this with cluster mode?**
A: Yes. Slot migration works with tiered data. SWAPDB is O(1) db-id indirection. FLUSHDB drains the store synchronously bounded by IO depth.
