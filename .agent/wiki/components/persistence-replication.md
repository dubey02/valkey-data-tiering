---
title: Persistence & Replication
status: active
sources:
  - src/rdb.c:1190-1195
  - src/rdb.c:1188-1212
  - src/rdb.c:1242-1251
  - src/rdb.c:1447
  - src/rdb.c:1480-1487
  - src/rdb.c:1483-1545
  - src/rdb.c:1516
  - src/rdb.c:1569-1578
  - src/rdb.c:1636
  - src/rdb.c:1668-1693
  - src/rdb.c:1700-1733
  - src/rdb.c:1745-1761
  - src/rdb.c:1786-1789
  - src/rdb.c:3803-3808
  - src/rdb.c:3845-3947
  - src/rdb.c:4028-4036
  - src/rdb.c:4079-4085
  - src/ext_storage.c:1693-1715
  - src/ext_storage.c:1716-1720
  - src/ext_storage.c:1721-1732
  - src/ext_storage.c:1734-1761
  - src/ext_storage.c:1763-1773
  - src/ext_storage.c:1775-1814
  - src/ext_storage.c:1816-1828
  - src/ext_storage_bridge.h:36-40
  - src/aof.c:1446
  - src/aof.c:2356-2409
  - src/aof.c:2414-2456
  - src/aof.c:2447-2496
  - src/aof.c:2458-2519
  - src/aof.c:2528-2559
  - src/aof.c:2530-2532
  - src/aof.c:2645-2663
  - src/aof.c:2683-2694
  - src/config.c:3339
  - src/config.c:3345
  - src/defrag.c:704-712
  - src/defrag.c:712-713
  - src/evict.c:345-358
  - src/evict.c:528-540
  - src/object.c:1205-1211
  - src/object.c:1237-1243
  - src/server.h:774
  - src/server.h:779-839
  - src/server.h:824
  - src/server.h:833-834
  - src/replication.c:1016-1022
  - src/replication.c:1018-1021
  - tests/unit/data-tiering/ext-storage-snapshot.tcl:70-227
  - tests/unit/data-tiering/ext-storage-persistence.tcl:82-87
updated: 2026-07-30
type: component
tier: working
claim_count: 16
edges:
  - to: components/state-machine.md
    kind: depends_on
    source: llm_relation
    created: 2026-07-30
    note: materialization/skip decisions read tiering_state (PENDING_DELETION) and the COPYING_* settle invariant
  - to: components/serialization.md
    kind: depends_on
    source: llm_relation
    created: 2026-07-30
    note: the on-flash DUMP payload IS the RDB entry — footer stripped, spliced in verbatim
  - to: components/memory-accounting.md
    kind: refers_to
    source: llm_relation
    created: 2026-06-05
    note: tiered val is an empty-SDS placeholder, relevant to plain-AOF emit
  - to: components/engine-integration.md
    kind: refers_to
    source: llm_relation
    created: 2026-06-05
  - to: decisions/known-limitations.md
    kind: refers_to
    source: human
    created: 2026-06-03
    note: remaining gaps — non-preamble AOF skip, slot-migration skip, unprepared diskless full-sync fork
  - to: components/backends.md
    kind: depends_on
    source: llm_relation
    created: 2026-07-30
    note: snapshot support is a backend capability; backends without it make saves refuse
  - to: components/bridge-layer.md
    kind: depends_on
    source: llm_relation
    created: 2026-07-30
    note: snapshotHold / gcPause / forkRead are the bridge primitives the snapshot protocol drives
---

# Persistence & Replication

> Tiered values **are** persisted. RDB save, the default AOF base and replication full-sync all
> **materialize** flash-resident values back into standard RDB entries; a fork-time snapshot
> protocol makes that consistent, and paths that cannot do it **refuse the save** rather than
> write a lossy one. What still skips tiered keys: the non-default plain-AOF rewrite, slot
> snapshots (cluster slot migration) and active defrag.

The tiered marker is `objectIsTiered(o)` (`server.h:834`) = `encoding == OBJ_ENCODING_TIERED`
(`server.h:774`); the per-object state lives in the `tiering_state` bitfield (`server.h:824`).
For a tiered object the in-memory value is only an **empty-SDS placeholder** — the real bytes are
on flash (`object.c:1237-1243`; see [memory-accounting](memory-accounting.md)).

## Materialization: the on-flash payload *is* an RDB entry

`rdbSaveKeyValuePair` (`rdb.c:1191`) calls `extStorageMaterializeTiered()` for a tiered value
(`rdb.c:1209-1211`) and, on success, splices the result into the stream: the type byte
(`rdb.c:1245`), the key (`rdb.c:1246`), then the object bytes (`rdb.c:1247`), returning `1`
(`rdb.c:1250`). The inline comment (`rdb.c:1192-1198`) states the outcome plainly — the entry is
*"a 100% standard RDB entry -- loadable by any node, tiered or not (on load the value starts in
memory and re-tiers under memory pressure)"*.

This works because the on-flash format already **is** the serialization format:
`extStorageMaterializeTiered` (`ext_storage.c:1784`) does a synchronous `extStorageBridge_forkRead`
(`ext_storage.c:1797`; declared `ext_storage_bridge.h:40`) that never touches the async IO path,
then strips the 10-byte DUMP footer (`ext_storage.c:1803-1811`) leaving
`[type byte][rdbSaveObject bytes]` — byte-for-byte what `rdbSaveKeyValuePair` would have written
(see [serialization](serialization.md)).

`return 0` (skip the pair) now means only two narrow things (`ext_storage.c:1775-1782`):

| Reason | Cite |
|---|---|
| `TIERING_STATE_PENDING_DELETION` — key is logically gone (client DEL in flight) | `ext_storage.c:1789-1792` |
| Not on flash — GC evicted it before the freeze | `ext_storage.c:1797-1802` |
| Payload ≤ 10 bytes (no type byte under the footer) — defensive | `ext_storage.c:1805-1809` |

`rdbSaveRio` (`rdb.c:1516`) treats `res == 0` as "not saved, not an error" (`rdb.c:1480`, only
`res < 0` is the error branch) and is the single funnel for **every** RDB consumer — the on-disk
file (`rdbSave`, `rdb.c:1636`), the AOF preamble base (`aof.c:2553`) and replication full-sync
(`rdbSaveRioWithEOFMark`, `rdb.c:1578`) — so materialization applies uniformly to all three.

> ⚠️ Forward-compat caveat, from the code's own comment (`rdb.c:1204-1206`): the payload was
> serialized at spill time with **this binary's** `RDB_VERSION`. A future dual-channel replication
> downgrade (`rdbver < RDB_VERSION`) would need a deserialize/reserialize fallback here; there is
> none today.

## The snapshot protocol (what makes a fork-based save consistent)

Documented in the code as a four-step protocol (`ext_storage.c:1693-1715`):

1. **`extStorageSnapshotPrepare()`** (`ext_storage.c:1729`) — a bounded settle loop (200 passes of
   `drainOnly` + `processCompletedStorageRequests`, `ext_storage.c:1736-1742`) until no key is in a
   `COPYING_*` state, i.e. every value is *either* in the hashtable *or* on flash (the
   disjointness invariant); then park the backend IO thread at a safe point
   (`extStorageBridge_snapshotHold`, `ext_storage.c:1756`) so `fork()` inherits no torn state and
   no held locks; then pause backend GC (`extStorageBridge_gcPause(1)`, `ext_storage.c:1757`) so
   flash offsets stay valid for the child. If IO does not settle it logs and returns `C_ERR`
   (`ext_storage.c:1743-1751`).
2. **`fork()`** — the child inherits a consistent CoW hashtable, a consistent CoW backend index and
   stable flash regions (`ext_storage.c:1705-1706`).
3. **`extStorageSnapshotResume()`** (`ext_storage.c:1763`) — parent unparks the IO thread; normal
   spill/fetch traffic resumes while the child writes. **GC stays paused.**
4. **`extStorageSnapshotDone()`** (`ext_storage.c:1768`) — GC unpaused when the child is reaped
   (`backgroundSaveDoneHandler`, `rdb.c:3803-3807`) or when a foreground save finishes
   (`rdb.c:1731`).

A foreground save runs steps 1 and 4 on the main thread and never forks; `rdbSave` prepares only
when `!server.in_fork_child` (`rdb.c:1679`) precisely so a BGSAVE child does **not** re-prepare
over the state it inherited (`rdb.c:1671-1675`).

Observability: `snapshot_supported`, `snapshot_active`, `snapshot_saves`,
`snapshot_tiered_values_saved`, `snapshot_tiered_values_skipped` (`ext_storage.c:1816-1827`;
counters at `ext_storage.c:1716-1719`). Note `snapshot_tiered_saved` is incremented in the *child*
for a BGSAVE, so it is parent-visible only for a foreground SAVE (`ext_storage.c:1718`).

## Refuse rather than lose

When tiered data exists and the backend cannot snapshot it, the save **fails loudly** instead of
writing an incomplete snapshot. All three guards test the same triple —
`!extStorageSnapshotSupported() || extStorageSnapshotActive() || extStorageSnapshotPrepare() != C_OK`
— under `ext_data_enabled && num_items_on_flash > 0`:

| Path | Behaviour | Cite |
|---|---|---|
| Foreground `rdbSave` (SAVE, DEBUG RELOAD, SHUTDOWN save, cron auto-save) | log `"Refusing RDB save: … the active backend cannot snapshot them"` (rate-limited to 1/60s), `errno = EPERM`, `C_ERR` | `rdb.c:1679-1691` |
| `rdbSaveBackground` (BGSAVE, disk-target full-sync) | log `"Refusing background save: …"`, `lastbgsave_status = C_ERR`, `C_ERR` — **before** `fork()` | `rdb.c:1751-1759` |
| `rewriteAppendOnlyFileBackground` (AOF rewrite) | log `"Refusing AOF rewrite: …"`, `aof_lastbgrewrite_status = C_ERR`, `C_ERR` | `aof.c:2653-2661` |
| `SAVE` command | early client error: *"SAVE is not supported while values reside on external storage…"* | `rdb.c:4031-4036` |
| `BGSAVE` command | early client error: *"BGSAVE is not supported while values reside on external storage…"* | `rdb.c:4080-4085` |

`extStorageSnapshotSupported()` is `ext_data_enabled && extStorageBridge_snapshotSupported()`
(`ext_storage.c:1721-1723`) — i.e. a **backend capability** (see [backends](backends.md),
[bridge-layer](bridge-layer.md)). Module-registered backends without snapshot support "still
refuse, loudly" (`rdb.c:1676-1677`) rather than writing a lossy snapshot.

## Where tiering is handled — per path

| Path | Function | Tiered handling | Cite |
|---|---|---|---|
| RDB save | `rdbSaveKeyValuePair` | **Materialize** → standard RDB entry | `rdb.c:1209-1211`, `rdb.c:1244-1251` |
| RDB iterate | `rdbSaveRio` loop | `res==0` ⇒ key omitted (only PENDING_DELETION / GC-evicted) | `rdb.c:1480` |
| RDB fork child CoW hint | `rdbSaveRio` | `dismissObject` **skipped** for tiered (no in-memory value to dismiss) | `rdb.c:1487` |
| AOF base (preamble, **default**) | `rewriteAppendOnlyFile` → `rdbSaveRio` | **Materialize** (inherits the RDB path) | `aof.c:2551-2556` |
| AOF base (plain, `aof-use-rdb-preamble no`) | `rewriteAppendOnlyFileRio` | **Skip** tiered key + `LL_WARNING` | `aof.c:2504-2508` |
| AOF tail | `feedAppendOnlyFile` | logs the write *command*, not the value — tiering-agnostic | `aof.c:1447` |
| Slot snapshot (cluster slot migration) | `rewriteSlotToAppendOnlyFileRio` | **Skip** tiered key + `LL_WARNING` | `aof.c:2446-2450` |
| Active defrag | `defragKey` | **Skip** (`return`) | `defrag.c:713` |
| Full-sync, disk target | `rdbSaveBackground` (`RDBFLAGS_REPLICATION`) | **Materialize**, snapshot-prepared, refuses if unsupported | `replication.c:1021`, `rdb.c:1751-1761` |
| Full-sync, socket target (diskless / dual-channel) | `rdbSaveToReplicasSockets` → `rdbSaveRioWithEOFMark` | **Materialize**, but **no** snapshot prepare ⚠️ | `replication.c:1018`, `rdb.c:3845-3947` |

> ⚠️ The two AOF skip sites have their log strings and comments **swapped relative to their
> enclosing functions.** `rewriteSlotToAppendOnlyFileRio` (`aof.c:2414`, the slot-scoped snapshot)
> logs *"AOF rewrite (non-preamble): skipping tiered key"* (`aof.c:2447-2448`), while
> `rewriteAppendOnlyFileRio` (`aof.c:2458`, the actual non-preamble rewrite reached from
> `aof.c:2558`) logs *"Slot snapshot: skipping tiered key (tiered slot migration not yet
> supported)"* (`aof.c:2505-2506`). The **behaviour** is identical on both (skip + warn), so no
> data-correctness consequence — but an operator reading either warning will mis-attribute which
> path dropped the key. Cosmetic, source-side; not fixed here (wiki is read-only over `src/`).

> ⚠️ `rdbSaveToReplicasSockets` (`rdb.c:3845`) forks at `rdb.c:3925` and serializes via
> `rdbSaveRioWithEOFMark` (`rdb.c:3947`) — which materializes tiered values — but the function
> contains **no** `extStorageSnapshot*` call anywhere in `rdb.c:3845-4020`, and `replication.c`
> contains no tiering code at all. So the diskless/dual-channel full-sync child reads flash
> **without** the settle loop, without the IO-thread park and **without GC paused**, and there is
> no refuse-guard on this path. From the code alone the consequences are unclear: a concurrent GC
> could invalidate an offset mid-read, in which case `forkRead` fails and
> `extStorageMaterializeTiered` returns `0` — silently omitting the key from the replica's dataset
> (`ext_storage.c:1797-1802`) rather than erroring. Tracked as issue #20. Flagged, not asserted: whether
> `repl-diskless-sync` is exercised with tiering anywhere is not established by the tree (no
> snapshot test covers it — ext-storage-snapshot.tcl:70-227 covers SAVE / BGSAVE / DEBUG RELOAD
> only).

## Replication

`replication.c` contains **no** tiering-specific code (zero `objectIsTiered` /
`OBJ_ENCODING_TIERED` / `extStorage*` references). Tiering reaches replication only through
`rdbSaveRio`:

- **Full sync (disk target)** — `rdbSaveBackground(..., RDBFLAGS_REPLICATION | RDBFLAGS_KEEP_CACHE)`
  (`replication.c:1021`) ⇒ snapshot-prepared fork, tiered values materialized, refuses if the
  backend cannot snapshot (`rdb.c:1751-1759`). **A freshly full-synced replica therefore receives
  the primary's tiered data.**
- **Full sync (socket target)** — `rdbSaveToReplicasSockets` (`replication.c:1018`): materializes,
  but unprepared — see the ⚠️ above.
- **Steady-state stream** — write commands propagate at execution time (the same propagation path
  that feeds the AOF tail, `aof.c:1447`), independent of tiering state.

### What a freshly full-synced replica actually holds

**All of the data, entirely in DRAM.** Two facts compose:

1. The RDB stream carries no tiering metadata — a materialized tiered value is an ordinary RDB
   entry, so the loader reconstructs an ordinary in-memory object. There is **no**
   `objectIsTiered` / `OBJ_ENCODING_TIERED` / `extStorage*` reference anywhere in the load half of
   `rdb.c` (all such references sit at `rdb.c:1209-1487` on the save side and `rdb.c:1680-4085` in
   save/command guards). The save-side comment says the same: *"on load the value starts in memory
   and re-tiers under memory pressure"* (`rdb.c:1197-1198`).
2. **A replica does not re-tier.** Spilling is only ever entered from `performEvictions`
   (`evict.c:531-539`, which routes memory pressure to `extStoragePerformEvictions` instead of
   eviction), and `performEvictions` returns immediately via `isSafeToPerformEvictions`
   (`evict.c:345-358`) when `server.loading` is set (`evict.c:348`) or when
   `server.primary_host && server.repl_replica_ignore_maxmemory` (`evict.c:352`).
   `replica-ignore-maxmemory` **defaults to yes** (`config.c:3345`).

So: **a replica stays fully in DRAM** and never spills, unless an operator sets
`replica-ignore-maxmemory no` — at which point it spills under its *own* memory pressure against
its *own* node-local backend, not as a copy of the primary's tiered set. Tiering state itself is
never replicated: each node's tiered set is a local property (see [backends](backends.md),
[bridge-layer](bridge-layer.md)).

> This corrects the previous version of this page, which claimed a fresh replica "does not receive
> already-tiered keys/values". It does receive them; what it does not receive is their *tiered
> state*, and the replica will not recreate it while `replica-ignore-maxmemory` holds its default.

## Active defragmentation

`defragKey` (`defrag.c:704`) returns early for a tiered object (`defrag.c:713`): *"Skip tiered
entries — value is on external storage, nothing to defrag."* The guard sits at the top, before
`activeDefragStringOb` and any `defragLater`/`scanLater*` deferral, so **all** deferred defrag
work is skipped for tiered objects too — there is no in-memory value graph to relocate. Objects
with in-flight tiering IO (`COPYING_TO_FLASH` / `COPYING_TO_MEMORY`) are skipped by a separate
adjacent guard (`defrag.c:715-717`) because the IO thread holds references.

## Remaining limitations (still true)

| Limitation | Status | Cite |
|---|---|---|
| Plain (non-preamble) AOF rewrite **drops** tiered keys | Real. Non-default (`aof-use-rdb-preamble` defaults to 1, `config.c:3339`), warns per key. A RESTORE-based emit is "planned with slot migration" | `aof.c:2504-2508`, `aof.c:2441-2445` |
| **Cluster slot migration unsupported** with tiering — slot snapshots drop tiered keys | Real, no default-path workaround | `aof.c:2446-2450`, `aof.c:2500-2503` |
| Active defrag cannot compact tiered objects | Real, by design (nothing in memory to move) | `defrag.c:713` |
| Fork-child `dismissObject` CoW optimisation skipped for tiered objects | Real, benign | `rdb.c:1487` |
| Backends without snapshot support cannot be persisted at all | Real — every save path refuses | `rdb.c:1679-1691`, `rdb.c:1751-1759`, `aof.c:2653-2661` |
| Diskless/dual-channel full-sync forks without the snapshot protocol | ⚠️ unresolved from code (see above) | `rdb.c:3845-3947` |
| RDB-version downgrade during dual-channel replication has no reserialize fallback | ⚠️ noted by the code itself, unimplemented | `rdb.c:1204-1206` |

The plain-AOF **placeholder-emit** hazard flagged by the previous version of this page is
**resolved in the code**: `rewriteObjectRio` (`aof.c:2357-2409`) still has no `objectIsTiered`
guard of its own, but both of its callers now filter tiered objects before calling it
(`aof.c:2446-2450`, `aof.c:2504-2508`), so a tiered object no longer reaches it and cannot be
emitted from its empty-SDS placeholder.

## Test coverage

- tests/unit/data-tiering/ext-storage-snapshot.tcl:70-227 — 8 tests: INFO fields, SAVE of all
  data types round-tripping through DEBUG RELOAD, TTL survival, BGSAVE loadability, concurrent
  writes during BGSAVE, tiering still functional after the hold/GC release, prepare **refusing**
  when completions are stalled, and re-spill after reload under real memory pressure.
- tests/unit/data-tiering/ext-storage-persistence.tcl:82-87 — *"AOF rewrite (RDB preamble)
  preserves tiered keys"*; its in-file comment confirms tiered keys now survive an AOF reload and
  that the non-preamble path still skips them.

## See also

[state-machine](state-machine.md) · [serialization](serialization.md) ·
[memory-accounting](memory-accounting.md) · [engine-integration](engine-integration.md) ·
[backends](backends.md) · [bridge-layer](bridge-layer.md) ·
[known-limitations](../decisions/known-limitations.md)
