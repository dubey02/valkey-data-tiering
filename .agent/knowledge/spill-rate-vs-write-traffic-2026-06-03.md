# Spill Rate Trailing Incoming Writes → 4× Hard-Cap OOM (2026-06-03)

## Symptom
Under sustained high-concurrency write load, the active spill loop cannot drain
memory fast enough. `used_memory` climbs to the **4× maxmemory hard cap**, at
which point `extStoragePerformEvictions` returns `C_ERR` and every subsequent
write is OOM-rejected. Population/ingest stalls before completing.

## Repro (W5 stream, FlashCache tiering)
```
cd benchmark
./benchmark.sh --remote --config flashcache-stream W5
```
Config: `maxmemory=50mb`, `allkeys-lru`, 100K keys × 10 items × 100B, 200 clients,
50 parallel `cli --pipe` populate workers, real FlashCache on r7gd.8xlarge.
4× cap = 200 MiB.

## Evidence (two runs, same config)
| Run | DBSIZE reached | used_memory | spilled | in-flight spills | oom_reject_write_count | disk_util |
|-----|----------------|-------------|---------|------------------|------------------------|-----------|
| A   | 67,935/100,000 | 210 MB      | 20,248  | 0                | 307,852                | ~0–1%     |
| B   | 86,301/100,000 | ~206 MB     | 39,804  | 21 (cap 50)      | 137,749                | ~0–1%     |

Run-to-run numbers vary (the populate race is non-deterministic) but the failure
mode is identical: incomplete populate + writes OOM-rejected at the hard cap.

## Root cause: main-thread spill submission, NOT the backend
- In-flight spills stay **well under** the concurrency cap (`SPILL_CONCURRENT_BASE
  = 50`), and disk utilization is ~0–1%. The storage backend is **idle** — it is
  not the bottleneck.
- The active spill loop runs **on the main thread** and submits at most
  `items_spillover_batch_size` (=10) candidates per event-loop iteration. Under
  200-client write load the main thread is CPU-bound on command execution, so the
  spill submit rate trails the memory-growth rate.
- The hard-cap path (`used_memory > maxmemory * 4` in `ext_storage.c`) only
  *rejects* writes; it does **not** escalate spilling. So once memory diverges it
  never recovers.

## Why streams expose it (and hash/list/set/zset don't, same config)
Per-key RAM differs sharply: ~4.4 KB/resident stream key vs ~1.5 KB/resident hash
key (rax + listpack macro-nodes + `stream` struct fixed overhead). Streams cross
the 4× cap at far fewer keys and faster, and `XADD` (rax insert + auto-ID) costs
more main-thread CPU per op — so memory growth outruns submission. The lighter
types stay under the cap, so the same loop keeps pace and they populate to 100K.

## NOT a serialize/deserialize bug
Compound serialize (RDB DUMP format) and fetch-back are correct — hash/list/set/
zset each spilled and fetched 300K+ values intact in validation. This is purely a
spill-throughput vs ingest-rate imbalance.

## Fix directions (submit-side; backend has headroom)
- Escalate spill batch / submit until in-flight approaches the concurrency cap as
  `used_memory` approaches the hard cap (the 50 in-flight slots are never filled).
- Spill RAM-heavy types (streams) earlier / weight them in candidate selection.
- Consider a synchronous drain-spill burst before OOM-rejecting at the hard cap.

## Reporting
W5 now reports this as a test outcome (`POPULATE_ABORTED` + report "Outcome"
block) instead of silently aborting — see `scenarios/mixed-rw-compound/run.sh`
and `tools/generate-report/generate-report.py`.
